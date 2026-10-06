#include "virtual_matrix.h"
#include "storage.h"
#include "device_bindings.h"
#include "platform.h"
#include <string.h>

uint16_t VirtualMatrix::keymap_[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS];
uint16_t VirtualMatrix::active_translation_[MAX_KEYBOARDS][256];
VirtualMatrix::LayerState VirtualMatrix::layer_state_[NUM_LAYERS];
uint8_t VirtualMatrix::layer_key_group_[MAX_KEYBOARDS][256];
bool VirtualMatrix::save_pending_ = false;
uint32_t VirtualMatrix::last_edit_ms_ = 0;

// Stored in active_translation_ for a held key that is mapped to KC_NO, so that "held, disabled"
// can be told apart from "not held" (0).
static const uint16_t HELD_KC_NO = 0xFFFF;

// How long keymap edits must be quiet before they are written to flash.
static const uint32_t SAVE_DELAY_MS = 500;

void VirtualMatrix::init() {
    StorageManager::init();
    bool needs_save = false;
    if (!StorageManager::loadKeymap(keymap_, needs_save)) {
        resetKeymap();
    } else if (needs_save) {
        StorageManager::saveKeymap(keymap_);
    }

    memset(layer_state_, 0, sizeof(layer_state_));
    memset(active_translation_, 0, sizeof(active_translation_));
    memset(layer_key_group_, 0, sizeof(layer_key_group_));
}

uint8_t VirtualMatrix::groupOf(uint8_t dev_idx) {
    uint8_t layer = DeviceBindings::layerFor(dev_idx);
    return (layer == DeviceBindings::NO_LAYER) ? 0 : layer;
}

uint8_t VirtualMatrix::computeActiveLayer(uint8_t group) {
    const LayerState &st = layer_state_[group];
    for (int l = NUM_LAYERS - 1; l >= 0; l--) {
        if (st.momentary_mask & (1UL << l)) {
            return (uint8_t)l;
        }
    }
    for (int l = NUM_LAYERS - 1; l >= 0; l--) {
        if (st.toggle_mask & (1UL << l)) {
            return (uint8_t)l;
        }
    }
    return st.default_layer;
}

uint8_t VirtualMatrix::getActiveLayer() {
    return computeActiveLayer(0);
}

uint16_t VirtualMatrix::resolveAction(uint8_t dev_idx, uint8_t raw_keycode) {
    uint8_t row = raw_keycode / MATRIX_COLS;
    uint8_t col = raw_keycode % MATRIX_COLS;
    uint8_t group = groupOf(dev_idx);
    const LayerState &st = layer_state_[group];

    // Transparent entries fall through the layers that are switched on in the device's group, as in
    // QMK, rather than through every lower layer: layers nobody selected (such as the layers of
    // other bound devices) must not leak into the lookup. The order is the active layer, the other
    // switched-on layers from the highest down, the device's own layer if it is bound, then layer 0.
    // Layer 0 must come last even while it is the active layer.
    uint8_t order[NUM_LAYERS + 2];
    int n = 0;
    uint8_t active = computeActiveLayer(group);
    if (active != 0) order[n++] = active;
    uint32_t enabled = st.momentary_mask | st.toggle_mask | (1UL << st.default_layer);
    for (int l = NUM_LAYERS - 1; l > 0; l--) {
        if (enabled & (1UL << l)) order[n++] = (uint8_t)l;
    }
    if (group != 0) order[n++] = group;
    order[n++] = 0;

    uint32_t visited = 0;
    for (int i = 0; i < n; i++) {
        if (visited & (1UL << order[i])) continue;
        visited |= (1UL << order[i]);
        uint16_t action = keymap_[order[i]][row][col];
        if (action != KC_TRNS_) return action;
    }
    return KC_NO_;
}

uint8_t VirtualMatrix::getEffectiveLayer(uint8_t dev_idx) {
    // The same precedence as resolveAction().
    uint8_t group = (dev_idx < MAX_KEYBOARDS) ? groupOf(dev_idx) : 0;
    uint8_t active = computeActiveLayer(group);
    return (active != 0) ? active : group;
}

bool VirtualMatrix::processKeyPress(uint8_t dev_idx, uint8_t raw_keycode, uint16_t &out_keycode) {
    if (dev_idx >= MAX_KEYBOARDS) return false;

    // A key that is already held keeps what it was translated to. Some keyboards report a held
    // modifier both as a modifier bit and in the key array, or list a key twice; both are the same
    // virtual key. Translating it again (with the layer its first press may have switched on) would
    // overwrite the stored action, and the release would then not undo a layer key.
    if (active_translation_[dev_idx][raw_keycode] != 0) {
        out_keycode = 0;
        return false;
    }

    uint16_t action = resolveAction(dev_idx, raw_keycode);

    // Handle Layer Switch Actions
    if (IS_ACTION_MO(action) || IS_ACTION_TG(action) || IS_ACTION_TO(action)) {
        uint8_t group = groupOf(dev_idx);
        LayerState &st = layer_state_[group];
        uint8_t target_l = ACTION_LAYER_NUM(action);
        if (target_l < NUM_LAYERS) {
            if (IS_ACTION_MO(action)) {
                st.momentary_mask |= (1UL << target_l);
            } else if (IS_ACTION_TG(action)) {
                st.toggle_mask ^= (1UL << target_l);
            } else {
                st.default_layer = target_l;
                st.toggle_mask = 0;
                st.momentary_mask = 0;
            }
        }
        active_translation_[dev_idx][raw_keycode] = action;
        layer_key_group_[dev_idx][raw_keycode] = group;
        out_keycode = 0;
        return false;
    }

    // Store mapped action so release matches
    active_translation_[dev_idx][raw_keycode] = (action != 0) ? action : HELD_KC_NO;
    out_keycode = action;
    return action != 0;
}

