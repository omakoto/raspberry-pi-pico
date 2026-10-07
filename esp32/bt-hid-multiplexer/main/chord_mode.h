#ifndef CHORD_MODE_H_
#define CHORD_MODE_H_

#include <stdint.h>
#include <stdbool.h>

// Chord mode: turns a keypad whose keys send fixed shortcuts into a keypad of distinct keys.
//
// The XP-Pen ACK05 ("Shortcut Remote") sends e.g. Ctrl+Z on one key, Ctrl+Shift+Z on another and a
// bare Ctrl on a third, so in the virtual matrix its keys share the Ctrl and Z positions and cannot
// be remapped one by one. Each key press arrives as one report holding the whole shortcut, so in
// chord mode every report is looked up as a whole, and a known shortcut becomes a single key of its
// own (with its modifiers dropped), which VIAL can then remap like any other key. Keys held together
// arrive merged into one report, which is split back into the shortcuts of the keys held.
//
// The ACK05's dial button has no shortcut: it sends an empty report when pressed and another when
// released. An empty report while nothing is held is therefore taken as that button.
//
// This file does not need BTstack, so that the host tests can run it (test/chord_mode_test.cpp).
// Run test/run-host-test.sh after changing this file or chord_mode.cpp.

struct ChordEntry {
    uint8_t modifiers;  // the HID modifier byte the device sends
    uint8_t key;        // the keycode it sends, 0 for a modifier-only shortcut
    uint8_t out;        // the keycode reported instead
};

// The most entries a profile can have (they are tracked as bits of a uint16_t).
#define CHORD_MAX_ENTRIES 16

struct ChordProfile {
    const char *device_name;
    const ChordEntry *entries;
    uint8_t entry_count;
    uint8_t idle_button_key;  // the keycode of the button that sends only empty reports
};

// The idle button is released by itself after this long, in case its release report was lost.
#define CHORD_IDLE_BUTTON_TIMEOUT_MS 30000

// The chord mode profile for a device, or nullptr if it has none.
const ChordProfile *chord_profile_for(const char *device_name);

// Per-device state. All zero is the initial state.
struct ChordState {
    bool input_held;           // the last report from the device was not empty
    uint16_t held_entries;     // the shortcuts held, as bits of entry indices
    bool button_down;
    uint32_t button_down_ms;
    uint8_t out_modifiers;     // the translated report without the idle button
    uint8_t out_keys[6];
};

// Translates a keyboard report from the device into the report to hand to the multiplexer.
// A report that is not entirely made of known shortcuts is passed on unchanged. `allow_idle_button`
// is false while an empty report may be the release of a key pressed before the connection (right
// after connecting), so that it is not taken for the idle button.
void chord_translate(const ChordProfile &profile, ChordState *state, uint8_t modifiers, const uint8_t keys[6],
                     bool allow_idle_button, uint32_t now_ms, uint8_t *out_modifiers, uint8_t out_keys[6]);

// Releases the idle button if it has been down for CHORD_IDLE_BUTTON_TIMEOUT_MS. Returns true, with
// the report to hand to the multiplexer, if it did.
bool chord_expire(const ChordProfile &profile, ChordState *state, uint32_t now_ms, uint8_t *out_modifiers,
                  uint8_t out_keys[6]);

#endif  // CHORD_MODE_H_
