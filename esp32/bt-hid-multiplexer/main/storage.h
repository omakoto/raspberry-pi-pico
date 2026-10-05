#ifndef STORAGE_H_
#define STORAGE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "config.h"

// A Bluetooth device (by address) bound to a keymap layer, see DeviceBindings.
struct DeviceBindingEntry {
    uint8_t addr[6];
    uint8_t layer;
    uint8_t used;
};

// Keymap and device bindings in NVS (namespace NVS_NAMESPACE). Writes block both cores for the
// duration of the flash operation, so callers batch them (see VirtualMatrix::flushPendingSave).
class StorageManager {
public:
    static void init();
    // Loads the stored keymap. needs_save is set when the stored data had to be converted and should
    // be written back in the current format (nothing needs converting at the moment).
    static bool loadKeymap(uint16_t keymap[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS], bool &needs_save);
    // Writes only the layers that differ from what is stored.
    static void saveKeymap(const uint16_t keymap[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS]);
    static void clearKeymap();
    static bool loadBindings(DeviceBindingEntry entries[MAX_DEVICE_BINDINGS]);
    static void saveBindings(const DeviceBindingEntry entries[MAX_DEVICE_BINDINGS]);
    // The VIAL macro buffer (MACRO_BUFFER_SIZE bytes).
    static bool loadMacros(uint8_t *buffer);
    static void saveMacros(const uint8_t *buffer);
};

#endif // STORAGE_H_
