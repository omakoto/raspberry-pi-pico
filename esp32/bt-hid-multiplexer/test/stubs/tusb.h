// Host-test stub of the few TinyUSB calls used by multiplexer.cpp; records the reports "sent".
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <vector>

struct SentKeyboard { uint8_t report_id; uint8_t mods; uint8_t keys[6]; };
struct SentMouse { uint8_t buttons; int8_t dx, dy, wheel, pan; };
extern std::vector<SentKeyboard> g_sent_keyboard;
extern std::vector<SentMouse> g_sent_mouse;
// While positive, report calls fail (return false without sending), counting it down.
extern int g_usb_fail_count;
// While false, the HID interface is busy and nothing can be sent.
extern bool g_usb_ready;
// The HID protocol the host has selected (HID_PROTOCOL_REPORT unless a test sets the boot protocol).
extern uint8_t g_usb_protocol;

enum { HID_PROTOCOL_BOOT = 0, HID_PROTOCOL_REPORT = 1 };

inline uint8_t tud_hid_n_get_protocol(uint8_t) { return g_usb_protocol; }

inline bool tud_hid_n_ready(uint8_t) { return g_usb_ready; }
inline bool tud_hid_n_report(uint8_t, uint8_t report_id, const void *report, uint16_t) {
    if (g_usb_fail_count > 0) { g_usb_fail_count--; return false; }
    const uint8_t *r = (const uint8_t *)report;
    SentKeyboard k;
    k.report_id = report_id;
    k.mods = r[0];
    for (int i = 0; i < 6; i++) k.keys[i] = r[2 + i];
    g_sent_keyboard.push_back(k);
    return true;
}
inline bool tud_hid_n_mouse_report(uint8_t, uint8_t, uint8_t buttons, int8_t x, int8_t y, int8_t wheel, int8_t pan) {
    if (g_usb_fail_count > 0) { g_usb_fail_count--; return false; }
    g_sent_mouse.push_back({buttons, x, y, wheel, pan});
    return true;
}
