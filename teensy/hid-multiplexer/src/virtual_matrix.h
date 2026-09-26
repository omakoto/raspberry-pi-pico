#ifndef VIRTUAL_MATRIX_H_
#define VIRTUAL_MATRIX_H_

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

class VirtualMatrix {
public:
    static void init();
    
    // Process raw HID keycode through the virtual matrix and layer engine
    // Returns true if an output keycode should be sent to the PC
    static bool processKeyPress(uint8_t dev_idx, uint8_t raw_keycode, uint16_t &out_keycode);
    static bool processKeyRelease(uint8_t dev_idx, uint8_t raw_keycode, uint16_t &out_keycode);
    
    // Purge state for a disconnected device
    static void purgeDevice(uint8_t dev_idx);

    // VIAL/VIA keymap access
    static uint16_t getKeycode(uint8_t layer, uint8_t row, uint8_t col);
    static void setKeycode(uint8_t layer, uint8_t row, uint8_t col, uint16_t keycode);
    static void resetKeymap();
    static uint8_t getActiveLayer();

private:
    static uint16_t keymap_[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS];
    static uint16_t active_translation_[MAX_KEYBOARDS][256];
    static uint8_t default_layer_;
    static uint32_t momentary_layer_mask_;
    static uint32_t toggle_layer_mask_;

    static uint8_t computeActiveLayer();
};

#endif // VIRTUAL_MATRIX_H_
