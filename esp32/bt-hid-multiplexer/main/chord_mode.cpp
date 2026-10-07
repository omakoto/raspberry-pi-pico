#include "chord_mode.h"
#include <string.h>

// HID modifier bits and keycodes (HID Usage Tables, Keyboard/Keypad page).
enum : uint8_t { MOD_LCTRL = 0x01, MOD_LSHIFT = 0x02, MOD_LALT = 0x04 };
enum : uint8_t {
    KC_N = 0x11, KC_O = 0x12, KC_S = 0x16, KC_Z = 0x1D, KC_SPACE = 0x2C, KC_F5 = 0x3E,
    KC_KP_MINUS = 0x56, KC_KP_PLUS = 0x57, KC_KP_ENTER = 0x58,
    KC_KP_1 = 0x59, KC_KP_2, KC_KP_3, KC_KP_4, KC_KP_5, KC_KP_6, KC_KP_7, KC_KP_8, KC_KP_9, KC_KP_0,
};

// The XP-Pen ACK05's factory shortcuts, as a numeric keypad: the dial is - and +, its button is
// Enter, and keys 1-10 (top left to bottom right) are 1-9 and 0. The dial sends Ctrl+Keypad -/+
// itself, so its turns become the bare keypad keys.
static const ChordEntry ACK05_ENTRIES[] = {
    {MOD_LCTRL, KC_KP_MINUS, KC_KP_MINUS},       // dial left
    {MOD_LCTRL, KC_KP_PLUS, KC_KP_PLUS},         // dial right
    {MOD_LCTRL, KC_O, KC_KP_1},                  // key 1
    {MOD_LCTRL, KC_N, KC_KP_2},                  // key 2
    {0, KC_F5, KC_KP_3},                         // key 3
    {MOD_LSHIFT, 0, KC_KP_4},                    // key 4
    {MOD_LCTRL, 0, KC_KP_5},                     // key 5
    {MOD_LALT, 0, KC_KP_6},                      // key 6
    {MOD_LCTRL, KC_S, KC_KP_7},                  // key 7
    {MOD_LCTRL, KC_Z, KC_KP_8},                  // key 8
    {0, KC_SPACE, KC_KP_9},                      // key 9
    {MOD_LCTRL | MOD_LSHIFT, KC_Z, KC_KP_0},     // key 10
};

static const ChordProfile PROFILES[] = {
    {"Shortcut Remote", ACK05_ENTRIES, sizeof(ACK05_ENTRIES) / sizeof(ACK05_ENTRIES[0]), KC_KP_ENTER},
};

const ChordProfile *chord_profile_for(const char *device_name) {
    for (const ChordProfile &p : PROFILES) {
        if (strcmp(device_name, p.device_name) == 0) return &p;
    }
    return nullptr;
}

static bool lookup(const ChordProfile &profile, uint8_t modifiers, uint8_t key, uint8_t *out) {
    for (uint8_t i = 0; i < profile.entry_count; i++) {
        if (profile.entries[i].modifiers == modifiers && profile.entries[i].key == key) {
            *out = profile.entries[i].out;
            return true;
        }
    }
    return false;
}

// The translated report plus the idle button, if it is down and there is room for it.
static void emit(const ChordProfile &profile, const ChordState &state, uint8_t *out_modifiers, uint8_t out_keys[6]) {
    *out_modifiers = state.out_modifiers;
    memcpy(out_keys, state.out_keys, 6);
    if (!state.button_down) return;
    for (uint8_t i = 0; i < 6; i++) {
        if (out_keys[i] == 0) {
            out_keys[i] = profile.idle_button_key;
            return;
        }
    }
}

void chord_translate(const ChordProfile &profile, ChordState *state, uint8_t modifiers, const uint8_t keys[6],
                     bool allow_idle_button, uint32_t now_ms, uint8_t *out_modifiers, uint8_t out_keys[6]) {
    bool empty = (modifiers == 0);
    for (uint8_t i = 0; i < 6; i++) {
        if (keys[i] != 0) empty = false;
    }

    if (empty) {
        // After a shortcut this is its release; otherwise it is the idle button's press or release.
        if (!state->input_held && allow_idle_button) {
            state->button_down = !state->button_down;
            state->button_down_ms = now_ms;
        }
        state->out_modifiers = 0;
        memset(state->out_keys, 0, 6);
    } else {
        // Each key with the report's modifiers is one shortcut; with no keys, the modifiers are.
        uint8_t mapped[6] = {0};
        uint8_t n = 0;
        bool known = true;
        for (uint8_t i = 0; i < 6; i++) {
            if (keys[i] == 0) continue;
            if (!lookup(profile, modifiers, keys[i], &mapped[n++])) known = false;
        }
        if (n == 0 && !lookup(profile, modifiers, 0, &mapped[n++])) known = false;

        if (known) {
            state->out_modifiers = 0;
            memcpy(state->out_keys, mapped, 6);
        } else {
            state->out_modifiers = modifiers;
            memcpy(state->out_keys, keys, 6);
        }
    }
    state->input_held = !empty;
    emit(profile, *state, out_modifiers, out_keys);
}

bool chord_expire(const ChordProfile &profile, ChordState *state, uint32_t now_ms, uint8_t *out_modifiers,
                  uint8_t out_keys[6]) {
    if (!state->button_down || now_ms - state->button_down_ms < CHORD_IDLE_BUTTON_TIMEOUT_MS) return false;
    state->button_down = false;
    emit(profile, *state, out_modifiers, out_keys);
    return true;
}
