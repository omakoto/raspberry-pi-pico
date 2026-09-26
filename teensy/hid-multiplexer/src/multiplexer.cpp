#include "multiplexer.h"
#include "virtual_matrix.h"
#include <Arduino.h>

// Teensy core externs for USB Device Keyboard
extern "C" {
    extern volatile uint8_t keyboard_leds;
    extern uint8_t keyboard_modifier_keys;
    extern uint8_t keyboard_keys[6];
    int usb_keyboard_send(void);
}

uint8_t Multiplexer::dev_modifiers_[MAX_KEYBOARDS];
uint8_t Multiplexer::dev_keys_[MAX_KEYBOARDS][6];
uint8_t Multiplexer::last_sent_modifiers_ = 0;
uint8_t Multiplexer::last_sent_keys_[6] = {0};
uint8_t Multiplexer::last_synced_leds_ = 0xFF;

uint8_t Multiplexer::dev_mouse_buttons_[MAX_MICE];
uint8_t Multiplexer::last_mouse_buttons_ = 0;
int32_t Multiplexer::accum_dx_ = 0;
int32_t Multiplexer::accum_dy_ = 0;
int32_t Multiplexer::accum_wheel_ = 0;
int32_t Multiplexer::accum_wheelH_ = 0;

void Multiplexer::init() {
    VirtualMatrix::init();

    for (uint8_t i = 0; i < MAX_KEYBOARDS; i++) {
        dev_modifiers_[i] = 0;
        for (int k = 0; k < 6; k++) dev_keys_[i][k] = 0;
    }
    for (uint8_t i = 0; i < MAX_MICE; i++) {
        dev_mouse_buttons_[i] = 0;
    }
    last_sent_modifiers_ = 0;
    for (int k = 0; k < 6; k++) last_sent_keys_[k] = 0;
    last_synced_leds_ = 0xFF;

    last_mouse_buttons_ = 0;
    accum_dx_ = 0;
    accum_dy_ = 0;
    accum_wheel_ = 0;
    accum_wheelH_ = 0;

    Mouse.begin();
    Keyboard.begin();
}

void Multiplexer::onRawKeyPress(uint8_t dev_idx, uint8_t raw_keycode) {
    if (dev_idx >= MAX_KEYBOARDS) return;

    // Standard HID modifier keycodes: 0xE0 to 0xE7 (224 to 231)
    // Note: USBHost_t36 reports modifier raw keycodes as 103 to 110 (or 0xE0-0xE7 depending on mode)
    if (raw_keycode >= 103 && raw_keycode <= 110) {
        uint8_t bit = 1 << (raw_keycode - 103);
        dev_modifiers_[dev_idx] |= bit;
        updateKeyboardOutput();
        return;
    }
    if (raw_keycode >= 0xE0 && raw_keycode <= 0xE7) {
        uint8_t bit = 1 << (raw_keycode - 0xE0);
        dev_modifiers_[dev_idx] |= bit;
        updateKeyboardOutput();
        return;
    }

    // Process through virtual matrix & layer engine
    uint16_t mapped_action = 0;
    if (VirtualMatrix::processKeyPress(dev_idx, raw_keycode, mapped_action)) {
        if ((mapped_action & 0xFF00) == 0xE400) {
            // Media key
            Keyboard.press(mapped_action);
        } else {
            uint8_t out_kc = (uint8_t)(mapped_action & 0xFF);
            // Insert into dev_keys slot
            for (int i = 0; i < 6; i++) {
                if (dev_keys_[dev_idx][i] == 0 || dev_keys_[dev_idx][i] == out_kc) {
                    dev_keys_[dev_idx][i] = out_kc;
                    break;
                }
            }
            updateKeyboardOutput();
        }
    }
}

void Multiplexer::onRawKeyRelease(uint8_t dev_idx, uint8_t raw_keycode) {
    if (dev_idx >= MAX_KEYBOARDS) return;

    if (raw_keycode >= 103 && raw_keycode <= 110) {
        uint8_t bit = 1 << (raw_keycode - 103);
        dev_modifiers_[dev_idx] &= ~bit;
        updateKeyboardOutput();
        return;
    }
    if (raw_keycode >= 0xE0 && raw_keycode <= 0xE7) {
        uint8_t bit = 1 << (raw_keycode - 0xE0);
        dev_modifiers_[dev_idx] &= ~bit;
        updateKeyboardOutput();
        return;
    }

    uint16_t mapped_action = 0;
    if (VirtualMatrix::processKeyRelease(dev_idx, raw_keycode, mapped_action)) {
        if ((mapped_action & 0xFF00) == 0xE400) {
            Keyboard.release(mapped_action);
        } else {
            uint8_t out_kc = (uint8_t)(mapped_action & 0xFF);
            for (int i = 0; i < 6; i++) {
                if (dev_keys_[dev_idx][i] == out_kc) {
                    dev_keys_[dev_idx][i] = 0;
                }
            }
            updateKeyboardOutput();
        }
    }
}

