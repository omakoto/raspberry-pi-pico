#include "multiplexer.h"
#include "virtual_matrix.h"
#include "usb_descriptors.h"
#include "tusb.h"
#include <string.h>

Multiplexer::KeyboardDeviceState Multiplexer::keyboards_[MAX_KEYBOARDS];
Multiplexer::MouseDeviceState Multiplexer::mice_[MAX_MICE];

int32_t Multiplexer::accum_dx_ = 0;
int32_t Multiplexer::accum_dy_ = 0;
int32_t Multiplexer::accum_wheel_ = 0;
int32_t Multiplexer::accum_pan_ = 0;
int32_t Multiplexer::wheel_remainder_ = 0;
int32_t Multiplexer::pan_remainder_ = 0;
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
    wheel_remainder_ = 0;
    pan_remainder_ = 0;
    merged_mouse_buttons_ = 0;
    host_leds_ = 0;
    last_synced_leds_ = 0xFF;
    kbd_dirty_ = false;
}

void Multiplexer::handleKeyboardReport(uint8_t dev_idx, uint8_t modifiers, const uint8_t *keys, uint8_t key_count) {
    if (dev_idx >= MAX_KEYBOARDS) return;

    keyboards_[dev_idx].connected = true;
    uint8_t old_mods = keyboards_[dev_idx].modifiers;
    keyboards_[dev_idx].modifiers = modifiers;

    uint8_t new_keys[6] = {0};
    uint8_t count = (key_count > 6) ? 6 : key_count;
    if (keys && count > 0) {
        memcpy(new_keys, keys, count);
    }

    uint16_t out_kc = 0;

    // 1. Detect released keys (in previous state but not in new state). Each modifier bit is a
    //    virtual key of its own so that it can be remapped like any other key.
    for (int bit = 0; bit < 8; bit++) {
        if ((old_mods & ~modifiers) & (1 << bit)) {
            VirtualMatrix::processKeyRelease(dev_idx, VKEY_MODIFIER_BASE + bit, out_kc);
        }
    }
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
            VirtualMatrix::processKeyRelease(dev_idx, old_k, out_kc);
        }
    }

    // 2. Detect newly pressed keys (in new state but not in previous state)
    for (int bit = 0; bit < 8; bit++) {
        if ((modifiers & ~old_mods) & (1 << bit)) {
            VirtualMatrix::processKeyPress(dev_idx, VKEY_MODIFIER_BASE + bit, out_kc);
        }
    }
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
            VirtualMatrix::processKeyPress(dev_idx, new_k, out_kc);
        }
    }

    memcpy(keyboards_[dev_idx].keys, new_keys, 6);
    kbd_dirty_ = true;
    flushKeyboard();
    // A key may have been remapped to a mouse button.
    flushMouse();
}

void Multiplexer::purgeKeyboard(uint8_t dev_idx) {
    if (dev_idx >= MAX_KEYBOARDS) return;
    VirtualMatrix::purgeDevice(dev_idx);
    memset(&keyboards_[dev_idx], 0, sizeof(KeyboardDeviceState));
    kbd_dirty_ = true;
    flushKeyboard();
}

// Adds what one held virtual key was translated to into the USB report state.
void Multiplexer::addAction(OutputState &out, uint16_t action) {
    if (action == 0 || action >= 0x2000) return;  // KC_NO, or layer/tap-hold actions

    if (action >= KC_BTN1_ && action <= KC_BTN5_) {
        out.mouse_buttons |= (uint8_t)(1 << (action - KC_BTN1_));
        return;
    }

    uint8_t mods = 0;
    if (IS_MODS_KEYCODE(action)) {
        mods = MODS_KEYCODE_MODS(action);
    }
    uint8_t kc = (uint8_t)(action & 0xFF);
    if (action > 0xFF && kc == 0) {
        // Modifiers only, e.g. a bare LSFT().
    } else if (kc >= VKEY_MODIFIER_BASE && kc < VKEY_MODIFIER_BASE + 8) {
        mods |= (uint8_t)(1 << (kc - VKEY_MODIFIER_BASE));
        kc = 0;
    } else if (kc >= KC_SPECIAL_FIRST_ && kc <= KC_SPECIAL_LAST_) {
        return;  // Consumer/system/mouse-movement keycodes have no report here.
    }
    out.mods |= mods;

    if (kc == 0) return;
    for (int e = 0; e < out.key_count; e++) {
        if (out.keys[e] == kc) return;  // Prevent duplicate keycodes
    }
    if (out.key_count < 6) {
        out.keys[out.key_count++] = kc;
    }
}

