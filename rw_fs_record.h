#ifndef RW_FS_RECORD_H_
#define RW_FS_RECORD_H_

#define FS_REC_FILE_ERROR -1

typedef void (*fs_write_done_f) (int32_t len);

typedef struct
{
    int          partition;
    const char * p_file_name;
    const void * p_value;
    int32_t      len;
    uint16_t     caller_id;
    fs_write_done_f callback_func;
} fs_rw_params_t;

/*****************************************************************************
 * Write one data record to the file
 * @params p_file_name - Pointer to the file name
 * @params p_value - pointer to the data record
 * @params len - data record length in bytes
 * @return Returns number of bytes written on success, FILE_ERROR otherwise
 ****************************************************************************/
int32_t fs_write_record (int partition, const char * p_file_name, const void * p_value, int32_t len, fs_write_done_f callback_func);

/*****************************************************************************
 * Read one data record from the file
 * @params p_file_name - Pointer to the file name
 * @params p_value - Pointer to the data record to read
 * @params len - data record length in bytes
 * @return Returns number of bytes read on success, FILE_ERROR otherwise
 ****************************************************************************/
int32_t fs_read_record (int partition, const char * p_file_name, void * p_value, int32_t len, fs_write_done_f callback_func);

#endif //RW_FS_RECORD_H_
