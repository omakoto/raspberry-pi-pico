/*
 * Flash FatFs Driver for nsbackend-pico.
 * Implements low-level SPI flash read/write buffering for FatFs diskio
 * and TinyUSB MSC callbacks.
 */

#include "flash_fatfs.hpp"
#include <cstring>
#include <cstdio>
#include <algorithm>
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "tusb.h"

static constexpr uint32_t SECTOR_SIZE = 512;
static constexpr uint32_t ERASE_BLOCK_SIZE = FLASH_SECTOR_SIZE; // 4096 bytes
static constexpr uint32_t SECTORS_PER_BLOCK = ERASE_BLOCK_SIZE / SECTOR_SIZE; // 8

static uint8_t s_cache_buf[ERASE_BLOCK_SIZE];
static int32_t s_cached_block = -1;
static bool s_cache_dirty = false;
static bool s_initialized = false;

static void flush_cache_internal() {
    if (s_cache_dirty && s_cached_block >= 0) {
        uint32_t flash_offset = FLASH_PARTITION_OFFSET + static_cast<uint32_t>(s_cached_block) * ERASE_BLOCK_SIZE;
        uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(flash_offset, ERASE_BLOCK_SIZE);
        flash_range_program(flash_offset, s_cache_buf, ERASE_BLOCK_SIZE);
        restore_interrupts(ints);
        s_cache_dirty = false;
    }
}

static void load_cache_block(int32_t block) {
    if (block == s_cached_block) {
        return;
    }
    flush_cache_internal();
    uint32_t flash_offset = FLASH_PARTITION_OFFSET + static_cast<uint32_t>(block) * ERASE_BLOCK_SIZE;
    const uint8_t* src = reinterpret_cast<const uint8_t*>(XIP_BASE + flash_offset);
    std::memcpy(s_cache_buf, src, ERASE_BLOCK_SIZE);
    s_cached_block = block;
}

bool flash_fatfs_init() {
    s_cached_block = -1;
    s_cache_dirty = false;
    s_initialized = true;
    return true;
}

void flash_fatfs_sync() {
    flush_cache_internal();
}

static void read_single_sector(uint32_t sector, uint8_t* dst) {
    int32_t block = static_cast<int32_t>(sector / SECTORS_PER_BLOCK);
    uint32_t offset_in_block = (sector % SECTORS_PER_BLOCK) * SECTOR_SIZE;

    if (block == s_cached_block && s_cache_dirty) {
        std::memcpy(dst, s_cache_buf + offset_in_block, SECTOR_SIZE);
    } else {
        uint32_t flash_offset = FLASH_PARTITION_OFFSET + sector * SECTOR_SIZE;
        const uint8_t* src = reinterpret_cast<const uint8_t*>(XIP_BASE + flash_offset);
        std::memcpy(dst, src, SECTOR_SIZE);
    }
}

static void write_single_sector(uint32_t sector, const uint8_t* src) {
    int32_t block = static_cast<int32_t>(sector / SECTORS_PER_BLOCK);
    uint32_t offset_in_block = (sector % SECTORS_PER_BLOCK) * SECTOR_SIZE;

    load_cache_block(block);
    std::memcpy(s_cache_buf + offset_in_block, src, SECTOR_SIZE);
    s_cache_dirty = true;
}

void flash_msc_read(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    uint8_t* dst = static_cast<uint8_t*>(buffer);
    uint32_t remaining = bufsize;
    uint32_t curr_lba = lba;
    uint32_t curr_offset = offset;

    while (remaining > 0) {
        uint8_t sector_data[SECTOR_SIZE];
        read_single_sector(curr_lba, sector_data);

        uint32_t chunk = std::min(remaining, SECTOR_SIZE - curr_offset);
        std::memcpy(dst, sector_data + curr_offset, chunk);

        dst += chunk;
        remaining -= chunk;
        curr_lba++;
        curr_offset = 0;
    }
}

