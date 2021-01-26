/**
 * Filesystem wrapper for SPIFFS.
 *
 * Multiple filesystem functionality:
 * The fs wrapper can be used to access multiple filesystems, like one in an
 * external dataflash and another small one for backup settings in a part of
 * the internal mcu flash. Set the number of filesystems with FS_MAX_COUNT.
 *
 * Suspend functionality:
 * If FS_MANAGE_FLASH_SLEEP is defined, after accessing a filesystem, the fs
 * will start a timer and after it expires, will suspend the underlying
 * flash device. The requirement here is that resume is automatic. The flow is
 * to first lock the device, then suspend it, then unlock. If the same device
 * is used for multiple filesystems, then it is possible that suspend is called
 * multiple times or suspend is called even though a different filesystem is
 * being actively accessed, causing additional delay from the unneccessary
 * suspend-resume cycle. Also, if the flash device is accessed externally,
 * then it must also be suspended externally, otherwise it will take until the
 * next fs access for it to be suspended again.
 *
 * Copyright Thinnect Inc. 2020
 * @license MIT
 */

#include "fs.h"
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "platform_mutex.h"
#include "spi_flash.h"
#include "spiffs.h"
#include "cmsis_os2.h"

#include "loglevels.h"
#define __MODUUL__ "fs"
#define __LOG_LEVEL__ (LOG_LEVEL_fs & BASE_LOG_LEVEL)
#include "log.h"
#include "sys_panic.h"

#ifndef FS_MAX_COUNT
#define FS_MAX_COUNT 1
#endif//FS_MAX_COUNT

#ifndef FS_MAX_DESCRIPTORS
#define FS_MAX_DESCRIPTORS 6
#endif//FS_MAX_DESCRIPTORS

#define FS_SPIFFS_LOG_PAGE_SZ  (128UL)
#define FS_SPIFFS_LOG_BLOCK_SZ (32UL * 1024UL)

#define MAX_Q_WR_COUNT 10
#define MAX_Q_RD_COUNT 10

#define FS_WRITE_DATA 1
#define FS_READ_DATA 2

struct fs_struct
{
	fs_driver_t *driver;
	volatile int ready;
	int partition;
	uint8_t mount_count;
	platform_mutex_t mutex;
	spiffs_config cfg;
	spiffs fs;
	uint8_t work_buf[FS_SPIFFS_LOG_PAGE_SZ * 2];
	uint8_t fds[32 * FS_MAX_DESCRIPTORS];
};

static struct fs_struct fs[FS_MAX_COUNT];

#ifdef FS_MANAGE_FLASH_SLEEP
static osTimerId_t  m_sleep_timers[FS_MAX_COUNT];
static void fs_suspend_timer_cb(void * arg);
#endif//FS_MANAGE_FLASH_SLEEP

#define FS_THREAD_FLAGS_ALL 0x7FFFFFFFU
#define FS_SUSPENDFLAGS     0x00000007U

// define read/write flags after filesystem suspend timer flags
#define FS_WRITE_FLAG       (0x01 << FS_MAX_COUNT)
#define FS_READ_FLAG        (0x01 << (FS_MAX_COUNT + 1))

static osThreadId_t m_thread_id;
static osMessageQueueId_t m_wr_queue_id;
static osMessageQueueId_t m_rd_queue_id;

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

static int32_t fs_read0(uint32_t addr, uint32_t size, uint8_t * dst);
static int32_t fs_write0(uint32_t addr, uint32_t size, uint8_t * src);
static int32_t fs_erase0(uint32_t addr, uint32_t size);
#if FS_MAX_COUNT > 1
static int32_t fs_read1(uint32_t addr, uint32_t size, uint8_t * dst);
static int32_t fs_write1(uint32_t addr, uint32_t size, uint8_t * src);
static int32_t fs_erase1(uint32_t addr, uint32_t size);
#endif// FS_MAX_COUNT > 1
#if FS_MAX_COUNT > 2
static int32_t fs_read2(uint32_t addr, uint32_t size, uint8_t * dst);
static int32_t fs_write2(uint32_t addr, uint32_t size, uint8_t * src);
static int32_t fs_erase2(uint32_t addr, uint32_t size);
#endif// FS_MAX_COUNT > 2

