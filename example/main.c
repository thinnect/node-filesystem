/**
 * Filesystem demo.
 *
 * Copyright Thinnect Inc. 2020
 * @license MIT
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#include "cmsis_os2_ext.h"

#include "platform.h"
#include "retargetspi.h"

#include "sleep.h"

#include "spi_flash.h"
#include "fs.h"

#include "basic_rtos_logger_setup.h"

#include "loglevels.h"
#define __MODUUL__ "main"
#define __LOG_LEVEL__ (LOG_LEVEL_main & BASE_LOG_LEVEL)
#include "log.h"

#ifndef DATAFLASH_SPIFFS_PARTITION
#define DATAFLASH_SPIFFS_PARTITION 2
#endif//DATAFLASH_SPIFFS_PARTITION

// Add the headeredit block
#include "incbin.h"
INCBIN(Header, "header.bin");

static fs_driver_t m_fs_driver;
static int m_fs_id;
const char m_test_data_rec[] = "Hello World!";
char m_buffer_rec[sizeof(m_test_data_rec)];

static void test_fs_direct (int fs_id);
static void test_fs_record (int fs_id, void * p_user);

static void cb_read_done (int32_t res, void * p_user);
static void cb_write_done (int32_t res, void * p_user);

extern void start_fs_rw_thread ();

void main_loop (void * arg)
{
    // Switch to a thread-safe logger
    basic_rtos_logger_setup();

    RETARGET_SpiInit();
    spi_flash_init();

    uint8_t jedec[4] = {0};
    RETARGET_SpiTransferHalf(0, "\x9F", 1, jedec, 4);
    info1("jedec %02x%02x%02x%02x", jedec[0], jedec[1], jedec[2], jedec[3]);

    // Put the flash to sleep, it should resume automatically
    spi_flash_suspend();

    // Obtaining the JEDEC id should now return all zeros
    RETARGET_SpiTransferHalf(0, "\x9F", 1, jedec, 4);

    if (0 == memcmp(jedec, (char[4]){0}, 4)) // tsb0
    {
        debug1("sleeping %02x%02x%02x%02x", jedec[0], jedec[1], jedec[2], jedec[3]);
    }
    else if (0 == memcmp(jedec, (char[4]){0xff, 0xff, 0xff, 0xff}, 4)) // tsb2
    {
        debug1("sleeping %02x%02x%02x%02x", jedec[0], jedec[1], jedec[2], jedec[3]);
    }
    else
    {
        err1("not sleeping %02x%02x%02x%02x", jedec[0], jedec[1], jedec[2], jedec[3]);
    }

    debug1("performing mass-erase");
    spi_flash_mass_erase();

    debug1("initializing filesystem...");
    m_fs_driver.read = spi_flash_read;
    m_fs_driver.write = spi_flash_write;
    m_fs_driver.erase = spi_flash_erase;
    m_fs_driver.size = spi_flash_size;
    m_fs_driver.erase_size = spi_flash_erase_size;
    m_fs_driver.suspend = spi_flash_suspend;
    m_fs_driver.lock = spi_flash_lock;
    m_fs_driver.unlock = spi_flash_unlock;

    int fs_id = 0; // Will use first filesystem (possible to use up to 3 usually)
    fs_init(fs_id, DATAFLASH_SPIFFS_PARTITION, &m_fs_driver);
    fs_start();

    test_fs_direct(fs_id);

    bool record_callbacks_called = false;
    test_fs_record(fs_id, &record_callbacks_called);

    for (;;)
    {
        osDelay(1000);
        if (record_callbacks_called)
        {
            info1("DONE!");
            record_callbacks_called = false;
        }
    }
}

static void test_fs_direct (int fs_id)
{
    info1("TEST: test_fs_direct");

    const char test_data[] = "ABCDEFGH";

    debug1("creating file...");
    fs_fd fd = fs_open(fs_id, "test.txt", FS_TRUNC | FS_CREAT | FS_RDWR);
    debug1("FD: %d\n", (int)fd);

    debug1("writing file...");
    int32_t retw = fs_write(fs_id, fd, test_data, strlen(test_data));
    debug1("RET: %d\n", (int)retw);

    debug1("closing file...");
    fs_close(fs_id, fd);

    debug1("opening file...");
    fd = fs_open(fs_id, "test.txt", FS_RDONLY);
    debug1("FD: %d\n", (int)fd);

    debug1("reading file...");
    char buffer[sizeof(test_data)];
    int32_t retr = fs_read(fs_id, fd, buffer, strlen(test_data));
    debug1("RET: %d\n", (int)retr);

    debug1("closing file...");
    fs_close(fs_id, fd);

    buffer[strlen(test_data)] = '\0';
    debug1("data: %s", buffer);

    if (0 == memcmp(test_data, buffer, sizeof(test_data)))
    {
        info1("GOOD DATA: %s", buffer);
    }
    else
    {
        err1("BAD DATA");
    }
}

static void cb_write_done (int32_t res, void * p_user)
{
    debug1("cb_write_done:%d", res);

    if (sizeof(m_buffer_rec) != fs_read_record(m_fs_id, "helloworld.txt", m_buffer_rec, sizeof(m_buffer_rec), 0, cb_read_done, p_user))
    {
        err1("BAD record length on read");
    }
}

static void cb_read_done (int32_t res, void * p_user)
{
    debug1("cb_read_done:%d", res);
    *((bool*)p_user) = true;

    info1("Read: %s", m_test_data_rec);
    if (0 == memcmp(m_test_data_rec, m_buffer_rec, sizeof(m_test_data_rec)))
    {
        info1("GOOD DATA: %s", m_buffer_rec);
    }
    else
    {
        err1("BAD DATA");
    }
}

static void test_fs_record (int fs_id, void * p_user)
{
    m_fs_id = fs_id;

    info1("TEST: test_fs_record");
    info1("Write: %s", m_test_data_rec);
    if (sizeof(m_test_data_rec) != fs_write_record(m_fs_id, "helloworld.txt", m_test_data_rec, sizeof(m_test_data_rec), 0, cb_write_done, p_user))
    {
        err1("BAD record length on write");
    }
}

int main ()
{
    PLATFORM_Init();

    // LEDs
    PLATFORM_LedsInit();

    PLATFORM_LedsSet(0); // Indicate: starting OS

    basic_noos_logger_setup();

    info1("filesystem demo");

    PLATFORM_LedsSet(1); // Indicate: starting OS

    // Initialize sleep management
    SLEEP_Init(NULL, NULL);

    osKernelInitialize();

    const osThreadAttr_t main_thread_attr = { .name = "main", .stack_size = 4096 };
    osThreadNew(main_loop, NULL, &main_thread_attr);

    if (osKernelReady == osKernelGetState())
    {
        osKernelStart();
    }
    else
    {
        err1("!osKernelReady");
    }

    while (true);
}

