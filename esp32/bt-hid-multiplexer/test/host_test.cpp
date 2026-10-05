// Host-side test of the virtual matrix and multiplexer (key/modifier/mouse remapping) with USB and
// flash stubbed out. Build and run with test/run-host-test.sh.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tusb.h"
#include "multiplexer.h"
#include "virtual_matrix.h"
#include "storage.h"
#include "device_bindings.h"
#include "macros.h"

std::vector<SentKeyboard> g_sent_keyboard;
std::vector<SentMouse> g_sent_mouse;
uint32_t g_now_ms = 0;

static int g_saves = 0;
static uint16_t g_flash[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS];
void StorageManager::init() {}
bool StorageManager::loadKeymap(uint16_t km[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS], bool &needs_save) { (void)km; needs_save = false; return false; }
void StorageManager::saveKeymap(const uint16_t km[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS]) {
    memcpy(g_flash, km, sizeof(g_flash));
    g_saves++;
}
void StorageManager::clearKeymap() {}
static DeviceBindingEntry g_flash_bindings[MAX_DEVICE_BINDINGS];
bool StorageManager::loadBindings(DeviceBindingEntry e[MAX_DEVICE_BINDINGS]) { (void)e; return false; }
void StorageManager::saveBindings(const DeviceBindingEntry e[MAX_DEVICE_BINDINGS]) {
    memcpy(g_flash_bindings, e, sizeof(g_flash_bindings));
}
static int g_macro_saves = 0;
bool StorageManager::loadMacros(uint8_t *buffer) { (void)buffer; return false; }
void StorageManager::saveMacros(const uint8_t *buffer) { (void)buffer; g_macro_saves++; }

// Fake Bluetooth addresses: device d is 00:00:00:00:00:d+1, except the ones marked disconnected.
static bool g_connected[MAX_KEYBOARDS];
static bool fake_address(uint8_t dev_idx, uint8_t addr[6]) {
    if (dev_idx >= MAX_KEYBOARDS || !g_connected[dev_idx]) return false;
    memset(addr, 0, 6);
    addr[5] = dev_idx + 1;
    return true;
}

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

static void reset() {
    g_sent_keyboard.clear();
    g_sent_mouse.clear();
    memset(g_connected, 0, sizeof(g_connected));
    g_connected[0] = g_connected[1] = true;
    DeviceBindings::init(fake_address);
    DeviceBindings::clearAll();
    MacroStore::init();
    Multiplexer::init();
    VirtualMatrix::init();
}

// Lets the multiplexer send every report it has queued (as USB completions do on the device).
static void pump() {
    for (int i = 0; i < 200; i++) Multiplexer::flushKeyboard();
}

// Writes the macro buffer in 28-byte pieces, as VIAL does.
static void set_macros(const uint8_t *data, uint16_t len) {
    for (uint16_t off = 0; off < len; off += 28) {
        uint8_t n = (uint8_t)((len - off) < 28 ? (len - off) : 28);
        MacroStore::write(off, n, data + off);
    }
}
static void set(int layer, int vkey, uint16_t kc) { VirtualMatrix::setKeycode(layer, vkey / 16, vkey % 16, kc); }
static SentKeyboard lastKbd() { return g_sent_keyboard.back(); }
static SentMouse lastMouse() { return g_sent_mouse.back(); }

