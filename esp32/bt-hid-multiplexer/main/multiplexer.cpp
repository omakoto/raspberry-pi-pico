#include "multiplexer.h"
#include "virtual_matrix.h"
#include "device_bindings.h"
#include "usb_descriptors.h"
#include "macros.h"
#include "platform.h"
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
uint16_t Multiplexer::tap_queue_[Multiplexer::TAP_QUEUE_SIZE];
uint8_t Multiplexer::tap_head_ = 0;
uint8_t Multiplexer::tap_count_ = 0;
uint16_t Multiplexer::tap_active_ = 0;
bool Multiplexer::tap_pressed_sent_ = false;
int32_t Multiplexer::tap_remainder_[8];
uint8_t Multiplexer::merged_mouse_buttons_ = 0;
bool Multiplexer::mouse_resend_ = false;
const uint8_t *Multiplexer::macro_pos_ = nullptr;
const uint8_t *Multiplexer::macro_end_ = nullptr;
uint32_t Multiplexer::macro_wait_until_ms_ = 0;
uint16_t Multiplexer::macro_held_[Multiplexer::MACRO_MAX_HELD];
uint8_t Multiplexer::macro_held_count_ = 0;

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
    tap_head_ = 0;
    tap_count_ = 0;
    tap_active_ = 0;
    tap_pressed_sent_ = false;
    memset(tap_remainder_, 0, sizeof(tap_remainder_));
    merged_mouse_buttons_ = 0;
    mouse_resend_ = false;
    macro_pos_ = nullptr;
    macro_end_ = nullptr;
    macro_held_count_ = 0;
    host_leds_ = 0;
    last_synced_leds_ = 0xFF;
    kbd_dirty_ = false;
}

void Multiplexer::handleKeyboardReport(uint8_t dev_idx, uint8_t modifiers, const uint8_t *keys, uint8_t key_count) {
    if (dev_idx >= MAX_KEYBOARDS) return;

    keyboards_[dev_idx].connected = true;
    DeviceBindings::noteActivity(dev_idx);
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
            pressVkey(dev_idx, VKEY_MODIFIER_BASE + bit);
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
            pressVkey(dev_idx, new_k);
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
    DeviceBindings::deviceChanged(dev_idx);
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

    // Consumer volume keys are sent as the keyboard-page volume usages.
    if (action == KC_MUTE_) action = 0x7F;
    else if (action == KC_VOLU_) action = 0x80;
    else if (action == KC_VOLD_) action = 0x81;

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
    } else if (kc >= KC_SPECIAL_FIRST_) {
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

    if (tap_active_ != 0) {
        addAction(out, tap_active_);
    }
    for (uint8_t i = 0; i < macro_held_count_; i++) {
        addAction(out, macro_held_[i]);
    }
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
    if (!tud_hid_n_ready(0)) {
        return;
    }
    // A running macro moves on once the previous step's reports have gone out.
    if (macro_pos_ && tap_active_ == 0 && tap_count_ == 0 && !tap_pressed_sent_) {
        runMacroStep();
    }
    if (!kbd_dirty_) {
        return;
    }

    // A tapped key needs a report with it down and then a report with it up, even when the same key
    // is tapped again right away.
    uint16_t releasing_tap = tap_pressed_sent_ ? tap_active_ : 0;
    if (tap_pressed_sent_) {
        tap_active_ = 0;
        tap_pressed_sent_ = false;
    } else if (tap_active_ == 0 && tap_count_ > 0) {
        tap_active_ = tap_queue_[tap_head_];
        tap_head_ = (tap_head_ + 1) % TAP_QUEUE_SIZE;
        tap_count_--;
    }

    OutputState out;
    collectOutputs(out);

    uint8_t report[8];
    report[0] = out.mods;
    report[1] = 0x00; // Reserved
    memcpy(&report[2], out.keys, 6);

    if (!tud_hid_n_report(0, REPORT_ID_KEYBOARD, report, sizeof(report))) {
        // Not sent: stay dirty, and keep a tap's release pending, so that this report goes out on
        // the next flush. Dropping it could leave a key held on the host.
        if (releasing_tap != 0) {
            tap_active_ = releasing_tap;
            tap_pressed_sent_ = true;
        }
        kbd_dirty_ = true;
        return;
    }
    if (tap_active_ != 0) {
        tap_pressed_sent_ = true;
    }
    // Stay dirty until the pending release / the remaining taps have gone out.
    kbd_dirty_ = (tap_active_ != 0) || (tap_count_ > 0);
}

