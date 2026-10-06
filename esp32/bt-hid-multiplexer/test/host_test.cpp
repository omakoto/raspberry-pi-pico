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
int g_usb_fail_count = 0;
bool g_usb_ready = true;
uint32_t g_now_ms = 0;

// Fake flash. Loading finds nothing (a first boot) unless g_flash_loadable is set, which simulates a
// reboot: then whatever was saved last is loaded.
static bool g_flash_loadable = false;
static bool g_keymap_needs_save = false;  // what loadKeymap reports, e.g. after a format upgrade
static int g_saves = 0;
static uint16_t g_flash[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS];
void StorageManager::init() {}
bool StorageManager::loadKeymap(uint16_t km[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS], bool &needs_save) {
    needs_save = false;
    if (!g_flash_loadable) return false;
    memcpy(km, g_flash, sizeof(g_flash));
    needs_save = g_keymap_needs_save;
    return true;
}
void StorageManager::saveKeymap(const uint16_t km[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS]) {
    memcpy(g_flash, km, sizeof(g_flash));
    g_saves++;
}
void StorageManager::clearKeymap() {}
static DeviceBindingEntry g_flash_bindings[MAX_DEVICE_BINDINGS];
static bool g_bindings_need_save = false;  // what loadBindings reports, e.g. after a format upgrade
static int g_binding_saves = 0;
bool StorageManager::loadBindings(DeviceBindingEntry e[MAX_DEVICE_BINDINGS], bool &needs_save) {
    needs_save = false;
    if (!g_flash_loadable) return false;
    memcpy(e, g_flash_bindings, sizeof(g_flash_bindings));
    needs_save = g_bindings_need_save;
    return true;
}
void StorageManager::saveBindings(const DeviceBindingEntry e[MAX_DEVICE_BINDINGS]) {
    memcpy(g_flash_bindings, e, sizeof(g_flash_bindings));
    g_binding_saves++;
}
static int g_macro_saves = 0;
static uint8_t g_flash_macros[MACRO_BUFFER_SIZE];
bool StorageManager::loadMacros(uint8_t *buffer) {
    if (!g_flash_loadable) return false;
    memcpy(buffer, g_flash_macros, sizeof(g_flash_macros));
    return true;
}
void StorageManager::saveMacros(const uint8_t *buffer) {
    memcpy(g_flash_macros, buffer, sizeof(g_flash_macros));
    g_macro_saves++;
}

// Fake Bluetooth addresses: device d is 00:00:00:00:00:d+1, except the ones marked disconnected.
static bool g_connected[MAX_KEYBOARDS];
static bool fake_address(uint8_t dev_idx, uint8_t addr[6]) {
    if (dev_idx >= MAX_KEYBOARDS || !g_connected[dev_idx]) return false;
    memset(addr, 0, 6);
    addr[5] = dev_idx + 1;
    return true;
}

// Fake pairings: device d's address is paired as "Device d" while g_paired_dev[d] is set, and the
// extra addresses 00:00:00:00:01:i (devices that are not connected) as "Extra i" while
// g_paired_extra[i] is set.
static bool g_paired_dev[MAX_KEYBOARDS];
static bool g_paired_extra[32];
static char g_dev_name[MAX_KEYBOARDS][32];
static bool fake_paired(const uint8_t addr[6], char *name, size_t name_size) {
    if (addr[0] || addr[1] || addr[2] || addr[3]) return false;
    if (addr[4] == 0 && addr[5] >= 1 && addr[5] <= MAX_KEYBOARDS && g_paired_dev[addr[5] - 1]) {
        snprintf(name, name_size, "%s", g_dev_name[addr[5] - 1]);
        return true;
    }
    if (addr[4] == 1 && addr[5] < 32 && g_paired_extra[addr[5]]) {
        snprintf(name, name_size, "Extra %u", addr[5]);
        return true;
    }
    return false;
}
static void extra_addr(uint8_t i, uint8_t addr[6]) {
    memset(addr, 0, 6);
    addr[4] = 1;
    addr[5] = i;
}

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

