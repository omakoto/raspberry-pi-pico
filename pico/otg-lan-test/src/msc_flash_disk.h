/*
 * 32 KB FAT12 configuration drive, stored in the last 32 KB of the on-board flash and
 * exposed to the host as a USB mass storage device (TinyUSB MSC).
 */

#ifndef MSC_FLASH_DISK_H
#define MSC_FLASH_DISK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Disk geometry. All sizes in bytes unless stated otherwise.
#define MSC_DISK_SIZE            (32 * 1024)
#define MSC_DISK_SECTOR_SIZE     512
#define MSC_DISK_SECTOR_COUNT    (MSC_DISK_SIZE / MSC_DISK_SECTOR_SIZE)  // 64
#define MSC_DISK_ROOT_ENTRIES    64                                      // 4 root directory sectors

// Must be called before TinyUSB starts serving MSC requests. Formats the drive with an
// empty FAT12 file system if it does not hold one yet (first boot / erased flash).
void msc_flash_disk_init(void);

// Call from the main loop. Writes back dirty data to flash once the host has been idle
// for a short while (see MSC_DISK_FLUSH_IDLE_MS in the .c file).
void msc_flash_disk_poll(void);

// Write back any dirty data to flash right now.
void msc_flash_disk_sync(void);

#ifdef __cplusplus
}
#endif

#endif /* MSC_FLASH_DISK_H */