void Multiplexer::collectOutputs(OutputState &out) {
    memset(&out, 0, sizeof(out));

    for (uint8_t d = 0; d < MAX_KEYBOARDS; d++) {
        if (!keyboards_[d].connected) continue;

        // Query active mapped keys without mutating layer switch state
        for (int bit = 0; bit < 8; bit++) {
            if (keyboards_[d].modifiers & (1 << bit)) {
                addAction(out, VirtualMatrix::getActiveTranslation(d, VKEY_MODIFIER_BASE + bit));
            }
        }
        for (int k = 0; k < 6; k++) {
            uint8_t raw = keyboards_[d].keys[k];
            if (raw != 0) {
                addAction(out, VirtualMatrix::getActiveTranslation(d, raw));
            }
        }
    }
    for (uint8_t d = 0; d < MAX_MICE; d++) {
        if (!mice_[d].connected) continue;
        for (int b = 0; b < VKEY_MOUSE_BTN_COUNT; b++) {
            if (mice_[d].buttons & (1 << b)) {
                addAction(out, VirtualMatrix::getActiveTranslation(d, VKEY_MOUSE_BTN_BASE + b));
            }
        }
    }
}

void Multiplexer::flushKeyboard() {
    if (!kbd_dirty_) {
        return;
    }
    if (!tud_hid_n_ready(0)) {
        return;
    }

    OutputState out;
    collectOutputs(out);

    uint8_t report[8];
    report[0] = out.mods;
    report[1] = 0x00; // Reserved
    memcpy(&report[2], out.keys, 6);

    tud_hid_n_report(0, REPORT_ID_KEYBOARD, report, sizeof(report));
    kbd_dirty_ = false;
}

void Multiplexer::routeMotion(int32_t value, uint8_t vkey_positive, uint8_t vkey_negative, bool wheel_units) {
    if (value == 0) return;
    uint16_t action = VirtualMatrix::resolveAction(value > 0 ? vkey_positive : vkey_negative);
    int32_t magnitude = (value > 0) ? value : -value;
    // Both kinds of source are converted to cursor counts, so any source can drive any target.
    int32_t counts = wheel_units ? magnitude * MOUSE_COUNTS_PER_WHEEL_NOTCH : magnitude;

    switch (action) {
        case KC_MS_U_: accum_dy_ -= counts; break;
        case KC_MS_D_: accum_dy_ += counts; break;
        case KC_MS_L_: accum_dx_ -= counts; break;
        case KC_MS_R_: accum_dx_ += counts; break;
        case KC_WH_U_:
        case KC_WH_D_:
            wheel_remainder_ += (action == KC_WH_U_) ? counts : -counts;
            accum_wheel_ += wheel_remainder_ / MOUSE_COUNTS_PER_WHEEL_NOTCH;
            wheel_remainder_ %= MOUSE_COUNTS_PER_WHEEL_NOTCH;
            break;
        case KC_WH_L_:
        case KC_WH_R_:
            pan_remainder_ += (action == KC_WH_R_) ? counts : -counts;
            accum_pan_ += pan_remainder_ / MOUSE_COUNTS_PER_WHEEL_NOTCH;
            pan_remainder_ %= MOUSE_COUNTS_PER_WHEEL_NOTCH;
            break;
        default:
            break;  // Disabled (KC_NO) or not a mouse keycode: the motion is dropped.
    }
}

