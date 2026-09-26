#ifndef MULTIPLEXER_H_
#define MULTIPLEXER_H_

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

class Multiplexer {
public:
    static void init();

    // Keyboard handling
    static void onRawKeyPress(uint8_t dev_idx, uint8_t raw_keycode);
    static void onRawKeyRelease(uint8_t dev_idx, uint8_t raw_keycode);
    static void onExtrasPress(uint8_t dev_idx, uint32_t top, uint16_t key);
    static void onExtrasRelease(uint8_t dev_idx, uint32_t top, uint16_t key);
    static void purgeKeyboard(uint8_t dev_idx);

    // Mouse handling
    static void onMouseMove(uint8_t dev_idx, int dx, int dy, int wheel, int wheelH);
    static void onMouseButtons(uint8_t dev_idx, uint8_t buttons);
    static void purgeMouse(uint8_t dev_idx);
    static void flushMouse();

    // LED status synchronization
    static uint8_t getHostLeds();
    static bool hasLedsChanged();
    static void acknowledgeLeds();

private:
    // Keyboard state
    static uint8_t dev_modifiers_[MAX_KEYBOARDS];
    static uint8_t dev_keys_[MAX_KEYBOARDS][6];
    static uint8_t last_sent_modifiers_;
    static uint8_t last_sent_keys_[6];
    static uint8_t last_synced_leds_;

    // Mouse state
    static uint8_t dev_mouse_buttons_[MAX_MICE];
    static uint8_t last_mouse_buttons_;
    static int32_t accum_dx_;
    static int32_t accum_dy_;
    static int32_t accum_wheel_;
    static int32_t accum_wheelH_;

    static void updateKeyboardOutput();
    static int8_t clamp8(int32_t val);
};

#endif // MULTIPLEXER_H_
