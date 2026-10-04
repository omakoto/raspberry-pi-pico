#include "storage.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include <string.h>

#define FLASH_STORAGE_ADDR ((const uint8_t *)(XIP_BASE + FLASH_KEYMAP_OFFSET))

uint16_t StorageManager::computeChecksum(const uint16_t *keymap, size_t count) {
    uint16_t sum = 0x55AA;
    for (size_t i = 0; i < count; i++) {
        sum = (sum << 1) ^ keymap[i];
    }
    return sum;
}

// Keymap layout of firmware with LEGACY_NUM_LAYERS layers.
struct LegacyKeymapStorageData {
    uint32_t magic;
    uint16_t version;
    uint16_t checksum;
    uint16_t keymap[LEGACY_NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS];
};

static const size_t KEYMAP_FLASH_BYTES =
    (sizeof(KeymapStorageData) + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE * FLASH_SECTOR_SIZE;
static_assert(KEYMAP_FLASH_BYTES <= FLASH_KEYMAP_RESERVED_BYTES, "keymap does not fit its flash area");

void StorageManager::init() {
    // A 4-layer firmware kept the bindings in the sector that now holds the second half of the
    // keymap. Move them out of the way before anything gets written.
    DeviceBindingEntry entries[MAX_DEVICE_BINDINGS];
    if (!loadBindings(entries)) {
        const DeviceBindingStorageData *legacy =
            (const DeviceBindingStorageData *)(XIP_BASE + FLASH_LEGACY_BINDINGS_OFFSET);
        if (legacy->magic == FLASH_BINDINGS_MAGIC && legacy->version == 1 &&
            legacy->checksum == bindingChecksum(legacy->entries)) {
            memcpy(entries, legacy->entries, sizeof(entries));
            saveBindings(entries);
        }
    }
}

bool StorageManager::loadKeymap(uint16_t keymap[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS], bool &needs_save) {
    needs_save = false;
    const KeymapStorageData *data = (const KeymapStorageData *)FLASH_STORAGE_ADDR;
    if (data->magic != FLASH_KEYMAP_MAGIC) {
        return false;
    }
    if (data->version == FLASH_KEYMAP_VERSION) {
        if (data->checksum != computeChecksum(&data->keymap[0][0][0], NUM_LAYERS * MATRIX_ROWS * MATRIX_COLS)) {
            return false;
        }
        memcpy(keymap, data->keymap, sizeof(data->keymap));
        return true;
    }
    if (data->version == FLASH_KEYMAP_LEGACY_VERSION) {
        const LegacyKeymapStorageData *old = (const LegacyKeymapStorageData *)FLASH_STORAGE_ADDR;
        if (old->checksum != computeChecksum(&old->keymap[0][0][0], LEGACY_NUM_LAYERS * MATRIX_ROWS * MATRIX_COLS)) {
            return false;
        }
        memcpy(keymap, old->keymap, sizeof(old->keymap));
        for (int l = LEGACY_NUM_LAYERS; l < NUM_LAYERS; l++) {
            for (int r = 0; r < MATRIX_ROWS; r++) {
                for (int c = 0; c < MATRIX_COLS; c++) {
                    keymap[l][r][c] = 0x0001;  // KC_TRNS
                }
            }
        }
        needs_save = true;
        return true;
    }
    return false;
}

void StorageManager::saveKeymap(const uint16_t keymap[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS]) {
    KeymapStorageData data;
    data.magic = FLASH_KEYMAP_MAGIC;
    data.version = FLASH_KEYMAP_VERSION;
    data.checksum = computeChecksum(&keymap[0][0][0], NUM_LAYERS * MATRIX_ROWS * MATRIX_COLS);
    memcpy(data.keymap, keymap, sizeof(data.keymap));

    // Pad buffer to page boundary (multiple of 256 bytes). Static: too large for the stack.
    static uint8_t page_buf[(sizeof(KeymapStorageData) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE];
    memset(page_buf, 0xFF, sizeof(page_buf));
    memcpy(page_buf, &data, sizeof(data));

    // Erase the sectors and program flash with interrupts disabled
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_KEYMAP_OFFSET, KEYMAP_FLASH_BYTES);
    flash_range_program(FLASH_KEYMAP_OFFSET, page_buf, sizeof(page_buf));
    restore_interrupts(ints);
}

void StorageManager::clearKeymap() {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_KEYMAP_OFFSET, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
}

uint16_t StorageManager::bindingChecksum(const DeviceBindingEntry entries[MAX_DEVICE_BINDINGS]) {
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
        data->checksum != bindingChecksum(data->entries)) {
        return false;
    }
    memcpy(entries, data->entries, sizeof(data->entries));
    return true;
}

void StorageManager::saveBindings(const DeviceBindingEntry entries[MAX_DEVICE_BINDINGS]) {
    DeviceBindingStorageData data;
    data.magic = FLASH_BINDINGS_MAGIC;
    data.version = 1;
    data.checksum = bindingChecksum(entries);
    memcpy(data.entries, entries, sizeof(data.entries));

    static uint8_t page_buf[(sizeof(DeviceBindingStorageData) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE];
    memset(page_buf, 0xFF, sizeof(page_buf));
    memcpy(page_buf, &data, sizeof(data));

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_BINDINGS_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_BINDINGS_OFFSET, page_buf, sizeof(page_buf));
    restore_interrupts(ints);
}
