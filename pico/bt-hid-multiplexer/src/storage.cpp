#include "storage.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include <string.h>

#define FLASH_STORAGE_ADDR ((const uint8_t *)(XIP_BASE + FLASH_KEYMAP_OFFSET))

uint16_t StorageManager::computeChecksum(const uint16_t keymap[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS]) {
    uint16_t sum = 0x55AA;
    const uint16_t *ptr = (const uint16_t *)keymap;
    size_t count = (NUM_LAYERS * MATRIX_ROWS * MATRIX_COLS);
    for (size_t i = 0; i < count; i++) {
        sum = (sum << 1) ^ ptr[i];
    }
    return sum;
}

void StorageManager::init() {
    // Memory mapped flash read requires no hardware setup
}

bool StorageManager::loadKeymap(uint16_t keymap[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS]) {
    const KeymapStorageData *data = (const KeymapStorageData *)FLASH_STORAGE_ADDR;
    if (data->magic != FLASH_KEYMAP_MAGIC || data->version != 1) {
        return false;
    }
    uint16_t expected_cs = computeChecksum(data->keymap);
    if (data->checksum != expected_cs) {
        return false;
    }
    memcpy(keymap, data->keymap, sizeof(data->keymap));
    return true;
}

void StorageManager::saveKeymap(const uint16_t keymap[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS]) {
    KeymapStorageData data;
    data.magic = FLASH_KEYMAP_MAGIC;
    data.version = 1;
    data.checksum = computeChecksum(keymap);
    memcpy(data.keymap, keymap, sizeof(data.keymap));

    // Pad buffer to page boundary (multiple of 256 bytes)
    uint8_t page_buf[1024];
    memset(page_buf, 0xFF, sizeof(page_buf));
    memcpy(page_buf, &data, sizeof(data));

    // Erase sector and program flash with interrupts disabled
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_KEYMAP_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_KEYMAP_OFFSET, page_buf, sizeof(page_buf));
    restore_interrupts(ints);
}

void StorageManager::clearKeymap() {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_KEYMAP_OFFSET, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
}
