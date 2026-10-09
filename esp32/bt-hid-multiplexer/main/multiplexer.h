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
    // Sends the current keyboard and mouse state again, even if it has not changed, on the next
    // flush. Used when the host may have missed reports (a failed transfer, a resume or a mount), so
    // that a release it missed cannot leave a key or button held on the host.
    static void resendState();

    // Continues a macro that is waiting out a delay and repeats the held mouse keys; call every
    // ~10 ms.
    static void poll();
    static bool macroRunning();

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
    static bool mouse_resend_;
    // The mouse buttons held as of the last flushMouse(), and the ones pressed since the last mouse
    // report was sent. A press is reported even when the button was released again before the
    // report could go out (the endpoint is busy for a frame after every report), so that a quick
    // click is not lost.
    static uint8_t held_mouse_buttons_;
    static uint8_t unsent_button_presses_;

    // Mouse keys (keys mapped to KC_MS_*, KC_WH_* and KC_ACL*), as QMK's accelerated mode: a press
    // moves once at once, and after a delay the movement repeats, faster the longer it is held.
    enum : uint16_t {
        MK_UP = 1 << 0, MK_DOWN = 1 << 1, MK_LEFT = 1 << 2, MK_RIGHT = 1 << 3,
        MK_WH_UP = 1 << 4, MK_WH_DOWN = 1 << 5, MK_WH_LEFT = 1 << 6, MK_WH_RIGHT = 1 << 7,
        MK_ACL0 = 1 << 8, MK_ACL1 = 1 << 9, MK_ACL2 = 1 << 10,
        MK_MOVE_MASK = MK_UP | MK_DOWN | MK_LEFT | MK_RIGHT,
        MK_WHEEL_MASK = MK_WH_UP | MK_WH_DOWN | MK_WH_LEFT | MK_WH_RIGHT,
    };
    static uint16_t mk_held_;          // MK_* bits of the mouse keys held
    static uint8_t mk_repeat_;         // movement repeats so far (QMK's mousekey_repeat)
    static uint8_t mk_wheel_repeat_;
    static uint32_t mk_move_due_ms_;   // when the next movement repeat is due
    static uint32_t mk_wheel_due_ms_;
    // Picks up mouse keys and buttons that were pressed or released since the last call.
    static void updateMouseKeys();
    // Repeats the held mouse keys whose time has come.
    static void pollMouseKeys();
    // Adds one step of movement / scrolling in the directions dirs (MK_* bits).
    static void stepMouseKeyMove(uint16_t dirs, bool repeat);
    static void stepMouseKeyWheel(uint16_t dirs, bool repeat);

    // Macro playback (VIAL macros M0..): a macro's steps run one at a time, each followed by a
    // keyboard report, so the host sees every press and release. Taps go through the tap queue;
    // keys a macro holds down are in macro_held_ until it releases them or ends.
    static const uint8_t MACRO_MAX_HELD = 6;
    static const uint8_t *macro_pos_;   // next step, or nullptr when no macro runs
    static const uint8_t *macro_end_;
    static uint32_t macro_wait_until_ms_;
    static uint16_t macro_held_[MACRO_MAX_HELD];
    static uint8_t macro_held_count_;
    static void startMacro(uint8_t index);
    static void runMacroStep();
    static void endMacro();
    // Presses a virtual key and starts the macro it is mapped to, if it is a macro key.
    static void pressVkey(uint8_t dev_idx, uint8_t vkey);

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
        uint16_t mouse_keys;  // MK_* bits
    };
    static void collectOutputs(OutputState &out);
    static void addAction(OutputState &out, uint16_t action);
    // Send one axis of mouse motion to whatever its virtual key is currently mapped to.
    static void routeMotion(uint8_t dev_idx, int32_t value, uint8_t vkey_positive, uint8_t vkey_negative, bool wheel_units);
};

#endif // MULTIPLEXER_H_
