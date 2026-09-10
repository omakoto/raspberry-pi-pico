/*
 * Nintendo Switch Pro Controller emulation implementation (see procon_device.hpp).
 *
 * Protocol references: the dekuNukem Nintendo_Switch_Reverse_Engineering notes and the
 * behaviour of Linux's hid-nintendo driver; descriptor and calibration bytes were
 * captured from a real Pro Controller (firmware 4.21) through this firmware's host port.
 */

#include "procon_device.hpp"

#include <cstring>
#include <cstdio>
#include <algorithm>
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "tusb.h"
#include "dual_logger.hpp"
#include "gamepad_hid.hpp"
#include "motion_bridge.hpp"

static const char* TAG = "ProCon";

namespace procon {
namespace {

// ---------------------------------------------------------------------------
// Descriptors (captured from a real Pro Controller)
// ---------------------------------------------------------------------------

const tusb_desc_device_t kDeviceDescriptor = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x057E,
    .idProduct          = 0x2009,
    .bcdDevice          = 0x0210,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

// Same descriptor, but as a composite device so CDC + MSC can ride along (IAD protocol)
const tusb_desc_device_t kDeviceDescriptorComposite = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x057E,
    .idProduct          = 0x2009,
    .bcdDevice          = 0x0210,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

const uint8_t kReportDescriptor[] = {
    0x05, 0x01, 0x15, 0x00, 0x09, 0x04, 0xa1, 0x01, 0x85, 0x30, 0x05, 0x01, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x0a, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0a, 0x55, 0x00, 0x65, 0x00, 0x81, 0x02,
    0x05, 0x09, 0x19, 0x0b, 0x29, 0x0e, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02,
    0x75, 0x01, 0x95, 0x02, 0x81, 0x03, 0x0b, 0x01, 0x00, 0x01, 0x00, 0xa1, 0x00, 0x0b, 0x30, 0x00,
    0x01, 0x00, 0x0b, 0x31, 0x00, 0x01, 0x00, 0x0b, 0x32, 0x00, 0x01, 0x00, 0x0b, 0x35, 0x00, 0x01,
    0x00, 0x15, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00, 0x75, 0x10, 0x95, 0x04, 0x81, 0x02, 0xc0, 0x0b,
    0x39, 0x00, 0x01, 0x00, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3b, 0x01, 0x65, 0x14, 0x75,
    0x04, 0x95, 0x01, 0x81, 0x02, 0x05, 0x09, 0x19, 0x0f, 0x29, 0x12, 0x15, 0x00, 0x25, 0x01, 0x75,
    0x01, 0x95, 0x04, 0x81, 0x02, 0x75, 0x08, 0x95, 0x34, 0x81, 0x03, 0x06, 0x00, 0xff, 0x85, 0x21,
    0x09, 0x01, 0x75, 0x08, 0x95, 0x3f, 0x81, 0x03, 0x85, 0x81, 0x09, 0x02, 0x75, 0x08, 0x95, 0x3f,
    0x81, 0x03, 0x85, 0x01, 0x09, 0x03, 0x75, 0x08, 0x95, 0x3f, 0x91, 0x83, 0x85, 0x10, 0x09, 0x04,
    0x75, 0x08, 0x95, 0x3f, 0x91, 0x83, 0x85, 0x80, 0x09, 0x05, 0x75, 0x08, 0x95, 0x3f, 0x91, 0x83,
    0x85, 0x82, 0x09, 0x06, 0x75, 0x08, 0x95, 0x3f, 0x91, 0x83, 0xc0,
};
static_assert(sizeof(kReportDescriptor) == 203, "Pro Controller report descriptor must be 203 bytes");

constexpr uint8_t EP_HID_OUT = 0x01;
constexpr uint8_t EP_HID_IN  = 0x81;
constexpr uint8_t EP_SIZE    = 64;
constexpr uint8_t EP_INTERVAL_MS = 8;

// Interface 0: HID only, bus powered with remote wakeup, 500 mA (as the real unit)
#define PROCON_CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN)
const uint8_t kConfigHidOnly[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, PROCON_CONFIG_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 500),
    TUD_HID_INOUT_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_NONE, sizeof(kReportDescriptor), EP_HID_OUT, EP_HID_IN, EP_SIZE, EP_INTERVAL_MS),
};