void Multiplexer::enqueueTap(uint16_t action) {
    if (tap_count_ >= TAP_QUEUE_SIZE) return;  // Fast movement: drop the excess taps.
    tap_queue_[(tap_head_ + tap_count_) % TAP_QUEUE_SIZE] = action;
    tap_count_++;
    kbd_dirty_ = true;
}

void Multiplexer::routeMotion(uint8_t dev_idx, int32_t value, uint8_t vkey_positive, uint8_t vkey_negative, bool wheel_units) {
    if (value == 0) return;
    uint8_t vkey = (value > 0) ? vkey_positive : vkey_negative;
    uint16_t action = VirtualMatrix::resolveAction(dev_idx, vkey);
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
            // A key (or modifier/volume key): tap it once per wheel notch worth of movement. Mouse
            // buttons, layer actions and KC_NO are not tappable, so the movement is dropped.
            if (action != 0 && action < 0x2000 && !(action >= KC_BTN1_ && action <= KC_BTN5_)) {
                uint8_t index = vkey - VKEY_MOTION_BASE;
                tap_remainder_[index ^ 1] = 0;  // Reversing direction starts over.
                tap_remainder_[index] += counts;
                while (tap_remainder_[index] >= MOUSE_COUNTS_PER_WHEEL_NOTCH) {
                    tap_remainder_[index] -= MOUSE_COUNTS_PER_WHEEL_NOTCH;
                    enqueueTap(action);
                }
            }
            break;
    }
}

void Multiplexer::handleMouseReport(uint8_t dev_idx, uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan) {
    if (dev_idx >= MAX_MICE) return;

    mice_[dev_idx].connected = true;
    // Only real input counts as activity, not empty reports.
    if (buttons != mice_[dev_idx].buttons || dx != 0 || dy != 0 || wheel != 0 || pan != 0) {
        DeviceBindings::noteActivity(dev_idx);
    }
    uint8_t mask = (uint8_t)((1u << VKEY_MOUSE_BTN_COUNT) - 1);
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
            pressVkey(dev_idx, VKEY_MOUSE_BTN_BASE + b);
        }
    }

    routeMotion(dev_idx, dx, VKEY_MOTION_RIGHT, VKEY_MOTION_LEFT, false);
    routeMotion(dev_idx, dy, VKEY_MOTION_DOWN, VKEY_MOTION_UP, false);
    routeMotion(dev_idx, wheel, VKEY_WHEEL_UP, VKEY_WHEEL_DOWN, true);
    routeMotion(dev_idx, pan, VKEY_WHEEL_RIGHT, VKEY_WHEEL_LEFT, true);

    if (changed) {
        // A button may have been remapped to a key or modifier.
        kbd_dirty_ = true;
    }
    flushKeyboard();
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
    DeviceBindings::deviceChanged(dev_idx);
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

    if (accum_dx_ == 0 && accum_dy_ == 0 && accum_wheel_ == 0 && accum_pan_ == 0 &&
        merged_buttons == merged_mouse_buttons_ && !mouse_resend_) {
        return;
    }

    // Clamp delta movement to signed 8-bit limits (-127 to 127) for the standard USB mouse report
    int8_t report_dx = (accum_dx_ > 127) ? 127 : ((accum_dx_ < -127) ? -127 : (int8_t)accum_dx_);
    int8_t report_dy = (accum_dy_ > 127) ? 127 : ((accum_dy_ < -127) ? -127 : (int8_t)accum_dy_);
    int8_t report_wheel = (accum_wheel_ > 127) ? 127 : ((accum_wheel_ < -127) ? -127 : (int8_t)accum_wheel_);
    int8_t report_pan = (accum_pan_ > 127) ? 127 : ((accum_pan_ < -127) ? -127 : (int8_t)accum_pan_);

    // Transmit standard 5-byte mouse report (buttons, dx, dy, wheel, pan) matching descriptor. The
    // state only counts as sent once the report is accepted, so that a report that could not be
    // queued (e.g. a button release) goes out on the next flush.
    if (!tud_hid_n_mouse_report(0, REPORT_ID_MOUSE, merged_buttons, report_dx, report_dy, report_wheel, report_pan)) {
        return;
    }
    accum_dx_ -= report_dx;
    accum_dy_ -= report_dy;
    accum_wheel_ -= report_wheel;
    accum_pan_ -= report_pan;
    merged_mouse_buttons_ = merged_buttons;
    mouse_resend_ = false;
}

