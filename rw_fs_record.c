/******************************************************************************
 * Read/write data structure from/to the file
 *
 * Copyright Thinnect Inc. 2020
 * @license MIT
 *****************************************************************************/
#include "loglevels.h"
#define __MODUUL__ "SNST"
#define __LOG_LEVEL__ (LOG_LEVEL_SeqNumStorage & BASE_LOG_LEVEL)
#include "log.h"

#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>

#include "fs.h"

#define FILE_INST 0
#define FILE_ERROR -1

/*****************************************************************************
 * Write one data record to the file
 * @params p_file_name - Pointer to the file name
 * @params p_value - pointer to the data record
 * @params len - data record length in bytes
 * @return Returns number of bytes written on success, FILE_ERROR otherwise
 ****************************************************************************/
int32_t fs_write_record (const char * p_file_name, const void * p_value, int32_t len)
{
    fs_fd file_desc;
    int32_t res;
    
    debug1("FSWr:%s:%X", p_file_name, p_value);
    // open file for writing
    file_desc = fs_open(FILE_INST, (void*)p_file_name, FS_WRONLY);
    if (file_desc < 0)
    {
        // file does not exists or some other error
        debug1("File not exists:%s", p_file_name);
        // try to create new file
        file_desc = fs_open(FILE_INST, (void*)p_file_name, FS_TRUNC | FS_CREAT | FS_WRONLY);
        if (file_desc < 0)
        {
            err1("Cannot create file:%s", p_file_name);
            return FILE_ERROR;
        }
    }
    res = fs_write(FILE_INST, file_desc, p_value, len);
    fs_close(FILE_INST, file_desc);
    return res;
};

/*****************************************************************************
 * Read one data record from the file
 * @params p_file_name - Pointer to the file name
 * @params p_value - Pointer to the data record to read
 * @params len - data record length in bytes
 * @return Returns number of bytes read on success, FILE_ERROR otherwise
 ****************************************************************************/
int32_t fs_read_record (const char * p_file_name, void * p_value, int32_t len)
{
    fs_fd file_desc;
    int32_t res;

    debug1("FSRd:%s", p_file_name);
    // open file for reading
    file_desc = fs_open(FILE_INST, (void*)p_file_name, FS_RDONLY);
    debug1("fd:%d", file_desc);
    if (file_desc < 0)
    {
        // file does not exists or some other error
        debug1("File not exists:%s", p_file_name);
        return FILE_ERROR;
    }
    res = fs_read(FILE_INST, file_desc, p_value, len);
    fs_close(FILE_INST, file_desc);
    return res;
};