// Interfaces 0: HID, 1-2: CDC, 3: MSC (endpoint numbers as in the Pokken identity)
#define PROCON_CONFIG_COMPOSITE_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)
const uint8_t kConfigComposite[] = {
    TUD_CONFIG_DESCRIPTOR(1, 4, 0, PROCON_CONFIG_COMPOSITE_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 500),
    TUD_HID_INOUT_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_NONE, sizeof(kReportDescriptor), EP_HID_OUT, EP_HID_IN, EP_SIZE, EP_INTERVAL_MS),
    TUD_CDC_DESCRIPTOR(1, 5, 0x82, 8, 0x03, 0x83, 64),
    TUD_MSC_DESCRIPTOR(3, 6, 0x04, 0x84, 64),
};

const char* kStrings[] = {
    nullptr,               // 0: language id, handled by the caller
    "Nintendo Co., Ltd.",  // 1
    "Pro Controller",      // 2
    "000000000001",        // 3
    nullptr,               // 4: (unused)
    "Pico CDC Console",    // 5
    "Pico MSC Storage",    // 6
};

// ---------------------------------------------------------------------------
// Protocol constants
// ---------------------------------------------------------------------------

constexpr uint8_t OUT_SUBCMD    = 0x01;
constexpr uint8_t OUT_RUMBLE    = 0x10;
constexpr uint8_t OUT_USB_CMD   = 0x80;
constexpr uint8_t IN_SUBCMD_ACK = 0x21;
constexpr uint8_t IN_FULL       = 0x30;
constexpr uint8_t IN_USB_ACK    = 0x81;

constexpr uint8_t USB_CMD_STATUS    = 0x01;
constexpr uint8_t USB_CMD_HANDSHAKE = 0x02;
constexpr uint8_t USB_CMD_BAUD_3M   = 0x03;
constexpr uint8_t USB_CMD_FORCE_USB = 0x04;
constexpr uint8_t USB_CMD_ALLOW_BT  = 0x05;

constexpr uint16_t REPORT_PAYLOAD = 63;  // report body after the ID byte, packet is 64
constexpr uint32_t REPORT_PERIOD_MS = 15;
constexpr uint32_t MOTION_MAX_AGE_MS = 100;

// ---------------------------------------------------------------------------
// Virtual SPI flash: factory 0x6000-0x60FF, user 0x8000-0x80FF
// ---------------------------------------------------------------------------

uint8_t s_spi_factory[0x100];
uint8_t s_spi_user[0x100];

// Two 12-bit values packed into three bytes, the layout used by all stick calibration
void pack12(uint8_t* out, uint16_t a, uint16_t b) {
    out[0] = static_cast<uint8_t>(a & 0xFF);
    out[1] = static_cast<uint8_t>(((a >> 8) & 0x0F) | ((b & 0x0F) << 4));
    out[2] = static_cast<uint8_t>(b >> 4);
}