int main() {
    // Defaults pass everything through.
    reset();
    uint8_t keys[1] = {0x04};
    Multiplexer::handleKeyboardReport(0, 0x02, keys, 1);
    CHECK(lastKbd().mods == 0x02 && lastKbd().keys[0] == 0x04);
    Multiplexer::handleKeyboardReport(0, 0, nullptr, 0);
    CHECK(lastKbd().mods == 0 && lastKbd().keys[0] == 0);
    Multiplexer::handleMouseReport(0, 0x01, 5, -3, 1, 0);
    CHECK(lastMouse().buttons == 1 && lastMouse().dx == 5 && lastMouse().dy == -3 && lastMouse().wheel == 1);
    Multiplexer::handleMouseReport(0, 0, 0, 0, 0, 0);
    CHECK(lastMouse().buttons == 0);

    // A key beyond the old 4x16 matrix (F12 = 0x45) and a keypad key can be remapped; so can a
    // modifier (Caps Lock -> Left Ctrl, Left Shift -> disabled).
    reset();
    set(0, 0x45, 0x04);   // F12 -> A
    set(0, 0x5F, 0x0204); // Keypad 7 -> LSFT(A)
    set(0, 0xE1, 0x0000); // LShift -> KC_NO
    set(0, 0x39, 0x00E0); // Caps -> LCtrl
    uint8_t f12[1] = {0x45};
    Multiplexer::handleKeyboardReport(0, 0, f12, 1);
    CHECK(lastKbd().keys[0] == 0x04 && lastKbd().mods == 0);
    uint8_t kp7[1] = {0x5F};
    Multiplexer::handleKeyboardReport(0, 0, kp7, 1);
    CHECK(lastKbd().keys[0] == 0x04 && lastKbd().mods == 0x02);
    Multiplexer::handleKeyboardReport(0, 0x02, nullptr, 0);  // press Left Shift
    CHECK(lastKbd().mods == 0);
    Multiplexer::handleKeyboardReport(0, 0, nullptr, 0);
    uint8_t caps[1] = {0x39};
    Multiplexer::handleKeyboardReport(0, 0, caps, 1);
    CHECK(lastKbd().mods == 0x01 && lastKbd().keys[0] == 0);

    // A mouse button remapped to a key, and a key remapped to a mouse button.
    reset();
    set(0, 0xE8 + 3, 0x0028);  // mouse button 4 -> Enter
    set(0, 0x2C, KC_BTN1_ + 1);      // Space -> KC_BTN2
    Multiplexer::handleMouseReport(0, 0x08, 0, 0, 0, 0);
    CHECK(lastKbd().keys[0] == 0x28);
    CHECK(g_sent_mouse.empty() || lastMouse().buttons == 0);
    Multiplexer::handleMouseReport(0, 0, 0, 0, 0, 0);
    CHECK(lastKbd().keys[0] == 0);
    uint8_t space[1] = {0x2C};
    Multiplexer::handleKeyboardReport(0, 0, space, 1);
    CHECK(lastMouse().buttons == 0x02);
    Multiplexer::handleKeyboardReport(0, 0, nullptr, 0);
    CHECK(lastMouse().buttons == 0);

    // Hold a mouse button (MO(1)) and layer 1 turns up/down motion into wheel scrolling.
    reset();
    set(0, 0xE8 + 3, 0x5101);        // button 4 -> MO(1)
    set(1, VKEY_MOTION_UP, KC_WH_U_);
    set(1, VKEY_MOTION_DOWN, KC_WH_D_);
    Multiplexer::handleMouseReport(0, 0, 0, -48, 0, 0);  // no layer: plain cursor motion
    CHECK(lastMouse().dy == -48 && lastMouse().wheel == 0);
    Multiplexer::handleMouseReport(0, 0x08, 0, 0, 0, 0);  // hold button 4
    CHECK(VirtualMatrix::getActiveLayer() == 1);
    g_sent_mouse.clear();
    Multiplexer::handleMouseReport(0, 0x08, 7, -48, 0, 0);  // up 48 counts = 2 notches up; dx passes
    CHECK(lastMouse().wheel == 2 && lastMouse().dy == 0 && lastMouse().dx == 7);
    Multiplexer::handleMouseReport(0, 0x08, 0, 30, 0, 0);   // down 30 = 1 notch down, 6 left over
    CHECK(lastMouse().wheel == -1 && lastMouse().dy == 0);
    Multiplexer::handleMouseReport(0, 0x08, 0, 18, 0, 0);   // 6 + 18 = 24 -> one more notch
    CHECK(lastMouse().wheel == -1);
    CHECK(lastMouse().buttons == 0);  // MO(1) press itself is not sent to the host
    Multiplexer::handleMouseReport(0, 0, 0, 0, 0, 0);       // release
    CHECK(VirtualMatrix::getActiveLayer() == 0);
    Multiplexer::handleMouseReport(0, 0, 0, -10, 0, 0);
    CHECK(lastMouse().dy == -10 && lastMouse().wheel == 0);

    // Real wheel remapped to cursor motion; motion direction disabled.
    reset();
    set(0, VKEY_WHEEL_UP, KC_MS_U_);
    set(0, VKEY_MOTION_LEFT, KC_NO_);
    Multiplexer::handleMouseReport(0, 0, -9, 0, 1, 0);
    CHECK(lastMouse().dx == 0 && lastMouse().dy == -MOUSE_COUNTS_PER_WHEEL_NOTCH && lastMouse().wheel == 0);

    // Keymap edits are written to flash once, after they have been quiet for a moment.
    reset();
    g_saves = 0;
    g_now_ms = 1000;
    set(0, 4, 5);
    set(0, 5, 6);
    VirtualMatrix::flushPendingSave();
    CHECK(g_saves == 0);
    g_now_ms = 1600;
    VirtualMatrix::flushPendingSave();
    CHECK(g_saves == 1 && g_flash[0][0][5] == 6);
    VirtualMatrix::flushPendingSave();
    CHECK(g_saves == 1);

    // All layers work, including the highest one, and layers start out transparent.
    reset();
    CHECK(NUM_LAYERS == 8);
    CHECK(VirtualMatrix::getKeycode(NUM_LAYERS - 1, 3, 4) == KC_TRNS_);
    set(NUM_LAYERS - 1, 0x04, 0x05);
    CHECK(VirtualMatrix::getKeycode(NUM_LAYERS - 1, 0, 4) == 0x05);
    CHECK(VirtualMatrix::getKeycode(NUM_LAYERS, 0, 4) == 0);
    set(0, VKEY_MOUSE_BTN_BASE, 0x5100 + NUM_LAYERS - 1);  // button 1 -> MO(7)
    Multiplexer::handleMouseReport(0, 0x01, 0, 0, 0, 0);
    CHECK(VirtualMatrix::getActiveLayer() == NUM_LAYERS - 1);
    uint8_t layer_a[1] = {0x04};
    Multiplexer::handleKeyboardReport(0, 0, layer_a, 1);
    CHECK(lastKbd().keys[0] == 0x05);
    Multiplexer::handleMouseReport(0, 0, 0, 0, 0, 0);
    Multiplexer::handleKeyboardReport(0, 0, nullptr, 0);

    // Transparent keys on upper layers fall through to layer 0.
    reset();
    set(0, 0x04, 0x05);
    set(2, 0x04, KC_TRNS_);
    set(0, 0xE8, 0x5102);  // MO(2)
    Multiplexer::handleMouseReport(0, 0x01, 0, 0, 0, 0);
    uint8_t a[1] = {0x04};
    Multiplexer::handleKeyboardReport(0, 0, a, 1);
    CHECK(lastKbd().keys[0] == 0x05);

    // Mouse movement mapped to a key taps it once per wheel notch worth of movement (press, release,
    // press, release...), and the wheel taps once per notch. Volume keys go out as the keyboard
    // page volume usages.
    reset();
    set(0, VKEY_MOTION_UP, KC_VOLU_);
    set(0, VKEY_MOTION_DOWN, KC_VOLD_);
    set(0, VKEY_WHEEL_UP, 0x04);  // KC_A
    g_sent_keyboard.clear();
    Multiplexer::handleMouseReport(0, 0, 0, -50, 0, 0);  // 50 counts = 2 taps, 2 counts left
    for (int i = 0; i < 10; i++) Multiplexer::flushKeyboard();
    CHECK(g_sent_keyboard.size() == 4);
    if (g_sent_keyboard.size() == 4) {
        CHECK(g_sent_keyboard[0].keys[0] == 0x80 && g_sent_keyboard[1].keys[0] == 0);
        CHECK(g_sent_keyboard[2].keys[0] == 0x80 && g_sent_keyboard[3].keys[0] == 0);
    }
    CHECK(g_sent_mouse.empty() || (lastMouse().dy == 0 && lastMouse().wheel == 0));
    g_sent_keyboard.clear();
    Multiplexer::handleMouseReport(0, 0, 0, 22, 0, 0);   // down: remainder is reset, 22 < 24
    for (int i = 0; i < 10; i++) Multiplexer::flushKeyboard();
    CHECK(g_sent_keyboard.empty());
    Multiplexer::handleMouseReport(0, 0, 0, 2, 0, 0);    // 24 in total -> volume down
    for (int i = 0; i < 10; i++) Multiplexer::flushKeyboard();
    CHECK(g_sent_keyboard.size() == 2 && g_sent_keyboard[0].keys[0] == 0x81);
    g_sent_keyboard.clear();
    Multiplexer::handleMouseReport(0, 0, 0, 0, 1, 0);    // one wheel notch up -> one tap of A
    for (int i = 0; i < 10; i++) Multiplexer::flushKeyboard();
    CHECK(g_sent_keyboard.size() == 2 && g_sent_keyboard[0].keys[0] == 0x04 && g_sent_keyboard[1].keys[0] == 0);

    // The same key tapped twice in a row still gets a release in between; modifiers work too.
    reset();
    set(0, VKEY_WHEEL_UP, 0x0204);  // LSFT(A)
    g_sent_keyboard.clear();
    Multiplexer::handleMouseReport(0, 0, 0, 0, 2, 0);
    for (int i = 0; i < 10; i++) Multiplexer::flushKeyboard();
    CHECK(g_sent_keyboard.size() == 4);
    if (g_sent_keyboard.size() == 4) {
        CHECK(g_sent_keyboard[0].mods == 0x02 && g_sent_keyboard[0].keys[0] == 0x04);
        CHECK(g_sent_keyboard[1].mods == 0 && g_sent_keyboard[1].keys[0] == 0);
        CHECK(g_sent_keyboard[2].keys[0] == 0x04);
    }

    // Mouse buttons 6-8 default to F13-F15 and can be remapped to anything else.
    reset();
    Multiplexer::handleMouseReport(0, 0x20, 0, 0, 0, 0);
    CHECK(lastKbd().keys[0] == 0x68);
    Multiplexer::handleMouseReport(0, 0x40, 0, 0, 0, 0);
    CHECK(lastKbd().keys[0] == 0x69);
    Multiplexer::handleMouseReport(0, 0x80, 0, 0, 0, 0);
    CHECK(lastKbd().keys[0] == 0x6A);
    Multiplexer::handleMouseReport(0, 0, 0, 0, 0, 0);
    CHECK(lastKbd().keys[0] == 0);
    set(0, VKEY_MOUSE_BTN_BASE + 6, KC_VOLD_);
    Multiplexer::handleMouseReport(0, 0x40, 0, 0, 0, 0);
    CHECK(lastKbd().keys[0] == 0x81);
    Multiplexer::handleMouseReport(0, 0, 0, 0, 0, 0);
    CHECK(lastKbd().keys[0] == 0);

    // Per-device layers: device 1 is bound to layer 3, where the wheel is volume; device 0 keeps its
    // wheel. Entries left transparent on the device layer use the base layer.
    reset();
    CHECK(DeviceBindings::lastActiveDevice() == DeviceBindings::NO_DEVICE);
    set(3, VKEY_WHEEL_UP, KC_VOLU_);
    set(3, VKEY_WHEEL_DOWN, KC_VOLD_);
    Multiplexer::handleMouseReport(1, 0, 3, 0, 0, 0);  // device 1 moved last
    CHECK(DeviceBindings::lastActiveDevice() == 1);
    CHECK(DeviceBindings::bind(1, 3));
    CHECK(!DeviceBindings::bind(1, 0) && !DeviceBindings::bind(1, NUM_LAYERS));
    CHECK(!DeviceBindings::bind(5, 2));  // not connected
    CHECK(DeviceBindings::layerFor(1) == 3 && DeviceBindings::layerFor(0) == DeviceBindings::NO_LAYER);
    g_sent_keyboard.clear();
    g_sent_mouse.clear();
    Multiplexer::handleMouseReport(0, 0, 0, 0, 1, 0);  // device 0: plain wheel
    CHECK(!g_sent_mouse.empty() && lastMouse().wheel == 1);
    CHECK(g_sent_keyboard.empty());
    g_sent_mouse.clear();
    Multiplexer::handleMouseReport(1, 0, 4, 0, 1, 0);  // device 1: wheel is volume, motion passes
    for (int i = 0; i < 10; i++) Multiplexer::flushKeyboard();
    CHECK(g_sent_keyboard.size() == 2 && g_sent_keyboard[0].keys[0] == 0x80);
    CHECK(!g_sent_mouse.empty() && lastMouse().dx == 4 && lastMouse().wheel == 0);

    // A held layer key outranks the device layer; the device layer outranks the base layer.
    set(0, VKEY_MOUSE_BTN_BASE + 3, 0x5101);       // button 4 -> MO(1)
    set(1, VKEY_WHEEL_UP, 0x04);                   // layer 1: wheel up -> A
    g_sent_keyboard.clear();
    Multiplexer::handleMouseReport(1, 0x08, 0, 0, 1, 0);
    for (int i = 0; i < 10; i++) Multiplexer::flushKeyboard();
    CHECK(g_sent_keyboard.size() == 2 && g_sent_keyboard[0].keys[0] == 0x04);
    Multiplexer::handleMouseReport(1, 0, 0, 0, 0, 0);

    // The binding follows the device's address across reconnects and is removed again.
    Multiplexer::purgeMouse(1);
    g_connected[1] = false;
    CHECK(DeviceBindings::layerFor(1) == DeviceBindings::NO_LAYER);
    g_connected[1] = true;
    CHECK(DeviceBindings::layerFor(1) == 3);
    CHECK(DeviceBindings::entryCount() == 1);
    CHECK(DeviceBindings::unbind(1) && DeviceBindings::layerFor(1) == DeviceBindings::NO_LAYER);
    CHECK(DeviceBindings::entryCount() == 0);

    // The effective layer shown on the OLED is the one of the device used last: its bound layer,
    // unless a layer key selects another one.
    reset();
    CHECK(VirtualMatrix::getEffectiveLayer(DeviceBindings::NO_DEVICE) == 0);
    CHECK(DeviceBindings::bind(1, 6));
    CHECK(VirtualMatrix::getEffectiveLayer(1) == 6);
    CHECK(VirtualMatrix::getEffectiveLayer(0) == 0);
    Multiplexer::handleMouseReport(1, 0, 2, 0, 0, 0);          // touch the bound device
    CHECK(VirtualMatrix::getEffectiveLayer(DeviceBindings::lastActiveDevice()) == 6);
    Multiplexer::handleMouseReport(0, 0, 2, 0, 0, 0);          // touch the other device
    CHECK(VirtualMatrix::getEffectiveLayer(DeviceBindings::lastActiveDevice()) == 0);
    set(0, VKEY_MOUSE_BTN_BASE, 0x5300 + 2);                   // button 1 -> TG(2)
    Multiplexer::handleMouseReport(0, 0x01, 0, 0, 0, 0);       // toggle layer 2 on the other device
    Multiplexer::handleMouseReport(0, 0, 0, 0, 0, 0);
    CHECK(VirtualMatrix::getEffectiveLayer(DeviceBindings::lastActiveDevice()) == 2);
    Multiplexer::handleMouseReport(1, 0, 2, 0, 0, 0);          // the toggled layer outranks the binding
    CHECK(VirtualMatrix::getEffectiveLayer(DeviceBindings::lastActiveDevice()) == 2);
    Multiplexer::handleMouseReport(0, 0x01, 0, 0, 0, 0);       // toggle it off again
    Multiplexer::handleMouseReport(0, 0, 0, 0, 0, 0);
    Multiplexer::handleMouseReport(1, 0, 2, 0, 0, 0);
    CHECK(VirtualMatrix::getEffectiveLayer(DeviceBindings::lastActiveDevice()) == 6);

    // Binding by address (from VIAL) works for devices that are not connected, and a connected device
    // with that address picks it up.
    reset();
    uint8_t addr2[6] = {0, 0, 0, 0, 0, 3};  // device 2's fake address
    g_connected[2] = false;
    CHECK(DeviceBindings::layerForAddress(addr2) == DeviceBindings::NO_LAYER);
    CHECK(DeviceBindings::bindAddress(addr2, 5));
    CHECK(!DeviceBindings::bindAddress(addr2, 0) && !DeviceBindings::bindAddress(addr2, NUM_LAYERS));
    CHECK(DeviceBindings::layerForAddress(addr2) == 5);
    g_connected[2] = true;
    CHECK(DeviceBindings::layerFor(2) == 5);
    CHECK(DeviceBindings::bindAddress(addr2, 4) && DeviceBindings::layerFor(2) == 4);
    CHECK(DeviceBindings::unbindAddress(addr2) && DeviceBindings::layerFor(2) == DeviceBindings::NO_LAYER);
    CHECK(!DeviceBindings::unbindAddress(addr2));

    // Macros: M0 types "aB" (text), M1 holds Shift, taps A, releases Shift, waits 10 ms and types
    // "x", M2 taps LSFT(KC_C) given as a 16-bit keycode.
    reset();
    {
        const uint8_t macros[] = {
            'a', 'B', 0,
            1, 2, 0xE1, 1, 1, 0x04, 1, 3, 0xE1, 1, 4, 11, 1, 'x', 0,
            1, 5, 0x06, 0x02, 0,
        };
        set_macros(macros, sizeof(macros));
    }
    set(0, 0x3A, KC_MACRO_FIRST_ + 0);  // F1 -> M0
    set(0, 0x3B, KC_MACRO_FIRST_ + 1);  // F2 -> M1
    set(0, 0x3C, KC_MACRO_FIRST_ + 2);  // F3 -> M2
    g_sent_keyboard.clear();
    uint8_t f1[1] = {0x3A};
    Multiplexer::handleKeyboardReport(0, 0, f1, 1);
    pump();
    CHECK(!Multiplexer::macroRunning());
    {
        // a down, up, Shift+b down, up (plus reports for the key itself, which sends nothing).
        std::vector<SentKeyboard> typed;
        for (auto &k : g_sent_keyboard) {
            if (k.keys[0] != 0 && (typed.empty() || typed.back().keys[0] != k.keys[0] || typed.back().mods != k.mods)) typed.push_back(k);
        }
        CHECK(typed.size() == 2);
        if (typed.size() == 2) {
            CHECK(typed[0].keys[0] == 0x04 && typed[0].mods == 0);
            CHECK(typed[1].keys[0] == 0x05 && typed[1].mods == 0x02);
        }
        CHECK(lastKbd().keys[0] == 0 && lastKbd().mods == 0);
    }
    Multiplexer::handleKeyboardReport(0, 0, nullptr, 0);

    g_now_ms = 5000;
    g_sent_keyboard.clear();
    uint8_t f2[1] = {0x3B};
    Multiplexer::handleKeyboardReport(0, 0, f2, 1);
    pump();
    CHECK(Multiplexer::macroRunning());  // waiting out the delay
    {
        bool shift_a = false, shift_only = false;
        for (auto &k : g_sent_keyboard) {
            if (k.mods == 0x02 && k.keys[0] == 0x04) shift_a = true;
            if (k.mods == 0x02 && k.keys[0] == 0) shift_only = true;
        }
        CHECK(shift_a && shift_only);
        CHECK(lastKbd().mods == 0 && lastKbd().keys[0] == 0);  // Shift released before the delay
    }
    size_t before = g_sent_keyboard.size();
    g_now_ms = 5005;
    Multiplexer::poll();
    pump();
    CHECK(Multiplexer::macroRunning());
    CHECK(g_sent_keyboard.size() == before);  // nothing typed during the delay
    g_now_ms = 5011;
    Multiplexer::poll();
    pump();
    CHECK(!Multiplexer::macroRunning());
    {
        bool x = false;
        for (size_t i = before; i < g_sent_keyboard.size(); i++) x = x || g_sent_keyboard[i].keys[0] == 0x1B;
        CHECK(x);
    }
    Multiplexer::handleKeyboardReport(0, 0, nullptr, 0);

    g_sent_keyboard.clear();
    uint8_t f3[1] = {0x3C};
    Multiplexer::handleKeyboardReport(0, 0, f3, 1);
    pump();
    {
        bool shift_c = false;
        for (auto &k : g_sent_keyboard) shift_c = shift_c || (k.mods == 0x02 && k.keys[0] == 0x06);
        CHECK(shift_c);
    }
    Multiplexer::handleKeyboardReport(0, 0, nullptr, 0);

    // A macro key while a macro runs is ignored; an empty macro does nothing; unknown characters are
    // skipped.
    g_now_ms = 6000;
    Multiplexer::handleKeyboardReport(0, 0, f2, 1);  // M1 starts, then waits in its delay
    pump();
    CHECK(Multiplexer::macroRunning());
    Multiplexer::handleKeyboardReport(0, 0, nullptr, 0);
    Multiplexer::handleKeyboardReport(0, 0, f1, 1);  // ignored
    g_now_ms = 7000;
    Multiplexer::poll();
    pump();
    CHECK(!Multiplexer::macroRunning());
    Multiplexer::handleKeyboardReport(0, 0, nullptr, 0);
    set(0, 0x3D, KC_MACRO_FIRST_ + 5);  // F4 -> M5, which is empty
    uint8_t f4[1] = {0x3D};
    Multiplexer::handleKeyboardReport(0, 0, f4, 1);
    CHECK(!Multiplexer::macroRunning());
    Multiplexer::handleKeyboardReport(0, 0, nullptr, 0);
    {
        const uint8_t macros[] = {0xE2, 0x82, 0xAC, 'z', 0};  // "€z": the euro sign is skipped
        set_macros(macros, sizeof(macros));
    }
    g_sent_keyboard.clear();
    Multiplexer::handleKeyboardReport(0, 0, f1, 1);
    pump();
    CHECK(!Multiplexer::macroRunning());
    {
        int typed = 0;
        for (auto &k : g_sent_keyboard) if (k.keys[0] != 0) typed++;
        CHECK(typed == 1 && g_sent_keyboard.size() >= 2);
    }
    Multiplexer::handleKeyboardReport(0, 0, nullptr, 0);

    // Macro edits are saved once they have been quiet for a moment.
    g_macro_saves = 0;
    g_now_ms = 8000;
    {
        const uint8_t macros[] = {'q', 0};
        set_macros(macros, sizeof(macros));
    }
    MacroStore::flushPendingSave();
    CHECK(g_macro_saves == 0);
    g_now_ms = 8600;
    MacroStore::flushPendingSave();
    CHECK(g_macro_saves == 1);

    if (g_failures) { printf("%d FAILURES\n", g_failures); return 1; }
    printf("All host tests passed\n");
    return 0;
}
