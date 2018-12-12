#ifndef _FS_H_
#define _FS_H_

#include <stdint.h>
#include "spiffs.h"

#define FS_APPEND (SPIFFS_APPEND)
#define FS_TRUNC  (SPIFFS_TRUNC)
#define FS_CREAT  (SPIFFS_CREAT)
#define FS_RDONLY (SPIFFS_RDONLY)
#define FS_WRONLY (SPIFFS_WRONLY)
#define FS_RDWR   (SPIFFS_RDWR)

#define FS_SEEK_SET (SPIFFS_SEEK_SET)
#define FS_SEEK_CUR (SPIFFS_SEEK_CUR)
#define FS_SEEK_END (SPIFFS_SEEK_END)

#define FS_ERR_REFORMATTED (-70000)

typedef int32_t fs_fd;

struct fs_stat_struct{
	uint32_t size;
};

typedef struct fs_stat_struct fs_stat;

void fs_init();
void fs_lock();
void fs_unlock();
fs_fd fs_open(char *path, uint32_t flags);
int32_t fs_read(fs_fd fd, void *buf, int32_t count);
int32_t fs_write(fs_fd fd, const void *buf, int32_t count);
void fs_close(fs_fd fd);
void fs_unlink(char *path);
int32_t fs_lseek(fs_fd fd, int32_t offs, int whence);
int32_t fs_fstat(fs_fd fd, fs_stat *s);

#endif