void fs_init (int file_sys_nr, int partition, fs_driver_t *driver)
{
	fs[file_sys_nr].ready = 0;
	fs[file_sys_nr].partition = partition;
	fs[file_sys_nr].driver = driver;
	fs[file_sys_nr].mount_count = 0;
	fs[file_sys_nr].mutex = platform_mutex_new("fs");

	fs[file_sys_nr].cfg.phys_size = driver->size(partition);
	fs[file_sys_nr].cfg.phys_addr = 0;
	fs[file_sys_nr].cfg.phys_erase_block = driver->erase_size(partition);
	fs[file_sys_nr].cfg.log_block_size = FS_SPIFFS_LOG_BLOCK_SZ;
	fs[file_sys_nr].cfg.log_page_size = FS_SPIFFS_LOG_PAGE_SZ;
	if(0 == file_sys_nr)
	{
		fs[file_sys_nr].cfg.hal_read_f = fs_read0;
		fs[file_sys_nr].cfg.hal_write_f = fs_write0;
		fs[file_sys_nr].cfg.hal_erase_f = fs_erase0;
#if FS_MAX_COUNT > 1
	}
	else if (1 == file_sys_nr)
	{
		fs[file_sys_nr].cfg.hal_read_f = fs_read1;
		fs[file_sys_nr].cfg.hal_write_f = fs_write1;
		fs[file_sys_nr].cfg.hal_erase_f = fs_erase1;
#endif
#if FS_MAX_COUNT > 2
	}
	else if (2 == file_sys_nr)
	{
		fs[file_sys_nr].cfg.hal_read_f = fs_read2;
		fs[file_sys_nr].cfg.hal_write_f = fs_write2;
		fs[file_sys_nr].cfg.hal_erase_f = fs_erase2;
#endif
	}

	#ifndef FS_NO_CONFIG_VALIDATION
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
	const osThreadAttr_t thread_attr = { .name = "fs", .stack_size = 2048 };
	m_thread_id = osThreadNew(fs_thread, NULL, &thread_attr);
	if (NULL == m_thread_id)
	{
		err1("!Thread");
		while(1);
	}

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
	// For now we just mount it in the current thread
	fs_mount();
}

fs_fd fs_open (int file_sys_nr, char *path, uint32_t flags)
{
	spiffs_file sfd;
	fs_fd fd;

	fs_abort_suspend(file_sys_nr);
	platform_mutex_acquire(fs[file_sys_nr].mutex);
	while(!fs[file_sys_nr].ready);
	fs[file_sys_nr].driver->lock();
	debug1("open %d: %s", file_sys_nr, path);
	sfd = SPIFFS_open(&fs[file_sys_nr].fs, path, flags, 0);
	debug1("sfd:%d", sfd);
	fs[file_sys_nr].driver->unlock();
	fd = (fs[file_sys_nr].mount_count << 16) | sfd;
	fs_plan_suspend(file_sys_nr);
	platform_mutex_release(fs[file_sys_nr].mutex);
	if(sfd < 0)
	{
		return sfd;
	}
	return fd;
}

int32_t fs_read (int file_sys_nr, fs_fd fd, void *buf, int32_t len)
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
		ret = SPIFFS_read(&fs[file_sys_nr].fs, (fd & 0xFFFF), buf, len);
		fs[file_sys_nr].driver->unlock();
	}
	fs_plan_suspend(file_sys_nr);
	platform_mutex_release(fs[file_sys_nr].mutex);
	return ret;
}

int32_t fs_write (int file_sys_nr, fs_fd fd, const void *buf, int32_t len)
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
		ret = SPIFFS_write(&fs[file_sys_nr].fs, (fd & 0xFFFF), (void *)buf, len);
		fs[file_sys_nr].driver->unlock();
	}
	fs_plan_suspend(file_sys_nr);
	platform_mutex_release(fs[file_sys_nr].mutex);
	return ret;
}

int32_t fs_lseek (int file_sys_nr, fs_fd fd, int32_t offs, int whence)
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
		ret = SPIFFS_lseek(&fs[file_sys_nr].fs, (fd & 0xFFFF), offs, whence);
		fs[file_sys_nr].driver->unlock();
	}
	fs_plan_suspend(file_sys_nr);
	platform_mutex_release(fs[file_sys_nr].mutex);
	return ret;
}

