/**
 * Filesystem wrapper for LittleFS, replacing SPIFFS.
 *
 * Copyright Thinnect Inc. 2020
 * Copyright Raido Pahtma 2025
 * @license MIT
 */

#include "fs.h"
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "platform_mutex.h"
#include "spi_flash.h"
#include "lfs.h"
#include "cmsis_os2.h"

#include "loglevels.h"
#define __MODUUL__ "fs"
#define __LOG_LEVEL__ (LOG_LEVEL_fs & BASE_LOG_LEVEL)
#include "log.h"
#include "sys_panic.h"

#ifndef FS_MAX_COUNT
#define FS_MAX_COUNT 1
#endif//FS_MAX_COUNT

#ifndef FS_MAX_SIZE
#define FS_MAX_SIZE (2*1024*1024)
#endif//FS_MAX_SIZE

#ifndef FS_MAX_DESCRIPTORS
#define FS_MAX_DESCRIPTORS 6
#endif//FS_MAX_DESCRIPTORS

#define MAX_Q_WR_COUNT 10
#define MAX_Q_RD_COUNT 10

#define FS_WRITE_DATA 1
#define FS_READ_DATA 2

#define FS_CACHE_SIZE 16
#define FS_LOOKAHEAD_SIZE 16

struct fs_struct
{
	fs_driver_t * driver;
	volatile int ready;
	int partition;
	uint8_t mount_count;
	platform_mutex_t mutex;

    lfs_t fs;
    struct lfs_config cfg;

    uint8_t prog_buffer[FS_CACHE_SIZE];
    uint8_t read_buffer[FS_CACHE_SIZE];
    uint8_t lookahead_buffer[FS_LOOKAHEAD_SIZE];
};
static struct fs_struct fs[FS_MAX_COUNT];

struct fs_files
{
    bool used;
    lfs_file_t file;
    struct lfs_file_config config;
    uint8_t buffer[FS_CACHE_SIZE];
};
static struct fs_files m_files[FS_MAX_DESCRIPTORS];

#ifdef FS_MANAGE_FLASH_SLEEP
static osTimerId_t  m_sleep_timers[FS_MAX_COUNT];
static void fs_suspend_timer_cb(void * arg);
#endif//FS_MANAGE_FLASH_SLEEP

#define FS_THREAD_FLAGS_ALL 0x7FFFFFFFU
#define FS_SUSPENDFLAGS     ((1 << FS_MAX_COUNT) - 1)

// define read/write flags after filesystem suspend timer flags
#define FS_QUEUED_WRITE_FLAG    (0x01 << FS_MAX_COUNT)
#define FS_QUEUED_READ_FLAG     (0x01 << (FS_MAX_COUNT + 1))
#define FS_CHECK_FLAG           (0x01 << (FS_MAX_COUNT + 2))
#define FS_BLOCKING_ACTION_FLAG (0x01 << (FS_MAX_COUNT + 3))

enum fs_blocking_actions
{
	FS_BLOCKING_ACTION_OPEN,
	FS_BLOCKING_ACTION_READ,
	FS_BLOCKING_ACTION_WRITE,
	FS_BLOCKING_ACTION_UNLINK,
	FS_BLOCKING_ACTION_CLOSE,
	FS_BLOCKING_ACTION_STAT,
	FS_BLOCKING_ACTION_SEEK,
	FS_BLOCKING_ACTION_FLUSH,
	FS_BLOCKING_ACTION_INFO
};

struct fs_blocking_action
{
	int file_sys;
	fs_fd fd;
	union
	{
		struct
		{
			char * path;
			uint32_t flags;
		} open;
		struct
		{
			char * path;
		} unlink;
		struct
		{
			void * buffer;
			uint32_t length;
		} read;
		struct
		{
			const void * buffer;
			uint32_t length;
		} write;
		struct
		{
			int32_t offset;
			int32_t whence;
		} seek;
		struct
		{
			uint32_t * p_total;
			uint32_t * p_used;
		} info;
		fs_stat * stat;
	} args;
	enum fs_blocking_actions action;
	int32_t result;
} m_blocking;

static osThreadId_t m_thread_id;
static osMessageQueueId_t m_wr_queue_id;
static osMessageQueueId_t m_rd_queue_id;
static osMessageQueueId_t m_chk_queue_id;

static osMutexId_t m_fs_evt_mtx_id;
static osEventFlagsId_t m_fs_evt_id;

static bool volatile m_fs_ready = false;

typedef struct fs_rw_params
{
	int           file_sys_nr;
	char *        p_file_name;
	void *        p_value;
	int32_t       len;
	fs_rw_done_f  f_callback;
	void *        p_user;
} fs_rw_params_t;

static void fs_thread(void *p);
static void fs_plan_suspend(int f);
static void fs_abort_suspend(int f);
static void fs_mount();


static int fs_read_dev (const struct lfs_config *c, lfs_block_t block,
                        lfs_off_t off, void *buffer, lfs_size_t size)
{
    struct fs_struct * fs = (struct fs_struct*)c->context;
	uint32_t addr = block * c->block_size + off;
    //debug1("fs_read_dev %d:%d (%d,%d,%p)", addr, size, block, off, buffer);
    if (fs->driver->read(fs->partition, addr, size, buffer) < 0)
	{
		return LFS_ERR_IO;
	}
	return LFS_ERR_OK;
}

static int fs_write_dev (const struct lfs_config *c, lfs_block_t block,
                         lfs_off_t off, const void *buffer, lfs_size_t size)
{
	struct fs_struct * fs = (struct fs_struct*)c->context;
    uint32_t addr = block * c->block_size + off;
    //debug1("fs_write_dev %d:%d (%d,%d,%p)", addr, size, block, off, buffer);
    if (fs->driver->write(fs->partition, addr, size, buffer) < 0)
	{
		return LFS_ERR_IO;
	}
	return LFS_ERR_OK;
}

