#include "fs.h"
#include <stdio.h>
#include <stdint.h>
#include "cmsis_os2.h"
#include "spi_flash.h"
#include "spiffs.h"

static volatile int fs_ready;
static osMutexId_t fs_mutex;
static osMessageQueueId_t fs_queue;
static spiffs_config fs_cfg;
static spiffs fs_fs;
static uint8_t fs_work_buf[SPIFFS_CFG_LOG_PAGE_SZ(0) * 2];
static uint8_t fs_fds[32 * 4];

static void fs_thread(void *p);

void fs_init(){
	fs_ready = 0;
	fs_mutex = osMutexNew(NULL);
	fs_queue = osMessageQueueNew(16, sizeof(void *), NULL);
	osThreadNew(fs_thread, NULL, NULL);
	// osMessageQueuePut(fs_queue, &fs_queue, 0, 0);
}

void fs_lock(){
	while(!fs_ready);
	while(osMutexAcquire(fs_mutex, 1000) != osOK);
}

void fs_unlock(){
	while(!fs_ready);
	osMutexRelease(fs_mutex);
}

fs_fd fs_open(char *path, uint32_t flags){
	fs_fd fd;
	while(!fs_ready);
	while(osMutexAcquire(fs_mutex, 1000) != osOK);
	fd = SPIFFS_open(&fs_fs, path, flags, 0);
	osMutexRelease(fs_mutex);
	return(fd);
}

int32_t fs_read(fs_fd fd, void *buf, int32_t len){
	int32_t ret;
	while(!fs_ready);
	while(osMutexAcquire(fs_mutex, 1000) != osOK);
	ret = SPIFFS_read(&fs_fs, fd, buf, len);
	osMutexRelease(fs_mutex);
	return(ret);
}

int32_t fs_write(fs_fd fd, const void *buf, int32_t len){
	int32_t ret;
	while(!fs_ready);
	while(osMutexAcquire(fs_mutex, 1000) != osOK);
	ret = SPIFFS_write(&fs_fs, fd, (void *)buf, len);
	osMutexRelease(fs_mutex);
	return(ret);
}

void fs_close(fs_fd fd){
	while(!fs_ready);
	while(osMutexAcquire(fs_mutex, 1000) != osOK);
	SPIFFS_close(&fs_fs, fd);
	osMutexRelease(fs_mutex);
}

void fs_unlink(char *path){
	while(!fs_ready);
	while(osMutexAcquire(fs_mutex, 1000) != osOK);
	SPIFFS_remove(&fs_fs, path);
	osMutexRelease(fs_mutex);
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
	fs_ready = 1;
	printf("FS: READY\n");
	while(1){
		if(osMessageQueueGet(fs_queue, &command, NULL, 1000) != osOK)continue;
		printf("FS: GOT MESSAGE\n");
	}
}

