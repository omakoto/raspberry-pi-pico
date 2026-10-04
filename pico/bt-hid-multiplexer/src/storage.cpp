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
    if (data->magic != FLASH_KEYMAP_MAGIC || data->version != FLASH_KEYMAP_VERSION) {
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
    data.version = FLASH_KEYMAP_VERSION;
    data.checksum = computeChecksum(keymap);
    memcpy(data.keymap, keymap, sizeof(data.keymap));

    // Pad buffer to page boundary (multiple of 256 bytes). Static: too large for the stack.
    static uint8_t page_buf[(sizeof(KeymapStorageData) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE];
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

static uint16_t binding_checksum(const DeviceBindingEntry entries[MAX_DEVICE_BINDINGS]) {
    const uint8_t *p = (const uint8_t *)entries;
    uint16_t sum = 0xB1D5;
    for (size_t i = 0; i < sizeof(DeviceBindingEntry) * MAX_DEVICE_BINDINGS; i++) {
        sum = (sum << 1) ^ p[i];
    }
    return sum;
}

bool StorageManager::loadBindings(DeviceBindingEntry entries[MAX_DEVICE_BINDINGS]) {
    const DeviceBindingStorageData *data = (const DeviceBindingStorageData *)(XIP_BASE + FLASH_BINDINGS_OFFSET);
    if (data->magic != FLASH_BINDINGS_MAGIC || data->version != 1 ||
        data->checksum != binding_checksum(data->entries)) {
        return false;
    }
    memcpy(entries, data->entries, sizeof(data->entries));
    return true;
}

void StorageManager::saveBindings(const DeviceBindingEntry entries[MAX_DEVICE_BINDINGS]) {
    DeviceBindingStorageData data;
    data.magic = FLASH_BINDINGS_MAGIC;
    data.version = 1;
    data.checksum = binding_checksum(entries);
    memcpy(data.entries, entries, sizeof(data.entries));

    static uint8_t page_buf[(sizeof(DeviceBindingStorageData) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE];
    memset(page_buf, 0xFF, sizeof(page_buf));
    memcpy(page_buf, &data, sizeof(data));

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_BINDINGS_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_BINDINGS_OFFSET, page_buf, sizeof(page_buf));
    restore_interrupts(ints);
}