static int fs_erase_dev (const struct lfs_config *c, lfs_block_t block)
{
	struct fs_struct * fs = (struct fs_struct*)c->context;
    uint32_t addr = block * c->block_size;
    //debug1("fs_erase_dev %d (%d)", addr, block);
    if (fs->driver->erase(fs->partition, addr, c->block_size) < 0)
	{
		return LFS_ERR_IO;
	}
	return LFS_ERR_OK;
}

static int fs_sync_dev (const struct lfs_config *c)
{
    //debug1("fs_sync_dev");
    return LFS_ERR_OK;
}


void fs_init (int file_sys_nr, int partition, fs_driver_t * driver)
{
	debug1("fs_init(%d, %d, %p)", file_sys_nr, partition, driver);

    fs[file_sys_nr].ready = 0;
	fs[file_sys_nr].partition = partition;
	fs[file_sys_nr].driver = driver;
	fs[file_sys_nr].mount_count = 0;
	fs[file_sys_nr].mutex = platform_mutex_new("fs");

	fs[file_sys_nr].cfg.context = &fs[file_sys_nr];
    fs[file_sys_nr].cfg.read = fs_read_dev;
    fs[file_sys_nr].cfg.prog = fs_write_dev;
    fs[file_sys_nr].cfg.erase = fs_erase_dev;
    fs[file_sys_nr].cfg.sync = fs_sync_dev;
    fs[file_sys_nr].cfg.read_size = FS_CACHE_SIZE;
    fs[file_sys_nr].cfg.prog_size = FS_CACHE_SIZE;
    fs[file_sys_nr].cfg.block_size = driver->erase_size(partition);
    debug1("block_size %d", fs[file_sys_nr].cfg.block_size);
    fs[file_sys_nr].cfg.block_count = driver->size(partition) / fs[file_sys_nr].cfg.block_size;
    if (fs[file_sys_nr].cfg.block_count > FS_MAX_SIZE / fs[file_sys_nr].cfg.block_size)
    {
        fs[file_sys_nr].cfg.block_count = FS_MAX_SIZE / fs[file_sys_nr].cfg.block_size;
    }
    debug1("block_count %d", fs[file_sys_nr].cfg.block_count);
    fs[file_sys_nr].cfg.block_cycles = 500; // Wear leveling
    fs[file_sys_nr].cfg.cache_size = FS_CACHE_SIZE;
    fs[file_sys_nr].cfg.lookahead_size = FS_LOOKAHEAD_SIZE;
    fs[file_sys_nr].cfg.compact_thresh = 0;
    fs[file_sys_nr].cfg.read_buffer = fs[file_sys_nr].read_buffer;
    fs[file_sys_nr].cfg.prog_buffer = fs[file_sys_nr].prog_buffer;
    fs[file_sys_nr].cfg.lookahead_buffer = fs[file_sys_nr].lookahead_buffer;
    fs[file_sys_nr].cfg.name_max = 64;
    fs[file_sys_nr].cfg.file_max = 0;
    fs[file_sys_nr].cfg.attr_max = 64;
    fs[file_sys_nr].cfg.metadata_max = 0;
    fs[file_sys_nr].cfg.inline_max = 0;

	#ifndef FS_NO_CONFIG_VALIDATION
#if 0
		uint32_t spiffs_file_system_size = fs[file_sys_nr].cfg.phys_size;
		uint32_t log_block_size = fs[file_sys_nr].cfg.log_block_size;
		uint32_t log_page_size = fs[file_sys_nr].cfg.log_page_size;
		// Block index type. Make sure the size of this type can hold
		// the highest number of all blocks - i.e. spiffs_file_system_size / log_block_size
		// DEFAULT: typedef u16_t spiffs_block_ix;
		uint32_t highest_number_of_blocks = spiffs_file_system_size / log_block_size;
		debug1("spiffs_block_ix %u", highest_number_of_blocks);
		if (highest_number_of_blocks > ((1 << 8*sizeof(spiffs_block_ix))-1))
		{
			sys_panic("spiffs_block_ix");
		}

		// Page index type. Make sure the size of this type can hold
		// the highest page number of all pages - i.e. spiffs_file_system_size / log_page_size
		// DEFAULT: typedef u16_t spiffs_page_ix;
		uint32_t highest_page_number = spiffs_file_system_size / log_page_size;
		debug1("spiffs_page_ix %"PRIu32, highest_page_number);
		if (highest_page_number > ((1 << 8*sizeof(spiffs_page_ix))-1))
		{
			sys_panic("spiffs_page_ix");
		}

		// Object id type - most significant bit is reserved for index flag. Make sure the
		// size of this type can hold the highest object id on a full system,
		// i.e. 2 + (spiffs_file_system_size / (2*log_page_size))*2
		// DEFAULT: typedef u16_t spiffs_obj_id;
		uint32_t highest_object_id = (2 + (spiffs_file_system_size / (2*log_page_size))*2);
		debug1("spiffs_obj_id %"PRIu32, highest_object_id);
		if (highest_object_id > ((1 << 8*sizeof(spiffs_obj_id))-1))
		{
			sys_panic("spiffs_obj_id");
		}

		// Object span index type. Make sure the size of this type can
		// hold the largest possible span index on the system -
		// i.e. (spiffs_file_system_size / log_page_size) - 1
		// DEFAULT: typedef u16_t spiffs_span_ix;
		uint32_t largest_span_index = spiffs_file_system_size / log_page_size - 1;
		debug1("spiffs_span_ix %"PRIu32, largest_span_index);
		if (largest_span_index > ((1 << 8*sizeof(spiffs_span_ix))-1))
		{
			sys_panic("spiffs_span_ix");
		}
#endif//0
	#endif//FS_NO_CONFIG_VALIDATION

#ifdef FS_MANAGE_FLASH_SLEEP
	if(file_sys_nr < FS_MAX_COUNT)
	{
		m_sleep_timers[file_sys_nr] = osTimerNew(&fs_suspend_timer_cb, osTimerOnce, (void*)(intptr_t)file_sys_nr, NULL);
	}
#endif
}

