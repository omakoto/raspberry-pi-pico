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

    // Keycode mapped to a virtual key of a device on its active layer. Transparent (KC_TRNS) entries
    // fall through the other layers switched on in the device's group (highest first), then the
    // device's own layer if it is bound to one (see DeviceBindings), then layer 0. Layers that are
    // not switched on are skipped. Used for mouse motion, which has no press/release.
    //
    // Layer keys (MO/TG/TO) only affect the devices in the same group as the device they were pressed
    // on: all unbound devices form one group, and the devices bound to the same layer form one. This
    // way a layer key on a bound device does not change what other devices do.
    static uint16_t resolveAction(uint8_t dev_idx, uint8_t raw_keycode);

    // VIAL/VIA keymap access
    static uint16_t getKeycode(uint8_t layer, uint8_t row, uint8_t col);
    static void setKeycode(uint8_t layer, uint8_t row, uint8_t col, uint16_t keycode);
    static void resetKeymap();
    // Persist keymap edits to flash once they have been quiet for a moment. Call from the main
    // loop: VIAL writes one key at a time, and each flash save blocks everything for tens of ms.
    static void flushPendingSave();
    // The layer selected by the layer keys of the unbound devices.
    static uint8_t getActiveLayer();
    // The layer that takes effect for a device right now: the layer selected by a held or toggled
    // layer key of its group if there is one, else the device's bound layer, else the base layer.
    // With no device (DeviceBindings::NO_DEVICE) it is the layer of the unbound devices.
    static uint8_t getEffectiveLayer(uint8_t dev_idx);

private:
    static uint16_t keymap_[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS];
    static uint16_t active_translation_[MAX_KEYBOARDS][256];

    // Layer key state of one group of devices.
    struct LayerState {
        uint8_t default_layer;
        uint32_t momentary_mask;
        uint32_t toggle_mask;
    };
    // Indexed by group: 0 for the unbound devices, N for the devices bound to layer N.
    static LayerState layer_state_[NUM_LAYERS];
    // The group a held layer key was pressed in, so that its release updates the same group even if
    // the device's binding changed in between.
    static uint8_t layer_key_group_[MAX_KEYBOARDS][256];

    static bool save_pending_;
    static uint32_t last_edit_ms_;

    static uint8_t groupOf(uint8_t dev_idx);
    static uint8_t computeActiveLayer(uint8_t group);
};

#endif // VIRTUAL_MATRIX_H_
