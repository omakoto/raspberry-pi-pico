#ifndef VIRTUAL_MATRIX_H_
#define VIRTUAL_MATRIX_H_

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

class VirtualMatrix {
public:
    static void init();
    
    // Process raw HID keycode through the virtual matrix and layer engine
    static bool processKeyPress(uint8_t dev_idx, uint8_t raw_keycode, uint16_t &out_keycode);
    static bool processKeyRelease(uint8_t dev_idx, uint8_t raw_keycode, uint16_t &out_keycode);
    
    // Purge state for a disconnected device
    static void purgeDevice(uint8_t dev_idx);

    // Query the keycode a currently held virtual key was translated to, without re-triggering layer
    // actions. Returns 0 if the key is not held or is mapped to KC_NO.
    static uint16_t getActiveTranslation(uint8_t dev_idx, uint8_t raw_keycode);

    // Keycode mapped to a virtual key of a device on the currently active layer, following
    // transparent (KC_TRNS) entries down to the lower layers. A device bound to a layer (see
    // DeviceBindings) is looked up on the active layer (when a layer key is held), then on its own
    // layer, then on layer 0. Used for mouse motion, which has no press/release.
    static uint16_t resolveAction(uint8_t dev_idx, uint8_t raw_keycode);

    // VIAL/VIA keymap access
    static uint16_t getKeycode(uint8_t layer, uint8_t row, uint8_t col);
    static void setKeycode(uint8_t layer, uint8_t row, uint8_t col, uint16_t keycode);
    static void resetKeymap();
    // Persist keymap edits to flash once they have been quiet for a moment. Call from the main
    // loop: VIAL writes one key at a time, and each flash save blocks everything for tens of ms.
    static void flushPendingSave();
    static uint8_t getActiveLayer();

private:
    static uint16_t keymap_[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS];
    static uint16_t active_translation_[MAX_KEYBOARDS][256];
    static uint8_t default_layer_;
    static uint32_t momentary_layer_mask_;
    static uint32_t toggle_layer_mask_;

    static bool save_pending_;
    static uint32_t last_edit_ms_;

    static uint8_t computeActiveLayer();
};

#endif // VIRTUAL_MATRIX_H_