int32_t fs_fstat (int file_sys_nr, fs_fd fd, fs_stat *s)
{
	int32_t ret;
	spiffs_stat stat;

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
		ret = SPIFFS_fstat(&fs[file_sys_nr].fs, (fd & 0xFFFF), &stat);
		fs[file_sys_nr].driver->unlock();
		s->size = stat.size;
	}
	fs_plan_suspend(file_sys_nr);
	platform_mutex_release(fs[file_sys_nr].mutex);
	return ret;
}

void fs_flush (int file_sys_nr, fs_fd fd)
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
		SPIFFS_fflush(&fs[file_sys_nr].fs, (fd & 0xFFFF));
		fs[file_sys_nr].driver->unlock();
	}
	fs_plan_suspend(file_sys_nr);
	platform_mutex_release(fs[file_sys_nr].mutex);
}

void fs_close (int file_sys_nr, fs_fd fd)
{
	fs_abort_suspend(file_sys_nr);
	platform_mutex_acquire(fs[file_sys_nr].mutex);
	while(!fs[file_sys_nr].ready);
	if(((fd >> 16) & 0xFF) != fs[file_sys_nr].mount_count)
	{
		;
	}
	else
	{
		fs[file_sys_nr].driver->lock();
		SPIFFS_close(&fs[file_sys_nr].fs, (fd & 0xFFFF));
		fs[file_sys_nr].driver->unlock();
	}
	fs_plan_suspend(file_sys_nr);
	platform_mutex_release(fs[file_sys_nr].mutex);
}

