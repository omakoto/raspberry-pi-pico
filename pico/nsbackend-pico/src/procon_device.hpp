/*
 * Nintendo Switch Pro Controller emulation for nsbackend-pico (USB device side).
 *
 * When the firmware is configured with switch_identity = "procon", the native USB port
 * presents a Pro Controller (VID 057e, PID 2009) instead of the HORI Pokken pad, using
 * descriptors captured from a real controller. The Switch (or Linux's hid-nintendo
 * driver) then runs Nintendo's proprietary protocol against us: USB handshake commands
 * (report 0x80), rumble + subcommand packets (report 0x01) answered through report 0x21,
 * and a 60 Hz stream of full input reports (0x30) that carry the buttons, sticks and,
 * once enabled, IMU samples. The IMU data comes from a Pro Controller attached to the
 * USB-A host port through the motion bridge, which is the whole point of this mode:
 * motion controls in games.
 *
 * Stick calibration served from the virtual SPI flash is an ideal linear map (centre
 * 2048, 1792 counts of travel), so the merged 0-255 stick values convert directly.
 * IMU calibration and controller colours are the values captured from a real unit.
 */

#pragma once

#include <cstdint>

namespace procon {

// Merged controller input, in the same terms the Pokken report uses (0-255 sticks with
// Y growing downward, hat 0-7 or 8 for centre).
struct InputState {
    uint16_t buttons = 0;
    uint8_t hat = 8;
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
};

// Prepares descriptors. composite = true keeps the CDC console and MSC drive interfaces
// alongside the HID interface (a real Pro Controller is HID-only, and the console may
// reject a composite device, so the default is false).
void init(bool composite);

const uint8_t* device_descriptor();
const uint8_t* configuration_descriptor();
const char* string_descriptor(uint8_t index);  // nullptr when index is out of range
const uint8_t* report_descriptor();
uint16_t report_descriptor_len();

void on_mount();
void on_umount();

// Output report from the host (raw packet starting with the report ID).
void on_output_report(const uint8_t* buf, uint16_t len);

// Latest merged input, taken by the periodic report.
void set_input(const InputState& in);

// Called from the USB device task loop; sends pending replies and the periodic report.
void tick();

}  // namespace procon
