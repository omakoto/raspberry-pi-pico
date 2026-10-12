#include "chord_mode.h"
#include <string.h>

// HID modifier bits and keycodes (HID Usage Tables, Keyboard/Keypad page).
enum : uint8_t { MOD_LCTRL = 0x01, MOD_LSHIFT = 0x02, MOD_LALT = 0x04 };
enum : uint8_t {
    KC_C = 0x06, KC_N = 0x11, KC_O = 0x12, KC_S = 0x16, KC_V = 0x19, KC_Y = 0x1C, KC_Z = 0x1D, KC_SPACE = 0x2C,
    KC_F5 = 0x3E,
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

static_assert(sizeof(ACK05_ENTRIES) / sizeof(ACK05_ENTRIES[0]) <= CHORD_MAX_ENTRIES, "too many entries");

// The Ulanzi Dial as a numeric keypad with a wheel: its three top buttons (media keys) are Keypad
// 1-3, its four lower buttons (shortcuts) Keypad 4-7, the dial (Volume Up/Down) scrolls, and the
// dial button (Mute) is Keypad Enter.
static const ChordEntry ULANZI_DIAL_ENTRIES[] = {
    {MOD_LCTRL, KC_Y, KC_KP_4},
    {MOD_LCTRL, KC_Z, KC_KP_5},
    {MOD_LCTRL, KC_V, KC_KP_6},
    {MOD_LCTRL, KC_C, KC_KP_7},
};

static const ChordConsumerEntry ULANZI_DIAL_CONSUMER_ENTRIES[] = {
    {0x00B6, KC_KP_1, 0},  // top button 1: Scan Previous Track
    {0x00CD, KC_KP_2, 0},  // top button 2: Play/Pause
    {0x00B5, KC_KP_3, 0},  // top button 3: Scan Next Track
    {0x00E9, 0, 1},        // dial right: Volume Increment
    {0x00EA, 0, -1},       // dial left: Volume Decrement
    {0x00E2, KC_KP_ENTER, 0},  // dial button: Mute
};

static const ChordProfile PROFILES[] = {
    {"Shortcut Remote", ACK05_ENTRIES, sizeof(ACK05_ENTRIES) / sizeof(ACK05_ENTRIES[0]), KC_KP_ENTER, false,
     nullptr, 0},
    {"Ulanzi Dial", ULANZI_DIAL_ENTRIES, sizeof(ULANZI_DIAL_ENTRIES) / sizeof(ULANZI_DIAL_ENTRIES[0]), 0, true,
     ULANZI_DIAL_CONSUMER_ENTRIES,
     sizeof(ULANZI_DIAL_CONSUMER_ENTRIES) / sizeof(ULANZI_DIAL_CONSUMER_ENTRIES[0])},
};

const ChordProfile *chord_profile_for(const char *device_name) {
    for (const ChordProfile &p : PROFILES) {
        if (strcmp(device_name, p.device_name) == 0) return &p;
    }
    return nullptr;
}

const ChordConsumerEntry *chord_consumer_entry(const ChordProfile &profile, uint16_t usage) {
    for (uint8_t i = 0; i < profile.consumer_entry_count; i++) {
        if (profile.consumer_entries[i].usage == usage) return &profile.consumer_entries[i];
    }
    return nullptr;
}

static bool has_key(const uint8_t keys[6], uint8_t key) {
    for (uint8_t i = 0; i < 6; i++) {
        if (keys[i] == key) return true;
    }
    return false;
}

// Whether the shortcuts `set` (bits are entry indices) add up to exactly the report.
static bool explains(const ChordProfile &profile, uint16_t set, uint8_t modifiers, const uint8_t keys[6]) {
    uint8_t union_modifiers = 0;
    for (uint8_t i = 0; i < profile.entry_count; i++) {
        if (set & (1u << i)) union_modifiers |= profile.entries[i].modifiers;
    }
    if (union_modifiers != modifiers) return false;
    for (uint8_t k = 0; k < 6; k++) {
        if (keys[k] == 0) continue;
        bool found = false;
        for (uint8_t i = 0; i < profile.entry_count && !found; i++) {
            found = (set & (1u << i)) && profile.entries[i].key == keys[k];
        }
        if (!found) return false;
    }
    return true;
}

// Whether a shortcut of `set` could be left out and the rest would still add up to the report.
static bool has_redundant(const ChordProfile &profile, uint16_t set, uint8_t modifiers, const uint8_t keys[6]) {
    for (uint8_t i = 0; i < profile.entry_count; i++) {
        if ((set & (1u << i)) && explains(profile, set & ~(1u << i), modifiers, keys)) return true;
    }
    return false;
}

// Finds which shortcuts are held. The device reports the shortcuts of all its held keys merged into
// one report (their modifiers ORed, their keys side by side), so this looks for the set of
// shortcuts that adds up to exactly the report. Several sets can: holding key 4 (Shift) and key 8
// (Ctrl+Z) gives Ctrl+Shift+Z, which is also key 10 alone. Keys are pressed and released one at a
// time, so the set that differs least from the keys held before wins, and among those, one where
// every shortcut adds something to the report. Returns false if no set adds up to the report.
static bool decode(const ChordProfile &profile, uint16_t held, uint8_t modifiers, const uint8_t keys[6],
                   uint16_t *out_held) {
    // The shortcuts that fit in the report.
    uint8_t candidates[CHORD_MAX_ENTRIES];
    uint8_t n = 0;
    for (uint8_t i = 0; i < profile.entry_count; i++) {
        const ChordEntry &e = profile.entries[i];
        if ((e.modifiers & ~modifiers) == 0 && (e.key == 0 || has_key(keys, e.key))) candidates[n++] = i;
    }

    bool found = false;
    uint16_t best = 0;
    int best_score = 0;
    for (uint32_t pick = 1; pick < (1u << n); pick++) {
        uint16_t set = 0;
        for (uint8_t j = 0; j < n; j++) {
            if (pick & (1u << j)) set |= (uint16_t)(1u << candidates[j]);
        }
        if (!explains(profile, set, modifiers, keys)) continue;
        int score = __builtin_popcount(set ^ held) * 2 + (has_redundant(profile, set, modifiers, keys) ? 1 : 0);
        if (!found || score < best_score) {
            found = true;
            best = set;
            best_score = score;
        }
    }
    *out_held = best;
    return found;
}

// Whether the report is part of shortcut `e`: its modifiers and keys all belong to that shortcut.
static bool part_of(const ChordEntry &e, uint8_t modifiers, const uint8_t keys[6]) {
    if ((modifiers & ~e.modifiers) != 0) return false;
    for (uint8_t k = 0; k < 6; k++) {
        if (keys[k] != 0 && keys[k] != e.key) return false;
    }
    return true;
}

// For a staged profile: whether the report is part of a shortcut, and if so, whether it is part of
// one of the shortcuts held (which is then being released).
static bool find_partial(const ChordProfile &profile, uint16_t held, uint8_t modifiers, const uint8_t keys[6],
                         bool *of_held) {
    bool found = false;
    *of_held = false;
    for (uint8_t i = 0; i < profile.entry_count; i++) {
        if (!part_of(profile.entries[i], modifiers, keys)) continue;
        found = true;
        if (held & (1u << i)) *of_held = true;
    }
    return found;
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
        if (!state->input_held && allow_idle_button && profile.idle_button_key != 0) {
            state->button_down = !state->button_down;
            state->button_down_ms = now_ms;
        }
        state->held_entries = 0;
        state->out_modifiers = 0;
        memset(state->out_keys, 0, 6);
    } else {
        uint16_t held;
        bool of_held = false;
        if (decode(profile, state->held_entries, modifiers, keys, &held)) {
            state->held_entries = held;
            state->out_modifiers = 0;
            memset(state->out_keys, 0, 6);
            uint8_t n = 0;
            for (uint8_t i = 0; i < profile.entry_count && n < 6; i++) {
                if (held & (1u << i)) state->out_keys[n++] = profile.entries[i].out;
            }
        } else if (profile.staged && find_partial(profile, state->held_entries, modifiers, keys, &of_held)) {
            // Being released: keep the shortcut held until the empty report. Being pressed: nothing
            // yet, the whole shortcut follows.
            if (!of_held) {
                state->held_entries = 0;
                state->out_modifiers = 0;
                memset(state->out_keys, 0, 6);
            }
        } else {
            state->held_entries = 0;
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
