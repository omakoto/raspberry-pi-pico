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

#define MAX_HID_PARSERS 8

static USBHIDParser s_hid_parsers[MAX_HID_PARSERS] = {
    USBHIDParser(s_usb_host),
    USBHIDParser(s_usb_host),
    USBHIDParser(s_usb_host),
    USBHIDParser(s_usb_host),
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

static inline USBHIDInput& hidInput(MouseController &m) {
    return static_cast<USBHIDInput&>(m);
}

void UsbHostManager::poll() {
    s_usb_host.Task();

    // 1. Process Mice movement and buttons
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
        }

        // Watchdog for mouse connect / disconnect
        bool mouse_conn = (bool)hidInput(s_mice[i]);
        if (mouse_conn != prev_mouse_connected_[i]) {
            if (mouse_conn) {
                logger_printf("[USB Host] Mouse %u connected (VID: %04X, PID: %04X)\n",
                    i, hidInput(s_mice[i]).idVendor(), hidInput(s_mice[i]).idProduct());
            } else {
                logger_printf("[USB Host] Mouse %u disconnected. Purging buttons...\n", i);
                Multiplexer::purgeMouse(i);
            }
            prev_mouse_connected_[i] = mouse_conn;
        }
    }

    // 2. Watchdog for keyboard connect / disconnect
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

bool UsbHostManager::isKeyboardConnected(uint8_t dev_idx) {
    if (dev_idx >= MAX_KEYBOARDS) return false;
    return (bool)s_keyboards[dev_idx];
}

bool UsbHostManager::isMouseConnected(uint8_t dev_idx) {
    if (dev_idx >= MAX_MICE) return false;
    return (bool)hidInput(s_mice[dev_idx]);
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
        if (hidInput(s_mice[i])) count++;
    }
    return count;
}

void UsbHostManager::printDiagnostics() {
    logger_println("\n--- USB Host Diagnostic Report ---");

    // Root port status (from IMXRT EHCI controller register)
    uint32_t portsc = USBHS_PORTSC1;
    const char *speed_str = "Full (12M)";
    uint32_t spd = (portsc >> 26) & 3;
    if (spd == 2) speed_str = "High (480M)";
    else if (spd == 1) speed_str = "Low (1.5M)";

    logger_printf("EHCI Root Port: [0x%08X] %s, %s, Speed: %s\n",
        portsc,
        (portsc & USBHS_PORTSC_CCS) ? "Device Present" : "Empty",
        (portsc & USBHS_PORTSC_PE) ? "Port Enabled" : "Disabled",
        speed_str
    );
    if (portsc & USBHS_PORTSC_OCC) {
        logger_println("  WARNING: Over-current condition detected on host port!");
    }

    // Hubs
    logger_printf("Hub 1: %s\n", s_hub1 ? "Active" : "Not detected");
    logger_printf("Hub 2: %s\n", s_hub2 ? "Active" : "Not detected");

    // Keyboards
    uint8_t kbd_count = 0;
    for (uint8_t i = 0; i < MAX_KEYBOARDS; i++) {
        if (s_keyboards[i]) {
            kbd_count++;
            const uint8_t *mfr = s_keyboards[i].manufacturer();
            const uint8_t *prod = s_keyboards[i].product();
            logger_printf("  Keyboard %u: VID:PID = %04X:%04X, Mfr: \"%s\", Product: \"%s\"\n",
                i, s_keyboards[i].idVendor(), s_keyboards[i].idProduct(),
                mfr ? (const char *)mfr : "N/A",
                prod ? (const char *)prod : "N/A");
        }
    }
    if (kbd_count == 0) {
        logger_println("  Keyboards: None connected");
    }

    // Mice
    uint8_t mouse_count = 0;
    for (uint8_t i = 0; i < MAX_MICE; i++) {
        if (hidInput(s_mice[i])) {
            mouse_count++;
            const uint8_t *mfr = hidInput(s_mice[i]).manufacturer();
            const uint8_t *prod = hidInput(s_mice[i]).product();
            logger_printf("  Mouse %u: VID:PID = %04X:%04X, Mfr: \"%s\", Product: \"%s\"\n",
                i, hidInput(s_mice[i]).idVendor(), hidInput(s_mice[i]).idProduct(),
                mfr ? (const char *)mfr : "N/A",
                prod ? (const char *)prod : "N/A");
        }
    }
    if (mouse_count == 0) {
        logger_println("  Mice: None connected");
    }

    // Driver resource pool status
    uint32_t free_devs = 0, free_pipes = 0, free_transfers = 0, free_strings = 0;
    USBHost::countFree(free_devs, free_pipes, free_transfers, free_strings);
    logger_printf("Resource Pools: Free Devs=%lu, Pipes=%lu, Transfers=%lu\n",
        free_devs, free_pipes, free_transfers);
}

void UsbHostManager::resetBus() {
    logger_println("[USB Host] Initiating hardware bus reset recovery...");

#if defined(ARDUINO_TEENSY41)
    // Hardware VBUS power cycle on Teensy 4.1 (TPS2051B power switch on GPIO_EMC_40 / GPIO8 bit 26)
    logger_println("[USB Host] Cycling 5V VBUS power to downstream peripherals...");
    GPIO8_DR_CLEAR = (1 << 26); // Cut 5V VBUS power
    delay(150);
    GPIO8_DR_SET = (1 << 26);   // Restore 5V VBUS power
    delay(100);
#endif

    // Purge state across all multiplexer channels
    for (uint8_t i = 0; i < MAX_KEYBOARDS; i++) {
        Multiplexer::purgeKeyboard(i);
        prev_kbd_connected_[i] = false;
    }
    for (uint8_t i = 0; i < MAX_MICE; i++) {
        Multiplexer::purgeMouse(i);
        prev_mouse_connected_[i] = false;
    }

    // Force EHCI host controller port reset
    USBHS_PORTSC1 |= USBHS_PORTSC_PR;
    delay(50);

    logger_println("[USB Host] Bus reset completed. Awaiting device enumeration.");
}
