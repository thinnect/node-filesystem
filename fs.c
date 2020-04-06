#include "fs.h"
#include <stdio.h>
#include <stdint.h>
#include "cmsis_os2.h"
#include "platform_mutex.h"
#include "spi_flash.h"
#include "spiffs.h"
#include "loglevels.h"

#define __MODUUL__ "fs"
#define __LOG_LEVEL__ (LOG_LEVEL_fs & BASE_LOG_LEVEL)
#include "log.h"

#define FS_MAX                 2

#define FS_SPIFFS_LOG_PAGE_SZ  (128UL)
#define FS_SPIFFS_LOG_BLOCK_SZ (32UL * 1024UL)

struct fs_struct{
	fs_driver_t *driver;
	volatile int ready;
	int partition;
	uint8_t mount_count;
	platform_mutex_t mutex;
	spiffs_config cfg;
	spiffs fs;
	uint8_t work_buf[FS_SPIFFS_LOG_PAGE_SZ * 2];
	uint8_t fds[32 * 4];
}fs[FS_MAX];

osMessageQueueId_t fs_queue;

static void fs_thread(void *p);

static int32_t fs_read0(uint32_t addr, uint32_t size, uint8_t * dst);
static int32_t fs_write0(uint32_t addr, uint32_t size, uint8_t * src);
static int32_t fs_erase0(uint32_t addr, uint32_t size);
static int32_t fs_read1(uint32_t addr, uint32_t size, uint8_t * dst);
static int32_t fs_write1(uint32_t addr, uint32_t size, uint8_t * src);
static int32_t fs_erase1(uint32_t addr, uint32_t size);
static int32_t fs_read2(uint32_t addr, uint32_t size, uint8_t * dst);
static int32_t fs_write2(uint32_t addr, uint32_t size, uint8_t * src);
static int32_t fs_erase2(uint32_t addr, uint32_t size);

void fs_init(int f, int partition, fs_driver_t *driver)
{
	fs[f].ready = 0;
	fs[f].partition = partition;
	fs[f].driver = driver;
	fs[f].mount_count = 0;
	fs[f].mutex = platform_mutex_new("fs");

	fs[f].cfg.phys_size = driver->size(partition);
	fs[f].cfg.phys_addr = 0;
	fs[f].cfg.phys_erase_block = driver->erase_size(partition);
	fs[f].cfg.log_block_size = FS_SPIFFS_LOG_BLOCK_SZ;
	fs[f].cfg.log_page_size = FS_SPIFFS_LOG_PAGE_SZ;
	if(f == 0){
		fs[f].cfg.hal_read_f = fs_read0;
		fs[f].cfg.hal_write_f = fs_write0;
		fs[f].cfg.hal_erase_f = fs_erase0;
#if FS_MAX > 0
	}else if(f == 1){
		fs[f].cfg.hal_read_f = fs_read1;
		fs[f].cfg.hal_write_f = fs_write1;
		fs[f].cfg.hal_erase_f = fs_erase1;
#endif
#if FS_MAX > 1
	}else if(f == 2){
		fs[f].cfg.hal_read_f = fs_read2;
		fs[f].cfg.hal_write_f = fs_write2;
		fs[f].cfg.hal_erase_f = fs_erase2;
#endif
	}
}

void fs_start()
{
	fs_queue = osMessageQueueNew(16, sizeof(void *), NULL);
	osThreadNew(fs_thread, NULL, NULL);
	// osMessageQueuePut(fs_queue, &fs_queue, 0, 0);
}

fs_fd fs_open(int f, char *path, uint32_t flags)
{
	spiffs_file sfd;
	fs_fd fd;
	platform_mutex_acquire(fs[f].mutex);
	while(!fs[f].ready);
	fs[f].driver->lock();
	debug1("open %d: %s", f, path);
	sfd = SPIFFS_open(&fs[f].fs, path, flags, 0);
	fs[f].driver->unlock();
	fd = (fs[f].mount_count << 16) | sfd;
	platform_mutex_release(fs[f].mutex);
	if(sfd < 0)
	{
		return sfd;
	}
	return fd;
}

int32_t fs_read(int f, fs_fd fd, void *buf, int32_t len)
{
	int32_t ret;
	platform_mutex_acquire(fs[f].mutex);
	while(!fs[f].ready);
	if(((fd >> 16) & 0xFF) != fs[f].mount_count)
	{
		ret = -1;
	}
	else
	{
		fs[f].driver->lock();
		ret = SPIFFS_read(&fs[f].fs, (fd & 0xFFFF), buf, len);
		fs[f].driver->unlock();
	}
	platform_mutex_release(fs[f].mutex);
	return ret;
}

int32_t fs_write(int f, fs_fd fd, const void *buf, int32_t len)
{
	int32_t ret;
	platform_mutex_acquire(fs[f].mutex);
	while(!fs[f].ready);
	if(((fd >> 16) & 0xFF) != fs[f].mount_count)
	{
		ret = -1;
	}
	else
	{
		fs[f].driver->lock();
		ret = SPIFFS_write(&fs[f].fs, (fd & 0xFFFF), (void *)buf, len);
		fs[f].driver->unlock();
	}
	platform_mutex_release(fs[f].mutex);
	return ret;
}

int32_t fs_lseek(int f, fs_fd fd, int32_t offs, int whence)
{
	int32_t ret;
	platform_mutex_acquire(fs[f].mutex);
	while(!fs[f].ready);
	if(((fd >> 16) & 0xFF) != fs[f].mount_count)
	{
		ret = -1;
	}
	else
	{
		fs[f].driver->lock();
		ret = SPIFFS_lseek(&fs[f].fs, (fd & 0xFFFF), offs, whence);
		fs[f].driver->unlock();
	}
	platform_mutex_release(fs[f].mutex);
	return ret;
}

