/*
 * 32 KB FAT12 configuration drive on flash, served over USB mass storage.
 *
 * Layout on flash: the last MSC_DISK_SIZE bytes of the chip, so the region stays put
 * across firmware reflashes (a firmware UF2 never reaches that high) and its offset is
 * derived from PICO_FLASH_SIZE_BYTES for whatever board is selected.
 *
 * Writes: flash is programmed in 4 KB erase blocks, but the host writes 512 B sectors.
 * One erase block is cached in RAM; sector writes land in the cache and the block is
 * written back (erase + program) when the host touches a different block, ejects the
 * drive, or has been idle for MSC_DISK_FLUSH_IDLE_MS. Reads of sectors not in the dirty
 * cache go straight to the memory-mapped flash (XIP).
 *
 * Formatting: on boot, if the boot sector does not carry a valid FAT12 signature, a
 * fresh empty file system is written. FAT12 geometry for 64 sectors:
 *   sector 0        boot sector / BPB
 *   sector 1        FAT (1 copy; 64 clusters * 1.5 B = 96 B fits easily)
 *   sectors 2..5    root directory, 64 entries of 32 B
 *   sectors 6..63   58 data clusters of 1 sector (512 B) each
 */

#include "msc_flash_disk.h"

#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/stdlib.h"
#include "tusb.h"

#define ERASE_BLOCK_SIZE        FLASH_SECTOR_SIZE  // 4096
#define SECTORS_PER_BLOCK       (ERASE_BLOCK_SIZE / MSC_DISK_SECTOR_SIZE)
#define BLOCK_COUNT             (MSC_DISK_SIZE / ERASE_BLOCK_SIZE)
#define FLASH_DISK_OFFSET       (PICO_FLASH_SIZE_BYTES - MSC_DISK_SIZE)
#define MSC_DISK_FLUSH_IDLE_MS  250

// SCSI SYNCHRONIZE CACHE (10); TinyUSB's msc.h does not name this opcode.
#define SCSI_OPCODE_SYNCHRONIZE_CACHE_10 0x35

#define FAT_RESERVED_SECTORS    1
#define FAT_COUNT               1
#define FAT_SECTORS             1
#define ROOT_DIR_SECTORS        (MSC_DISK_ROOT_ENTRIES * 32 / MSC_DISK_SECTOR_SIZE)
#define FIRST_DATA_SECTOR       (FAT_RESERVED_SECTORS + FAT_COUNT * FAT_SECTORS + ROOT_DIR_SECTORS)

static uint8_t s_cache[ERASE_BLOCK_SIZE];
static int32_t s_cached_block = -1;
static bool s_cache_dirty = false;
static absolute_time_t s_flush_deadline;

static const uint8_t *flash_disk_base(void) {
    return (const uint8_t *)(XIP_BASE + FLASH_DISK_OFFSET);
}

// Runs with interrupts off (and core 1 parked, if it were running) via flash_safe_execute().
static void flash_write_block_unsafe(void *param) {
    uint32_t block = *(uint32_t *)param;
    uint32_t offset = FLASH_DISK_OFFSET + block * ERASE_BLOCK_SIZE;
    flash_range_erase(offset, ERASE_BLOCK_SIZE);
    flash_range_program(offset, s_cache, ERASE_BLOCK_SIZE);
}

static void flush_cache(void) {
    if (!s_cache_dirty || s_cached_block < 0) return;
    uint32_t block = (uint32_t)s_cached_block;
    int rc = flash_safe_execute(flash_write_block_unsafe, &block, UINT32_MAX);
    if (rc != PICO_OK) {
        printf("msc: flash write of block %lu failed: %d\n", (unsigned long)block, rc);
    }
    s_cache_dirty = false;
}

static void load_block(int32_t block) {
    if (block == s_cached_block) return;
    flush_cache();
    memcpy(s_cache, flash_disk_base() + (uint32_t)block * ERASE_BLOCK_SIZE, ERASE_BLOCK_SIZE);
    s_cached_block = block;
}

static void read_sector(uint32_t sector, uint8_t *dst) {
    int32_t block = (int32_t)(sector / SECTORS_PER_BLOCK);
    uint32_t offset_in_block = (sector % SECTORS_PER_BLOCK) * MSC_DISK_SECTOR_SIZE;
    if (block == s_cached_block && s_cache_dirty) {
        memcpy(dst, s_cache + offset_in_block, MSC_DISK_SECTOR_SIZE);
    } else {
        memcpy(dst, flash_disk_base() + (uint32_t)block * ERASE_BLOCK_SIZE + offset_in_block, MSC_DISK_SECTOR_SIZE);
    }
}

static void write_sector(uint32_t sector, const uint8_t *src) {
    int32_t block = (int32_t)(sector / SECTORS_PER_BLOCK);
    uint32_t offset_in_block = (sector % SECTORS_PER_BLOCK) * MSC_DISK_SECTOR_SIZE;
    load_block(block);
    if (memcmp(s_cache + offset_in_block, src, MSC_DISK_SECTOR_SIZE) != 0) {
        memcpy(s_cache + offset_in_block, src, MSC_DISK_SECTOR_SIZE);
        s_cache_dirty = true;
    }
    s_flush_deadline = make_timeout_time_ms(MSC_DISK_FLUSH_IDLE_MS);
}

//--------------------------------------------------------------------+
// Formatting
//--------------------------------------------------------------------+

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v) {
    put_u16(p, (uint16_t)v);
    put_u16(p + 2, (uint16_t)(v >> 16));
}

static bool has_valid_filesystem(void) {
    const uint8_t *bs = flash_disk_base();
    return bs[510] == 0x55 && bs[511] == 0xAA && memcmp(bs + 54, "FAT12   ", 8) == 0 &&
           bs[11] == (MSC_DISK_SECTOR_SIZE & 0xFF) && bs[12] == (MSC_DISK_SECTOR_SIZE >> 8);
}