void Multiplexer::resendState() {
    kbd_dirty_ = true;
    mouse_resend_ = true;
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

// ---------------------------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------------------------

// VIA macro step encoding (QMK send_string): plain bytes are text to type; SS_QMK_PREFIX starts an
// action: tap/down/up with an 8-bit keycode, the same with a 16-bit keycode, or a delay.
static const uint8_t SS_QMK_PREFIX = 1;
static const uint8_t SS_TAP_CODE = 1;
static const uint8_t SS_DOWN_CODE = 2;
static const uint8_t SS_UP_CODE = 3;
static const uint8_t SS_DELAY_CODE = 4;
static const uint8_t VIAL_MACRO_EXT_TAP = 5;   // 5..7 = tap/down/up with a 16-bit keycode
static const uint8_t VIAL_MACRO_EXT_UP = 7;

// Keycode that types an ASCII character on a US layout (LSFT(kc) for shifted ones), or 0.
static uint16_t ascii_to_keycode(uint8_t c) {
    static const uint16_t SHIFT = 0x0200;
    if (c >= 'a' && c <= 'z') return 0x04 + (c - 'a');
    if (c >= 'A' && c <= 'Z') return SHIFT | (0x04 + (c - 'A'));
    if (c >= '1' && c <= '9') return 0x1E + (c - '1');
    switch (c) {
        case '0': return 0x27;
        case '\n': return 0x28;  // Enter
        case '\t': return 0x2B;
        case '\b': return 0x2A;
        case ' ': return 0x2C;
        case '-': return 0x2D;
        case '=': return 0x2E;
        case '[': return 0x2F;
        case ']': return 0x30;
        case '\\': return 0x31;
        case ';': return 0x33;
        case '\'': return 0x34;
        case '`': return 0x35;
        case ',': return 0x36;
        case '.': return 0x37;
        case '/': return 0x38;
        case '!': return SHIFT | 0x1E;
        case '@': return SHIFT | 0x1F;
        case '#': return SHIFT | 0x20;
        case '$': return SHIFT | 0x21;
        case '%': return SHIFT | 0x22;
        case '^': return SHIFT | 0x23;
        case '&': return SHIFT | 0x24;
        case '*': return SHIFT | 0x25;
        case '(': return SHIFT | 0x26;
        case ')': return SHIFT | 0x27;
        case '_': return SHIFT | 0x2D;
        case '+': return SHIFT | 0x2E;
        case '{': return SHIFT | 0x2F;
        case '}': return SHIFT | 0x30;
        case '|': return SHIFT | 0x31;
        case ':': return SHIFT | 0x33;
        case '"': return SHIFT | 0x34;
        case '~': return SHIFT | 0x35;
        case '<': return SHIFT | 0x36;
        case '>': return SHIFT | 0x37;
        case '?': return SHIFT | 0x38;
        default: return 0;  // other control characters and UTF-8 bytes are not typed
    }
}

void Multiplexer::pressVkey(uint8_t dev_idx, uint8_t vkey) {
    uint16_t action = 0;
    VirtualMatrix::processKeyPress(dev_idx, vkey, action);
    if (IS_MACRO_KEYCODE(action)) {
        startMacro((uint8_t)(action - KC_MACRO_FIRST_));
    }
}

bool Multiplexer::macroRunning() {
    return macro_pos_ != nullptr;
}

void Multiplexer::startMacro(uint8_t index) {
    if (macro_pos_) return;  // one macro at a time; a second macro key is ignored meanwhile
    const uint8_t *start, *end;
    if (!MacroStore::find(index, &start, &end) || start == end) return;
    macro_pos_ = start;
    macro_end_ = end;
    macro_wait_until_ms_ = 0;
    macro_held_count_ = 0;
    kbd_dirty_ = true;
}

void Multiplexer::endMacro() {
    macro_pos_ = nullptr;
    // Keys the macro left down are released, so that a macro cannot leave a key stuck.
    if (macro_held_count_ > 0) {
        macro_held_count_ = 0;
        kbd_dirty_ = true;
    }
}

void Multiplexer::runMacroStep() {
    if ((int32_t)(macro_wait_until_ms_ - platform_now_ms()) > 0) return;
    if (macro_pos_ >= macro_end_) {
        endMacro();
        return;
    }
    const uint8_t *p = macro_pos_;
    if (p[0] != SS_QMK_PREFIX) {
        macro_pos_ = p + 1;
        uint16_t kc = ascii_to_keycode(p[0]);
        if (kc != 0) enqueueTap(kc);
        else kbd_dirty_ = true;  // nothing to type; keep going on the next flush
        return;
    }
    if (macro_end_ - p < 2) {
        endMacro();
        return;
    }
    uint8_t act = p[1];
    uint16_t kc = 0;
    if (act >= SS_TAP_CODE && act <= SS_UP_CODE) {
        if (macro_end_ - p < 3) { endMacro(); return; }
        kc = p[2];
        macro_pos_ = p + 3;
    } else if (act >= VIAL_MACRO_EXT_TAP && act <= VIAL_MACRO_EXT_UP) {
        if (macro_end_ - p < 4) { endMacro(); return; }
        kc = (uint16_t)(p[2] | (p[3] << 8));
        if (kc > 0xFF00) kc = (uint16_t)((kc & 0xFF) << 8);  // a keycode whose low byte is 0
        act = (uint8_t)(act - (VIAL_MACRO_EXT_TAP - SS_TAP_CODE));
        macro_pos_ = p + 4;
    } else if (act == SS_DELAY_CODE) {
        if (macro_end_ - p < 4) { endMacro(); return; }
        uint32_t delay_ms = (uint32_t)(p[2] - 1) + (uint32_t)(p[3] - 1) * 255;
        macro_wait_until_ms_ = platform_now_ms() + delay_ms;
        macro_pos_ = p + 4;
        kbd_dirty_ = true;
        return;
    } else {
        macro_pos_ = p + 2;  // malformed; skip it
        kbd_dirty_ = true;
        return;
    }

    if (act == SS_TAP_CODE) {
        enqueueTap(kc);
    } else if (act == SS_DOWN_CODE) {
        if (macro_held_count_ < MACRO_MAX_HELD) macro_held_[macro_held_count_++] = kc;
        kbd_dirty_ = true;
    } else {  // SS_UP_CODE
        for (uint8_t i = 0; i < macro_held_count_; i++) {
            if (macro_held_[i] == kc) {
                macro_held_[i] = macro_held_[--macro_held_count_];
                break;
            }
        }
        kbd_dirty_ = true;
    }
}

void Multiplexer::poll() {
    if (macro_pos_) {
        flushKeyboard();
    }
}
