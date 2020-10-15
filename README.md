# Filesystem

This project contains code for the Filesystem abstraction layer.

# API

## Initializes filesystem
`void fs_init(int f, int partition, fs_driver_t *driver);`

## Starts filesystem thread
`void fs_start();`

## Opens the file specified by path
`fs_fd fs_open(int f, char *path, uint32_t flags);`

## Attempts to read up to count bytes from file descriptor fd into the buffer starting at buf
`int32_t fs_read(int f, fs_fd fd, void *buf, int32_t count);`

## Writes up to count bytes from the buffer starting at buf
`int32_t fs_write(int f, fs_fd fd, const void *buf, int32_t count);`

## Flushes cached writes to flash.
`void fs_flush(int f, fs_fd fd);`

## Closes the file descriptor
`void fs_close(int f, fs_fd fd);`

## Deletes a name from the filesystem
`void fs_unlink(int f, char *path);`

## Repositions the file offset of the open file descriptor
`int32_t fs_lseek(int f, fs_fd fd, int32_t offs, int whence);`

## Return information about a file
`int32_t fs_fstat(int f, fs_fd fd, fs_stat *s);`

## Write one data record to the file
`int32_t fs_write_record (int file_sys_nr, const char * p_file_name, const void * p_value, int32_t len, fs_rw_done_f callback_func, uint32_t wait)`

## Read one data record from the file
`int32_t fs_read_record (int file_sys_nr, const char * p_file_name, void * p_value, int32_t len, fs_rw_done_f callback_func, uint32_t wait)`

# Dependencies / submodules

Thinnect LowLevelLogging (submodule, MIT license)
Thinnect node-platform & buildsystem components.
SPIFFS (SPI Flash File System) https://github.com/pellepl/spiffs

# Example

See the [example](example) directory.