static void format_disk(void) {
    uint8_t sector[MSC_DISK_SECTOR_SIZE];

    // Boot sector with a FAT12 BIOS Parameter Block.
    memset(sector, 0, sizeof(sector));
    sector[0] = 0xEB;  // jmp short
    sector[1] = 0x3C;
    sector[2] = 0x90;  // nop
    memcpy(sector + 3, "MSWIN4.1", 8);            // OEM name (the value most drivers expect)
    put_u16(sector + 11, MSC_DISK_SECTOR_SIZE);   // bytes per sector
    sector[13] = 1;                               // sectors per cluster
    put_u16(sector + 14, FAT_RESERVED_SECTORS);   // reserved sectors
    sector[16] = FAT_COUNT;                       // number of FATs
    put_u16(sector + 17, MSC_DISK_ROOT_ENTRIES);  // root directory entries
    put_u16(sector + 19, MSC_DISK_SECTOR_COUNT);  // total sectors (16-bit)
    sector[21] = 0xF8;                            // media descriptor: fixed disk
    put_u16(sector + 22, FAT_SECTORS);            // sectors per FAT
    put_u16(sector + 24, 1);                      // sectors per track (unused)
    put_u16(sector + 26, 1);                      // heads (unused)
    put_u32(sector + 28, 0);                      // hidden sectors
    put_u32(sector + 32, 0);                      // total sectors (32-bit), unused
    sector[36] = 0x80;                            // drive number
    sector[38] = 0x29;                            // extended boot signature
    put_u32(sector + 39, 0x4F54474C);             // volume serial number
    memcpy(sector + 43, "OTG-LAN    ", 11);       // volume label
    memcpy(sector + 54, "FAT12   ", 8);           // file system type (informational)
    sector[510] = 0x55;
    sector[511] = 0xAA;
    write_sector(0, sector);

    // FAT: entries 0 and 1 are reserved (media descriptor + end-of-chain), rest free.
    memset(sector, 0, sizeof(sector));
    sector[0] = 0xF8;
    sector[1] = 0xFF;
    sector[2] = 0xFF;
    write_sector(FAT_RESERVED_SECTORS, sector);

    // Root directory: a volume label entry, then empty.
    memset(sector, 0, sizeof(sector));
    memcpy(sector, "OTG-LAN    ", 11);
    sector[11] = 0x08;  // ATTR_VOLUME_ID
    write_sector(FAT_RESERVED_SECTORS + FAT_COUNT * FAT_SECTORS, sector);
    memset(sector, 0, sizeof(sector));
    for (uint32_t s = 1; s < ROOT_DIR_SECTORS; s++) {
        write_sector(FAT_RESERVED_SECTORS + FAT_COUNT * FAT_SECTORS + s, sector);
    }

    // Data area: erased flash reads as 0xFF, which is fine for unallocated clusters, but
    // an old file system may have left data behind; clear it so nothing leaks through.
    for (uint32_t s = FIRST_DATA_SECTOR; s < MSC_DISK_SECTOR_COUNT; s++) {
        write_sector(s, sector);
    }
    flush_cache();
}

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+

void msc_flash_disk_init(void) {
    s_cached_block = -1;
    s_cache_dirty = false;
    s_flush_deadline = at_the_end_of_time;

    if (has_valid_filesystem()) {
        printf("msc: %u KB FAT12 config drive at flash offset 0x%lx\n", MSC_DISK_SIZE / 1024,
               (unsigned long)FLASH_DISK_OFFSET);
    } else {
        printf("msc: no file system found, formatting %u KB FAT12 config drive at flash offset 0x%lx\n",
               MSC_DISK_SIZE / 1024, (unsigned long)FLASH_DISK_OFFSET);
        format_disk();
    }
}

void msc_flash_disk_poll(void) {
    if (s_cache_dirty && time_reached(s_flush_deadline)) {
        flush_cache();
    }
}

void msc_flash_disk_sync(void) {
    flush_cache();
}

//--------------------------------------------------------------------+
// TinyUSB MSC callbacks
//--------------------------------------------------------------------+

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;
    memcpy(vendor_id, "omakoto ", 8);
    memcpy(product_id, "otg-lan-test cfg", 16);
    memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    *block_count = MSC_DISK_SECTOR_COUNT;
    *block_size = MSC_DISK_SECTOR_SIZE;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void)lun;
    (void)power_condition;
    // Stop / eject: the host is done with the drive, get everything onto flash now.
    if (!start || load_eject) {
        flush_cache();
    }
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    (void)lun;
    (void)offset;  // always 0: CFG_TUD_MSC_EP_BUFSIZE is a whole sector
    if (lba >= MSC_DISK_SECTOR_COUNT || bufsize < MSC_DISK_SECTOR_SIZE) return -1;
    read_sector(lba, buffer);
    return MSC_DISK_SECTOR_SIZE;
}

bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    return true;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    (void)lun;
    (void)offset;
    if (lba >= MSC_DISK_SECTOR_COUNT || bufsize < MSC_DISK_SECTOR_SIZE) return -1;
    write_sector(lba, buffer);
    return MSC_DISK_SECTOR_SIZE;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize) {
    (void)lun;
    (void)buffer;
    (void)bufsize;
    switch (scsi_cmd[0]) {
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
        // Hosts send this when mounting; there is no tray to lock, just acknowledge it.
        return 0;
    case SCSI_OPCODE_SYNCHRONIZE_CACHE_10:
        flush_cache();
        return 0;
    default:
        // Unsupported command: stall it with a sense code so the host does not retry forever.
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
        return -1;
    }
}
