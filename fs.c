#include "fs.h"
#include <stdio.h>
#include <stdint.h>
#include "cmsis_os2.h"
#include "spi_flash.h"
#include "spiffs.h"
#include "loglevels.h"

#define __MODUUL__ "fs"
#define __LOG_LEVEL__ (LOG_LEVEL_fs & BASE_LOG_LEVEL)
#include "log.h"

static volatile int fs_ready;
static uint8_t fs_mount_count;
static osMutexId_t fs_exclusive_mutex, fs_mutex;
static osMessageQueueId_t fs_queue;
static spiffs_config fs_cfg;
static spiffs fs_fs;
static uint8_t fs_work_buf[SPIFFS_CFG_LOG_PAGE_SZ(0) * 2];
static uint8_t fs_fds[32 * 4];

static void fs_thread(void *p);

void fs_init(){
	fs_ready = 0;
	fs_mount_count = 0;
	fs_exclusive_mutex = osMutexNew(NULL);
	fs_mutex = osMutexNew(NULL);
	fs_queue = osMessageQueueNew(16, sizeof(void *), NULL);
	osThreadNew(fs_thread, NULL, NULL);
	// osMessageQueuePut(fs_queue, &fs_queue, 0, 0);
}

void fs_lock(){
	while(osMutexAcquire(fs_exclusive_mutex, 1000) != osOK);
	while(!fs_ready);
}

void fs_unlock(){
	osMutexRelease(fs_exclusive_mutex);
}

fs_fd fs_open(char *path, uint32_t flags){
	spiffs_file sfd;
	fs_fd fd;
	while(osMutexAcquire(fs_exclusive_mutex, 1000) != osOK);
	while(osMutexAcquire(fs_mutex, 1000) != osOK);
	while(!fs_ready);
	sfd = SPIFFS_open(&fs_fs, path, flags, 0);
	fd = (fs_mount_count << 16) | sfd;
	osMutexRelease(fs_mutex);
	osMutexRelease(fs_exclusive_mutex);
	if(sfd < 0)return(sfd);
	return(fd);
}

int32_t fs_read(fs_fd fd, void *buf, int32_t len){
	int32_t ret;
	while(osMutexAcquire(fs_exclusive_mutex, 1000) != osOK);
	while(osMutexAcquire(fs_mutex, 1000) != osOK);
	while(!fs_ready);
	if(((fd >> 16) & 0xFF) != fs_mount_count){
		ret = -1;
	}else{
		ret = SPIFFS_read(&fs_fs, (fd & 0xFFFF), buf, len);
	}
	osMutexRelease(fs_mutex);
	osMutexRelease(fs_exclusive_mutex);
	return(ret);
}

int32_t fs_write(fs_fd fd, const void *buf, int32_t len){
	int32_t ret;
	while(osMutexAcquire(fs_exclusive_mutex, 1000) != osOK);
	while(osMutexAcquire(fs_mutex, 1000) != osOK);
	while(!fs_ready);
	if(((fd >> 16) & 0xFF) != fs_mount_count){
		ret = -1;
	}else{
		ret = SPIFFS_write(&fs_fs, (fd & 0xFFFF), (void *)buf, len);
	}
	osMutexRelease(fs_mutex);
	osMutexRelease(fs_exclusive_mutex);
	return(ret);
}

int32_t fs_lseek(fs_fd fd, int32_t offs, int whence){
	int32_t ret;
	while(osMutexAcquire(fs_exclusive_mutex, 1000) != osOK);
	while(osMutexAcquire(fs_mutex, 1000) != osOK);
	while(!fs_ready);
	if(((fd >> 16) & 0xFF) != fs_mount_count){
		ret = -1;
	}else{
		ret = SPIFFS_lseek(&fs_fs, (fd & 0xFFFF), offs, whence);
	}
	osMutexRelease(fs_mutex);
	osMutexRelease(fs_exclusive_mutex);
	return(ret);
}

int32_t fs_fstat(fs_fd fd, fs_stat *s){
	int32_t ret;
	spiffs_stat stat;
	while(osMutexAcquire(fs_exclusive_mutex, 1000) != osOK);
	while(osMutexAcquire(fs_mutex, 1000) != osOK);
	while(!fs_ready);
	if(((fd >> 16) & 0xFF) != fs_mount_count){
		ret = -1;
	}else{
		ret = SPIFFS_fstat(&fs_fs, (fd & 0xFFFF), &stat);
		s->size = stat.size;
	}
	osMutexRelease(fs_mutex);
	osMutexRelease(fs_exclusive_mutex);
	return(ret);
}

void fs_close(fs_fd fd){
	while(osMutexAcquire(fs_exclusive_mutex, 1000) != osOK);
	while(osMutexAcquire(fs_mutex, 1000) != osOK);
	while(!fs_ready);
	if(((fd >> 16) & 0xFF) != fs_mount_count){
		;
	}else{
		SPIFFS_close(&fs_fs, (fd & 0xFFFF));
	}
	osMutexRelease(fs_mutex);
	osMutexRelease(fs_exclusive_mutex);
}

void fs_unlink(char *path){
	while(osMutexAcquire(fs_exclusive_mutex, 1000) != osOK);
	while(osMutexAcquire(fs_mutex, 1000) != osOK);
	while(!fs_ready);
	SPIFFS_remove(&fs_fs, path);
	osMutexRelease(fs_mutex);
	osMutexRelease(fs_exclusive_mutex);
}

static void fs_thread(void *p){
	int ret;
	void *command;
	fs_cfg.hal_erase_f = spi_flash_erase;
	fs_cfg.hal_read_f = spi_flash_read;
	fs_cfg.hal_write_f = spi_flash_write;
	ret = SPIFFS_mount(&fs_fs, &fs_cfg, fs_work_buf, fs_fds, sizeof(fs_fds), NULL, 0, 0);
	if(ret != SPIFFS_OK){
		SPIFFS_format(&fs_fs);
		SPIFFS_mount(&fs_fs, &fs_cfg, fs_work_buf, fs_fds, sizeof(fs_fds), NULL, 0, 0);
	}
	fs_mount_count++;
	fs_ready = 1;
	debug1("filesystem ready\n");
	while(1){
		if(osMessageQueueGet(fs_queue, &command, NULL, 1000) != osOK)continue;
		printf("FS: GOT MESSAGE\n");
	}
}
/*
static int fs_error_increase(int32_t error){
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

