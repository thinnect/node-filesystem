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

typedef struct fs_driver_struct
{
	int32_t(*read)(int partition, uint32_t addr, uint32_t size, uint8_t * dst);
	int32_t(*write)(int partition, uint32_t addr, uint32_t size, uint8_t * src);
	int32_t(*erase)(int partition, uint32_t addr, uint32_t size);
	int32_t(*size)(int partition);
	int32_t(*erase_size)(int partition);
	void (*suspend)();
	void (*lock)();
	void (*unlock)();
} fs_driver_t;

typedef int32_t fs_fd;

typedef struct fs_stat_struct
{
	uint32_t size;
} fs_stat;


/**
 * Callback function for queued actions.
 * @param len Number of bytes read / written.
 * @param p_user User pointer provided with the call.
 */
typedef void (*fs_rw_done_f) (int32_t len,  void * p_user);

/**
 * Initializes filesystem on the specified partition of the driver.
 *
 * @param file_sys_nr - File system number 0..2
 * @param partition - The partition on the device to use for the filesystem.
 * @param driver - The device to use.
 */
void fs_init(int file_sys_nr, int partition, fs_driver_t *driver);

/**
 * Starts filesystem thread.
 */
void fs_start();

/**
 * Return filesystem total and used space.
 * 
 * @param file_sys_nr - File system number 0..2
 * @param p_total - Memory to stored total available space value (may be NULL if not needed).
 * @param p_used - Memory to stored used space value (may be NULL if not needed).
 * 
 * @return 0 for success, SPIFFS error otherwise.
 */
int32_t fs_info (int file_sys_nr, uint32_t * p_total, uint32_t * p_used);

/**
 * Opens the file specified by path
 *
 * @param file_sys_nr - File system number 0..2
 * @param path Name of the file to be opened
 * @param flags Flags
 *
 * @return Returns file descriptor or error
 */
fs_fd fs_open(int file_sys_nr, char *path, uint32_t flags);

/**
 * Attempts to read up to count bytes from file descriptor fd into the buffer starting at buf
 *
 * @param file_sys_nr - File system number 0..2
 * @param fd File descriptor
 * @param buf Pointer where to read data
 * @param count Length of data to be read
 *
 * @return the number of bytes read or error
 */
int32_t fs_read(int file_sys_nr, fs_fd fd, void *buf, int32_t count);

/**
 * Writes up to count bytes from the buffer starting at buf
 *
 * @param file_sys_nr - File system number 0..2
 * @param fd File descriptor
 * @param buf Pointer to the data to be written
 * @param count Length of data to be written
 *
 * @return the number of bytes written or error
 */
int32_t fs_write(int file_sys_nr, fs_fd fd, const void *buf, int32_t count);

/**
 * Flushes cached writes to flash.
 *
 * @param file_sys_nr - File system number 0..2
 * @param fd File descriptor
 */
void fs_flush(int file_sys_nr, fs_fd fd);

/**
 * Closes the file descriptor
 *
 * @param file_sys_nr - File system number 0..2
 * @param fd File descriptor
 */
void fs_close(int file_sys_nr, fs_fd fd);

/**
 * Deletes a name from the filesystem
 *
 * @param file_sys_nr - File system number 0..2
 * @param path Name of the file to be deleted
 */
void fs_unlink(int file_sys_nr, char *path);

/**
 * Repositions the file offset of the open file descriptor
 *
 * @param file_sys_nr - File system number 0..2
 * @param fd File descriptor
 * @param offs The new offset
 * @param whence Offset from beginning, from current location or from end of file
 *
 * @return The resulting offset or error
 */
int32_t fs_lseek(int file_sys_nr, fs_fd fd, int32_t offs, int whence);

/**
 * Return information about a file
 *
 * @param file_sys_nr - File system number 0..2
 * @param fd File descriptor
 * @param s Buffer to store the information
 *
 * @return 0 or negative on error
 */
int32_t fs_fstat(int file_sys_nr, fs_fd fd, fs_stat *s);

/*****************************************************************************
 * Put one data read request to the read queue
 * @param file_sys_nr - File system number 0..2
 * @param p_file_name - Pointer to the file name
 * @param p_value - Pointer to the data record
 * @param len - Data record length in bytes
 * @param wait - When wait = 0 function returns immediately, even when putting fails,
 *                otherwise waits until put succeeds (and blocks calling thread)
 * @param f_callback - Callback when operation has been completed.
 * @param p_user - User pointer passed to callback.
 *
 * @return Returns number of bytes to read on success, 0 otherwise
 ****************************************************************************/
int32_t fs_read_record (int file_sys_nr,
                        const char * p_file_name,
                        void * p_value,
                        int32_t len,
                        uint32_t wait,
                        fs_rw_done_f f_callback,
                        void * p_user);

/*****************************************************************************
 * Put one data write request to the write queue
 * @param file_sys_nr - File system number 0..2
 * @param p_file_name - Pointer to the file name
 * @param p_value - Pointer to the data record
 * @param len - Data record length in bytes
 * @param wait - When wait = 0 function returns immediately, even when putting fails,
 *                otherwise waits until put succeeds (and blocks calling thread)
 * @param f_callback - Callback when operation has been completed.
 * @param p_user - User pointer passed to callback.
 *
 * @return Returns number of bytes to write on success, 0 otherwise
 ****************************************************************************/
int32_t fs_write_record (int file_sys_nr,
                        const char * p_file_name,
                        const void * p_value,
                        int32_t len,
                        uint32_t wait,
                        fs_rw_done_f f_callback,
                        void * p_user);

#endif//_FS_H_
