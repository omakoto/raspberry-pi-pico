/*
 * Flash FatFs Driver for nsbackend-pico.
 * Provides on-flash FAT12 partition access for FatFs and TinyUSB MSC.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "ff.h"
#include "diskio.h"

// 1MB flash partition located at 1MB offset (0x100000)
constexpr uint32_t FLASH_PARTITION_OFFSET = 0x100000;
constexpr uint32_t FLASH_PARTITION_SIZE   = 0x100000;
constexpr uint32_t FLASH_SECTOR_COUNT     = FLASH_PARTITION_SIZE / 512;

bool flash_fatfs_init();
void flash_fatfs_sync();

void flash_msc_read(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize);
void flash_msc_write(uint32_t lba, uint32_t offset, const uint8_t* buffer, uint32_t bufsize);
