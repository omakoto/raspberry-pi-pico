// Host-side test of main/chord_mode.cpp: the XP-Pen ACK05's shortcuts becoming keypad keys, reports
// that pass through unchanged, and the dial button that only sends empty reports.
#include <stdio.h>
#include <string.h>
#include "chord_mode.h"

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

static const ChordProfile *ack05() {
    return chord_profile_for("Shortcut Remote");
}

struct Out {
    uint8_t mods;
    uint8_t keys[6];
    bool is(uint8_t m, uint8_t k0 = 0, uint8_t k1 = 0) const {
        const uint8_t want[6] = {k0, k1, 0, 0, 0, 0};
        return mods == m && memcmp(keys, want, 6) == 0;
    }
};

// Sends a report with up to 2 keys at time now_ms.
static Out send(ChordState *st, uint8_t mods, uint8_t k0 = 0, uint8_t k1 = 0, uint32_t now_ms = 5000,
                bool allow_idle_button = true) {
    const uint8_t keys[6] = {k0, k1, 0, 0, 0, 0};
    Out o;
    memset(&o, 0xAA, sizeof(o));
    chord_translate(*ack05(), st, mods, keys, allow_idle_button, now_ms, &o.mods, o.keys);
    return o;
}

static void test_profile_lookup() {
    CHECK(ack05() != nullptr && ack05()->idle_button_key == 0x58);
    CHECK(chord_profile_for("Keychron K2") == nullptr);
    CHECK(chord_profile_for("Shortcut Remote 2") == nullptr);
}

// The shortcuts as captured from the device, top left to bottom right.
static void test_ack05_keys() {
    struct { uint8_t mods, key, out; } cases[] = {
        {0x01, 0x56, 0x56}, {0x01, 0x57, 0x57},                      // dial: Keypad - and +
        {0x01, 0x12, 0x59}, {0x01, 0x11, 0x5A}, {0x00, 0x3E, 0x5B},  // keys 1-3: Keypad 1-3
        {0x02, 0x00, 0x5C}, {0x01, 0x00, 0x5D}, {0x04, 0x00, 0x5E},  // keys 4-6
        {0x01, 0x16, 0x5F}, {0x01, 0x1D, 0x60}, {0x00, 0x2C, 0x61},  // keys 7-9
        {0x03, 0x1D, 0x62},                                          // key 10: Keypad 0
    };
    for (auto &c : cases) {
        ChordState st = {};
        CHECK(send(&st, c.mods, c.key).is(0, c.out));
        CHECK(send(&st, 0).is(0));  // the release
        CHECK(!st.button_down);
    }
}

static void test_pass_through() {
    ChordState st = {};
    CHECK(send(&st, 0x01, 0x04).is(0x01, 0x04));  // Ctrl+A is not a known shortcut
    CHECK(send(&st, 0).is(0));
    CHECK(send(&st, 0x10).is(0x10));  // nor is a bare Right Ctrl
    CHECK(send(&st, 0).is(0));
    // One unknown key keeps the whole report as it is.
    CHECK(send(&st, 0x01, 0x12, 0x04).is(0x01, 0x12, 0x04));
    // Two known keys at once.
    CHECK(send(&st, 0x01, 0x12, 0x11).is(0, 0x59, 0x5A));
    CHECK(send(&st, 0).is(0));
    CHECK(!st.button_down);
}

// The dial button sends an empty report on press and another on release.
static void test_idle_button() {
    ChordState st = {};
    CHECK(send(&st, 0).is(0, 0x58));
    CHECK(send(&st, 0).is(0));

    // Held while a key is pressed and released: the key's release leaves the button down.
    CHECK(send(&st, 0).is(0, 0x58));
    CHECK(send(&st, 0x01, 0x12).is(0, 0x59, 0x58));
    CHECK(send(&st, 0).is(0, 0x58));
    CHECK(send(&st, 0).is(0));

    // Right after connecting, an empty report may be a key's release and is ignored.
    CHECK(send(&st, 0, 0, 0, 5000, false).is(0));
    CHECK(!st.button_down);

    // With 6 keys down there is no room for the button; it shows up once there is.
    const uint8_t six[6] = {0x12, 0x11, 0x16, 0x1D, 0x56, 0x57};
    Out o;
    CHECK(send(&st, 0).is(0, 0x58));
    chord_translate(*ack05(), &st, 0x01, six, true, 5000, &o.mods, o.keys);
    const uint8_t want[6] = {0x59, 0x5A, 0x5F, 0x60, 0x56, 0x57};
    CHECK(o.mods == 0 && memcmp(o.keys, want, 6) == 0);
    CHECK(send(&st, 0x01, 0x12).is(0, 0x59, 0x58));
}

// A button whose release report was lost is released by itself.
static void test_expire() {
    ChordState st = {};
    Out o;
    CHECK(!chord_expire(*ack05(), &st, 100000, &o.mods, o.keys));  // not down

    CHECK(send(&st, 0, 0, 0, 1000).is(0, 0x58));
    CHECK(send(&st, 0x01, 0x12, 0, 2000).is(0, 0x59, 0x58));
    CHECK(!chord_expire(*ack05(), &st, 1000 + CHORD_IDLE_BUTTON_TIMEOUT_MS - 1, &o.mods, o.keys));
    CHECK(chord_expire(*ack05(), &st, 1000 + CHORD_IDLE_BUTTON_TIMEOUT_MS, &o.mods, o.keys));
    CHECK(o.is(0, 0x59));  // the key that is still down stays down
    CHECK(!st.button_down);
    CHECK(send(&st, 0).is(0));  // the key's release, not the button
    CHECK(!st.button_down);
}

int main() {
    test_profile_lookup();
    test_ack05_keys();
    test_pass_through();
    test_idle_button();
    test_expire();
    if (g_failures) {
        printf("chord_mode_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("All chord mode tests passed\n");
    return 0;
}