void fs_unlink (int file_sys_nr, char *path)
{
	fs_abort_suspend(file_sys_nr);
	platform_mutex_acquire(fs[file_sys_nr].mutex);
	while(!fs[file_sys_nr].ready);
	fs[file_sys_nr].driver->lock();
	debug1("unlink: %s", path);
	SPIFFS_remove(&fs[file_sys_nr].fs, path);
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


		int ret = SPIFFS_mount(&fs[f].fs, &fs[f].cfg, fs[f].work_buf, fs[f].fds, sizeof(fs[f].fds), NULL, 0, NULL);
		if(SPIFFS_OK != ret)
		{
			debug1("formatting #%d", f);
			s32_t r = SPIFFS_format(&fs[f].fs);
			logger(0 == r ? LOG_DEBUG1: LOG_ERR1, "fmt %d", (int)r);
			r = SPIFFS_mount(&fs[f].fs, &fs[f].cfg, fs[f].work_buf, fs[f].fds, sizeof(fs[f].fds), NULL, 0, NULL);
			logger(0 == r ? LOG_DEBUG1: LOG_ERR1, "mnt %d", (int)r);
			ret = r;
		}

		if(SPIFFS_OK == ret)
		{
			uint32_t total, used;
			ret = SPIFFS_info(&fs[f].fs, &total, &used);
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

static void fs_thread (void * p)
{
	osStatus_t res;
	fs_rw_params_t params;
	fs_fd file_desc;
	int32_t fs_res;
	uint32_t flags;

	debug1("Thread starts");
	flags = osThreadFlagsClear(FS_THREAD_FLAGS_ALL);
	debug1("ThrFlgs:0x%X", flags);

	for (;;)
	{
		flags = osThreadFlagsWait(FS_THREAD_FLAGS_ALL, osFlagsWaitAny, osWaitForever);

		debug1("ThrFlgs:0x%X", flags);
		if (flags & ~FS_THREAD_FLAGS_ALL)
		{
			err1("ThrdError:%X", flags);
			continue;
		}
		if (flags & FS_WRITE_FLAG)
		{
			debug1("Wr Thread");
			// wait parameter is set to 0 to avoid thread blocking because there should be data in the queue
			res = osMessageQueueGet(m_wr_queue_id, (void*)&params, NULL, 0);
			switch (res)
			{
				case osOK:
					// open file for writing
					debug2("p:%d f:%s pv:%p l:%d fnc:%p",
						   params.file_sys_nr, \
						   params.p_file_name, \
						   params.p_value, \
						   params.len, \
						   params.f_callback);

					file_desc = fs_open(params.file_sys_nr, (void*)params.p_file_name, FS_WRONLY);
					if (file_desc < 0)
					{
						// file does not exists or some other error
						debug1("File not exists:%s", params.p_file_name);
						// try to create new file
						file_desc = fs_open(params.file_sys_nr, (void*)params.p_file_name, FS_TRUNC | FS_CREAT | FS_WRONLY);
						if (file_desc < 0)
						{
							err1("Cannot create file:%s", params.p_file_name);
							params.f_callback(0, params.p_user);
						}
					}
					if (file_desc >= 0)
					{
						fs_res = fs_write(params.file_sys_nr, file_desc, params.p_value, params.len);
						fs_close(params.file_sys_nr, file_desc);
						params.f_callback(fs_res, params.p_user);
					}
				break;

				case osErrorResource:
					err1("Queue empty!");
					params.f_callback(0, params.p_user);
				break;

				case osErrorParameter:
					err1("Parameter!");
					params.f_callback(0, params.p_user);
				break;

				default:
					err1("Unknown error!");
					params.f_callback(0, params.p_user);
			}
			if (osMessageQueueGetCount(m_wr_queue_id) > 0)
			{
				debug1("Wr pending");
				osThreadFlagsSet(m_thread_id, FS_WRITE_FLAG);
			}
		}

		if (flags & FS_READ_FLAG)
		{
			debug1("Rd Thread");
			// open file for reading
			// wait parameter is set to 0 to avoid thread blocking because there should be data in the queue
			res = osMessageQueueGet(m_rd_queue_id, (void*)&params, NULL, 0);
			switch (res)
			{
				case osOK:
					file_desc = fs_open(params.file_sys_nr, (void*)params.p_file_name, FS_RDONLY);
					debug1("fd:%d", file_desc);
					if (file_desc < 0)
					{
						// file does not exists or some other error
						debug1("File not exists:%s", params.p_file_name);
						params.f_callback(0, params.p_user);
					}
					else
					{
						fs_res = fs_read(params.file_sys_nr, file_desc, params.p_value, params.len);
						fs_close(params.file_sys_nr, file_desc);
						params.f_callback(fs_res, params.p_user);
					}
				break;

				case osErrorResource:
					err1("Queue empty!");
					params.f_callback(0, params.p_user);
				break;

				case osErrorParameter:
					err1("Parameter!");
					params.f_callback(0, params.p_user);
				break;

				default:
					err1("Unknown error!");
					params.f_callback(0, params.p_user);
			}
			if (osMessageQueueGetCount(m_rd_queue_id) > 0)
			{
				debug1("Rd pending");
				osThreadFlagsSet(m_thread_id, FS_READ_FLAG);
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

#if 0 // Error handling is not implemented fully
static int fs_error_increase (int32_t error)
{
	int32_t err; // s32_t
	if(error >= 0)return(0);
	if(error == SPIFFS_ERR_FULL)return(0);
	if(error == SPIFFS_ERR_NOT_FOUND)return(0);
	if((error != SPIFFS_ERR_NOT_FINALIZED)
	 &&(error != SPIFFS_ERR_NOT_INDEX)
	 &&(error != SPIFFS_ERR_IS_INDEX)
	 &&(error != SPIFFS_ERR_IS_FREE)
	 &&(error != SPIFFS_ERR_INDEX_SPAN_MISMATCH)
	 &&(error != SPIFFS_ERR_DATA_SPAN_MISMATCH)
	 &&(error != SPIFFS_ERR_INDEX_REF_FREE)
	 &&(error != SPIFFS_ERR_INDEX_REF_LU)
	 &&(error != SPIFFS_ERR_INDEX_REF_INVALID)
	 &&(error != SPIFFS_ERR_INDEX_FREE)
	 &&(error != SPIFFS_ERR_INDEX_LU)
	 &&(error != SPIFFS_ERR_INDEX_INVALID))return(0);
	if (fs_error_count < 254)fs_error_count++;

	#ifdef SPIFFS_CHECK_ENABLED
		warn1("FS: CHECKING FS");
		fs_checking = 1;
		err = SPIFFS_check(&fs_fs);
		fs_checking = 0;
		info1("FS: CHECKING FS DONE r=%"PRIi32, err);
		//
		// TODO if the check failed, then a format should be performed, but we
		//      do not know the behavior and return codes of the check yet
		//
	#else // checking is disabled, just try to format when encountering errors
		err1("FS: FS BROKEN");
		// TODO maybe formatting should not take place on the first error?
		SPIFFS_unmount(&fs_fs);
		err = SPIFFS_format(&fs_fs);
		logger(err==0?LOG_INFO1:LOG_ERR1, "FS: FS FORMAT r=%"PRIi32, err);
		if (fs_task.error == 0)
		{
			err = SPIFFS_mount(&fs_fs, &fs_cfg, fs_work_buf, fs_fds, sizeof(fs_fds), NULL, 0, 0);
			logger(err==0?LOG_DEBUG1:LOG_ERR1, "FS: FS MOUNT r=%"PRIi32, err);
			return err;
		}
	#endif//SPIFFS_CHECK_ENABLED
	return(1);
}
#endif

static int32_t fs_read0 (uint32_t addr, uint32_t size, uint8_t * dst)
{
	if (fs[0].driver->read(fs[0].partition, addr, size, dst) < 0)
	{
		return SPIFFS_ERR_INTERNAL;
	}
	return SPIFFS_OK;
}

static int32_t fs_write0 (uint32_t addr, uint32_t size, uint8_t * src)
{
	if (fs[0].driver->write(fs[0].partition, addr, size, src) < 0)
	{
		return SPIFFS_ERR_INTERNAL;
	}
	return SPIFFS_OK;
}

static int32_t fs_erase0 (uint32_t addr, uint32_t size)
{
	if (fs[0].driver->erase(fs[0].partition, addr, size) < 0)
	{
		return SPIFFS_ERR_INTERNAL;
	}
	return SPIFFS_OK;
}

#if FS_MAX_COUNT > 1
static int32_t fs_read1 (uint32_t addr, uint32_t size, uint8_t * dst)
{
	if (fs[1].driver->read(fs[1].partition, addr, size, dst) < 0)
	{
		return SPIFFS_ERR_INTERNAL;
	}
	return SPIFFS_OK;
}

static int32_t fs_write1 (uint32_t addr, uint32_t size, uint8_t * src)
{
	if (fs[1].driver->write(fs[1].partition, addr, size, src) < 0)
	{
		return SPIFFS_ERR_INTERNAL;
	}
	return SPIFFS_OK;
}

static int32_t fs_erase1 (uint32_t addr, uint32_t size)
{
	if (fs[1].driver->erase(fs[1].partition, addr, size) < 0)
	{
		return SPIFFS_ERR_INTERNAL;
	}
	return SPIFFS_OK;
}
#endif

#if FS_MAX_COUNT > 2
static int32_t fs_read2 (uint32_t addr, uint32_t size, uint8_t * dst)
{
	if (fs[2].driver->read(fs[2].partition, addr, size, dst) < 0)
	{
		return SPIFFS_ERR_INTERNAL;
	}
	return SPIFFS_OK;
}

static int32_t fs_write2 (uint32_t addr, uint32_t size, uint8_t * src)
{
	if (fs[2].driver->write(fs[2].partition, addr, size, src) < 0)
	{
		return SPIFFS_ERR_INTERNAL;
	}
	return SPIFFS_OK;
}

static int32_t fs_erase2 (uint32_t addr, uint32_t size)
{
	if (fs[2].driver->erase(fs[2].partition, addr, size) < 0)
	{
		return SPIFFS_ERR_INTERNAL;
	}
	return SPIFFS_OK;
}
#endif

#if FS_MAX_COUNT > 3
	#error FS_MAX_COUNT > 3
#endif

/*****************************************************************************
 * Put one data read/write request to the read/write queue and sets
 * FS_READ_FLAG/FS_WRITE_FLAG on success
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
			flags = FS_WRITE_FLAG;
		break;

		case FS_READ_DATA:
			debug1("FSQRd:%s l:%d", p_file_name, len);
			q_id = m_rd_queue_id;
			flags = FS_READ_FLAG;
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