void flash_msc_write(uint32_t lba, uint32_t offset, const uint8_t* buffer, uint32_t bufsize) {
    const uint8_t* src = buffer;
    uint32_t remaining = bufsize;
    uint32_t curr_lba = lba;
    uint32_t curr_offset = offset;

    while (remaining > 0) {
        if (curr_offset == 0 && remaining >= SECTOR_SIZE) {
            write_single_sector(curr_lba, src);
            src += SECTOR_SIZE;
            remaining -= SECTOR_SIZE;
            curr_lba++;
        } else {
            uint8_t sector_data[SECTOR_SIZE];
            read_single_sector(curr_lba, sector_data);
            uint32_t chunk = std::min(remaining, SECTOR_SIZE - curr_offset);
            std::memcpy(sector_data + curr_offset, src, chunk);
            write_single_sector(curr_lba, sector_data);

            src += chunk;
            remaining -= chunk;
            curr_lba++;
            curr_offset = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// FatFs diskio Interface
// ---------------------------------------------------------------------------

extern "C" {

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    return s_initialized ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    flash_fatfs_init();
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_PARERR;
    if (sector + count > FLASH_SECTOR_COUNT) return RES_PARERR;

    for (UINT i = 0; i < count; ++i) {
        read_single_sector(sector + i, buff + (i * SECTOR_SIZE));
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_PARERR;
    if (sector + count > FLASH_SECTOR_COUNT) return RES_PARERR;

    for (UINT i = 0; i < count; ++i) {
        write_single_sector(sector + i, buff + (i * SECTOR_SIZE));
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    if (pdrv != 0) return RES_PARERR;

    switch (cmd) {
        case CTRL_SYNC:
            flash_fatfs_sync();
            return RES_OK;

        case GET_SECTOR_COUNT:
            *reinterpret_cast<LBA_t*>(buff) = FLASH_SECTOR_COUNT;
            return RES_OK;

        case GET_SECTOR_SIZE:
            *reinterpret_cast<WORD*>(buff) = SECTOR_SIZE;
            return RES_OK;

        case GET_BLOCK_SIZE:
            *reinterpret_cast<DWORD*>(buff) = SECTORS_PER_BLOCK;
            return RES_OK;

        default:
            return RES_PARERR;
    }
}

DWORD get_fattime(void) {
    // Returns fixed time: 2026-09-06 12:00:00
    return ((DWORD)(2026 - 1980) << 25) |
           ((DWORD)9 << 21) |
           ((DWORD)6 << 16) |
           ((DWORD)12 << 11) |
           ((DWORD)0 << 5) |
           ((DWORD)0 >> 1);
}

// ---------------------------------------------------------------------------
// TinyUSB Mass Storage Class (MSC) Callbacks
// ---------------------------------------------------------------------------

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;
    const char vid[] = "RaspberryPi";
    const char pid[] = "Pico Storage";
    const char rev[] = "1.0";
    std::memcpy(vendor_id, vid, std::min(sizeof(vid) - 1, size_t(8)));
    std::memcpy(product_id, pid, std::min(sizeof(pid) - 1, size_t(16)));
    std::memcpy(product_rev, rev, std::min(sizeof(rev) - 1, size_t(4)));
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size) {
    (void)lun;
    *block_count = FLASH_SECTOR_COUNT;
    *block_size  = SECTOR_SIZE;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void)lun; (void)power_condition; (void)start; (void)load_eject;
    if (!start || load_eject) {
        flash_fatfs_sync();
    }
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    (void)lun;
    flash_msc_read(lba, offset, buffer, bufsize);
    return static_cast<int32_t>(bufsize);
}

bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    return true;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    (void)lun;
    flash_msc_write(lba, offset, buffer, bufsize);
    return static_cast<int32_t>(bufsize);
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize) {
    (void)lun; (void)buffer; (void)bufsize;
    switch (scsi_cmd[0]) {
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL: // 0x1E
        // Allow or prevent medium removal command from host operating systems
        return 0;
    default:
        return -1;
    }
}

} // extern "C"
