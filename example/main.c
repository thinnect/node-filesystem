/**
 * Filesystem demo.
 *
 * Copyright Thinnect Inc. 2020
 *
 * @license <PROPRIETARY>
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#include "cmsis_os2.h"

#include "retargetserial.h"

#include "platform.h"
#include "retargetspi.h"

#include "watchdog.h"

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

// Add the headeredit block
#include "incbin.h"
INCLUDE_BINARY_FILE(header_start, header_end, "header.bin", ".text", "x");

void main_loop (void * arg)
{
    fs_driver_t fs_driver;
    fs_fd fd;
    int32_t ret;
    char buffer[16];
    uint8_t jedec[4] = {0};

    RETARGET_SpiInit();
    spi_flash_init();

    RETARGET_SpiTransferHalf(0, "\xAB", 1, NULL, 0);
    osDelay(50);

    RETARGET_SpiTransferHalf(0, "\x9F", 1, jedec, 4);
    info1("jedec %02x%02x%02x%02x", jedec[0], jedec[1], jedec[2], jedec[3]);

    fs_driver.read = spi_flash_read;
    fs_driver.write = spi_flash_write;
    fs_driver.erase = spi_flash_erase;
    fs_driver.size = spi_flash_size;
    fs_driver.erase_size = spi_flash_erase_size;
    fs_driver.lock = spi_flash_lock;
    fs_driver.unlock = spi_flash_unlock;
    debug1("initializing filesystem...");
    fs_init(0, 2, &fs_driver);
    fs_start();

    debug1("creating file...");
    fd = fs_open(0, "test.txt", FS_TRUNC | FS_CREAT | FS_RDWR);
    debug1("FD: %d\n", (int)fd);

    debug1("writing file...");
    ret = fs_write(0, fd, "ABCDEFGH", 8);
    debug1("RET: %d\n", (int)ret);

    debug1("closing file...");
    fs_close(0, fd);

    debug1("opening file...");
    fd = fs_open(0, "test.txt", FS_RDONLY);
    debug1("FD: %d\n", (int)fd);

    debug1("reading file...");
    ret = fs_read(0, fd, buffer, 8);
    debug1("RET: %d\n", (int)ret);

    debug1("closing file...");
    fs_close(0, fd);

    buffer[8] = 0;
    debug1("data: %s", buffer);


    while (true)
    {
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

    watchdog_disable();

    RETARGET_SerialInit();
    log_init(BASE_LOG_LEVEL, &logger_fwrite_boot, NULL);

    info1("filesystem demo");

    PLATFORM_LedsSet(1); // Indicate: starting OS

    osKernelInitialize();

    const osThreadAttr_t main_thread_attr = { .name = "main", .stack_size = 4096 };
    osThreadNew(main_loop, NULL, &main_thread_attr);

    if (osKernelReady == osKernelGetState())
    {
        // Switch to a thread-safe logger
        #if defined(LOGGER_LDMA)
            logger_ldma_init();
            #if defined(LOGGER_TIMESTAMP)
                log_init(BASE_LOG_LEVEL, &logger_ldma, &osCounterMilliGet);
            #else
                log_init(BASE_LOG_LEVEL, &logger_ldma, NULL);
            #endif
        #elif defined(LOGGER_FWRITE)
            logger_fwrite_init();
            #if defined(LOGGER_TIMESTAMP)
                log_init(BASE_LOG_LEVEL, &logger_fwrite, &osCounterMilliGet);
            #else
                log_init(BASE_LOG_LEVEL, &logger_fwrite, NULL);
            #endif
        #else
            #warning "No LOGGER_* has been defined!"
        #endif//LOGGER_*

        // Start the kernel
        osKernelStart();
    }
    else
    {
        err1("!osKernelReady");
    }

    while(true);
}