bool VirtualMatrix::processKeyRelease(uint8_t dev_idx, uint8_t raw_keycode, uint16_t &out_keycode) {
    if (dev_idx >= MAX_KEYBOARDS) return false;

    uint16_t original_action = active_translation_[dev_idx][raw_keycode];
    active_translation_[dev_idx][raw_keycode] = 0;

    if (IS_ACTION_MO(original_action)) {
        uint8_t target_l = ACTION_LAYER_NUM(original_action);
        if (target_l < NUM_LAYERS) {
            layer_state_[layer_key_group_[dev_idx][raw_keycode]].momentary_mask &= ~(1UL << target_l);
        }
        out_keycode = 0;
        return false;
    }
    if (IS_ACTION_TG(original_action) || IS_ACTION_TO(original_action)) {
        out_keycode = 0;
        return false;
    }

    out_keycode = (original_action == HELD_KC_NO) ? 0 : original_action;
    return out_keycode != 0;
}

uint16_t VirtualMatrix::getActiveTranslation(uint8_t dev_idx, uint8_t raw_keycode) {
    if (dev_idx >= MAX_KEYBOARDS) return 0;
    uint16_t action = active_translation_[dev_idx][raw_keycode];
    return (action == HELD_KC_NO) ? 0 : action;
}

void VirtualMatrix::purgeDevice(uint8_t dev_idx) {
    if (dev_idx >= MAX_KEYBOARDS) return;
    for (int k = 0; k < 256; k++) {
        uint16_t action = active_translation_[dev_idx][k];
        if (action != 0) {
            if (IS_ACTION_MO(action)) {
                uint8_t target_l = ACTION_LAYER_NUM(action);
                if (target_l < NUM_LAYERS) {
                    layer_state_[layer_key_group_[dev_idx][k]].momentary_mask &= ~(1UL << target_l);
                }
            }
            active_translation_[dev_idx][k] = 0;
        }
    }
}

uint16_t VirtualMatrix::getKeycode(uint8_t layer, uint8_t row, uint8_t col) {
    if (layer >= NUM_LAYERS || row >= MATRIX_ROWS || col >= MATRIX_COLS) return 0;
    return keymap_[layer][row][col];
}

void VirtualMatrix::setKeycode(uint8_t layer, uint8_t row, uint8_t col, uint16_t keycode) {
    if (layer >= NUM_LAYERS || row >= MATRIX_ROWS || col >= MATRIX_COLS) return;
    keymap_[layer][row][col] = keycode;
    save_pending_ = true;
    last_edit_ms_ = platform_now_ms();
}

void VirtualMatrix::flushPendingSave() {
    if (!save_pending_) return;
    if (platform_now_ms() - last_edit_ms_ < SAVE_DELAY_MS) return;
    save_pending_ = false;
    StorageManager::saveKeymap(keymap_);
}

void VirtualMatrix::resetKeymap() {
    // Upper layers are fully transparent.
    for (uint8_t l = 1; l < NUM_LAYERS; l++) {
        for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
            for (uint8_t c = 0; c < MATRIX_COLS; c++) {
                keymap_[l][r][c] = KC_TRNS_;
            }
        }
    }
    // Layer 0 is a 1:1 passthrough of the HID usage, except for the mouse virtual keys, which map
    // to the QMK mouse keycodes that reproduce the physical behaviour.
    for (int vkey = 0; vkey < 256; vkey++) {
        uint16_t kc = (vkey < 4) ? KC_NO_ : (uint16_t)vkey;  // usages 1-3 are not real keys
        if (vkey >= VKEY_MOUSE_BTN_BASE && vkey < VKEY_MOUSE_BTN_BASE + MOUSE_OUTPUT_BTN_COUNT) {
            kc = KC_BTN1_ + (vkey - VKEY_MOUSE_BTN_BASE);
        } else if (vkey >= VKEY_MOUSE_BTN_BASE + MOUSE_OUTPUT_BTN_COUNT && vkey < VKEY_MOTION_BASE) {
            // Mouse buttons 6-8 cannot be sent to the host (no VIAL keycode, 5-button USB mouse), so
            // they default to the otherwise unused F13-F15.
            kc = KC_F13_ + (vkey - VKEY_MOUSE_BTN_BASE - MOUSE_OUTPUT_BTN_COUNT);
        } else if (vkey >= VKEY_MOTION_BASE) {
            static const uint16_t motion_kc[8] = {
                KC_MS_U_, KC_MS_D_, KC_MS_L_, KC_MS_R_, KC_WH_U_, KC_WH_D_, KC_WH_L_, KC_WH_R_,
            };
            kc = (vkey - VKEY_MOTION_BASE < 8) ? motion_kc[vkey - VKEY_MOTION_BASE] : KC_NO_;
        }
        keymap_[0][vkey / MATRIX_COLS][vkey % MATRIX_COLS] = kc;
    }
    save_pending_ = false;
    StorageManager::saveKeymap(keymap_);
}