void fs_start ()
{
	const osMessageQueueAttr_t wr_q_attr = { .name = "fs_wr_q" };
	m_wr_queue_id = osMessageQueueNew(MAX_Q_WR_COUNT, sizeof(fs_rw_params_t), &wr_q_attr);
	if (NULL == m_wr_queue_id)
	{
		err1("!Queue");
		while(1);
	}

	const osMessageQueueAttr_t rd_q_attr = { .name = "fs_rd_q" };
	m_rd_queue_id = osMessageQueueNew(MAX_Q_RD_COUNT, sizeof(fs_rw_params_t), &rd_q_attr);
	if (NULL == m_rd_queue_id)
	{
		err1("!Queue");
		while(1);
	}

	const osMessageQueueAttr_t chk_q_attr = { .name = "fs_chk_q" };
	m_chk_queue_id = osMessageQueueNew(1, sizeof(fs_rw_params_t), &chk_q_attr);
	if (NULL == m_chk_queue_id)
	{
		err1("!Queue");
		while(1);
	}

	m_fs_evt_mtx_id = osMutexNew(NULL);
	if (NULL == m_fs_evt_mtx_id)
	{
		err1("mtx");
		while(1);
	}

	m_fs_evt_id = osEventFlagsNew(NULL);
	if (NULL == m_fs_evt_id)
	{
		err1("!Evt");
		while(1);
	}

	const osThreadAttr_t thread_attr = { .name = "fs", .stack_size = 4096 };
	m_thread_id = osThreadNew(fs_thread, NULL, &thread_attr);
	if (NULL == m_thread_id)
	{
		err1("!Thread");
		while(1);
	}
}

bool fs_ready ()
{
	return m_fs_ready;
}

int32_t fs_check (int file_sys_nr)
{
	if (NULL == m_thread_id)
	{
		return -7;
	}

	fs_abort_suspend(file_sys_nr);
	platform_mutex_acquire(fs[file_sys_nr].mutex);

	int32_t err = 0; // Nothing to do with LittleFS?

	fs_plan_suspend(file_sys_nr);
	platform_mutex_release(fs[file_sys_nr].mutex);

	return err;
}

int32_t fs_info (int file_sys_nr, uint32_t * p_total, uint32_t * p_used)
{
	if (NULL == m_thread_id)
	{
		return -7;
	}

	int32_t res = -1;
	while(osOK != osMutexAcquire(m_fs_evt_mtx_id, osWaitForever));
	m_blocking.args.info.p_total = p_total;
	m_blocking.args.info.p_used = p_used;
	m_blocking.action = FS_BLOCKING_ACTION_INFO;
	osThreadFlagsSet(m_thread_id, FS_BLOCKING_ACTION_FLAG);
	uint32_t flags = osEventFlagsWait(m_fs_evt_id, 0x1U, osFlagsWaitAny, osWaitForever);
	if (flags == 1)
	{
		res = m_blocking.result;
	}
	else
	{
		err1("fs_info %x", flags);
	}
	osMutexRelease(m_fs_evt_mtx_id);
	return res;
}

static int32_t fs_direct_info (int file_sys_nr, uint32_t * p_total, uint32_t * p_used)
{
	fs_abort_suspend(file_sys_nr);
	platform_mutex_acquire(fs[file_sys_nr].mutex);

	while(!fs[file_sys_nr].ready);
	fs[file_sys_nr].driver->lock();
	lfs_ssize_t used = lfs_fs_size(&fs[file_sys_nr].fs);
	fs[file_sys_nr].driver->unlock();

	if (NULL != p_total)
	{
		*p_total = fs[file_sys_nr].cfg.block_count * fs[file_sys_nr].cfg.block_size;
	}
	if (NULL != p_used)
	{
		*p_used = used * fs[file_sys_nr].cfg.block_size;
	}

	fs_plan_suspend(file_sys_nr);
	platform_mutex_release(fs[file_sys_nr].mutex);

	return used >= 0 ? 0 : used;
}

fs_fd fs_open (int file_sys_nr, char *path, uint32_t spiffs_flags)
{
	if (NULL == m_thread_id)
	{
		return -7;
	}

	fs_fd fd = -1;
	while(osOK != osMutexAcquire(m_fs_evt_mtx_id, osWaitForever));
	m_blocking.file_sys = file_sys_nr;
	m_blocking.args.open.path = path;
	m_blocking.args.open.flags = spiffs_flags;
	m_blocking.action = FS_BLOCKING_ACTION_OPEN;
	osThreadFlagsSet(m_thread_id, FS_BLOCKING_ACTION_FLAG);
	uint32_t flags = osEventFlagsWait(m_fs_evt_id, 0x1U, osFlagsWaitAny, osWaitForever);
	if (flags == 1)
	{
		fd = m_blocking.fd;
	}
	else
	{
		err1("fs_open %x", flags);
	}
	osMutexRelease(m_fs_evt_mtx_id);
	return fd;
}