int32_t fs_fstat(int f, fs_fd fd, fs_stat *s)
{
	int32_t ret;
	spiffs_stat stat;
	platform_mutex_acquire(fs[f].mutex);
	while(!fs[f].ready);
	if(((fd >> 16) & 0xFF) != fs[f].mount_count)
	{
		ret = -1;
	}
	else
	{
		fs[f].driver->lock();
		ret = SPIFFS_fstat(&fs[f].fs, (fd & 0xFFFF), &stat);
		fs[f].driver->unlock();
		s->size = stat.size;
	}
	platform_mutex_release(fs[f].mutex);
	return ret;
}

void fs_flush(int f, fs_fd fd)
{
	platform_mutex_acquire(fs[f].mutex);
	while(!fs[f].ready);
	if(((fd >> 16) & 0xFF) != fs[f].mount_count)
	{
		warn1("stale fd");
	}
	else
	{
		fs[f].driver->lock();
		SPIFFS_fflush(&fs[f].fs, (fd & 0xFFFF));
		fs[f].driver->unlock();
	}
	platform_mutex_release(fs[f].mutex);
}

void fs_close(int f, fs_fd fd)
{
	platform_mutex_acquire(fs[f].mutex);
	while(!fs[f].ready);
	if(((fd >> 16) & 0xFF) != fs[f].mount_count)
	{
		;
	}
	else
	{
		fs[f].driver->lock();
		SPIFFS_close(&fs[f].fs, (fd & 0xFFFF));
		fs[f].driver->unlock();
	}
	platform_mutex_release(fs[f].mutex);
}

void fs_unlink(int f, char *path)
{
	platform_mutex_acquire(fs[f].mutex);
	while(!fs[f].ready);
	fs[f].driver->lock();
	debug1("unlink: %s", path);
	SPIFFS_remove(&fs[f].fs, path);
	fs[f].driver->unlock();
	platform_mutex_release(fs[f].mutex);
}

static void fs_thread(void *p)
{
	int f;
	int ret;
	void *command;
	uint32_t total, used;
	// spi_flash_mass_erase();
	for (f = 0; f < FS_MAX; f++) {
		debug1("mounting fs #%d", f);
		fs[f].driver->lock();
		ret = SPIFFS_mount(&fs[f].fs, &fs[f].cfg, fs[f].work_buf, fs[f].fds, sizeof(fs[f].fds), NULL, 0, 0);
		if(ret != SPIFFS_OK)
		{
			debug1("formatting #%d", f);
			SPIFFS_format(&fs[f].fs);
			SPIFFS_mount(&fs[f].fs, &fs[f].cfg, fs[f].work_buf, fs[f].fds, sizeof(fs[f].fds), NULL, 0, 0);
		}
		ret = SPIFFS_info(&fs[f].fs, &total, &used);
		fs[f].driver->unlock();
		fs[f].mount_count++;
		debug1("fs #%d ready, total: %u, used: %u", f, (unsigned int)total, (unsigned int)used);
		fs[f].ready = 1;
	}
	while(1)
	{
		if(osMessageQueueGet(fs_queue, &command, NULL, 1000) != osOK)continue;
		debug1("got message");
	}
}

/*
static int fs_error_increase(int32_t error)
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
	if(fs_error_count < 254)fs_error_count++;

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
		if(fs_task.error == 0){
			err = SPIFFS_mount(&fs_fs, &fs_cfg, fs_work_buf, fs_fds, sizeof(fs_fds), NULL, 0, 0);
			logger(err==0?LOG_DEBUG1:LOG_ERR1, "FS: FS MOUNT r=%"PRIi32, err);
			return err;
		}
	#endif//SPIFFS_CHECK_ENABLED
	return(1);
}
*/

static int32_t fs_read0(uint32_t addr, uint32_t size, uint8_t * dst)
{
	return fs[0].driver->read(fs[0].partition, addr, size, dst);
}

static int32_t fs_write0(uint32_t addr, uint32_t size, uint8_t * src)
{
	return fs[0].driver->write(fs[0].partition, addr, size, src);
}

static int32_t fs_erase0(uint32_t addr, uint32_t size)
{
	return fs[0].driver->erase(fs[0].partition, addr, size);
}

#if FS_MAX > 0
static int32_t fs_read1(uint32_t addr, uint32_t size, uint8_t * dst)
{
	return fs[1].driver->read(fs[1].partition, addr, size, dst);
}

static int32_t fs_write1(uint32_t addr, uint32_t size, uint8_t * src)
{
	return fs[1].driver->write(fs[1].partition, addr, size, src);
}

static int32_t fs_erase1(uint32_t addr, uint32_t size)
{
	return fs[1].driver->erase(fs[1].partition, addr, size);
}
#endif
#if FS_MAX > 1
static int32_t fs_read2(uint32_t addr, uint32_t size, uint8_t * dst)
{
	return fs[2].driver->read(fs[2].partition, addr, size, dst);
}

static int32_t fs_write2(uint32_t addr, uint32_t size, uint8_t * src)
{
	return fs[2].driver->write(fs[2].partition, addr, size, src);
}

static int32_t fs_erase2(uint32_t addr, uint32_t size)
{
	return fs[2].driver->erase(fs[2].partition, addr, size);
}
#endif

