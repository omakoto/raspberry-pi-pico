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
    // 0 while the device is paired. Otherwise the order in which bound devices stopped being paired
    // (higher = more recent), so that the oldest unpaired bindings are dropped first.
    uint32_t unpaired_seq;
    // The device's name, kept so that a binding can still be shown by name once the device is no
    // longer paired. May be empty.
    char name[32];
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
    // Loads the stored bindings. needs_save is set when they were stored in an older format and
    // should be written back in the current one.
    static bool loadBindings(DeviceBindingEntry entries[MAX_DEVICE_BINDINGS], bool &needs_save);
    static void saveBindings(const DeviceBindingEntry entries[MAX_DEVICE_BINDINGS]);
    // The VIAL macro buffer (MACRO_BUFFER_SIZE bytes).
    static bool loadMacros(uint8_t *buffer);
    static void saveMacros(const uint8_t *buffer);
};

#endif // STORAGE_H_