static fs_fd fs_direct_open (int file_sys_nr, char *path, uint32_t spiffs_flags)
{
	uint32_t lfs_flags = 0;
    if (spiffs_flags & SPIFFS_CREAT)
    {
        lfs_flags |= LFS_O_CREAT;
    }
    if (spiffs_flags & SPIFFS_RDONLY)
    {
        lfs_flags |= LFS_O_RDONLY;
    }
    if (spiffs_flags & SPIFFS_WRONLY)
    {
        lfs_flags |= LFS_O_WRONLY;
    }
    if (spiffs_flags & SPIFFS_TRUNC)
    {
        lfs_flags |= LFS_O_TRUNC;
    }
    if (spiffs_flags & SPIFFS_APPEND)
    {
        lfs_flags |= LFS_O_APPEND;
    }
    if (spiffs_flags & SPIFFS_EXCL)
    {
        lfs_flags |= LFS_O_EXCL;
    }

    fs_abort_suspend(file_sys_nr);
	platform_mutex_acquire(fs[file_sys_nr].mutex);
	while(!fs[file_sys_nr].ready);
	fs[file_sys_nr].driver->lock();
	debug1("open %d: %s", file_sys_nr, path);

    uint8_t fnum = FS_MAX_DESCRIPTORS;
    for (uint8_t i=0; i<FS_MAX_DESCRIPTORS; i++)
    {
        if ( ! m_files[i].used)
        {
            fnum = i;
            break;
        }
    }

    if (fnum >= FS_MAX_DESCRIPTORS)
    {
        return -1;
    }

    m_files[fnum].config.buffer = m_files[fnum].buffer;
    m_files[fnum].config.attr_count = 0;
    m_files[fnum].config.attrs = NULL;
    m_files[fnum].used = true;

    int err = -1;
    for (uint8_t i=0; i<2; i++)
    {
        err = lfs_file_opencfg(&fs[file_sys_nr].fs, &m_files[fnum].file, path, lfs_flags, &m_files[fnum].config);
        debug1("fopen(%s, 0x%x):%d, %d", path, lfs_flags, fnum, err);
        if (err == LFS_ERR_CORRUPT)
        {
            err = lfs_remove(&fs[file_sys_nr].fs, path);
            debug1("rem %d", err);
        }
		else
		{
			break;
		}
    }
    if (0 != err)
    {
        m_files[fnum].used = false;
        fnum = FS_MAX_DESCRIPTORS;
    }
	fs[file_sys_nr].driver->unlock();
	fs_plan_suspend(file_sys_nr);
	platform_mutex_release(fs[file_sys_nr].mutex);
	if(fnum == FS_MAX_DESCRIPTORS)
	{
		return -2;
	}
	return (fs[file_sys_nr].mount_count << 16) | fnum;
}

int32_t fs_read (int file_sys_nr, fs_fd fd, void *buf, int32_t len)
{
	if (NULL == m_thread_id)
	{
		return -7;
	}

	int32_t res = -1;
	while(osOK != osMutexAcquire(m_fs_evt_mtx_id, osWaitForever));
	m_blocking.file_sys = file_sys_nr;
	m_blocking.fd = fd;
	m_blocking.args.read.buffer = buf;
	m_blocking.args.read.length = len;
	m_blocking.action = FS_BLOCKING_ACTION_READ;
	osThreadFlagsSet(m_thread_id, FS_BLOCKING_ACTION_FLAG);
	uint32_t flags = osEventFlagsWait(m_fs_evt_id, 0x1U, osFlagsWaitAny, osWaitForever);
	if (flags == 1)
	{
		res = m_blocking.result;
	}
	else
	{
		err1("fs_read %x", flags);
	}
	osMutexRelease(m_fs_evt_mtx_id);
	return res;
}

static int32_t fs_direct_read (int file_sys_nr, fs_fd fd, void *buf, int32_t len)
{
	int32_t ret = -1;

	fs_abort_suspend(file_sys_nr);
	platform_mutex_acquire(fs[file_sys_nr].mutex);
	while(!fs[file_sys_nr].ready);
	if(((fd >> 16) & 0xFF) == fs[file_sys_nr].mount_count)
	{
		fs[file_sys_nr].driver->lock();
		debug1("read f:%d, %d", fd & 0xFFFF, len);
        ret = lfs_file_read(&fs[file_sys_nr].fs, &m_files[fd & 0xFFFF].file, buf, len);
		fs[file_sys_nr].driver->unlock();
	}
	fs_plan_suspend(file_sys_nr);
	platform_mutex_release(fs[file_sys_nr].mutex);
	return ret;
}

int32_t fs_write (int file_sys_nr, fs_fd fd, const void *buf, int32_t len)
{
	if (NULL == m_thread_id)
	{
		return -7;
	}

	int32_t res = -1;
	while(osOK != osMutexAcquire(m_fs_evt_mtx_id, osWaitForever));
	m_blocking.file_sys = file_sys_nr;
	m_blocking.fd = fd;
	m_blocking.args.write.buffer = buf;
	m_blocking.args.write.length = len;
	m_blocking.action = FS_BLOCKING_ACTION_WRITE;
	osThreadFlagsSet(m_thread_id, FS_BLOCKING_ACTION_FLAG);
	uint32_t flags = osEventFlagsWait(m_fs_evt_id, 0x1U, osFlagsWaitAny, osWaitForever);
	if (flags == 1)
	{
		res = m_blocking.result;
	}
	else
	{
		err1("fs_read %x", flags);
	}
	osMutexRelease(m_fs_evt_mtx_id);
	return res;
}

static int32_t fs_direct_write (int file_sys_nr, fs_fd fd, const void *buf, int32_t len)
{
	int32_t ret = -1;

	fs_abort_suspend(file_sys_nr);
	platform_mutex_acquire(fs[file_sys_nr].mutex);
	while(!fs[file_sys_nr].ready);
	if(((fd >> 16) & 0xFF) == fs[file_sys_nr].mount_count)
	{
		fs[file_sys_nr].driver->lock();
		debug1("write f:%d, %d", fd & 0xFFFF, len);
        ret = lfs_file_write(&fs[file_sys_nr].fs, &m_files[fd & 0xFFFF].file, buf, len);
		fs[file_sys_nr].driver->unlock();
	}
	fs_plan_suspend(file_sys_nr);
	platform_mutex_release(fs[file_sys_nr].mutex);
	return ret;
}