void Multiplexer::onExtrasPress(uint8_t dev_idx, uint32_t top, uint16_t key) {
    (void)dev_idx;
    if (top == 0x000C0000) { // Consumer / Media Page
        Keyboard.press(0xE400 | key);
    }
}

void Multiplexer::onExtrasRelease(uint8_t dev_idx, uint32_t top, uint16_t key) {
    (void)dev_idx;
    if (top == 0x000C0000) {
        Keyboard.release(0xE400 | key);
    }
}

void Multiplexer::purgeKeyboard(uint8_t dev_idx) {
    if (dev_idx >= MAX_KEYBOARDS) return;
    dev_modifiers_[dev_idx] = 0;
    for (int k = 0; k < 6; k++) dev_keys_[dev_idx][k] = 0;
    VirtualMatrix::purgeDevice(dev_idx);
    updateKeyboardOutput();
}

void Multiplexer::updateKeyboardOutput() {
    // 1. Bitwise OR modifiers across all keyboards
    uint8_t combined_mod = 0;
    for (uint8_t i = 0; i < MAX_KEYBOARDS; i++) {
        combined_mod |= dev_modifiers_[i];
    }

    // 2. Set union of active keycodes across all keyboards (up to 6 slots)
    uint8_t combined_keys[6] = {0};
    int slot = 0;
    for (uint8_t d = 0; d < MAX_KEYBOARDS && slot < 6; d++) {
        for (int k = 0; k < 6 && slot < 6; k++) {
            uint8_t kc = dev_keys_[d][k];
            if (kc != 0) {
                // Check if already in combined list
                bool found = false;
                for (int s = 0; s < slot; s++) {
                    if (combined_keys[s] == kc) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    combined_keys[slot++] = kc;
                }
            }
        }
    }

    // 3. Send report to host PC if changed
    keyboard_modifier_keys = combined_mod;
    for (int i = 0; i < 6; i++) {
        keyboard_keys[i] = combined_keys[i];
    }
    usb_keyboard_send();

    last_sent_modifiers_ = combined_mod;
    for (int i = 0; i < 6; i++) last_sent_keys_[i] = combined_keys[i];
}

void Multiplexer::onMouseMove(uint8_t dev_idx, int dx, int dy, int wheel, int wheelH) {
    (void)dev_idx;
    accum_dx_ += dx;
    accum_dy_ += dy;
    accum_wheel_ += wheel;
    accum_wheelH_ += wheelH;
}

void Multiplexer::onMouseButtons(uint8_t dev_idx, uint8_t buttons) {
    if (dev_idx >= MAX_MICE) return;
    dev_mouse_buttons_[dev_idx] = buttons;
}

void Multiplexer::purgeMouse(uint8_t dev_idx) {
    if (dev_idx >= MAX_MICE) return;
    dev_mouse_buttons_[dev_idx] = 0;
    flushMouse();
}

int8_t Multiplexer::clamp8(int32_t val) {
    if (val > 127) return 127;
    if (val < -127) return -127;
    return (int8_t)val;
}

void Multiplexer::flushMouse() {
    // 1. Bitwise OR buttons
    uint8_t combined_buttons = 0;
    for (uint8_t i = 0; i < MAX_MICE; i++) {
        combined_buttons |= dev_mouse_buttons_[i];
    }

    // 2. Clamp deltas
    int8_t dx = clamp8(accum_dx_);
    int8_t dy = clamp8(accum_dy_);
    int8_t wheel = clamp8(accum_wheel_);
    int8_t wheelH = clamp8(accum_wheelH_);

    accum_dx_ -= dx;
    accum_dy_ -= dy;
    accum_wheel_ -= wheel;
    accum_wheelH_ -= wheelH;

    bool motion = (dx != 0 || dy != 0 || wheel != 0 || wheelH != 0);
    bool buttons_changed = (combined_buttons != last_mouse_buttons_);

    if (motion || buttons_changed) {
        if (buttons_changed) {
            Mouse.set_buttons(
                combined_buttons & 0x01,  // Left
                combined_buttons & 0x04,  // Middle
                combined_buttons & 0x02,  // Right
                combined_buttons & 0x08,  // Back
                combined_buttons & 0x10   // Forward
            );
            last_mouse_buttons_ = combined_buttons;
        }
        if (motion) {
            Mouse.move(dx, dy, wheel, wheelH);
        }
    }
}

uint8_t Multiplexer::getHostLeds() {
    return keyboard_leds;
}

bool Multiplexer::hasLedsChanged() {
    return (keyboard_leds != last_synced_leds_);
}

void Multiplexer::acknowledgeLeds() {
    last_synced_leds_ = keyboard_leds;
}