static void reset() {
    g_sent_keyboard.clear();
    g_sent_mouse.clear();
    g_usb_fail_count = 0;
    g_usb_ready = true;
    g_flash_loadable = false;
    g_keymap_needs_save = false;
    g_bindings_need_save = false;
    memset(g_connected, 0, sizeof(g_connected));
    g_connected[0] = g_connected[1] = true;
    for (int d = 0; d < MAX_KEYBOARDS; d++) {
        g_paired_dev[d] = true;
        snprintf(g_dev_name[d], sizeof(g_dev_name[d]), "Device %d", d);
    }
    memset(g_paired_extra, 0, sizeof(g_paired_extra));
    DeviceBindings::init(fake_address, fake_paired);
    DeviceBindings::pairingChanged();
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

// Plays macro index with F1 (pressed, run to the end, released) and returns the reports it sent.
static std::vector<SentKeyboard> play_macro(int index) {
    set(0, 0x3A, KC_MACRO_FIRST_ + index);
    g_sent_keyboard.clear();
    uint8_t f1[1] = {0x3A};
    Multiplexer::handleKeyboardReport(0, 0, f1, 1);
    pump();
    Multiplexer::handleKeyboardReport(0, 0, nullptr, 0);
    return g_sent_keyboard;
}

// The reports with a key down, which a macro sends once per key it types.
static std::vector<SentKeyboard> typed_keys(const std::vector<SentKeyboard> &reports) {
    std::vector<SentKeyboard> typed;
    for (auto &k : reports) {
        if (k.keys[0] != 0) typed.push_back(k);
    }
    return typed;
}

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

    // Transparent keys skip layers that are not switched on, but fall through the ones that are
    // (QMK's rule), and a bound device's own layer comes after them. Layer 1 maps A to C, layer 3
    // maps A to D; button 1 is MO(2), button 2 is TG(1).
    reset();
    set(1, 0x04, 0x06);
    set(3, 0x04, 0x07);
    set(0, VKEY_MOUSE_BTN_BASE, 0x5102);
    set(0, VKEY_MOUSE_BTN_BASE + 1, 0x5300 + 1);
    Multiplexer::handleMouseReport(0, 0x01, 0, 0, 0, 0);      // MO(2): layer 1 is off, A stays A
    Multiplexer::handleKeyboardReport(0, 0, a, 1);
    CHECK(lastKbd().keys[0] == 0x04);
    Multiplexer::handleKeyboardReport(0, 0, nullptr, 0);
    Multiplexer::handleMouseReport(0, 0x03, 0, 0, 0, 0);      // TG(1) too: layer 2 falls to layer 1
    Multiplexer::handleMouseReport(0, 0x01, 0, 0, 0, 0);
    Multiplexer::handleKeyboardReport(0, 0, a, 1);
    CHECK(lastKbd().keys[0] == 0x06);
    Multiplexer::handleKeyboardReport(0, 0, nullptr, 0);
    Multiplexer::handleMouseReport(0, 0, 0, 0, 0, 0);
    Multiplexer::handleKeyboardReport(1, 0, a, 1);            // device 1, also unbound: layer 1 is on
    CHECK(lastKbd().keys[0] == 0x06);
    Multiplexer::handleKeyboardReport(1, 0, nullptr, 0);
    CHECK(DeviceBindings::bind(1, 3));
    Multiplexer::handleKeyboardReport(1, 0, a, 1);            // bound to 3: the unbound TG(1) is ignored
    CHECK(lastKbd().keys[0] == 0x07);
    Multiplexer::handleKeyboardReport(1, 0, nullptr, 0);
    set(3, 0x04, KC_TRNS_);
    Multiplexer::handleMouseReport(1, 0x02, 0, 0, 0, 0);      // its own TG(1) outranks its layer
    Multiplexer::handleMouseReport(1, 0, 0, 0, 0, 0);
    set(3, 0x04, 0x07);
    Multiplexer::handleKeyboardReport(1, 0, a, 1);
    CHECK(lastKbd().keys[0] == 0x06);
    Multiplexer::handleKeyboardReport(1, 0, nullptr, 0);
    set(1, 0x04, KC_TRNS_);                                   // transparent on 1: its own layer next
    Multiplexer::handleKeyboardReport(1, 0, a, 1);
    CHECK(lastKbd().keys[0] == 0x07);
    Multiplexer::handleKeyboardReport(1, 0, nullptr, 0);

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

    // A layer key held on the device outranks the device layer; the device layer outranks the base layer.
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
    // unless a layer key of its group selects another one.
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
    Multiplexer::handleMouseReport(1, 0, 2, 0, 0, 0);          // unbound layer keys skip bound devices
    CHECK(VirtualMatrix::getEffectiveLayer(DeviceBindings::lastActiveDevice()) == 6);
    Multiplexer::handleMouseReport(1, 0x01, 0, 0, 0, 0);       // the bound device toggles layer 2 itself
    Multiplexer::handleMouseReport(1, 0, 0, 0, 0, 0);
    CHECK(VirtualMatrix::getEffectiveLayer(1) == 2);
    Multiplexer::handleMouseReport(0, 0x01, 0, 0, 0, 0);       // the unbound one toggles it off
    Multiplexer::handleMouseReport(0, 0, 0, 0, 0, 0);
    CHECK(VirtualMatrix::getEffectiveLayer(0) == 0 && VirtualMatrix::getEffectiveLayer(1) == 2);
    Multiplexer::handleMouseReport(1, 0x01, 0, 0, 0, 0);
    Multiplexer::handleMouseReport(1, 0, 0, 0, 0, 0);
    CHECK(VirtualMatrix::getEffectiveLayer(1) == 6);

    // Layer keys are shared by the devices bound to the same layer, and by all unbound devices, but
    // not across those groups. Devices 0 and 1 are bound to layer 3, device 2 to layer 4, and devices
    // 3 and 4 are unbound. Button 4 is MO(1); on layer 1, wheel up is A.
    reset();
    for (int d = 2; d <= 4; d++) g_connected[d] = true;
    CHECK(DeviceBindings::bind(0, 3) && DeviceBindings::bind(1, 3) && DeviceBindings::bind(2, 4));
    set(0, VKEY_MOUSE_BTN_BASE + 3, 0x5101);
    set(1, VKEY_WHEEL_UP, 0x04);
    Multiplexer::handleMouseReport(0, 0x08, 0, 0, 0, 0);       // device 0 holds MO(1)
    for (int d = 0; d <= 4; d++) {
        static const uint8_t expected[5] = {1, 1, 4, 0, 0};
        CHECK(VirtualMatrix::getEffectiveLayer(d) == expected[d]);
    }
    g_sent_keyboard.clear();
    g_sent_mouse.clear();
    Multiplexer::handleMouseReport(1, 0, 0, 0, 1, 0);          // device 1 (same layer): wheel up is A
    pump();
    CHECK(g_sent_keyboard.size() == 2 && g_sent_keyboard[0].keys[0] == 0x04);
    g_sent_keyboard.clear();
    Multiplexer::handleMouseReport(2, 0, 0, 0, 1, 0);          // device 2 (other layer): plain wheel
    Multiplexer::handleMouseReport(3, 0, 0, 0, 1, 0);          // device 3 (unbound): plain wheel
    pump();
    CHECK(g_sent_keyboard.empty());
    CHECK(!g_sent_mouse.empty() && lastMouse().wheel == 1);
    Multiplexer::handleMouseReport(0, 0, 0, 0, 0, 0);          // release MO(1)
    CHECK(VirtualMatrix::getEffectiveLayer(1) == 3);
    Multiplexer::handleMouseReport(3, 0x08, 0, 0, 0, 0);       // device 3 (unbound) holds MO(1)
    CHECK(VirtualMatrix::getEffectiveLayer(4) == 1 && VirtualMatrix::getActiveLayer() == 1);
    CHECK(VirtualMatrix::getEffectiveLayer(0) == 3 && VirtualMatrix::getEffectiveLayer(2) == 4);
    // Binding the device while the key is held: the release still ends the layer in the old group.
    CHECK(DeviceBindings::bind(3, 4));
    CHECK(VirtualMatrix::getEffectiveLayer(3) == 4);
    Multiplexer::handleMouseReport(3, 0, 0, 0, 0, 0);
    CHECK(VirtualMatrix::getActiveLayer() == 0 && VirtualMatrix::getEffectiveLayer(4) == 0);
    // A disconnect while held does the same.
    Multiplexer::handleMouseReport(1, 0x08, 0, 0, 0, 0);
    CHECK(VirtualMatrix::getEffectiveLayer(0) == 1);
    Multiplexer::purgeMouse(1);
    CHECK(VirtualMatrix::getEffectiveLayer(0) == 3);

    // A virtual key pressed again while held keeps its first translation, so that the release still
    // ends a layer key. LCtrl (0xE0) is MO(1), and layer 1 maps that position to A. Some keyboards
    // report a held modifier both as a modifier bit and in the key array.
    reset();
    set(0, 0xE0, 0x5101);
    set(1, 0xE0, 0x04);
    uint8_t lctrl[1] = {0xE0};
    Multiplexer::handleKeyboardReport(0, 0x01, lctrl, 1);
    CHECK(VirtualMatrix::getActiveLayer() == 1);
    CHECK(lastKbd().mods == 0 && lastKbd().keys[0] == 0);  // the layer key itself sends nothing
    Multiplexer::handleKeyboardReport(0, 0, nullptr, 0);
    CHECK(VirtualMatrix::getActiveLayer() == 0);
    // The same for a key listed twice in one report.
    set(0, 0x04, 0x5101);
    set(1, 0x04, 0x05);
    uint8_t twice[2] = {0x04, 0x04};
    Multiplexer::handleKeyboardReport(0, 0, twice, 2);
    CHECK(VirtualMatrix::getActiveLayer() == 1);
    Multiplexer::handleKeyboardReport(0, 0, nullptr, 0);
    CHECK(VirtualMatrix::getActiveLayer() == 0);

    // A report that USB does not accept is sent on the next flush, so that a release is not lost.
    reset();
    uint8_t key_a[1] = {0x04};
    Multiplexer::handleKeyboardReport(0, 0x02, key_a, 1);       // LShift + A
    CHECK(lastKbd().mods == 0x02 && lastKbd().keys[0] == 0x04);
    g_sent_keyboard.clear();
    g_usb_fail_count = 1;
    Multiplexer::handleKeyboardReport(0, 0, nullptr, 0);
    CHECK(g_sent_keyboard.empty());
    Multiplexer::flushKeyboard();
    CHECK(g_sent_keyboard.size() == 1 && lastKbd().mods == 0 && lastKbd().keys[0] == 0);
    Multiplexer::flushKeyboard();
    CHECK(g_sent_keyboard.size() == 1);                          // and only once
    // The same for a mouse button release.
    Multiplexer::handleMouseReport(0, 0x01, 0, 0, 0, 0);
    CHECK(lastMouse().buttons == 0x01);
    g_sent_mouse.clear();
    g_usb_fail_count = 2;                                        // the keyboard and the mouse flush
    Multiplexer::handleMouseReport(0, 0, 3, 0, 0, 0);
    CHECK(g_sent_mouse.empty());
    Multiplexer::flushMouse();
    CHECK(g_sent_mouse.size() == 1 && lastMouse().buttons == 0 && lastMouse().dx == 3);
    Multiplexer::flushMouse();
    CHECK(g_sent_mouse.size() == 1);
    // And for the release half of a tap (wheel up mapped to LShift).
    set(0, VKEY_WHEEL_UP, 0xE1);
    g_sent_keyboard.clear();
    Multiplexer::handleMouseReport(0, 0, 0, 0, 1, 0);           // sends the tap's press
    CHECK(g_sent_keyboard.size() == 1 && lastKbd().mods == 0x02);
    g_usb_fail_count = 1;
    Multiplexer::flushKeyboard();                                // its release fails
    CHECK(g_sent_keyboard.size() == 1);
    pump();
    CHECK(g_sent_keyboard.size() == 2 && lastKbd().mods == 0);
    // resendState() sends the current state again even though nothing changed.
    Multiplexer::handleKeyboardReport(0, 0x02, key_a, 1);
    g_sent_keyboard.clear();
    g_sent_mouse.clear();
    Multiplexer::resendState();
    Multiplexer::flushKeyboard();
    Multiplexer::flushMouse();
    CHECK(g_sent_keyboard.size() == 1 && lastKbd().mods == 0x02 && lastKbd().keys[0] == 0x04);
    CHECK(g_sent_mouse.size() == 1 && lastMouse().buttons == 0);
    Multiplexer::flushKeyboard();
    Multiplexer::flushMouse();
    CHECK(g_sent_keyboard.size() == 1 && g_sent_mouse.size() == 1);

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

    // A device that disconnects while keys are held releases them, including a held layer key, and
    // the keys of other devices stay down. Device 0 holds button 4 (MO(1)) and Shift+A; device 1
    // holds B. Layer 1 maps A to D and B to C. The firmware purges both halves of a device on
    // disconnect.
    reset();
    set(0, VKEY_MOUSE_BTN_BASE + 3, 0x5101);
    set(1, 0x04, 0x07);
    set(1, 0x05, 0x06);
    uint8_t dc_a[1] = {0x04};
    uint8_t dc_b[1] = {0x05};
    Multiplexer::handleMouseReport(0, 0x08, 0, 0, 0, 0);
    Multiplexer::handleKeyboardReport(0, 0x02, dc_a, 1);
    Multiplexer::handleKeyboardReport(1, 0, dc_b, 1);
    CHECK(VirtualMatrix::getActiveLayer() == 1);
    CHECK(lastKbd().mods == 0x02 && lastKbd().keys[0] == 0x07 && lastKbd().keys[1] == 0x06);
    Multiplexer::purgeKeyboard(0);
    Multiplexer::purgeMouse(0);
    CHECK(VirtualMatrix::getActiveLayer() == 0);
    CHECK(lastKbd().mods == 0 && lastKbd().keys[0] == 0x06 && lastKbd().keys[1] == 0);
    CHECK(DeviceBindings::lastActiveDevice() == 1);  // only the device that went away is forgotten
    // After reconnecting, A is looked up again on the base layer rather than keeping what it was
    // translated to before the disconnect.
    Multiplexer::handleKeyboardReport(0, 0, dc_a, 1);
    CHECK(lastKbd().keys[0] == 0x04 && lastKbd().keys[1] == 0x06);
    Multiplexer::purgeKeyboard(0);
    CHECK(DeviceBindings::lastActiveDevice() == DeviceBindings::NO_DEVICE);
    CHECK(lastKbd().keys[0] == 0x06);
    Multiplexer::purgeKeyboard(1);
    CHECK(lastKbd().mods == 0 && lastKbd().keys[0] == 0);

    // Host LEDs (Caps Lock etc.): the first state always counts as changed, so that it is sent to the
    // devices once; after that only real changes do.
    reset();
    CHECK(Multiplexer::getHostLeds() == 0);
    CHECK(Multiplexer::hasLedsChanged());
    Multiplexer::acknowledgeLeds();
    CHECK(!Multiplexer::hasLedsChanged());
    Multiplexer::setHostLeds(0x02);
    CHECK(Multiplexer::getHostLeds() == 0x02 && Multiplexer::hasLedsChanged());
    Multiplexer::acknowledgeLeds();
    CHECK(!Multiplexer::hasLedsChanged());
    Multiplexer::setHostLeds(0x02);
    CHECK(!Multiplexer::hasLedsChanged());

    // Keys held across reports stay down while others come and go.
    reset();
    uint8_t ka[1] = {0x04};
    uint8_t kab[2] = {0x04, 0x05};
    uint8_t kb[1] = {0x05};
    Multiplexer::handleKeyboardReport(0, 0, ka, 1);
    Multiplexer::handleKeyboardReport(0, 0, kab, 2);
    CHECK(lastKbd().keys[0] == 0x04 && lastKbd().keys[1] == 0x05);
    Multiplexer::handleKeyboardReport(0, 0, kb, 1);
    CHECK(lastKbd().keys[0] == 0x05 && lastKbd().keys[1] == 0);
    Multiplexer::handleKeyboardReport(0, 0, nullptr, 0);

    // Two keys mapped to the same keycode send it once; a key mapped to cursor movement sends
    // nothing (mouse keys are only for mouse motion).
    set(0, 0x05, 0x04);  // B -> A
    Multiplexer::handleKeyboardReport(0, 0, kab, 2);
    CHECK(lastKbd().keys[0] == 0x04 && lastKbd().keys[1] == 0);
    Multiplexer::handleKeyboardReport(0, 0, nullptr, 0);
    set(0, 0x06, KC_MS_U_);  // C -> mouse up
    uint8_t kc_c[1] = {0x06};
    g_sent_mouse.clear();
    Multiplexer::handleKeyboardReport(0, 0, kc_c, 1);
    CHECK(lastKbd().mods == 0 && lastKbd().keys[0] == 0);
    CHECK(g_sent_mouse.empty());
    Multiplexer::handleKeyboardReport(0, 0, nullptr, 0);

    // A report holds at most 6 keys: a device's 7th key, and keys of other devices beyond 6, are
    // dropped.
    reset();
    uint8_t seven[7] = {0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A};
    Multiplexer::handleKeyboardReport(0, 0, seven, 7);
    CHECK(lastKbd().keys[0] == 0x04 && lastKbd().keys[5] == 0x09);
    uint8_t kz[1] = {0x1D};
    Multiplexer::handleKeyboardReport(1, 0, kz, 1);
    CHECK(lastKbd().keys[5] == 0x09);
    for (int i = 0; i < 6; i++) CHECK(lastKbd().keys[i] != 0x1D && lastKbd().keys[i] != 0x0A);

    // Motion can be remapped to any direction: here the axes are swapped, and real pan scrolls
    // horizontally (the default), as does left motion remapped to it.
    reset();
    set(0, VKEY_MOTION_RIGHT, KC_MS_D_);
    set(0, VKEY_MOTION_DOWN, KC_MS_L_);
    Multiplexer::handleMouseReport(0, 0, 5, 7, 0, 0);
    CHECK(lastMouse().dx == -7 && lastMouse().dy == 5);
    Multiplexer::handleMouseReport(0, 0, 0, 0, 0, 1);
    CHECK(lastMouse().pan == 1 && lastMouse().dx == 0 && lastMouse().dy == 0);
    Multiplexer::handleMouseReport(0, 0, 0, 0, 0, -2);
    CHECK(lastMouse().pan == -2);
    set(0, VKEY_MOTION_LEFT, KC_WH_L_);
    Multiplexer::handleMouseReport(0, 0, -30, 0, 0, 0);  // 30 counts = 1 notch left, 6 left over
    CHECK(lastMouse().pan == -1 && lastMouse().dx == 0);
    Multiplexer::handleMouseReport(0, 0, -18, 0, 0, 0);  // 6 + 18 = 24 -> one more notch
    CHECK(lastMouse().pan == -1);

    // Motion mapped to a mouse button or a layer key is dropped (they have no press/release here).
    reset();
    set(0, VKEY_WHEEL_UP, KC_BTN1_);
    set(0, VKEY_WHEEL_DOWN, 0x5101);
    g_sent_keyboard.clear();
    g_sent_mouse.clear();
    Multiplexer::handleMouseReport(0, 0, 0, 0, 3, 0);
    Multiplexer::handleMouseReport(0, 0, 0, 0, -3, 0);
    pump();
    CHECK(g_sent_keyboard.empty() && g_sent_mouse.empty());
    CHECK(VirtualMatrix::getActiveLayer() == 0);

    // Fast motion mapped to a key queues at most 16 taps; the rest is dropped instead of typing on
    // long after the motion stops.
    set(0, VKEY_WHEEL_UP, 0x04);
    Multiplexer::handleMouseReport(0, 0, 0, 0, 100, 0);
    pump();
    CHECK(g_sent_keyboard.size() == 32);

    // Motion larger than a report can carry (-127..127) is sent over several reports.
    reset();
    g_sent_mouse.clear();
    Multiplexer::handleMouseReport(0, 0, 300, -200, 0, 0);
    CHECK(g_sent_mouse.size() == 1 && lastMouse().dx == 127 && lastMouse().dy == -127);
    Multiplexer::flushMouse();
    CHECK(g_sent_mouse.size() == 2 && lastMouse().dx == 127 && lastMouse().dy == -73);
    Multiplexer::flushMouse();
    CHECK(g_sent_mouse.size() == 3 && lastMouse().dx == 46 && lastMouse().dy == 0);
    Multiplexer::flushMouse();
    CHECK(g_sent_mouse.size() == 3);

    // While USB is busy nothing is sent; the state goes out once it is ready.
    reset();
    g_usb_ready = false;
    Multiplexer::handleKeyboardReport(0, 0, ka, 1);
    Multiplexer::handleMouseReport(0, 0x01, 0, 0, 0, 0);
    CHECK(g_sent_keyboard.empty() && g_sent_mouse.empty());
    g_usb_ready = true;
    Multiplexer::flushKeyboard();
    Multiplexer::flushMouse();
    CHECK(g_sent_keyboard.size() == 1 && lastKbd().keys[0] == 0x04);
    CHECK(g_sent_mouse.size() == 1 && lastMouse().buttons == 0x01);

    // TO(n) switches the base layer and clears toggled layers; releasing it does not switch back.
    // Button 3 is TG(1), button 1 is TO(2), and layer 2 maps button 2 to TO(0) and A to B.
    reset();
    set(0, VKEY_MOUSE_BTN_BASE + 2, 0x5301);
    set(0, VKEY_MOUSE_BTN_BASE, 0x5000 + 2);
    set(2, VKEY_MOUSE_BTN_BASE + 1, 0x5000 + 0);
    set(2, 0x04, 0x05);
    Multiplexer::handleMouseReport(0, 0x04, 0, 0, 0, 0);
    Multiplexer::handleMouseReport(0, 0, 0, 0, 0, 0);
    CHECK(VirtualMatrix::getActiveLayer() == 1);
    Multiplexer::handleMouseReport(0, 0x01, 0, 0, 0, 0);
    Multiplexer::handleMouseReport(0, 0, 0, 0, 0, 0);
    CHECK(VirtualMatrix::getActiveLayer() == 2);
    Multiplexer::handleKeyboardReport(0, 0, ka, 1);
    CHECK(lastKbd().keys[0] == 0x05);
    Multiplexer::handleKeyboardReport(0, 0, nullptr, 0);
    Multiplexer::handleMouseReport(0, 0x02, 0, 0, 0, 0);
    Multiplexer::handleMouseReport(0, 0, 0, 0, 0, 0);
    CHECK(VirtualMatrix::getActiveLayer() == 0);
    Multiplexer::handleKeyboardReport(0, 0, ka, 1);
    CHECK(lastKbd().keys[0] == 0x04);
    Multiplexer::handleKeyboardReport(0, 0, nullptr, 0);

    // A key that is transparent on every layer, layer 0 included, is disabled.
    set(0, 0x07, KC_TRNS_);
    uint8_t kd[1] = {0x07};
    Multiplexer::handleKeyboardReport(0, 0, kd, 1);
    CHECK(lastKbd().mods == 0 && lastKbd().keys[0] == 0);
    Multiplexer::handleKeyboardReport(0, 0, nullptr, 0);

    // Macro corner cases.
    reset();
    {
        const uint8_t macros[] = {
            // M0: Shift and A down, then the macro ends.
            1, 2, 0xE1, 1, 2, 0x04, 0,
            // M1: all 8 modifiers down; a macro holds at most 6 keys.
            1, 2, 0xE0, 1, 2, 0xE1, 1, 2, 0xE2, 1, 2, 0xE3, 1, 2, 0xE4, 1, 2, 0xE5, 1, 2, 0xE6,
            1, 2, 0xE7, 0,
            // M2: "a", an unknown action (skipped), "b".
            'a', 1, 9, 'b', 0,
            // M3-M6: "c" to "f", each followed by an action that is cut short by the macro's end.
            'c', 1, 0,
            'd', 1, 1, 0,
            'e', 1, 5, 0x04, 0,
            'f', 1, 4, 2, 0,
            // M7: LSFT(KC_NO), a 16-bit keycode whose low byte is 0 (encoded as 0xFF02).
            1, 5, 0x02, 0xFF, 0,
            // M8: releases a key it did not press, then types "g".
            1, 3, 0x04, 'g', 0,
        };
        set_macros(macros, sizeof(macros));
    }
    {
        // Keys a macro leaves down are released when it ends, so that they cannot get stuck.
        std::vector<SentKeyboard> r = play_macro(0);
        bool held = false;
        for (auto &k : r) held = held || (k.mods == 0x02 && k.keys[0] == 0x04);
        CHECK(held);
        CHECK(!r.empty() && r.back().mods == 0 && r.back().keys[0] == 0);
        CHECK(!Multiplexer::macroRunning());
    }
    {
        std::vector<SentKeyboard> r = play_macro(1);
        uint8_t most = 0;
        for (auto &k : r) most |= k.mods;
        CHECK(most == 0x3F);
        CHECK(!r.empty() && r.back().mods == 0);
    }
    {
        std::vector<SentKeyboard> t = typed_keys(play_macro(2));
        CHECK(t.size() == 2 && t[0].keys[0] == 0x04 && t[1].keys[0] == 0x05);
    }
    for (int m = 3; m <= 6; m++) {
        std::vector<SentKeyboard> t = typed_keys(play_macro(m));
        CHECK(t.size() == 1 && t[0].keys[0] == 0x06 + (m - 3));
        CHECK(!Multiplexer::macroRunning());
    }
    {
        std::vector<SentKeyboard> r = play_macro(7);
        bool shift = false;
        for (auto &k : r) shift = shift || (k.mods == 0x02 && k.keys[0] == 0);
        CHECK(shift);
        CHECK(!r.empty() && r.back().mods == 0);
    }
    {
        std::vector<SentKeyboard> t = typed_keys(play_macro(8));
        CHECK(t.size() == 1 && t[0].keys[0] == 0x0A);
    }

    // Macro text is typed with US layout keycodes, shifted where needed.
    {
        static const struct { char c; uint8_t mods; uint8_t key; } chars[] = {
            {'0', 0, 0x27}, {'\n', 0, 0x28}, {'\t', 0, 0x2B}, {'\b', 0, 0x2A}, {' ', 0, 0x2C},
            {'-', 0, 0x2D}, {'=', 0, 0x2E}, {'[', 0, 0x2F}, {']', 0, 0x30}, {'\\', 0, 0x31},
            {';', 0, 0x33}, {'\'', 0, 0x34}, {'`', 0, 0x35}, {',', 0, 0x36}, {'.', 0, 0x37},
            {'/', 0, 0x38}, {'!', 2, 0x1E}, {'@', 2, 0x1F}, {'#', 2, 0x20}, {'$', 2, 0x21},
            {'%', 2, 0x22}, {'^', 2, 0x23}, {'&', 2, 0x24}, {'*', 2, 0x25}, {'(', 2, 0x26},
            {')', 2, 0x27}, {'_', 2, 0x2D}, {'+', 2, 0x2E}, {'{', 2, 0x2F}, {'}', 2, 0x30},
            {'|', 2, 0x31}, {':', 2, 0x33}, {'"', 2, 0x34}, {'~', 2, 0x35}, {'<', 2, 0x36},
            {'>', 2, 0x37}, {'?', 2, 0x38}, {'z', 0, 0x1D}, {'Z', 2, 0x1D}, {'9', 0, 0x26},
        };
        const int n = sizeof(chars) / sizeof(chars[0]);
        uint8_t text[n + 1];
        for (int i = 0; i < n; i++) text[i] = (uint8_t)chars[i].c;
        text[n] = 0;
        set_macros(text, sizeof(text));
        std::vector<SentKeyboard> t = typed_keys(play_macro(0));
        CHECK(t.size() == (size_t)n);
        for (int i = 0; i < n && i < (int)t.size(); i++) {
            if (t[i].mods != chars[i].mods || t[i].keys[0] != chars[i].key) {
                printf("FAIL %s:%d: macro typed '%c' as mods %02x key %02x\n", __FILE__, __LINE__,
                       chars[i].c, t[i].mods, t[i].keys[0]);
                g_failures++;
            }
        }
    }

    // The macro buffer as VIAL reads and resets it. Reads past the end return zeros, writes past
    // the end are ignored, and a macro without a NUL ends at the end of the buffer.
    reset();
    {
        const uint8_t macros[] = {'h', 'i', 0};
        set_macros(macros, sizeof(macros));
        uint8_t out[4];
        memset(out, 0xAA, sizeof(out));
        MacroStore::read(0, 4, out);
        CHECK(out[0] == 'h' && out[1] == 'i' && out[2] == 0 && out[3] == 0);
        CHECK(MacroStore::buffer()[1] == 'i');
        const uint8_t tail[2] = {7, 8};
        MacroStore::write(MACRO_BUFFER_SIZE - 1, 2, tail);
        memset(out, 0xAA, sizeof(out));
        MacroStore::read(MACRO_BUFFER_SIZE - 1, 3, out);
        CHECK(out[0] == 7 && out[1] == 0 && out[2] == 0);

        uint8_t fill[128];
        memset(fill, 'a', sizeof(fill));
        for (int off = 0; off < MACRO_BUFFER_SIZE; off += sizeof(fill)) {
            MacroStore::write((uint16_t)off, sizeof(fill), fill);
        }
        const uint8_t *start = nullptr, *end = nullptr;
        CHECK(MacroStore::find(0, &start, &end));
        CHECK(start == MacroStore::buffer() && end == MacroStore::buffer() + MACRO_BUFFER_SIZE);
        CHECK(!MacroStore::find(1, &start, &end));

        // A reset clears the buffer and saves it right away; the edits before it are not saved again.
        g_macro_saves = 0;
        MacroStore::reset();
        CHECK(g_macro_saves == 1 && MacroStore::buffer()[0] == 0);
        g_now_ms += 1000;
        MacroStore::flushPendingSave();
        CHECK(g_macro_saves == 1);
    }

    // Binding entries as the console lists them: in table order, skipping removed ones, with the
    // paired device's name.
    reset();
    {
        DeviceBindingEntry e;
        CHECK(DeviceBindings::bind(0, 1) && DeviceBindings::bind(1, 2));
        CHECK(DeviceBindings::getEntry(0, &e) && e.addr[5] == 1 && e.layer == 1 && strcmp(e.name, "Device 0") == 0);
        CHECK(DeviceBindings::getEntry(1, &e) && e.addr[5] == 2 && e.layer == 2 && e.unpaired_seq == 0);
        CHECK(!DeviceBindings::getEntry(2, &e));
        CHECK(DeviceBindings::unbind(0));
        CHECK(DeviceBindings::getEntry(0, &e) && e.addr[5] == 2 && e.layer == 2);
        CHECK(!DeviceBindings::unbind(0));  // no longer bound
        g_connected[1] = false;
        CHECK(!DeviceBindings::unbind(1));  // not connected
    }

    // A binding outlives the device's pairing: it becomes an unpaired binding, still applies to the
    // address, and is a paired one again when the device is paired again. A renamed device's binding
    // takes the new name.
    reset();
    {
        DeviceBindingEntry e;
        CHECK(DeviceBindings::bind(1, 3));
        CHECK(DeviceBindings::unpairedCount() == 0);
        g_paired_dev[1] = false;
        DeviceBindings::pairingChanged();
        CHECK(DeviceBindings::unpairedCount() == 1);
        CHECK(DeviceBindings::getUnpaired(0, &e) && e.addr[5] == 2 && e.layer == 3 && strcmp(e.name, "Device 1") == 0);
        CHECK(!DeviceBindings::getUnpaired(1, &e));
        CHECK(DeviceBindings::layerFor(1) == 3);
        g_paired_dev[1] = true;
        strcpy(g_dev_name[1], "Renamed");
        DeviceBindings::pairingChanged();
        CHECK(DeviceBindings::unpairedCount() == 0 && DeviceBindings::layerFor(1) == 3);
        CHECK(DeviceBindings::getEntry(0, &e) && strcmp(e.name, "Renamed") == 0 && e.unpaired_seq == 0);

        // Removing an unpaired binding (VIAL's checkbox), and putting it back as it was.
        g_paired_dev[1] = false;
        DeviceBindings::pairingChanged();
        CHECK(DeviceBindings::getUnpaired(0, &e));
        CHECK(DeviceBindings::unbindAddress(e.addr) && DeviceBindings::unpairedCount() == 0);
        CHECK(DeviceBindings::layerFor(1) == DeviceBindings::NO_LAYER);
        CHECK(DeviceBindings::bindAddress(e.addr, e.layer, e.name));
        DeviceBindingEntry back;
        CHECK(DeviceBindings::getUnpaired(0, &back) && back.layer == 3 && strcmp(back.name, "Renamed") == 0);
        CHECK(DeviceBindings::layerFor(1) == 3);
    }

    // Only the MAX_UNPAIRED_BINDINGS most recently unpaired bindings are kept, newest first. Ten
    // devices are bound to layer 2, then unpaired one at a time; the first two are dropped.
    reset();
    {
        uint8_t addr[6];
        for (uint8_t i = 0; i < 10; i++) {
            g_paired_extra[i] = true;
            extra_addr(i, addr);
            CHECK(DeviceBindings::bindAddress(addr, 2));
        }
        CHECK(DeviceBindings::unpairedCount() == 0 && DeviceBindings::entryCount() == 10);
        for (uint8_t i = 0; i < 10; i++) {
            g_paired_extra[i] = false;
            DeviceBindings::pairingChanged();
        }
        CHECK(DeviceBindings::unpairedCount() == MAX_UNPAIRED_BINDINGS);
        CHECK(DeviceBindings::entryCount() == MAX_UNPAIRED_BINDINGS);
        for (uint8_t i = 0; i < 10; i++) {
            extra_addr(i, addr);
            CHECK(DeviceBindings::layerForAddress(addr) == (i < 2 ? DeviceBindings::NO_LAYER : 2));
        }
        DeviceBindingEntry e;
        for (uint8_t k = 0; k < MAX_UNPAIRED_BINDINGS; k++) {
            char name[16];
            snprintf(name, sizeof(name), "Extra %u", 9 - k);
            CHECK(DeviceBindings::getUnpaired(k, &e) && e.addr[5] == 9 - k && strcmp(e.name, name) == 0);
        }
        CHECK(!DeviceBindings::getUnpaired(MAX_UNPAIRED_BINDINGS, &e));

        // A device bound while not paired (e.g. restored) is an unpaired binding at once and pushes
        // out the oldest one.
        extra_addr(20, addr);
        CHECK(DeviceBindings::bindAddress(addr, 5, "Old mouse"));
        CHECK(DeviceBindings::unpairedCount() == MAX_UNPAIRED_BINDINGS);
        CHECK(DeviceBindings::getUnpaired(0, &e) && e.addr[5] == 20 && strcmp(e.name, "Old mouse") == 0);
        extra_addr(2, addr);
        CHECK(DeviceBindings::layerForAddress(addr) == DeviceBindings::NO_LAYER);

        // Unpairing several at once (clearing all bonds) keeps the cap too.
        for (uint8_t i = 0; i < 8; i++) {
            g_paired_extra[21 + i] = true;
            extra_addr(21 + i, addr);
            CHECK(DeviceBindings::bindAddress(addr, 4));
        }
        CHECK(DeviceBindings::entryCount() == MAX_DEVICE_BINDINGS);
        // With 8 paired and 8 unpaired bindings the table is full.
        g_paired_extra[30] = true;
        extra_addr(30, addr);
        CHECK(!DeviceBindings::bindAddress(addr, 4));
        memset(g_paired_extra, 0, sizeof(g_paired_extra));
        DeviceBindings::pairingChanged();
        CHECK(DeviceBindings::unpairedCount() == MAX_UNPAIRED_BINDINGS);
        CHECK(DeviceBindings::getUnpaired(0, &e) && e.layer == 4);
        CHECK(DeviceBindings::getUnpaired(MAX_UNPAIRED_BINDINGS - 1, &e) && e.layer == 4);
    }

    // The keymap, bindings and macros come back after a reboot. A keymap that the storage layer
    // upgraded on loading is saved again in the new format.
    reset();
    {
        uint8_t addr[6] = {0, 0, 0, 0, 2, 1};
        set(1, 0x04, 0x05);
        CHECK(DeviceBindings::bindAddress(addr, 3));
        const uint8_t macros[] = {'m', 0};
        set_macros(macros, sizeof(macros));
        g_now_ms += 1000;
        VirtualMatrix::flushPendingSave();
        MacroStore::flushPendingSave();

        g_flash_loadable = true;
        g_keymap_needs_save = true;
        g_saves = 0;
        DeviceBindings::init(fake_address, fake_paired);
        MacroStore::init();
        Multiplexer::init();
        VirtualMatrix::init();
        CHECK(VirtualMatrix::getKeycode(1, 0, 4) == 0x05);
        CHECK(g_saves == 1);
        CHECK(DeviceBindings::layerForAddress(addr) == 3);
        CHECK(MacroStore::buffer()[0] == 'm' && MacroStore::buffer()[1] == 0);
        g_keymap_needs_save = false;
        VirtualMatrix::init();
        CHECK(g_saves == 1);

        // Bindings stored in an older format are written back in the current one, once.
        g_binding_saves = 0;
        DeviceBindings::init(fake_address, fake_paired);
        CHECK(g_binding_saves == 0);
        g_bindings_need_save = true;
        DeviceBindings::init(fake_address, fake_paired);
        CHECK(g_binding_saves == 1 && DeviceBindings::layerForAddress(addr) == 3);
    }

    // Device indexes out of range are ignored.
    reset();
    {
        g_sent_keyboard.clear();
        g_sent_mouse.clear();
        Multiplexer::handleKeyboardReport(MAX_KEYBOARDS, 0, ka, 1);
        Multiplexer::handleMouseReport(MAX_MICE, 0x01, 5, 5, 0, 0);
        Multiplexer::purgeKeyboard(MAX_KEYBOARDS);
        Multiplexer::purgeMouse(MAX_MICE);
        CHECK(g_sent_keyboard.empty() && g_sent_mouse.empty());
        uint16_t out_kc = 0x1234;
        CHECK(!VirtualMatrix::processKeyPress(MAX_KEYBOARDS, 0x04, out_kc));
        CHECK(!VirtualMatrix::processKeyRelease(MAX_KEYBOARDS, 0x04, out_kc));
        CHECK(VirtualMatrix::getActiveTranslation(MAX_KEYBOARDS, 0x04) == 0);
        VirtualMatrix::purgeDevice(MAX_KEYBOARDS);
        CHECK(VirtualMatrix::getKeycode(0, MATRIX_ROWS, 0) == 0 && VirtualMatrix::getKeycode(0, 0, MATRIX_COLS) == 0);
        g_saves = 0;
        VirtualMatrix::setKeycode(NUM_LAYERS, 0, 0, 0x05);
        VirtualMatrix::setKeycode(0, MATRIX_ROWS, 0, 0x05);
        g_now_ms += 1000;
        VirtualMatrix::flushPendingSave();
        CHECK(g_saves == 0);
        CHECK(DeviceBindings::layerFor(MAX_KEYBOARDS) == DeviceBindings::NO_LAYER);
        uint8_t addr[6];
        CHECK(!DeviceBindings::addressOf(MAX_KEYBOARDS, addr));
        Multiplexer::handleKeyboardReport(0, 0, ka, 1);
        DeviceBindings::noteActivity(MAX_KEYBOARDS);
        CHECK(DeviceBindings::lastActiveDevice() == 0);
    }

    if (g_failures) { printf("%d FAILURES\n", g_failures); return 1; }
    printf("All host tests passed\n");
    return 0;
}