void Multiplexer::handleMouseReport(uint8_t dev_idx, uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan) {
    if (dev_idx >= MAX_MICE) return;

    mice_[dev_idx].connected = true;
    uint8_t mask = (1 << VKEY_MOUSE_BTN_COUNT) - 1;
    uint8_t changed = (mice_[dev_idx].buttons ^ buttons) & mask;
    uint8_t old_buttons = mice_[dev_idx].buttons;
    mice_[dev_idx].buttons = buttons & mask;

    // Buttons are virtual keys, so they can be remapped and can switch layers (which then change
    // what the motion below is mapped to).
    uint16_t out_kc = 0;
    for (int b = 0; b < VKEY_MOUSE_BTN_COUNT; b++) {
        if (changed & (1 << b)) {
            if (old_buttons & (1 << b)) {
                VirtualMatrix::processKeyRelease(dev_idx, VKEY_MOUSE_BTN_BASE + b, out_kc);
            }
        }
    }
    for (int b = 0; b < VKEY_MOUSE_BTN_COUNT; b++) {
        if ((changed & (1 << b)) && (buttons & (1 << b))) {
            VirtualMatrix::processKeyPress(dev_idx, VKEY_MOUSE_BTN_BASE + b, out_kc);
        }
    }

    routeMotion(dx, VKEY_MOTION_RIGHT, VKEY_MOTION_LEFT, false);
    routeMotion(dy, VKEY_MOTION_DOWN, VKEY_MOTION_UP, false);
    routeMotion(wheel, VKEY_WHEEL_UP, VKEY_WHEEL_DOWN, true);
    routeMotion(pan, VKEY_WHEEL_RIGHT, VKEY_WHEEL_LEFT, true);

    if (changed) {
        // A button may have been remapped to a key or modifier.
        kbd_dirty_ = true;
        flushKeyboard();
    }
    flushMouse();
}

void Multiplexer::purgeMouse(uint8_t dev_idx) {
    if (dev_idx >= MAX_MICE) return;
    // Release buttons still held, so that layer switches and keys mapped from them do not stick.
    uint16_t out_kc = 0;
    for (int b = 0; b < VKEY_MOUSE_BTN_COUNT; b++) {
        if (mice_[dev_idx].buttons & (1 << b)) {
            VirtualMatrix::processKeyRelease(dev_idx, VKEY_MOUSE_BTN_BASE + b, out_kc);
        }
    }
    memset(&mice_[dev_idx], 0, sizeof(MouseDeviceState));
    kbd_dirty_ = true;
    flushKeyboard();
    flushMouse();
}

void Multiplexer::flushMouse() {
    if (!tud_hid_n_ready(0)) {
        return;
    }

    OutputState out;
    collectOutputs(out);
    uint8_t merged_buttons = out.mouse_buttons;

    if (accum_dx_ == 0 && accum_dy_ == 0 && accum_wheel_ == 0 && accum_pan_ == 0 && merged_buttons == merged_mouse_buttons_) {
        return;
    }

    // Clamp delta movement to signed 8-bit limits (-127 to 127) for the standard USB mouse report
    int8_t report_dx = (accum_dx_ > 127) ? 127 : ((accum_dx_ < -127) ? -127 : (int8_t)accum_dx_);
    int8_t report_dy = (accum_dy_ > 127) ? 127 : ((accum_dy_ < -127) ? -127 : (int8_t)accum_dy_);
    int8_t report_wheel = (accum_wheel_ > 127) ? 127 : ((accum_wheel_ < -127) ? -127 : (int8_t)accum_wheel_);
    int8_t report_pan = (accum_pan_ > 127) ? 127 : ((accum_pan_ < -127) ? -127 : (int8_t)accum_pan_);

    accum_dx_ -= report_dx;
    accum_dy_ -= report_dy;
    accum_wheel_ -= report_wheel;
    accum_pan_ -= report_pan;
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