void build_spi_image() {
    std::memset(s_spi_factory, 0xFF, sizeof(s_spi_factory));
    std::memset(s_spi_user, 0xFF, sizeof(s_spi_user));

    // 0x6020: factory IMU calibration (captured): accel origin/sensitivity, gyro origin/sensitivity
    static const uint8_t imu_cal[24] = {
        0x2e, 0x00, 0xc2, 0xff, 0x4a, 0x01, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40,
        0xf8, 0xff, 0x00, 0x00, 0xfc, 0xff, 0xe7, 0x3b, 0xe7, 0x3b, 0xe7, 0x3b,
    };
    std::memcpy(&s_spi_factory[0x20], imu_cal, sizeof(imu_cal));

    // 0x603D: stick calibration, ideal linear map. Left stick layout: max above centre,
    // centre, min below centre; right stick layout: centre, min below, max above.
    constexpr uint16_t CENTRE = 2048, TRAVEL = 1792;
    uint8_t* l = &s_spi_factory[0x3D];
    pack12(&l[0], TRAVEL, TRAVEL);
    pack12(&l[3], CENTRE, CENTRE);
    pack12(&l[6], TRAVEL, TRAVEL);
    uint8_t* r = &s_spi_factory[0x46];
    pack12(&r[0], CENTRE, CENTRE);
    pack12(&r[3], TRAVEL, TRAVEL);
    pack12(&r[6], TRAVEL, TRAVEL);

    // 0x6050: colours (captured: grey body, white buttons, grips unset)
    static const uint8_t colours[13] = {0x32, 0x32, 0x32, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    std::memcpy(&s_spi_factory[0x50], colours, sizeof(colours));

    // 0x6080: IMU horizontal offsets + left stick parameters; 0x6098: right stick parameters (captured)
    static const uint8_t offsets_and_l_params[24] = {
        0x50, 0xfd, 0x00, 0x00, 0xc6, 0x0f,
        0x0f, 0x30, 0x61, 0xae, 0x90, 0xd9, 0xd4, 0x14, 0x54, 0x41, 0x15, 0x54, 0xc7, 0x79, 0x9c, 0x33, 0x36, 0x63,
    };
    std::memcpy(&s_spi_factory[0x80], offsets_and_l_params, sizeof(offsets_and_l_params));
    std::memcpy(&s_spi_factory[0x98], &offsets_and_l_params[6], 18);
}

// Copies len bytes from the virtual flash at addr into out; unmapped areas read 0xFF
void spi_read(uint32_t addr, uint8_t len, uint8_t* out) {
    for (uint8_t i = 0; i < len; ++i) {
        uint32_t a = addr + i;
        if (a >= 0x6000 && a < 0x6100) out[i] = s_spi_factory[a - 0x6000];
        else if (a >= 0x8000 && a < 0x8100) out[i] = s_spi_user[a - 0x8000];
        else out[i] = 0xFF;
    }
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

bool s_composite = false;
uint8_t s_mac[6];  // Nintendo OUI + three bytes of the flash unique id

InputState s_input;
bool s_mounted = false;
bool s_streaming = false;     // full reports flow once a report mode was selected
bool s_imu_enabled = false;
uint8_t s_report_mode = 0x30;
uint8_t s_player_leds = 0;

uint8_t s_pending[REPORT_PAYLOAD];  // one queued reply (0x21 / 0x81 body)
uint8_t s_pending_id = 0;
uint32_t s_last_report_ms = 0;
uint32_t s_last_motion_version = 0;  // motion block already forwarded

uint8_t timer_byte() {
    return static_cast<uint8_t>(to_ms_since_boot(get_absolute_time()) / 5);
}

// Fills the 12-byte input prefix shared by reports 0x30 and 0x21 (payload indices 0-11,
// i.e. report bytes 1-12): timer, battery/connection, three button bytes, two packed
// sticks, vibrator status
void fill_input_prefix(uint8_t* p) {
    const InputState& in = s_input;
    p[0] = timer_byte();
    p[1] = 0x91;  // battery full + charging, USB powered (as captured)

    uint8_t right = 0, shared = 0, left = 0;
    if (in.buttons & BTN_Y) right |= 0x01;
    if (in.buttons & BTN_X) right |= 0x02;
    if (in.buttons & BTN_B) right |= 0x04;
    if (in.buttons & BTN_A) right |= 0x08;
    if (in.buttons & BTN_R) right |= 0x40;
    if (in.buttons & BTN_ZR) right |= 0x80;
    if (in.buttons & BTN_MINUS) shared |= 0x01;
    if (in.buttons & BTN_PLUS) shared |= 0x02;
    if (in.buttons & BTN_RSTICK) shared |= 0x04;
    if (in.buttons & BTN_LSTICK) shared |= 0x08;
    if (in.buttons & BTN_HOME) shared |= 0x10;
    if (in.buttons & BTN_CAPTURE) shared |= 0x20;
    if (in.buttons & BTN_L) left |= 0x40;
    if (in.buttons & BTN_ZL) left |= 0x80;
    switch (in.hat) {
    case HAT_UP: left |= 0x02; break;
    case HAT_UP_RIGHT: left |= 0x02 | 0x04; break;
    case HAT_RIGHT: left |= 0x04; break;
    case HAT_DOWN_RIGHT: left |= 0x01 | 0x04; break;
    case HAT_DOWN: left |= 0x01; break;
    case HAT_DOWN_LEFT: left |= 0x01 | 0x08; break;
    case HAT_LEFT: left |= 0x08; break;
    case HAT_UP_LEFT: left |= 0x02 | 0x08; break;
    default: break;
    }
    p[2] = right;
    p[3] = shared;
    p[4] = left;

    // 0-255 (Y down) -> 12-bit around 2048 with 1792 travel (Y up), matching the served calibration
    auto axis = [](uint8_t v, bool invert) {
        int delta = (static_cast<int>(v) - 128) * 14;  // 127 * 14 = 1778 ~ TRAVEL
        if (invert) delta = -delta;
        return static_cast<uint16_t>(std::max(0, std::min(4095, 2048 + delta)));
    };
    pack12(&p[5], axis(in.lx, false), axis(in.ly, true));
    pack12(&p[8], axis(in.rx, false), axis(in.ry, true));
    p[11] = 0x09;  // vibrator report (as captured)
}

// Fills the IMU block and returns the timer byte to report with it: the attached
// controller's own timer when its samples are being forwarded, ours otherwise, so the
// console's per-report timing matches the samples it receives
uint8_t fill_imu(uint8_t* p) {
    uint8_t timer = 0;
    if (!motion_bridge_read(p, &timer, MOTION_MAX_AGE_MS)) {
        // Nothing attached or stale: report a controller resting flat (1 g on Z, no rotation)
        static const int16_t rest[6] = {0, 0, 4096, 0, 0, 0};
        for (int sample = 0; sample < 3; ++sample) {
            std::memcpy(&p[sample * 12], rest, sizeof(rest));
        }
        return timer_byte();
    }
    return timer;
}

void queue_reply(uint8_t report_id, const uint8_t* body, uint16_t len) {
    std::memset(s_pending, 0, sizeof(s_pending));
    std::memcpy(s_pending, body, std::min<uint16_t>(len, REPORT_PAYLOAD));
    s_pending_id = report_id;
}

// Subcommand reply: input prefix, ack byte, subcommand id, then data
void reply_subcmd(uint8_t ack, uint8_t subcmd, const uint8_t* data, uint8_t data_len) {
    uint8_t body[REPORT_PAYLOAD] = {0};
    fill_input_prefix(body);
    body[12] = ack;
    body[13] = subcmd;
    if (data != nullptr && data_len > 0) {
        std::memcpy(&body[14], data, std::min<uint8_t>(data_len, REPORT_PAYLOAD - 14));
    }
    queue_reply(IN_SUBCMD_ACK, body, sizeof(body));
}

void handle_subcmd_packet(const uint8_t* buf, uint16_t len) {
    // [0]=0x01 [1]=counter [2..9]=rumble [10]=subcmd [11..]=args
    if (len < 11) return;
    uint8_t subcmd = buf[10];
    const uint8_t* args = &buf[11];
    uint16_t args_len = len - 11;

    switch (subcmd) {
    case 0x01: {  // Bluetooth manual pairing: not applicable over USB
        uint8_t d[1] = {0x03};
        reply_subcmd(0x81, subcmd, d, 1);
        break;
    }
    case 0x02: {  // Request device info: firmware 4.21, type 03 = Pro Controller, MAC, colours in SPI
        uint8_t d[12] = {0x04, 0x21, 0x03, 0x02, s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5], 0x03, 0x02};
        reply_subcmd(0x82, subcmd, d, sizeof(d));
        break;
    }
    case 0x03:  // Set input report mode
        if (args_len >= 1) s_report_mode = args[0];
        s_streaming = true;
        reply_subcmd(0x80, subcmd, nullptr, 0);
        LOG_I(TAG, "Host selected input report mode 0x%02x", s_report_mode);
        break;
    case 0x04: {  // Trigger buttons elapsed time
        uint8_t d[14] = {0};
        reply_subcmd(0x83, subcmd, d, sizeof(d));
        break;
    }
    case 0x08:  // Set shipment low power state
        reply_subcmd(0x80, subcmd, nullptr, 0);
        break;
    case 0x10: {  // SPI flash read: addr (4, LE), len
        if (args_len < 5) { reply_subcmd(0x00, subcmd, nullptr, 0); break; }
        uint32_t addr = args[0] | (args[1] << 8) | (args[2] << 16) | (static_cast<uint32_t>(args[3]) << 24);
        uint8_t n = std::min<uint8_t>(args[4], 0x1D);
        uint8_t d[5 + 0x1D];
        std::memcpy(d, args, 5);
        d[4] = n;
        spi_read(addr, n, &d[5]);
        reply_subcmd(0x90, subcmd, d, static_cast<uint8_t>(5 + n));
        break;
    }
    case 0x11:  // SPI flash write
    case 0x12:  // SPI sector erase
        reply_subcmd(0x80, subcmd, nullptr, 0);
        break;
    case 0x21: {  // Set NFC/IR MCU configuration: MCU status as reported by a real unit in standby
        uint8_t d[8] = {0x01, 0x00, 0xff, 0x00, 0x08, 0x00, 0x1b, 0x01};
        reply_subcmd(0xA0, subcmd, d, sizeof(d));
        break;
    }
    case 0x22:  // Set NFC/IR MCU state
        reply_subcmd(0x80, subcmd, nullptr, 0);
        break;
    case 0x30:  // Set player lights
        if (args_len >= 1) s_player_leds = args[0];
        reply_subcmd(0x80, subcmd, nullptr, 0);
        LOG_I(TAG, "Host set player lights 0x%02x", s_player_leds);
        break;
    case 0x38:  // Set HOME light
        reply_subcmd(0x80, subcmd, nullptr, 0);
        break;
    case 0x40: {  // Enable IMU: 0 = off, 1 = classic layout, 2 = newer layout (used by the console)
        uint8_t mode = args_len >= 1 ? args[0] : 0;
        s_imu_enabled = mode != 0;
        // The attached controller is put into the same mode so its blocks pass through verbatim
        motion_bridge_set_imu_mode(mode);
        reply_subcmd(0x80, subcmd, nullptr, 0);
        LOG_I(TAG, "Host set IMU mode %u", mode);
        break;
    }
    case 0x41: {  // Set IMU sensitivity: hand the same settings to the attached controller
        uint8_t cfg[IMU_CONFIG_SIZE] = {0x03, 0x00, 0x01, 0x01};  // controller defaults
        std::memcpy(cfg, args, std::min<uint16_t>(args_len, IMU_CONFIG_SIZE));
        motion_bridge_set_imu_config(cfg);
        reply_subcmd(0x80, subcmd, nullptr, 0);
        LOG_I(TAG, "Host set IMU sensitivity: gyro %u accel %u gyro-rate %u accel-filter %u",
              cfg[0], cfg[1], cfg[2], cfg[3]);
        break;
    }
    case 0x48:  // Enable vibration
        reply_subcmd(0x80, subcmd, nullptr, 0);
        break;
    case 0x50: {  // Get regulated voltage: ~4.1 V
        uint8_t d[2] = {0x83, 0x06};
        reply_subcmd(0xD0, subcmd, d, sizeof(d));
        break;
    }
    default:
        LOG_W(TAG, "Unhandled subcommand 0x%02x (%u arg bytes); acknowledging", subcmd, args_len);
        reply_subcmd(0x80, subcmd, nullptr, 0);
        break;
    }
}

void handle_usb_cmd(const uint8_t* buf, uint16_t len) {
    if (len < 2) return;
    uint8_t cmd = buf[1];
    uint8_t body[REPORT_PAYLOAD] = {0};
    switch (cmd) {
    case USB_CMD_STATUS: {  // -> 81 01 00 03 <MAC, reversed>
        body[0] = cmd;
        body[1] = 0x00;
        body[2] = 0x03;
        for (int i = 0; i < 6; ++i) body[3 + i] = s_mac[5 - i];
        queue_reply(IN_USB_ACK, body, sizeof(body));
        break;
    }
    case USB_CMD_HANDSHAKE:
    case USB_CMD_BAUD_3M:
        body[0] = cmd;
        queue_reply(IN_USB_ACK, body, sizeof(body));
        LOG_I(TAG, "USB command 0x%02x acknowledged", cmd);
        break;
    case USB_CMD_FORCE_USB:
        // No reply; the console only wants the Bluetooth timeout off
        LOG_I(TAG, "Host forced USB mode");
        break;
    case USB_CMD_ALLOW_BT:
        break;
    case 0x92: {
        // Console-side wrapper carrying a rumble+subcommand packet; the exact framing is
        // not confirmed, so accept the report 0x01 wherever it starts within the header
        for (uint16_t off = 6; off <= 8 && off < len; ++off) {
            if (buf[off] == OUT_SUBCMD) {
                handle_subcmd_packet(&buf[off], len - off);
                return;
            }
        }
        LOG_W(TAG, "Unrecognised 0x80 0x92 wrapper (%u bytes)", len);
        break;
    }
    default:
        LOG_W(TAG, "Unhandled USB command 0x80 0x%02x (%u bytes)", cmd, len);
        break;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void init(bool composite) {
    s_composite = composite;
    build_spi_image();

    // Nintendo's OUI followed by three bytes of this board's flash id, so several units
    // on one console do not collide and none of them shadows a real controller
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    s_mac[0] = 0x98; s_mac[1] = 0xB6; s_mac[2] = 0xE9;
    s_mac[3] = id.id[5]; s_mac[4] = id.id[6]; s_mac[5] = id.id[7];
    LOG_I(TAG, "Pro Controller identity ready (%s), MAC %02x:%02x:%02x:%02x:%02x:%02x",
          composite ? "HID + CDC + MSC" : "HID only",
          s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
}

const uint8_t* device_descriptor() {
    return reinterpret_cast<const uint8_t*>(s_composite ? &kDeviceDescriptorComposite : &kDeviceDescriptor);
}

const uint8_t* configuration_descriptor() {
    return s_composite ? kConfigComposite : kConfigHidOnly;
}

const char* string_descriptor(uint8_t index) {
    if (index >= sizeof(kStrings) / sizeof(kStrings[0])) return nullptr;
    return kStrings[index];
}

const uint8_t* report_descriptor() { return kReportDescriptor; }
uint16_t report_descriptor_len() { return sizeof(kReportDescriptor); }

void on_mount() {
    s_mounted = true;
    s_streaming = false;
    s_imu_enabled = false;
    s_pending_id = 0;
}

void on_umount() {
    s_mounted = false;
    s_streaming = false;
}

void on_output_report(const uint8_t* buf, uint16_t len) {
    if (len == 0) return;
    switch (buf[0]) {
    case OUT_SUBCMD:
        handle_subcmd_packet(buf, len);
        break;
    case OUT_RUMBLE:
        break;  // rumble only, nothing to answer
    case OUT_USB_CMD:
        handle_usb_cmd(buf, len);
        break;
    case 0x00:
        break;  // the console probes with empty 2-byte packets before the handshake
    default:
        LOG_W(TAG, "Unhandled output report 0x%02x (%u bytes)", buf[0], len);
        break;
    }
}

void set_input(const InputState& in) {
    s_input = in;
}

void tick() {
    if (!s_mounted || !tud_hid_ready()) return;

    if (s_pending_id != 0) {
        if (tud_hid_report(s_pending_id, s_pending, REPORT_PAYLOAD)) {
            s_pending_id = 0;
            s_last_report_ms = to_ms_since_boot(get_absolute_time());
        }
        return;
    }

    if (!s_streaming) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());

    // With an attached controller delivering IMU blocks, send exactly one report per block
    // as soon as it arrives, so the console gets each sample once at the controller's own
    // cadence (re-sending a block on our own clock would double or skip rotation). The
    // fixed period only paces reports when no fresh motion is coming in.
    uint32_t motion_version = motion_bridge_version();
    bool fresh_motion = s_imu_enabled && motion_version != s_last_motion_version;
    if (!fresh_motion && now - s_last_report_ms < REPORT_PERIOD_MS) return;

    uint8_t body[REPORT_PAYLOAD] = {0};
    fill_input_prefix(body);
    if (s_imu_enabled) {
        body[0] = fill_imu(&body[12]);
    }
    if (tud_hid_report(IN_FULL, body, REPORT_PAYLOAD)) {
        s_last_report_ms = now;
        s_last_motion_version = motion_version;
    }
}

}  // namespace procon
