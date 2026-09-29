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
    static int8_t  accum_wheel_;
    static int8_t  accum_pan_;
    static uint8_t merged_mouse_buttons_;

    // Host LED state
    static uint8_t host_leds_;
    static uint8_t last_synced_leds_;
    static bool kbd_dirty_;
};

#endif // MULTIPLEXER_H_