int32_t fs_lseek (int file_sys_nr, fs_fd fd, int32_t offs, int whence)
{
	if (NULL == m_thread_id)
	{
		return -7;
	}

	int32_t res = -1;
	while(osOK != osMutexAcquire(m_fs_evt_mtx_id, osWaitForever));
	m_blocking.file_sys = file_sys_nr;
	m_blocking.fd = fd;
	m_blocking.args.seek.offset = offs;
	m_blocking.args.seek.whence = whence;
	m_blocking.action = FS_BLOCKING_ACTION_SEEK;
	osThreadFlagsSet(m_thread_id, FS_BLOCKING_ACTION_FLAG);
	uint32_t flags = osEventFlagsWait(m_fs_evt_id, 0x1U, osFlagsWaitAny, osWaitForever);
	if (flags == 1)
	{
		res = m_blocking.result;
	}
	else
	{
		err1("fs_seek %x", flags);
	}
	osMutexRelease(m_fs_evt_mtx_id);
	return res;
}

static int32_t fs_direct_lseek (int file_sys_nr, fs_fd fd, int32_t offs, int whence)
{
	int32_t ret = -1;

	fs_abort_suspend(file_sys_nr);
	platform_mutex_acquire(fs[file_sys_nr].mutex);
	while(!fs[file_sys_nr].ready);
	if(((fd >> 16) & 0xFF) == fs[file_sys_nr].mount_count)
	{
		fs[file_sys_nr].driver->lock();
		ret = lfs_file_seek(&fs[file_sys_nr].fs, &m_files[fd & 0xFFFF].file, offs, whence);
		fs[file_sys_nr].driver->unlock();
	}
	fs_plan_suspend(file_sys_nr);
	platform_mutex_release(fs[file_sys_nr].mutex);
	return ret;
}

int32_t fs_fstat (int file_sys_nr, fs_fd fd, fs_stat *s)
{
	if (NULL == m_thread_id)
	{
		return -7;
	}

	int32_t res = -1;
	while(osOK != osMutexAcquire(m_fs_evt_mtx_id, osWaitForever));
	m_blocking.file_sys = file_sys_nr;
	m_blocking.fd = fd;
	m_blocking.args.stat = s;
	m_blocking.action = FS_BLOCKING_ACTION_STAT;
	osThreadFlagsSet(m_thread_id, FS_BLOCKING_ACTION_FLAG);
	uint32_t flags = osEventFlagsWait(m_fs_evt_id, 0x1U, osFlagsWaitAny, osWaitForever);
	if (flags == 1)
	{
		res = m_blocking.result;
	}
	else
	{
		err1("fs_seek %x", flags);
	}
	osMutexRelease(m_fs_evt_mtx_id);
	return res;
}

static int32_t fs_direct_fstat (int file_sys_nr, fs_fd fd, fs_stat *s)
{
	int32_t ret;

	fs_abort_suspend(file_sys_nr);
	platform_mutex_acquire(fs[file_sys_nr].mutex);
	while(!fs[file_sys_nr].ready);
	if(((fd >> 16) & 0xFF) != fs[file_sys_nr].mount_count)
	{
		ret = -1;
	}
	else
	{
		fs[file_sys_nr].driver->lock();
		ret = lfs_file_size(&fs[file_sys_nr].fs, &m_files[fd & 0xFFFF].file);
		fs[file_sys_nr].driver->unlock();
		s->size = ret;
	}
	fs_plan_suspend(file_sys_nr);
	platform_mutex_release(fs[file_sys_nr].mutex);
	return ret;
}

void fs_flush (int file_sys_nr, fs_fd fd)
{
	if (NULL == m_thread_id)
	{
		return;
	}

	while(osOK != osMutexAcquire(m_fs_evt_mtx_id, osWaitForever));
	m_blocking.file_sys = file_sys_nr;
	m_blocking.fd = fd;
	m_blocking.action = FS_BLOCKING_ACTION_FLUSH;
	osThreadFlagsSet(m_thread_id, FS_BLOCKING_ACTION_FLAG);
	uint32_t flags = osEventFlagsWait(m_fs_evt_id, 0x1U, osFlagsWaitAny, osWaitForever);
	if (flags == 1)
	{
		// Good
	}
	else
	{
		err1("fs_flush %x", flags);
	}
	osMutexRelease(m_fs_evt_mtx_id);
}


static void fs_direct_flush (int file_sys_nr, fs_fd fd)
{
	fs_abort_suspend(file_sys_nr);
	platform_mutex_acquire(fs[file_sys_nr].mutex);
	while(!fs[file_sys_nr].ready);
	if(((fd >> 16) & 0xFF) != fs[file_sys_nr].mount_count)
	{
		warn1("stale fd");
	}
	else
	{
		fs[file_sys_nr].driver->lock();
		lfs_file_sync(&fs[file_sys_nr].fs, &m_files[fd & 0xFFFF].file);
		fs[file_sys_nr].driver->unlock();
	}
	fs_plan_suspend(file_sys_nr);
	platform_mutex_release(fs[file_sys_nr].mutex);
}

void fs_close (int file_sys_nr, fs_fd fd)
{
	if (NULL == m_thread_id)
	{
		return;
	}

	while(osOK != osMutexAcquire(m_fs_evt_mtx_id, osWaitForever));
	m_blocking.file_sys = file_sys_nr;
	m_blocking.fd = fd;
	m_blocking.action = FS_BLOCKING_ACTION_CLOSE;
	osThreadFlagsSet(m_thread_id, FS_BLOCKING_ACTION_FLAG);
	uint32_t flags = osEventFlagsWait(m_fs_evt_id, 0x1U, osFlagsWaitAny, osWaitForever);
	if (flags == 1)
	{
		// Good
	}
	else
	{
		err1("fs_close %x", flags);
	}
	osMutexRelease(m_fs_evt_mtx_id);
}

