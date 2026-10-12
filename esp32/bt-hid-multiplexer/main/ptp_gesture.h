#ifndef PTP_GESTURE_H_
#define PTP_GESTURE_H_

#include <stdint.h>
#include <stdbool.h>
#include "hid_descriptor.h"

// Turns a Windows Precision Touchpad's finger reports into mouse input and gestures.
//
// Precision touchpads (the ProtoArc T1 Plus, the Perixx PERIPAD-706) report where each finger is
// rather than mouse movement, and ignore requests to send their mouse report instead, so the
// multiplexer does what the host's touchpad driver would:
//   - one finger moves the pointer (with mild acceleration);
//   - a short tap is a left click, a two-finger tap a right click;
//   - pressing a clickpad down is the left button, or the right button with two or more fingers on it;
//   - two fingers moving up or down scroll (a gesture notch per SCROLL_MM_PER_NOTCH);
//   - two fingers swiping left or right, and three fingers swiping in any direction, are one gesture
//     each per swipe.
// Gestures are reported by direction (PtpGesture); BleHidHost presses the virtual key of each, which
// VIAL can remap.
//
// This file does not need BTstack, so that the host tests can run it (test/ptp_gesture_test.cpp).
// Run test/run-host-test.sh after changing this file or ptp_gesture.cpp.

enum PtpGesture : uint8_t {
    PTP_GESTURE_2F_UP = 0,  // fingers moving up (towards the top of the pad)
    PTP_GESTURE_2F_DOWN,
    PTP_GESTURE_2F_LEFT,
    PTP_GESTURE_2F_RIGHT,
    PTP_GESTURE_3F_UP,
    PTP_GESTURE_3F_DOWN,
    PTP_GESTURE_3F_LEFT,
    PTP_GESTURE_3F_RIGHT,
    PTP_GESTURE_COUNT,
};

// A tap shorter than this, moving less than TAP_MAX_MOVE_MM, is a click.
#define PTP_TAP_MAX_MS        220
#define PTP_TAP_MAX_MOVE_MM   2.0f
// Two-finger movement of this much picks scrolling (vertical) or a swipe (horizontal).
#define PTP_AXIS_LOCK_MM      2.0f
#define PTP_SCROLL_MM_PER_NOTCH 3.0f
// A swipe fires once its fingers have moved this far.
#define PTP_SWIPE_MM          12.0f
// Pointer counts per millimetre of finger movement, before acceleration.
#define PTP_POINTER_COUNTS_PER_MM 6.0f

struct PtpGestureState {
    // A frame being assembled: in hybrid mode the fingers of one frame span several reports.
    HidPtpContact frame[2 * HID_PTP_MAX_CONTACTS];
    uint8_t frame_n;
    uint8_t frame_expected;

    // The fingers of the previous frame, in millimetres.
    struct Finger {
        uint8_t id;
        float x, y;
    } prev[2 * HID_PTP_MAX_CONTACTS];
    uint8_t prev_n;
    uint32_t prev_ms;

    // From the first finger down to the last one up.
    bool session;
    uint32_t session_start_ms;
    uint8_t session_max_fingers;
    float session_travel_mm;
    bool session_pressed;  // the pad was pressed down, so it is no tap

    // The current gesture (reset when the number of fingers changes).
    float gx, gy;
    uint8_t axis;  // 0 = undecided, 1 = vertical (scroll), 2 = horizontal (swipe)
    bool fired;
    float scroll_mm;

    float rem_x, rem_y;     // pointer movement not sent yet, in counts
    bool button_down;
    uint8_t button_mask;    // the mouse button the pad press is (1 = left, 2 = right)
};

struct PtpGestureOutput {
    int16_t dx, dy;          // pointer movement
    uint8_t buttons;         // mouse buttons held
    uint8_t clicks;          // mouse buttons to click (press and release) because of a tap
    uint8_t gestures[PTP_GESTURE_COUNT];  // how often each gesture happened
};

// Takes one touchpad report (decoded by hid_ptp_report_decode). `*out` is all zero, except for the
// buttons held, unless the report completes a frame.
void ptp_gesture_report(PtpGestureState *state, const HidPtpLayout &layout, const HidPtpContact *contacts,
                        uint8_t contact_count, bool button, uint32_t now_ms, PtpGestureOutput *out);

#endif  // PTP_GESTURE_H_
