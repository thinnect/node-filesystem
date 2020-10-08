/**
 * Filesystem wrapper for SPIFFS.
 *
 * Copyright Thinnect Inc. 2020
 * @license MIT
 */
#ifndef _FS_H_
#define _FS_H_

#include <stdint.h>
#include "spiffs.h"

#define FS_WRITE_DATA 1
#define FS_READ_DATA 2

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

struct fs_driver_struct
{
	int32_t(*read)(int partition, uint32_t addr, uint32_t size, uint8_t * dst);
	int32_t(*write)(int partition, uint32_t addr, uint32_t size, uint8_t * src);
	int32_t(*erase)(int partition, uint32_t addr, uint32_t size);
	int32_t(*size)(int partition);
	int32_t(*erase_size)(int partition);
	void (*suspend)();
	void (*lock)();
	void (*unlock)();
};

typedef struct fs_driver_struct fs_driver_t;

typedef int32_t fs_fd;

struct fs_stat_struct
{
	uint32_t size;
};

typedef struct fs_stat_struct fs_stat;

typedef void (*fs_write_done_f) (int32_t len);

typedef struct fs_rw_params
{
    int             partition;
    char *          p_file_name;
    void *          p_value;
    int32_t         len;
    uint16_t        caller_id;
    fs_write_done_f callback_func;
} fs_rw_params_t;

/**
 * Initializes filesystem
 */
void fs_init(int f, int partition, fs_driver_t *driver);

/**
 * Starts filesystem thread
 */
void fs_start();

/**
 * Opens the file specified by path
 *
 * @param f File system instance
 * @param path Name of the file to be opened
 * @param flags Flags
 *
 * @return Returns file descriptor or error
 */
fs_fd fs_open(int f, char *path, uint32_t flags);

/**
 * Attempts to read up to count bytes from file descriptor fd into the buffer starting at buf
 *
 * @param f File system instance
 * @param fd File descriptor
 * @param buf Pointer where to read data
 * @param count Length of data to be read
 *
 * @return the number of bytes read or error
 */
int32_t fs_read(int f, fs_fd fd, void *buf, int32_t count);

/**
 * Writes up to count bytes from the buffer starting at buf
 *
 * @param f File system instance
 * @param fd File descriptor
 * @param buf Pointer to the data to be written
 * @param count Length of data to be written
 *
 * @return the number of bytes written or error
 */
int32_t fs_write(int f, fs_fd fd, const void *buf, int32_t count);

/**
 * Flushes cached writes to flash.
 *
 * @param f File system instance
 * @param fd File descriptor
 */
void fs_flush(int f, fs_fd fd);

/**
 * Closes the file descriptor
 *
 * @param f File system instance
 * @param fd File descriptor
 */
void fs_close(int f, fs_fd fd);

/**
 * Deletes a name from the filesystem
 *
 * @param f File system instance
 * @param path Name of the file to be deleted
 */
void fs_unlink(int f, char *path);

/**
 * Repositions the file offset of the open file descriptor
 *
 * @param f File system instance
 * @param fd File descriptor
 * @param offs The new offset
 * @param whence Offset from beginning, from current location or from end of file
 *
 * @return The resulting offset or error
 */
int32_t fs_lseek(int f, fs_fd fd, int32_t offs, int whence);

/**
 * Return information about a file
 *
 * @param f File system instance
 * @param fd File descriptor
 * @param s Buffer to store the information
 *
 * @return 0 or negative on error
 */
int32_t fs_fstat(int f, fs_fd fd, fs_stat *s);

/*****************************************************************************
 * Put one data read/write request to the read/write queue and sets 
 * FS_READ_FLAG/FS_WRITE_FLAG on success
 * @params command_type - Command FS_CMD_RD or FS_CMD_WRITE
 * @params partition - Partition number 0..2
 * @params p_file_name - Pointer to the file name
 * @params p_value - Pointer to the data record
 * @params len - Data record length in bytes
 * @params wait - When wait = 0 function returns immediately, even when putting fails,
 *                otherwise waits until put succeeds (and blocks calling thread)
 *
 * @return Returns number of bytes to write on success, 0 otherwise
 ****************************************************************************/
int32_t fs_rw_record (uint8_t command_type,
                      int partition,
                      const char * p_file_name,
                      const void * p_value,
                      int32_t len,
                      fs_write_done_f callback_func,
                      uint32_t wait);

#endif//_FS_H_
