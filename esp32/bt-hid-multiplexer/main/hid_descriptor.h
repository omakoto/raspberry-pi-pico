#ifndef HID_DESCRIPTOR_H_
#define HID_DESCRIPTOR_H_

#include <stdint.h>
#include <stdbool.h>

// Reading a BLE peripheral's HID report descriptor (BleHidHost): which report IDs carry the
// keyboard, the mouse, the media keys and the LED output, and where the keyboard's modifiers and keys
// and the media key usages are in their reports. Devices put these on different IDs and in different layouts (e.g. the XP-Pen ACK05 sends
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

// Where a consumer control (media key) report's usage array is. The offset is a byte offset into the
// report's data, after the report ID byte.
struct HidConsumerLayout {
    uint8_t report_id;
    uint8_t offset;
    uint8_t size_bits;  // 8 or 16
    uint8_t count;      // the number of usages the report can hold
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

    // A Windows Precision Touchpad's Input Mode feature report (Digitizer page, usage 0x52), when it
    // is an 8-bit field at the start of its report. 0 asks the touchpad to send its mouse report,
    // 3 to send multi-touch contacts.
    bool has_input_mode;
    uint8_t input_mode_report_id;

    // The first consumer control report with a byte-aligned 8- or 16-bit usage array (Usage Page
    // 0x0C), as media keys and dials send.
    bool has_consumer;
    HidConsumerLayout consumer;
};

// A Windows Precision Touchpad's input report: where each finger's fields are, as bit offsets into the
// report's data (after the report ID byte) and sizes in bits.
struct HidPtpContactLayout {
    uint16_t tip_bit;          // Tip Switch: the finger touches the surface
    int16_t confidence_bit;    // Confidence (0 = palm or other accidental contact), -1 if none
    uint16_t id_bit;           // Contact Identifier
    uint8_t id_size;
    uint16_t x_bit, y_bit;     // absolute X and Y
    uint8_t x_size, y_size;
};

#define HID_PTP_MAX_CONTACTS 5

struct HidPtpLayout {
    uint8_t report_id;
    uint8_t contact_slots;     // fingers per report
    HidPtpContactLayout contacts[HID_PTP_MAX_CONTACTS];
    bool has_contact_count;    // Contact Count: valid fingers in this report (more follow if larger)
    uint16_t contact_count_bit;
    uint8_t contact_count_size;
    bool has_button;           // Button 1: the pad is pressed down (clickpads)
    uint16_t button_bit;
    // Logical units per millimetre, from the X/Y logical and physical ranges (0 if unknown).
    float x_per_mm, y_per_mm;
};

// One finger of a touchpad report.
struct HidPtpContact {
    uint8_t id;
    bool tip;
    bool confident;
    int32_t x, y;
};

// Finds a precision touchpad's input report (the Touch Pad application collection, Digitizer page
// 0x05, with at least one Finger collection holding Tip Switch, Contact Identifier, X and Y). Returns
// false if the descriptor has none.
bool hid_ptp_parse(const uint8_t *desc, uint16_t desc_len, HidPtpLayout *out);

// Reads a touchpad report laid out as `layout` (`data` without the report ID byte). Returns false if
// the report is too short. `contacts` gets one entry per finger slot, `*contact_count` the Contact
// Count (or the number of slots if the report has none) and `*button` the button.
bool hid_ptp_report_decode(const HidPtpLayout &layout, const uint8_t *data, uint16_t data_len,
                           HidPtpContact contacts[HID_PTP_MAX_CONTACTS], uint8_t *contact_count, bool *button);

// Reads the descriptor. Fields of what the descriptor does not have are left zero.
void hid_descriptor_parse(const uint8_t *desc, uint16_t desc_len, HidDescriptorInfo *out);

// Reads the modifiers and up to 6 keys of a keyboard report laid out as `layout` (`data` without the
// report ID byte). Keys are taken in order, skipping empty (0) entries, and the unused entries of
// `keys` are 0. Returns false if the report is too short for the layout.
bool hid_keyboard_report_decode(const HidKeyboardLayout &layout, const uint8_t *data, uint16_t data_len,
                                uint8_t *modifiers, uint8_t keys[6]);

// Reads the usages pressed in a consumer control report laid out as `layout` (`data` without the
// report ID byte) into `usages`, skipping empty (0) entries. Returns how many there are, or -1 if the
// report is too short for the layout.
int hid_consumer_report_decode(const HidConsumerLayout &layout, const uint8_t *data, uint16_t data_len,
                               uint16_t *usages, uint8_t max_usages);

#endif  // HID_DESCRIPTOR_H_
