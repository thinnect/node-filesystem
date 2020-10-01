/******************************************************************************
 * Read/write data structure from/to the file
 *
 * Copyright Thinnect Inc. 2020
 * @license MIT
 *****************************************************************************/
#include "loglevels.h"
#define __MODUUL__ "FSRW"
#define __LOG_LEVEL__ (LOG_LEVEL_FileSystemReadWrite & BASE_LOG_LEVEL)
#include "log.h"

#include <stdint.h>
#include <inttypes.h>

#include "cmsis_os2.h"

#include "fs.h"
#include "rw_fs_record.h"

static osThreadId_t m_fs_rw_thread_id;

#define FS_WRITE_FLAG 0x00000001U
#define FS_READ_FLAG 0x00000002U

static fs_rw_params_t m_write_params;
static fs_rw_params_t m_read_params;

/*****************************************************************************
 * Filesystem read/write thread waits read/write flag and starts 
 * read/write process.
 * When read/write is done the corresponding read/write callback is called
 ****************************************************************************/
static void fs_rw_loop (void * arg)
{
    uint32_t flags;
    fs_fd file_desc;
    int32_t res;

    for(;;)
    {
        flags = osThreadFlagsWait(FS_WRITE_FLAG | FS_READ_FLAG, osFlagsWaitAny, osWaitForever);

        if (flags & FS_WRITE_FLAG)
        {
            debug1("Wr Thread");
            // open file for writing
            file_desc = fs_open(m_write_params.partition, (void*)m_write_params.p_file_name, FS_WRONLY);
            if (file_desc < 0)
            {
                // file does not exists or some other error
                debug1("File not exists:%s", m_write_params.p_file_name);
                // try to create new file
                file_desc = fs_open(m_write_params.partition, (void*)m_write_params.p_file_name, FS_TRUNC | FS_CREAT | FS_WRONLY);
                if (file_desc < 0)
                {
                    err1("Cannot create file:%s", m_write_params.p_file_name);
                    continue;
                }
            }
            res = fs_write(m_write_params.partition, file_desc, m_write_params.p_value, m_write_params.len);
            fs_close(m_write_params.partition, file_desc);
            m_write_params.callback_func(res);
        }

        if (flags & FS_READ_FLAG)
        {
            debug1("Rd Thread");
            // open file for reading
            file_desc = fs_open(m_read_params.partition, (void*)m_read_params.p_file_name, FS_RDONLY);
            debug1("fd:%d", file_desc);
            if (file_desc < 0)
            {
                // file does not exists or some other error
                debug1("File not exists:%s", m_read_params.p_file_name);
            }
            else
            {
                res = fs_read(m_read_params.partition, file_desc, m_read_params.p_value, m_read_params.len);
                fs_close(m_read_params.partition, file_desc);
                m_read_params.callback_func(res);
            }
        }
    }
}

/*****************************************************************************
 * Start filesystem read/write thread
 ****************************************************************************/
void start_fs_rw_thread ()
{
    const osThreadAttr_t m_fs_rw_thread_attr = { .name = "fs_rw_loop" };
    
    debug1("RdWr Thread");
    m_fs_rw_thread_id = osThreadNew(fs_rw_loop, NULL, &m_fs_rw_thread_attr);

    if (NULL == m_fs_rw_thread_id)
    {
        err1("ThreadID=0");
        while (1);
    }
}

/*****************************************************************************
 * Write one data record to the file
 * @params partition - Partition number 0..2
 * @params p_file_name - Pointer to the file name
 * @params p_value - pointer to the data record
 * @params len - data record length in bytes
 * @return Returns number of bytes written on success, FS_REC_FILE_ERROR otherwise
 ****************************************************************************/
int32_t fs_write_record (int partition, const char * p_file_name, const void * p_value, int32_t len, fs_write_done_f callback_func)
{
    debug1("FSWr:%s", p_file_name);
    m_write_params.partition = partition;
    m_write_params.p_file_name = p_file_name;
    m_write_params.p_value = p_value;
    m_write_params.len = len;
    m_write_params.callback_func = callback_func;

    osThreadFlagsSet(m_fs_rw_thread_id, FS_WRITE_FLAG);

    return len;
};

/*****************************************************************************
 * Read one data record from the file
 * @params partition - Partition number 0..2
 * @params p_file_name - Pointer to the file name
 * @params p_value - Pointer to the data record to read
 * @params len - data record length in bytes
 * @return Returns number of bytes read on success, FS_REC_FILE_ERROR otherwise
 ****************************************************************************/
int32_t fs_read_record (int partition, const char * p_file_name, void * p_value, int32_t len, fs_write_done_f callback_func)
{
    debug1("FSRd:%s", p_file_name);
    m_read_params.partition = partition;
    m_read_params.p_file_name = p_file_name;
    m_read_params.p_value = p_value;
    m_read_params.len = len;
    m_read_params.callback_func = callback_func;

    osThreadFlagsSet(m_fs_rw_thread_id, FS_READ_FLAG);

    return len;
};