static void fs_direct_close (int file_sys_nr, fs_fd fd)
{
	fs_abort_suspend(file_sys_nr);
	platform_mutex_acquire(fs[file_sys_nr].mutex);
	while(!fs[file_sys_nr].ready);
	if(((fd >> 16) & 0xFF) != fs[file_sys_nr].mount_count)
	{
		warn1("stale fd");
	}
	else
	{
		fs[file_sys_nr].driver->lock();
		debug1("close f:%d", fd & 0xFFFF);
        lfs_file_close(&fs[file_sys_nr].fs, &m_files[fd & 0xFFFF].file);
		m_files[fd & 0xFFFF].used = false;
		fs[file_sys_nr].driver->unlock();
	}
	fs_plan_suspend(file_sys_nr);
	platform_mutex_release(fs[file_sys_nr].mutex);
}

void fs_unlink (int file_sys_nr, char * path)
{
	if (NULL == m_thread_id)
	{
		return;
	}

	while(osOK != osMutexAcquire(m_fs_evt_mtx_id, osWaitForever));
	m_blocking.file_sys = file_sys_nr;
	m_blocking.args.unlink.path = path;
	m_blocking.action = FS_BLOCKING_ACTION_UNLINK;
	osThreadFlagsSet(m_thread_id, FS_BLOCKING_ACTION_FLAG);
	uint32_t flags = osEventFlagsWait(m_fs_evt_id, 0x1U, osFlagsWaitAny, osWaitForever);
	if (flags == 1)
	{
		// Good
	}
	else
	{
		err1("fs_unlink %x", flags);
	}
	osMutexRelease(m_fs_evt_mtx_id);
}

static void fs_direct_unlink (int file_sys_nr, char * path)
{
	fs_abort_suspend(file_sys_nr);
	platform_mutex_acquire(fs[file_sys_nr].mutex);
	while(!fs[file_sys_nr].ready);
	fs[file_sys_nr].driver->lock();
	debug1("unlink: %s", path);
	lfs_remove(&fs[file_sys_nr].fs, path);
	fs[file_sys_nr].driver->unlock();
	fs_plan_suspend(file_sys_nr);
	platform_mutex_release(fs[file_sys_nr].mutex);
}

static void fs_mount ()
{
	for (int f = 0; f < FS_MAX_COUNT; f++)
	{
		if (!fs[f].driver) continue;

		fs_abort_suspend(f);
		platform_mutex_acquire(fs[f].mutex);

		debug1("mounting fs #%d", f);
		fs[f].driver->lock();


		int err = lfs_mount(&fs[f].fs, &fs[f].cfg);
		if (err)
		{
			debug1("formatting #%d (err=%d)", f, err);
			err = lfs_format(&fs[f].fs, &fs[f].cfg);
            logger(0 == err ? LOG_DEBUG1: LOG_ERR1, "fmt %d", (int)err);
            err = lfs_mount(&fs[f].fs, &fs[f].cfg);
			logger(0 == err ? LOG_DEBUG1: LOG_ERR1, "mnt %d", (int)err);
		}

		if (0 == err)
		{
			uint32_t total = fs[f].cfg.block_count;
            lfs_ssize_t used = lfs_fs_size(&fs[f].fs);
			debug1("fs #%d ready, total: %u, used: %u", f, (unsigned int)total, (unsigned int)used);
			fs[f].ready = 1;
		}

		fs[f].driver->unlock();
		fs[f].mount_count++;

		fs_plan_suspend(f);
		platform_mutex_release(fs[f].mutex);
	}
}

#ifdef FS_MANAGE_FLASH_SLEEP

static void fs_suspend_timer_cb (void * arg)
{
	int f = (intptr_t)arg;
	osThreadFlagsSet(m_thread_id, 1 << f);
}
#endif//FS_MANAGE_FLASH_SLEEP

static void run_blocking_action (void)
{
	switch(m_blocking.action)
	{
		case FS_BLOCKING_ACTION_OPEN:
			m_blocking.fd = fs_direct_open(m_blocking.file_sys, m_blocking.args.open.path, m_blocking.args.open.flags);
		break;
		case FS_BLOCKING_ACTION_READ:
			m_blocking.result = fs_direct_read(m_blocking.file_sys, m_blocking.fd, m_blocking.args.read.buffer, m_blocking.args.read.length);
		break;
		case FS_BLOCKING_ACTION_WRITE:
			m_blocking.result = fs_direct_write(m_blocking.file_sys, m_blocking.fd, m_blocking.args.write.buffer, m_blocking.args.write.length);
		break;
		case FS_BLOCKING_ACTION_UNLINK:
			fs_direct_unlink(m_blocking.file_sys, m_blocking.args.unlink.path);
		break;
		case FS_BLOCKING_ACTION_CLOSE:
			fs_direct_close(m_blocking.file_sys, m_blocking.fd);
		break;
		case FS_BLOCKING_ACTION_STAT:
			m_blocking.result = fs_direct_fstat(m_blocking.file_sys, m_blocking.fd, m_blocking.args.stat);
		break;
		case FS_BLOCKING_ACTION_SEEK:
			m_blocking.result = fs_direct_lseek(m_blocking.file_sys, m_blocking.fd, m_blocking.args.seek.offset, m_blocking.args.seek.whence);
		break;
		case FS_BLOCKING_ACTION_FLUSH:
			fs_direct_flush(m_blocking.file_sys, m_blocking.fd);
		break;
		case FS_BLOCKING_ACTION_INFO:
			m_blocking.result = fs_direct_info(m_blocking.file_sys, m_blocking.args.info.p_total, m_blocking.args.info.p_used);
		break;
		default:
			err1("ACT %d", m_blocking.action);
	}
	osEventFlagsSet(m_fs_evt_id, 0x1U);
}

