/**
 * Filesystem wrapper for SPIFFS
 *
 * Copyright Thinnect Inc. 2020
 * @license MIT
 */

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

struct fs_stat_struct
{
	uint32_t size;
};

typedef struct fs_stat_struct fs_stat;

/**
 * Initializes LMT01 driver
 */
void fs_init();

/**
 * Opens the file specified by path
 *
 * @param path Name of the file to be opened
 * @param flags Flags
 *
 * @return Returns file descriptor or error
 */
fs_fd fs_open(char *path, uint32_t flags);

/**
 * Attempts to read up to count bytes from file descriptor fd into the buffer starting at buf
 *
 * @param fd File descriptor
 * @param buf Pointer where to read data
 * @param count Length of data to be read
 *
 * @return the number of bytes read or error
 */
int32_t fs_read(fs_fd fd, void *buf, int32_t count);

/**
 * Writes up to count bytes from the buffer starting at buf
 *
 * @param fd File descriptor
 * @param buf Pointer to the data to be written
 * @param count Length of data to be written
 *
 * @return the number of bytes written or error
 */
int32_t fs_write(fs_fd fd, const void *buf, int32_t count);

/**
 * Closes the file descriptor
 *
 * @param fd File descriptor
 */
void fs_close(fs_fd fd);

/**
 * Deletes a name from the filesystem
 *
 * @param path Name of the file to be deleted
 */
void fs_unlink(char *path);

/**
 * Repositions the file offset of the open file descriptor
 *
 * @param fd File descriptor
 * @param offs The new offset
 * @param whence Offset from beginning, from current location or from end of file
 *
 * @return The resulting offset or error
 */
int32_t fs_lseek(fs_fd fd, int32_t offs, int whence);

/**
 * Return information about a file
 *
 * @param fd File descriptor
 * @param s Buffer to store the information
 *
 * @return 0 or negative on error 
 */
int32_t fs_fstat(fs_fd fd, fs_stat *s);

#endif

