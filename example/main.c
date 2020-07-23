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

#include "retargetserial.h"

#include "platform.h"
#include "retargetspi.h"

#include "sleep.h"

#include "spi_flash.h"
#include "fs.h"

#include "loggers_ext.h"
#if defined(LOGGER_LDMA)
#include "logger_ldma.h"
#elif defined(LOGGER_FWRITE)
#include "logger_fwrite.h"
#endif//LOGGER_*

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

static const char m_test_data[] = "ABCDEFGH";

static fs_driver_t m_fs_driver;

void main_loop (void * arg)
{
    // Switch to a thread-safe logger
    #if defined(LOGGER_LDMA)
        logger_ldma_init();
        #if defined(LOGGER_TIMESTAMP)
            log_init(BASE_LOG_LEVEL, &logger_ldma, &osCounterGetMilli);
        #else
            log_init(BASE_LOG_LEVEL, &logger_ldma, NULL);
        #endif
    #elif defined(LOGGER_FWRITE)
        logger_fwrite_init();
        #if defined(LOGGER_TIMESTAMP)
            log_init(BASE_LOG_LEVEL, &logger_fwrite, &osCounterGetMilli);
        #else
            log_init(BASE_LOG_LEVEL, &logger_fwrite, NULL);
        #endif
    #else
        #warning "No LOGGER_* has been defined!"
    #endif//LOGGER_*

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
    m_fs_driver.lock = spi_flash_lock;
    m_fs_driver.unlock = spi_flash_unlock;

    int fs_id = 0; // Will use first filesystem (possible to use up to 3 usually)
    fs_init(fs_id, DATAFLASH_SPIFFS_PARTITION, &m_fs_driver);
    fs_start();

    debug1("creating file...");
    fs_fd fd = fs_open(fs_id, "test.txt", FS_TRUNC | FS_CREAT | FS_RDWR);
    debug1("FD: %d\n", (int)fd);

    debug1("writing file...");
    int32_t retw = fs_write(fs_id, fd, m_test_data, strlen(m_test_data));
    debug1("RET: %d\n", (int)retw);

    debug1("closing file...");
    fs_close(fs_id, fd);

    debug1("opening file...");
    fd = fs_open(fs_id, "test.txt", FS_RDONLY);
    debug1("FD: %d\n", (int)fd);

    debug1("reading file...");
    char buffer[sizeof(m_test_data)];
    int32_t retr = fs_read(fs_id, fd, buffer, strlen(m_test_data));
    debug1("RET: %d\n", (int)retr);

    debug1("closing file...");
    fs_close(fs_id, fd);

    buffer[strlen(m_test_data)] = '\0';
    debug1("data: %s", buffer);

    if (0 == memcmp(m_test_data, buffer, sizeof(m_test_data)))
    {
        info1("GOOD DATA");
    }
    else
    {
        err1("BAD DATA");
    }

    for (;;)
    {
        osDelay(1000);
    }
}

// Basic logger before kernel boot
int logger_fwrite_boot (const char *ptr, int len)
{
    fwrite(ptr, len, 1, stdout);
    fflush(stdout);
    return len;
}

int main ()
{
    PLATFORM_Init();

    // LEDs
    PLATFORM_LedsInit();

    PLATFORM_LedsSet(0); // Indicate: starting OS

    RETARGET_SerialInit();
    log_init(BASE_LOG_LEVEL, &logger_fwrite_boot, NULL);

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

    while(true);
}