static void fs_thread (void * p)
{
	uint32_t flags;

	debug1("Thread starts");
	flags = osThreadFlagsClear(FS_THREAD_FLAGS_ALL);
	debug1("ThrFlgs:0x%X", flags);

	// For now we just mount it in the current thread
	fs_mount();

	m_fs_ready = true;

	for (;;)
	{
		flags = osThreadFlagsWait(FS_THREAD_FLAGS_ALL, osFlagsWaitAny, osWaitForever);
		//debug1("ThrFlgs:0x%X", flags);

		if (flags & ~FS_THREAD_FLAGS_ALL)
		{
			err1("ThrdError:%X", flags);
			continue;
		}

		if (flags & FS_BLOCKING_ACTION_FLAG)
		{
			run_blocking_action();
		}

		if (flags & FS_QUEUED_WRITE_FLAG)
		{
			debug1("WrThd");
			// wait parameter is set to 0 to avoid thread blocking because there should be data in the queue
			fs_rw_params_t params;
			osStatus_t res = osMessageQueueGet(m_wr_queue_id, (void*)&params, NULL, 0);
			switch (res)
			{
				case osOK:
				{
					// open file for writing
					debug2("p:%d f:%s pv:%p l:%d fnc:%p",
						   params.file_sys_nr, \
						   params.p_file_name, \
						   params.p_value, \
						   params.len, \
						   params.f_callback);

					fs_fd file_desc = fs_direct_open(params.file_sys_nr, (void*)params.p_file_name, FS_WRONLY);
					if (file_desc < 0)
					{
						// file does not exists or some other error
						debug1("File not exists:%s", params.p_file_name);
						// try to create new file
						file_desc = fs_direct_open(params.file_sys_nr, (void*)params.p_file_name, FS_TRUNC | FS_CREAT | FS_WRONLY);
						if (file_desc < 0)
						{
							err1("Cannot create file:%s", params.p_file_name);
							params.f_callback(0, params.p_user);
						}
					}
					if (file_desc >= 0)
					{
						int32_t fs_res = fs_direct_write(params.file_sys_nr, file_desc, params.p_value, params.len);
						fs_direct_close(params.file_sys_nr, file_desc);
						params.f_callback(fs_res, params.p_user);
					}
				}
				break;

				case osErrorResource:
					err1("Queue empty!");
				break;

				case osErrorParameter:
					err1("Parameter!");
				break;

				default:
					err1("Unknown error!");
			}
			if (osMessageQueueGetCount(m_wr_queue_id) > 0)
			{
				debug1("Wr pending");
				osThreadFlagsSet(m_thread_id, FS_QUEUED_WRITE_FLAG);
			}
		}

		if (flags & FS_QUEUED_READ_FLAG)
		{
			debug1("RdThd");
			// open file for reading
			// wait parameter is set to 0 to avoid thread blocking because there should be data in the queue
			fs_rw_params_t params;
			osStatus_t res = osMessageQueueGet(m_rd_queue_id, (void*)&params, NULL, 0);
			switch (res)
			{
				case osOK:
				{
					fs_fd file_desc = fs_direct_open(params.file_sys_nr, (void*)params.p_file_name, FS_RDONLY);
					debug1("fd:%d", file_desc);
					if (file_desc < 0)
					{
						// file does not exists or some other error
						debug1("File not exists:%s", params.p_file_name);
						params.f_callback(0, params.p_user);
					}
					else
					{
						int32_t fs_res = fs_direct_read(params.file_sys_nr, file_desc, params.p_value, params.len);
						fs_direct_close(params.file_sys_nr, file_desc);
						params.f_callback(fs_res, params.p_user);
					}
				}
				break;

				case osErrorResource:
					err1("Queue empty!");
				break;

				case osErrorParameter:
					err1("Parameter!");
				break;

				default:
					err1("Unknown error!");
			}
			if (osMessageQueueGetCount(m_rd_queue_id) > 0)
			{
				debug1("Rd pending");
				osThreadFlagsSet(m_thread_id, FS_QUEUED_READ_FLAG);
			}
		}

		if (flags & FS_CHECK_FLAG)
		{
			debug1("ChkThd");
			// Check a filesystem for errrors ... could take a long time
			// wait parameter is set to 0 to avoid thread blocking because there should be data in the queue
			fs_rw_params_t params;
			osStatus_t res = osMessageQueueGet(m_chk_queue_id, (void*)&params, NULL, 0);
			switch (res)
			{
				case osOK:
				{
					int32_t fchk = fs_check(params.file_sys_nr);
					debug1("fchk:%d", fchk);
					params.f_callback(fchk, params.p_user);
				}
				break;

				case osErrorResource:
					err1("Queue empty!");
				break;

				case osErrorParameter:
					err1("Parameter!");
				break;

				default:
					err1("Unknown error!");
			}
			if (osMessageQueueGetCount(m_rd_queue_id) > 0)
			{
				debug1("Rd pending");
				osThreadFlagsSet(m_thread_id, FS_QUEUED_READ_FLAG);
			}
		}

		if (flags & FS_SUSPENDFLAGS)
		{
			for (int f=0; f<FS_MAX_COUNT; f++)
			{
				if (flags & (1 << f))
				{
					debug1("Suspend:0x%X", (1 << f));
					platform_mutex_acquire(fs[f].mutex);
					fs[f].driver->lock();
					if (NULL != fs[f].driver->suspend)
					{
						fs[f].driver->suspend();
					}
					fs[f].driver->unlock();
					platform_mutex_release(fs[f].mutex);
				}
			}
		}
	}
}

static void fs_plan_suspend (int file_sys_nr)
{
	#ifdef FS_MANAGE_FLASH_SLEEP
		osTimerStart(m_sleep_timers[file_sys_nr], 100);
	#endif//FS_MANAGE_FLASH_SLEEP
}

static void fs_abort_suspend (int file_sys_nr)
{
	#ifdef FS_MANAGE_FLASH_SLEEP
		osTimerStop(m_sleep_timers[file_sys_nr]);
	#endif//FS_MANAGE_FLASH_SLEEP
}

