#include "multiplexer.h"
#include "virtual_matrix.h"
#include "usb_descriptors.h"
#include "tusb.h"
#include <string.h>

Multiplexer::KeyboardDeviceState Multiplexer::keyboards_[MAX_KEYBOARDS];
Multiplexer::MouseDeviceState Multiplexer::mice_[MAX_MICE];

int32_t Multiplexer::accum_dx_ = 0;
int32_t Multiplexer::accum_dy_ = 0;
int8_t  Multiplexer::accum_wheel_ = 0;
int8_t  Multiplexer::accum_pan_ = 0;
uint8_t Multiplexer::merged_mouse_buttons_ = 0;

uint8_t Multiplexer::host_leds_ = 0;
uint8_t Multiplexer::last_synced_leds_ = 0xFF;
bool Multiplexer::kbd_dirty_ = false;

void Multiplexer::init() {
    memset(keyboards_, 0, sizeof(keyboards_));
    memset(mice_, 0, sizeof(mice_));
    accum_dx_ = 0;
    accum_dy_ = 0;
    accum_wheel_ = 0;
    accum_pan_ = 0;
    merged_mouse_buttons_ = 0;
    host_leds_ = 0;
    last_synced_leds_ = 0xFF;
    kbd_dirty_ = false;
}

void Multiplexer::handleKeyboardReport(uint8_t dev_idx, uint8_t modifiers, const uint8_t *keys, uint8_t key_count) {
    if (dev_idx >= MAX_KEYBOARDS) return;

    keyboards_[dev_idx].connected = true;
    keyboards_[dev_idx].modifiers = modifiers;

    uint8_t new_keys[6] = {0};
    uint8_t count = (key_count > 6) ? 6 : key_count;
    if (keys && count > 0) {
        memcpy(new_keys, keys, count);
    }

    // 1. Detect released keys (in previous state but not in new state)
    for (int i = 0; i < 6; i++) {
        uint8_t old_k = keyboards_[dev_idx].keys[i];
        if (old_k == 0) continue;
        bool still_pressed = false;
        for (int j = 0; j < 6; j++) {
            if (new_keys[j] == old_k) {
                still_pressed = true;
                break;
            }
        }
        if (!still_pressed) {
            uint16_t out_kc = 0;
            VirtualMatrix::processKeyRelease(dev_idx, old_k, out_kc);
        }
    }

    // 2. Detect newly pressed keys (in new state but not in previous state)
    for (int i = 0; i < 6; i++) {
        uint8_t new_k = new_keys[i];
        if (new_k == 0) continue;
        bool was_pressed = false;
        for (int j = 0; j < 6; j++) {
            if (keyboards_[dev_idx].keys[j] == new_k) {
                was_pressed = true;
                break;
            }
        }
        if (!was_pressed) {
            uint16_t out_kc = 0;
            VirtualMatrix::processKeyPress(dev_idx, new_k, out_kc);
        }
    }

    memcpy(keyboards_[dev_idx].keys, new_keys, 6);
    kbd_dirty_ = true;
    flushKeyboard();
}

void Multiplexer::purgeKeyboard(uint8_t dev_idx) {
    if (dev_idx >= MAX_KEYBOARDS) return;
    VirtualMatrix::purgeDevice(dev_idx);
    memset(&keyboards_[dev_idx], 0, sizeof(KeyboardDeviceState));
    kbd_dirty_ = true;
    flushKeyboard();
}

void Multiplexer::flushKeyboard() {
    if (!kbd_dirty_) {
        return;
    }
    if (!tud_hid_n_ready(0)) {
        return;
    }

    uint8_t merged_mods = 0;
    uint8_t merged_keys[6] = {0};
    uint8_t out_idx = 0;

    for (uint8_t d = 0; d < MAX_KEYBOARDS; d++) {
        if (!keyboards_[d].connected) continue;
        merged_mods |= keyboards_[d].modifiers;

        for (int k = 0; k < 6; k++) {
            uint8_t raw = keyboards_[d].keys[k];
            if (raw == 0) continue;

            // Query active mapped key without mutating layer switch state
            uint16_t mapped = VirtualMatrix::getActiveTranslation(d, raw);
            if (mapped == 0 || mapped > 0xFF) continue; // Modifier/Layer action or transparent

            uint8_t final_kc = (uint8_t)mapped;
            // Prevent duplicate keycodes
            bool already_added = false;
            for (int e = 0; e < out_idx; e++) {
                if (merged_keys[e] == final_kc) {
                    already_added = true;
                    break;
                }
            }
            if (!already_added && out_idx < 6) {
                merged_keys[out_idx++] = final_kc;
            }
        }
    }

    uint8_t report[8];
    report[0] = merged_mods;
    report[1] = 0x00; // Reserved
    memcpy(&report[2], merged_keys, 6);

    tud_hid_n_report(0, REPORT_ID_KEYBOARD, report, sizeof(report));
    kbd_dirty_ = false;
}

void Multiplexer::handleMouseReport(uint8_t dev_idx, uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan) {
    if (dev_idx >= MAX_MICE) return;

    mice_[dev_idx].connected = true;
    mice_[dev_idx].buttons = buttons;

    accum_dx_ += dx;
    accum_dy_ += dy;
    accum_wheel_ += wheel;
    accum_pan_ += pan;

    flushMouse();
}

void Multiplexer::purgeMouse(uint8_t dev_idx) {
    if (dev_idx >= MAX_MICE) return;
    memset(&mice_[dev_idx], 0, sizeof(MouseDeviceState));
    flushMouse();
}

void Multiplexer::flushMouse() {
    if (!tud_hid_n_ready(0)) {
        return;
    }

    uint8_t merged_buttons = 0;
    for (uint8_t d = 0; d < MAX_MICE; d++) {
        if (mice_[d].connected) {
            merged_buttons |= mice_[d].buttons;
        }
    }

    if (accum_dx_ == 0 && accum_dy_ == 0 && accum_wheel_ == 0 && accum_pan_ == 0 && merged_buttons == merged_mouse_buttons_) {
        return;
    }

    // Clamp delta movement to signed 8-bit limits (-127 to 127) for the standard USB mouse report
    int8_t report_dx = (accum_dx_ > 127) ? 127 : ((accum_dx_ < -127) ? -127 : (int8_t)accum_dx_);
    int8_t report_dy = (accum_dy_ > 127) ? 127 : ((accum_dy_ < -127) ? -127 : (int8_t)accum_dy_);
    int8_t report_wheel = accum_wheel_;
    int8_t report_pan = accum_pan_;

    accum_dx_ -= report_dx;
    accum_dy_ -= report_dy;
    accum_wheel_ = 0;
    accum_pan_ = 0;
    merged_mouse_buttons_ = merged_buttons;

    // Transmit standard 5-byte mouse report (buttons, dx, dy, wheel, pan) matching descriptor
    tud_hid_n_mouse_report(0, REPORT_ID_MOUSE, merged_buttons, report_dx, report_dy, report_wheel, report_pan);
}

void Multiplexer::setHostLeds(uint8_t leds) {
    host_leds_ = leds;
}

uint8_t Multiplexer::getHostLeds() {
    return host_leds_;
}

bool Multiplexer::hasLedsChanged() {
    return (host_leds_ != last_synced_leds_);
}

void Multiplexer::acknowledgeLeds() {
    last_synced_leds_ = host_leds_;
}
