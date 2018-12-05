#ifndef _FILESYSTEM_H_
#define _FILESYSTEM_H_

#include <stdint.h>
#include "spiffs.h"

#define FS_APPEND (SPIFFS_APPEND)
#define FS_TRUNC  (SPIFFS_TRUNC)
#define FS_CREAT  (SPIFFS_CREAT)
#define FS_RDONLY (SPIFFS_RDONLY)
#define FS_WRONLY (SPIFFS_WRONLY)
#define FS_RDWR   (SPIFFS_RDWR)

typedef int32_t fs_fd;

void fs_init();
void fs_lock();
void fs_unlock();
fs_fd fs_open(char *path, uint32_t flags);
int32_t fs_read(fs_fd fd, void *buf, int32_t count);
int32_t fs_write(fs_fd fd, const void *buf, int32_t count);
void fs_close(fs_fd fd);
void fs_unlink(char *path);
// s32_t SPIFFS_lseek(spiffs *fs, spiffs_file fh, s32_t offs, int whence);
// s32_t SPIFFS_stat(spiffs *fs, const char *path, spiffs_stat *s);
// s32_t SPIFFS_close(spiffs *fs, spiffs_file fh);

#endif

