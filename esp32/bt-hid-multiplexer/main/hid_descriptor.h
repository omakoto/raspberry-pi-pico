#ifndef HID_DESCRIPTOR_H_
#define HID_DESCRIPTOR_H_

#include <stdint.h>
#include <stdbool.h>

// Reading a BLE peripheral's HID report descriptor (BleHidHost): which report IDs carry the
// keyboard, the mouse and the LED output, and where the keyboard's modifiers and keys are in its
// report. Devices put these on different IDs and in different layouts (e.g. the XP-Pen ACK05 sends
// a 7-byte keyboard report without the boot keyboard's reserved byte on ID 6, and its relative mouse
// on ID 1), so the report ID and length alone cannot tell them apart. This file does not need
// BTstack, so that the host tests can run it (test/hid_descriptor_test.cpp). Run
// test/run-host-test.sh after changing this file or hid_descriptor.cpp.

// Where a keyboard report's fields are. Offsets are byte offsets into the report's data, after the
// report ID byte.
struct HidKeyboardLayout {
    uint8_t report_id;
    int8_t modifier_offset;  // the byte of the 8 modifier bits (Left Control..Right GUI), -1 if none
    uint8_t keys_offset;     // the first byte of the keycode array
    uint8_t key_count;       // the number of keycode bytes
};

struct HidDescriptorInfo {
    // The first Output report with LED usages.
    bool has_led_output;
    uint8_t led_report_id;

    // The pointer report: the first one whose X/Y are relative, or else the first absolute one.
    bool has_mouse;
    uint8_t mouse_report_id;
    uint8_t mouse_xy_bits;  // the Report Size of X/Y (8, 12 or 16)
    bool mouse_absolute;

    // The first keyboard report that has a keycode array (Usage Page 0x07, 8-bit array items).
    bool has_keyboard;
    HidKeyboardLayout keyboard;
};

// Reads the descriptor. Fields of what the descriptor does not have are left zero.
void hid_descriptor_parse(const uint8_t *desc, uint16_t desc_len, HidDescriptorInfo *out);

// Reads the modifiers and up to 6 keys of a keyboard report laid out as `layout` (`data` without the
// report ID byte). Keys are taken in order, skipping empty (0) entries, and the unused entries of
// `keys` are 0. Returns false if the report is too short for the layout.
bool hid_keyboard_report_decode(const HidKeyboardLayout &layout, const uint8_t *data, uint16_t data_len,
                                uint8_t *modifiers, uint8_t keys[6]);

#endif  // HID_DESCRIPTOR_H_
