#ifndef MULTIPLEXER_H_
#define MULTIPLEXER_H_

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

class Multiplexer {
public:
    static void init();

    // Ingest events from BLE keyboards
    static void handleKeyboardReport(uint8_t dev_idx, uint8_t modifiers, const uint8_t *keys, uint8_t key_count);
    static void purgeKeyboard(uint8_t dev_idx);

    // Ingest events from BLE mice
    static void handleMouseReport(uint8_t dev_idx, uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan = 0);
    static void purgeMouse(uint8_t dev_idx);

    // Rate-decoupled flush to USB HID host
    static void flushMouse();
    static void flushKeyboard();

    // Reverse LED synchronization (Caps Lock / Num Lock from PC)
    static void setHostLeds(uint8_t leds);
    static uint8_t getHostLeds();
    static bool hasLedsChanged();
    static void acknowledgeLeds();

private:
    struct KeyboardDeviceState {
        uint8_t modifiers;
        uint8_t keys[6];
        bool connected;
    };

    struct MouseDeviceState {
        uint8_t buttons;
        bool connected;
    };

    static KeyboardDeviceState keyboards_[MAX_KEYBOARDS];
    static MouseDeviceState mice_[MAX_MICE];

    // Aggregated mouse accumulation
    static int32_t accum_dx_;
    static int32_t accum_dy_;
    static int32_t accum_wheel_;
    static int32_t accum_pan_;
    // Motion counts not yet converted to a whole wheel notch when a motion direction is remapped to
    // a wheel keycode (and vice versa).
    static int32_t wheel_remainder_;
    static int32_t pan_remainder_;

    // Keys tapped by mouse movement/wheel that is mapped to a key: each is sent as a press report
    // followed by a release report, one at a time.
    static const uint8_t TAP_QUEUE_SIZE = 16;
    static uint16_t tap_queue_[TAP_QUEUE_SIZE];
    static uint8_t tap_head_;
    static uint8_t tap_count_;
    static uint16_t tap_active_;      // keycode currently held down by a tap, or 0
    static bool tap_pressed_sent_;    // the report with tap_active_ down has been sent
    // Motion counts accumulated towards the next tap, per motion virtual key.
    static int32_t tap_remainder_[8];
    static void enqueueTap(uint16_t action);
    static uint8_t merged_mouse_buttons_;

    // Host LED state
    static uint8_t host_leds_;
    static uint8_t last_synced_leds_;
    static bool kbd_dirty_;

    // Everything the currently held keys, modifiers and mouse buttons of all devices translate to.
    struct OutputState {
        uint8_t mods;
        uint8_t keys[6];
        uint8_t key_count;
        uint8_t mouse_buttons;
    };
    static void collectOutputs(OutputState &out);
    static void addAction(OutputState &out, uint16_t action);
    // Send one axis of mouse motion to whatever its virtual key is currently mapped to.
    static void routeMotion(uint8_t dev_idx, int32_t value, uint8_t vkey_positive, uint8_t vkey_negative, bool wheel_units);
};

#endif // MULTIPLEXER_H_
