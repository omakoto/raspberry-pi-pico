// Host-test stub of the few TinyUSB calls used by multiplexer.cpp; records the reports "sent".
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <vector>

struct SentKeyboard { uint8_t mods; uint8_t keys[6]; };
struct SentMouse { uint8_t buttons; int8_t dx, dy, wheel, pan; };
extern std::vector<SentKeyboard> g_sent_keyboard;
extern std::vector<SentMouse> g_sent_mouse;
// While positive, report calls fail (return false without sending), counting it down.
extern int g_usb_fail_count;

inline bool tud_hid_n_ready(uint8_t) { return true; }
inline bool tud_hid_n_report(uint8_t, uint8_t, const void *report, uint16_t) {
    if (g_usb_fail_count > 0) { g_usb_fail_count--; return false; }
    const uint8_t *r = (const uint8_t *)report;
    SentKeyboard k;
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
