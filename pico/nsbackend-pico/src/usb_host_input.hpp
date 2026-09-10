/*
 * USB Host Controller Pass-Through for nsbackend-pico.
 *
 * Drives a secondary full-speed USB host port, bit-banged with PIO on two GPIOs
 * (D+ on dp_pin, D- on dp_pin + 1) and exposed on a USB-A receptacle, so a regular
 * PC controller can be plugged into the device and passed through to the Switch.
 *
 * Supported controllers:
 *  - XInput: Xbox 360 / Xbox One / Xbox Series wired controllers (vendored tusb_xinput driver)
 *  - Nintendo Switch Pro Controller and third-party pads in Switch mode (Nintendo's
 *    proprietary HID protocol: USB handshake, report mode 0x30, packed 12-bit sticks)
 *  - Generic HID gamepads: DirectInput pads, DualShock 4, DualSense, 8BitDo in D-mode, etc.,
 *    decoded by parsing each device's HID report descriptor.
 *
 * Face buttons are mapped positionally (bottom = B, right = A, left = Y, top = X), so
 * muscle memory carries over even though Xbox and Switch labels are swapped.
 */

#pragma once

#include <cstdint>
#include "controller_state.hpp"

struct UsbHostInputConfig {
    bool enabled = true;
    // GPIO carrying USB D+; D- must be wired to dp_pin + 1
    uint8_t dp_pin = 16;
    // Radial stick deadzone in percent of full deflection (0-99)
    int deadzone_percent = 10;
    bool log_enabled = true;
};

class UsbHostInput {
public:
    UsbHostInput(ControllerState& controller, const UsbHostInputConfig& config);
    ~UsbHostInput();

    // Starts the TinyUSB host stack task on the PIO-USB port. Returns false when disabled
    // by configuration or when the task could not be created.
    bool init();

private:
    static void host_task_entry(void* param);
    void run_host_task();

    ControllerState& controller_;
    UsbHostInputConfig config_;
    bool initialized_;
};