/*****************************************************************************
 * Put one data read/write request to the read/write queue and sets
 * FS_QUEUED_READ_FLAG/FS_QUEUED_WRITE_FLAG on success
 * @params command_type - Command FS_CMD_RD or FS_CMD_WRITE
 * @params file_sys_nr - File system number 0..2
 * @params p_file_name - Pointer to the file name
 * @params p_value - Pointer to the data record
 * @params len - Data record length in bytes
 * @params wait - When wait = 0 function returns immediately, even when putting fails,
 *                otherwise waits until put succeeds (and blocks calling thread)
 *
 * @return Returns number of bytes to write on success, 0 otherwise
 ****************************************************************************/
static int32_t fs_rw_record (uint8_t command_type,
                             int file_sys_nr,
                             const char * p_file_name,
                             const void * p_value,
                             int32_t len,
                             uint32_t wait,
                             fs_rw_done_f f_callback,
                             void * p_user)
{
	fs_rw_params_t params;
	osMessageQueueId_t q_id;
	uint32_t flags;

	params.file_sys_nr = file_sys_nr;
	params.p_file_name = (void*)p_file_name;
	params.p_value = (void*)p_value;
	params.len = len;
	params.f_callback = f_callback;
	params.p_user = p_user;

	debug2("p:%d f:%s pv:%p l:%d fnc:%p",
		   params.file_sys_nr, \
		   params.p_file_name, \
		   params.p_value, \
		   params.len, \
		   params.f_callback);

	switch (command_type)
	{
		case FS_WRITE_DATA:
			debug1("FSQWr:%s l:%d", p_file_name, len);
			q_id = m_wr_queue_id;
			flags = FS_QUEUED_WRITE_FLAG;
		break;

		case FS_READ_DATA:
			debug1("FSQRd:%s l:%d", p_file_name, len);
			q_id = m_rd_queue_id;
			flags = FS_QUEUED_READ_FLAG;
		break;

		default:
			err1("!Cmd");
			return 0;
	}

	if (wait != 0)
	{
		wait = osWaitForever;
	}
	osStatus_t res = osMessageQueuePut(q_id, &params, 0U, wait);
	switch (res)
	{
		case osOK:
			res = osThreadFlagsSet(m_thread_id, flags);
			return len;
		break;

		case osErrorResource:
			warn1("QFull!");
		break;

		case osErrorTimeout:
			warn1("Timeout!");
		break;

		case osErrorParameter:
			err1("Parameter!");
		break;

		default:
			err1("Error:%d", res);

	}
	return 0;
}

/*****************************************************************************
 * Put one data read request to the read queue
 * @params file_sys_nr - file_sys_nr number 0..2
 * @params p_file_name - Pointer to the file name
 * @params p_value - Pointer to the data record
 * @params len - Data record length in bytes
 * @params wait - When wait = 0 function returns immediately, even when putting fails,
 *                otherwise waits until put succeeds (and blocks calling thread)
 *
 * @return Returns number of bytes to write on success, 0 otherwise
 ****************************************************************************/
int32_t fs_read_record (int file_sys_nr,
                        const char * p_file_name,
                        void * p_value,
                        int32_t len,
                        uint32_t wait,
                        fs_rw_done_f f_callback,
                        void * p_user)
{
	if (NULL == m_thread_id)
	{
		return -7;
	}

	if ((file_sys_nr > 2) || (file_sys_nr < 0))
	{
		err1("File system number:%d", file_sys_nr);
		return 0;
	}
	if (NULL == p_value)
	{
		err1("p_value = NULL");
		return 0;
	}
	if (NULL == f_callback)
	{
		err1("Callback = NULL");
		return 0;
	}
	return fs_rw_record(FS_READ_DATA, file_sys_nr, p_file_name, p_value, len, wait, f_callback, p_user);
}

/*****************************************************************************
 * Put one data write request to the write queue
 * @params file_sys_nr - file_sys_nr number 0..2
 * @params p_file_name - Pointer to the file name
 * @params p_value - Pointer to the data record
 * @params len - Data record length in bytes
 * @params wait - When wait = 0 function returns immediately, even when putting fails,
 *                otherwise waits until put succeeds (and blocks calling thread)
 *
 * @return Returns number of bytes to write on success, 0 otherwise
 ****************************************************************************/
int32_t fs_write_record (int file_sys_nr,
                         const char * p_file_name,
                         const void * p_value,
                         int32_t len,
                         uint32_t wait,
                         fs_rw_done_f f_callback,
                         void * p_user)
{
	if (NULL == m_thread_id)
	{
		return -7;
	}

	if ((file_sys_nr > 2) || (file_sys_nr < 0))
	{
		err1("File system number:%d", file_sys_nr);
		return 0;
	}
	if (NULL == f_callback)
	{
		err1("Callback = NULL");
		return 0;
	}
	return fs_rw_record(FS_WRITE_DATA, file_sys_nr, p_file_name, p_value, len, wait, f_callback, p_user);
}

int32_t fs_queue_check (int file_sys_nr,
                        uint32_t wait,
                        fs_rw_done_f f_callback,
                        void * p_user)
{
	if (NULL == m_thread_id)
	{
		return -7;
	}

	if ((file_sys_nr > 2) || (file_sys_nr < 0))
	{
		err1("File system number:%d", file_sys_nr);
		return 0;
	}
	if (NULL == f_callback)
	{
		err1("Callback = NULL");
		return 0;
	}

	fs_rw_params_t params;
	params.file_sys_nr = file_sys_nr;
	params.p_file_name = NULL;
	params.p_value = NULL;
	params.len = 0;
	params.f_callback = f_callback;
	params.p_user = p_user;
	osStatus_t res = osMessageQueuePut(m_chk_queue_id, &params, 0U, wait == 0 ? 0 : osWaitForever);
	switch (res)
	{
		case osOK:
			osThreadFlagsSet(m_thread_id, FS_CHECK_FLAG);
			return 1;
		break;

		case osErrorResource:
			warn1("QFull!");
		break;

		case osErrorTimeout:
			warn1("Timeout!");
		break;

		case osErrorParameter:
			err1("Parameter!");
		break;

		default:
			err1("Error:%d", res);
	}
	return 0;
}
