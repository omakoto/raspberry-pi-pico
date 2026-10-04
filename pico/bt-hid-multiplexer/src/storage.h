#ifndef STORAGE_H_
#define STORAGE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "config.h"

struct KeymapStorageData {
    uint32_t magic;
    uint16_t version;
    uint16_t checksum;
    uint16_t keymap[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS];
};

// A Bluetooth device (by address) bound to a keymap layer, see DeviceBindings.
struct DeviceBindingEntry {
    uint8_t addr[6];
    uint8_t layer;
    uint8_t used;
};

struct DeviceBindingStorageData {
    uint32_t magic;
    uint16_t version;
    uint16_t checksum;
    DeviceBindingEntry entries[MAX_DEVICE_BINDINGS];
};

class StorageManager {
public:
    static void init();
    // Loads the stored keymap. A keymap stored by firmware with fewer layers is migrated: the extra
    // layers are made transparent, and needs_save is set so that the caller stores the new format.
    static bool loadKeymap(uint16_t keymap[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS], bool &needs_save);
    static void saveKeymap(const uint16_t keymap[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS]);
    static void clearKeymap();
    static bool loadBindings(DeviceBindingEntry entries[MAX_DEVICE_BINDINGS]);
    static void saveBindings(const DeviceBindingEntry entries[MAX_DEVICE_BINDINGS]);

private:
    static uint16_t computeChecksum(const uint16_t *keymap, size_t count);
    static uint16_t bindingChecksum(const DeviceBindingEntry entries[MAX_DEVICE_BINDINGS]);
};

#endif // STORAGE_H_
