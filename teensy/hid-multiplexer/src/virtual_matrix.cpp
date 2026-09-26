#include "virtual_matrix.h"
#include "storage.h"
#include <Arduino.h>

uint16_t VirtualMatrix::keymap_[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS];
uint16_t VirtualMatrix::active_translation_[MAX_KEYBOARDS][256];
uint8_t VirtualMatrix::default_layer_ = 0;
uint32_t VirtualMatrix::momentary_layer_mask_ = 0;
uint32_t VirtualMatrix::toggle_layer_mask_ = 0;

void VirtualMatrix::init() {
    StorageManager::init();
    StorageManager::loadKeymap(keymap_);

    default_layer_ = 0;
    momentary_layer_mask_ = 0;
    toggle_layer_mask_ = 0;

    for (uint8_t d = 0; d < MAX_KEYBOARDS; d++) {
        for (int k = 0; k < 256; k++) {
            active_translation_[d][k] = 0;
        }
    }
}

uint8_t VirtualMatrix::computeActiveLayer() {
    // Check momentary layers first from highest to lowest
    for (int l = NUM_LAYERS - 1; l >= 0; l--) {
        if (momentary_layer_mask_ & (1UL << l)) {
            return (uint8_t)l;
        }
    }
    // Then toggle layers
    for (int l = NUM_LAYERS - 1; l >= 0; l--) {
        if (toggle_layer_mask_ & (1UL << l)) {
            return (uint8_t)l;
        }
    }
    return default_layer_;
}

uint8_t VirtualMatrix::getActiveLayer() {
    return computeActiveLayer();
}

bool VirtualMatrix::processKeyPress(uint8_t dev_idx, uint8_t raw_keycode, uint16_t &out_keycode) {
    if (dev_idx >= MAX_KEYBOARDS) return false;

    uint8_t row = raw_keycode / MATRIX_COLS;
    uint8_t col = raw_keycode % MATRIX_COLS;
    uint8_t active_l = computeActiveLayer();

    uint16_t action = keymap_[active_l][row][col];

    // Fall back to Layer 0 if transparent (0x0000)
    if (action == 0x0000 && active_l != 0) {
        action = keymap_[0][row][col];
    }
    if (action == 0x0000) {
        action = raw_keycode;
    }

    // Handle Layer Switch Actions
    if (IS_ACTION_MO(action)) {
        uint8_t target_l = ACTION_LAYER_NUM(action);
        momentary_layer_mask_ |= (1UL << target_l);
        active_translation_[dev_idx][raw_keycode] = action;
        out_keycode = 0;
        return false;
    }
    if (IS_ACTION_TG(action)) {
        uint8_t target_l = ACTION_LAYER_NUM(action);
        toggle_layer_mask_ ^= (1UL << target_l);
        active_translation_[dev_idx][raw_keycode] = action;
        out_keycode = 0;
        return false;
    }
    if (IS_ACTION_TO(action)) {
        uint8_t target_l = ACTION_LAYER_NUM(action);
        default_layer_ = target_l;
        toggle_layer_mask_ = 0;
        momentary_layer_mask_ = 0;
        active_translation_[dev_idx][raw_keycode] = action;
        out_keycode = 0;
        return false;
    }

    // Store mapped action so release matches
    active_translation_[dev_idx][raw_keycode] = action;
    out_keycode = action;
    return true;
}

bool VirtualMatrix::processKeyRelease(uint8_t dev_idx, uint8_t raw_keycode, uint16_t &out_keycode) {
    if (dev_idx >= MAX_KEYBOARDS) return false;

    uint16_t original_action = active_translation_[dev_idx][raw_keycode];
    active_translation_[dev_idx][raw_keycode] = 0;

    if (IS_ACTION_MO(original_action)) {
        uint8_t target_l = ACTION_LAYER_NUM(original_action);
        momentary_layer_mask_ &= ~(1UL << target_l);
        out_keycode = 0;
        return false;
    }
    if (IS_ACTION_TG(original_action) || IS_ACTION_TO(original_action)) {
        out_keycode = 0;
        return false;
    }

    if (original_action != 0) {
        out_keycode = original_action;
        return true;
    }

    // Fallback if not tracked
    out_keycode = raw_keycode;
    return true;
}

void VirtualMatrix::purgeDevice(uint8_t dev_idx) {
    if (dev_idx >= MAX_KEYBOARDS) return;
    for (int k = 0; k < 256; k++) {
        uint16_t action = active_translation_[dev_idx][k];
        if (IS_ACTION_MO(action)) {
            uint8_t target_l = ACTION_LAYER_NUM(action);
            momentary_layer_mask_ &= ~(1UL << target_l);
        }
        active_translation_[dev_idx][k] = 0;
    }
}

uint16_t VirtualMatrix::getKeycode(uint8_t layer, uint8_t row, uint8_t col) {
    if (layer >= NUM_LAYERS || row >= MATRIX_ROWS || col >= MATRIX_COLS) return 0;
    return keymap_[layer][row][col];
}

void VirtualMatrix::setKeycode(uint8_t layer, uint8_t row, uint8_t col, uint16_t keycode) {
    if (layer >= NUM_LAYERS || row >= MATRIX_ROWS || col >= MATRIX_COLS) return;
    keymap_[layer][row][col] = keycode;
    StorageManager::saveKey(layer, row, col, keycode);
}

void VirtualMatrix::resetKeymap() {
    StorageManager::resetToDefaults(keymap_);
    StorageManager::saveKeymap(keymap_);
    default_layer_ = 0;
    momentary_layer_mask_ = 0;
    toggle_layer_mask_ = 0;
}
