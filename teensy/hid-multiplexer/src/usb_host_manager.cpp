#include "usb_host_manager.h"
#include "multiplexer.h"
#include "USBHost_t36.h"
#include "logger.h"
#include <Arduino.h>

static USBHost s_usb_host;
static USBHub s_hub1(s_usb_host);
static USBHub s_hub2(s_usb_host);

static KeyboardController s_keyboards[MAX_KEYBOARDS] = {
    KeyboardController(s_usb_host),
    KeyboardController(s_usb_host),
    KeyboardController(s_usb_host),
    KeyboardController(s_usb_host)
};

static USBHIDParser s_hid_parsers[4] = {
    USBHIDParser(s_usb_host),
    USBHIDParser(s_usb_host),
    USBHIDParser(s_usb_host),
    USBHIDParser(s_usb_host)
};

static MouseController s_mice[MAX_MICE] = {
    MouseController(s_usb_host),
    MouseController(s_usb_host),
    MouseController(s_usb_host),
    MouseController(s_usb_host)
};

bool UsbHostManager::prev_kbd_connected_[MAX_KEYBOARDS] = {false};
bool UsbHostManager::prev_mouse_connected_[MAX_MICE] = {false};

// Callback trampolines for keyboard 0
static void onRawPress0(uint8_t k) { Multiplexer::onRawKeyPress(0, k); }
static void onRawRelease0(uint8_t k) { Multiplexer::onRawKeyRelease(0, k); }
static void onExtrasPress0(uint32_t t, uint16_t k) { Multiplexer::onExtrasPress(0, t, k); }
static void onExtrasRelease0(uint32_t t, uint16_t k) { Multiplexer::onExtrasRelease(0, t, k); }

// Callback trampolines for keyboard 1
static void onRawPress1(uint8_t k) { Multiplexer::onRawKeyPress(1, k); }
static void onRawRelease1(uint8_t k) { Multiplexer::onRawKeyRelease(1, k); }
static void onExtrasPress1(uint32_t t, uint16_t k) { Multiplexer::onExtrasPress(1, t, k); }
static void onExtrasRelease1(uint32_t t, uint16_t k) { Multiplexer::onExtrasRelease(1, t, k); }

// Callback trampolines for keyboard 2
static void onRawPress2(uint8_t k) { Multiplexer::onRawKeyPress(2, k); }
static void onRawRelease2(uint8_t k) { Multiplexer::onRawKeyRelease(2, k); }
static void onExtrasPress2(uint32_t t, uint16_t k) { Multiplexer::onExtrasPress(2, t, k); }
static void onExtrasRelease2(uint32_t t, uint16_t k) { Multiplexer::onExtrasRelease(2, t, k); }

// Callback trampolines for keyboard 3
static void onRawPress3(uint8_t k) { Multiplexer::onRawKeyPress(3, k); }
static void onRawRelease3(uint8_t k) { Multiplexer::onRawKeyRelease(3, k); }
static void onExtrasPress3(uint32_t t, uint16_t k) { Multiplexer::onExtrasPress(3, t, k); }
static void onExtrasRelease3(uint32_t t, uint16_t k) { Multiplexer::onExtrasRelease(3, t, k); }

void UsbHostManager::init() {
    s_usb_host.begin();

    s_keyboards[0].attachRawPress(onRawPress0);
    s_keyboards[0].attachRawRelease(onRawRelease0);
    s_keyboards[0].attachExtrasPress(onExtrasPress0);
    s_keyboards[0].attachExtrasRelease(onExtrasRelease0);

    s_keyboards[1].attachRawPress(onRawPress1);
    s_keyboards[1].attachRawRelease(onRawRelease1);
    s_keyboards[1].attachExtrasPress(onExtrasPress1);
    s_keyboards[1].attachExtrasRelease(onExtrasRelease1);

    s_keyboards[2].attachRawPress(onRawPress2);
    s_keyboards[2].attachRawRelease(onRawRelease2);
    s_keyboards[2].attachExtrasPress(onExtrasPress2);
    s_keyboards[2].attachExtrasRelease(onExtrasRelease2);

    s_keyboards[3].attachRawPress(onRawPress3);
    s_keyboards[3].attachRawRelease(onRawRelease3);
    s_keyboards[3].attachExtrasPress(onExtrasPress3);
    s_keyboards[3].attachExtrasRelease(onExtrasRelease3);

    for (uint8_t i = 0; i < MAX_KEYBOARDS; i++) prev_kbd_connected_[i] = false;
    for (uint8_t i = 0; i < MAX_MICE; i++) prev_mouse_connected_[i] = false;

    logger_println("[USB Host] Initialized USBHost_t36 subsystem (Hubs, Keyboards, Mice).");
}

void UsbHostManager::poll() {
    s_usb_host.Task();

    // 1. Process Mice
    for (uint8_t i = 0; i < MAX_MICE; i++) {
        if (s_mice[i].available()) {
            Multiplexer::onMouseMove(
                i,
                s_mice[i].getMouseX(),
                s_mice[i].getMouseY(),
                s_mice[i].getWheel(),
                s_mice[i].getWheelH()
            );
            Multiplexer::onMouseButtons(i, s_mice[i].getButtons());
            s_mice[i].mouseDataClear();
            prev_mouse_connected_[i] = true;
        }
    }

    // 2. Watchdog for disconnects
    for (uint8_t i = 0; i < MAX_KEYBOARDS; i++) {
        bool connected = (bool)s_keyboards[i];
        if (connected != prev_kbd_connected_[i]) {
            if (connected) {
                logger_printf("[USB Host] Keyboard %u connected (VID: %04X, PID: %04X)\n", 
                    i, s_keyboards[i].idVendor(), s_keyboards[i].idProduct());
                s_keyboards[i].LEDS(Multiplexer::getHostLeds());
            } else {
                logger_printf("[USB Host] Keyboard %u disconnected. Purging keys...\n", i);
                Multiplexer::purgeKeyboard(i);
            }
            prev_kbd_connected_[i] = connected;
        }
    }

    // 3. Reverse LED sync (Host PC CapsLock/NumLock -> Downstream keyboards)
    if (Multiplexer::hasLedsChanged()) {
        uint8_t leds = Multiplexer::getHostLeds();
        for (uint8_t i = 0; i < MAX_KEYBOARDS; i++) {
            if (s_keyboards[i]) {
                s_keyboards[i].LEDS(leds);
            }
        }
        Multiplexer::acknowledgeLeds();
    }
}

uint8_t UsbHostManager::getConnectedKeyboardCount() {
    uint8_t count = 0;
    for (uint8_t i = 0; i < MAX_KEYBOARDS; i++) {
        if (s_keyboards[i]) count++;
    }
    return count;
}

uint8_t UsbHostManager::getConnectedMouseCount() {
    uint8_t count = 0;
    for (uint8_t i = 0; i < MAX_MICE; i++) {
        if (prev_mouse_connected_[i]) count++;
    }
    return count;
}
