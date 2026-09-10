/*
 * USB Host Controller Pass-Through Implementation for nsbackend-pico.
 *
 * Runs the TinyUSB host stack on rhport 1, backed by the PIO-USB full-speed port
 * (Pico-PIO-USB), and translates reports from attached controllers into
 * ControllerState updates:
 *  - XInput controllers are handled by the vendored tusb_xinput host class driver.
 *  - Nintendo Switch Pro Controllers (and third-party pads in Switch mode, which use
 *    the same VID/PID and protocol) speak Nintendo's proprietary HID protocol: they stay
 *    silent over USB until the host performs a handshake and selects a report mode, and
 *    their real report layout differs from what their HID descriptor declares. They get
 *    a small init state machine and a hand-written parser for report 0x30 / 0x3F.
 *  - Generic HID gamepads are handled by parsing each device's HID report descriptor
 *    to locate the axes, hat switch, and buttons, so arbitrary DirectInput-style pads
 *    work without per-device quirks tables.
 *
 * All TinyUSB host callbacks run in the single usb_host task (tuh_task() context),
 * so the pad slot bookkeeping below needs no locking; ControllerState does its own.
 */

#include "usb_host_input.hpp"

#include <cmath>
#include <cstring>
#include <algorithm>

#include "FreeRTOS.h"
#include "task.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "pico/time.h"
#include "tusb.h"
#include "pio_usb_configuration.h"
#include "xinput_host.h"
#include "dual_logger.hpp"
#include "gamepad_hid.hpp"

static const char* TAG = "UsbHost";
// Host-controller hooks (hcd_port_connect_status, hcd_event_device_attach/remove) used
// to re-kick enumeration; not part of the public tuh_* API but stable across releases
#include "host/hcd.h"

namespace {

// ---------------------------------------------------------------------------
// Shared pad state and merging
// ---------------------------------------------------------------------------

struct PadState {
    uint16_t buttons = BTN_NONE;
    bool up = false, down = false, left = false, right = false;
    float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
};

// Positional face-button layout shared by the mapping tables below:
// bottom -> B, right -> A, left -> Y, top -> X (Switch positions), so a pad keeps
// its physical layout even though Xbox/PlayStation labels differ from Nintendo's.

// Standard DirectInput/HID button order (matches DualShock 4 / DualSense and most
// generic pads): 1=west 2=south 3=east 4=north 5=L1 6=R1 7=L2 8=R2 9=select
// 10=start 11=L3 12=R3 13=home 14=touchpad/capture.
constexpr int MAX_HID_BUTTONS = 16;
static const uint16_t kHidButtonMap[MAX_HID_BUTTONS] = {
    BTN_Y, BTN_B, BTN_A, BTN_X,
    BTN_L, BTN_R, BTN_ZL, BTN_ZR,
    BTN_MINUS, BTN_PLUS, BTN_LSTICK, BTN_RSTICK,
    BTN_HOME, BTN_CAPTURE, BTN_NONE, BTN_NONE,
};

// Analog trigger travel (0-255) beyond which XInput LT/RT count as ZL/ZR presses
constexpr uint8_t XINPUT_TRIGGER_THRESHOLD = 32;

UsbHostInput* s_instance = nullptr;
ControllerState* s_controller = nullptr;
float s_deadzone = 0.10f;
bool s_log_enabled = true;

#define UH_LOG(fmt, ...) do { if (s_log_enabled) LOG_I(TAG, fmt, ##__VA_ARGS__); } while (0)

void apply_radial_deadzone(float& x, float& y) {
    float mag = std::sqrt(x * x + y * y);
    if (mag < s_deadzone) {
        x = 0.0f;
        y = 0.0f;
    }
}

// ---------------------------------------------------------------------------
// HID report descriptor parsing
// ---------------------------------------------------------------------------

struct HidField {
    bool present = false;
    uint16_t bit_offset = 0;
    uint8_t bit_size = 0;
    int32_t lmin = 0;
    int32_t lmax = 0;
};

struct HidGamepadLayout {
    bool valid = false;
    uint8_t report_id = 0;
    bool has_report_id = false;
    HidField x, y, z, rz, rx, ry, hat;
    HidField buttons[MAX_HID_BUTTONS];
    uint8_t button_count = 0;
};

// Per-report-ID field accumulator used while walking one report descriptor
struct LayoutBuilder {
    bool used = false;
    HidGamepadLayout layout;
    uint32_t input_bits = 0;
};

constexpr int MAX_REPORT_IDS = 8;

struct DescriptorParser {
    LayoutBuilder builders[MAX_REPORT_IDS];
    int builder_count = 0;
    bool any_report_id = false;

    LayoutBuilder* builder_for(uint8_t report_id) {
        for (int i = 0; i < builder_count; ++i) {
            if (builders[i].layout.report_id == report_id) {
                return &builders[i];
            }
        }
        if (builder_count >= MAX_REPORT_IDS) {
            return nullptr;
        }
        LayoutBuilder* b = &builders[builder_count++];
        b->used = true;
        b->layout.report_id = report_id;
        b->layout.has_report_id = any_report_id;
        return b;
    }
};

// Parses a HID report descriptor and extracts the input-report layout of the gamepad /
// joystick application collection: X/Y/Z/Rx/Ry/Rz axes, hat switch, and button bits.
// Returns false when the descriptor contains no usable gamepad report.
bool parse_hid_report_descriptor(const uint8_t* desc, uint16_t desc_len, HidGamepadLayout* out) {
    DescriptorParser parser;

    // Global item state
    uint16_t usage_page = 0;
    int32_t logical_min = 0, logical_max = 0;
    uint32_t report_size = 0, report_count = 0;
    uint8_t report_id = 0;

    // Local item state (reset after every Main item)
    constexpr int MAX_LOCAL_USAGES = 32;
    uint32_t usages[MAX_LOCAL_USAGES];  // (page << 16) | usage
    int usage_count = 0;
    uint32_t usage_min = 0, usage_max = 0;
    bool has_usage_range = false;

    // Collection tracking: fields are only harvested inside a Joystick/Gamepad/Multi-axis
    // application collection so mice, keyboards, and vendor collections are ignored.
    int collection_depth = 0;
    int gamepad_app_depth = -1;

    const uint8_t* p = desc;
    const uint8_t* end = desc + desc_len;

    while (p < end) {
        uint8_t prefix = *p++;
        if (prefix == 0xFE) {
            // Long item: bDataSize, bLongItemTag, data
            if (p >= end) break;
            uint8_t long_size = *p;
            p += 2 + long_size;
            continue;
        }
        uint8_t size = prefix & 0x03;
        if (size == 3) size = 4;
        uint8_t type = (prefix >> 2) & 0x03;
        uint8_t tag = (prefix >> 4) & 0x0F;
        if (p + size > end) break;

        uint32_t udata = 0;
        for (uint8_t i = 0; i < size; ++i) {
            udata |= static_cast<uint32_t>(p[i]) << (8 * i);
        }
        // Sign-extended variant, for logical min/max
        int32_t sdata = static_cast<int32_t>(udata);
        if (size == 1 && (udata & 0x80)) sdata = static_cast<int32_t>(udata | 0xFFFFFF00u);
        if (size == 2 && (udata & 0x8000)) sdata = static_cast<int32_t>(udata | 0xFFFF0000u);
        p += size;

        switch (type) {
        case 0:  // Main
            switch (tag) {
            case 0x8: {  // Input
                LayoutBuilder* b = parser.builder_for(report_id);
                if (b == nullptr) break;
                bool is_constant = (udata & 0x01) != 0;
                bool in_gamepad = (gamepad_app_depth >= 0);
                if (!is_constant && in_gamepad && report_size > 0 && report_count > 0) {
                    for (uint32_t i = 0; i < report_count; ++i) {
                        uint32_t usage;
                        if (has_usage_range) {
                            usage = (usage_min + i <= usage_max) ? (usage_min + i) : usage_max;
                            if (usage < 0x10000) usage |= static_cast<uint32_t>(usage_page) << 16;
                        } else if (usage_count > 0) {
                            // With fewer usages than report slots, the last usage repeats
                            usage = usages[std::min<int>(static_cast<int>(i), usage_count - 1)];
                        } else {
                            continue;
                        }

                        HidField field;
                        field.present = true;
                        field.bit_offset = static_cast<uint16_t>(b->input_bits + i * report_size);
                        field.bit_size = static_cast<uint8_t>(report_size);
                        field.lmin = logical_min;
                        field.lmax = logical_max;
                        // Repair descriptors that encode an unsigned max in a field the spec
                        // treats as signed (e.g. lmax byte 0xFF meaning 255, not -1)
                        if (field.lmax < field.lmin) {
                            field.lmax = static_cast<int32_t>((1u << std::min<uint32_t>(report_size, 31)) - 1);
                        }

                        uint16_t page = static_cast<uint16_t>(usage >> 16);
                        uint16_t u = static_cast<uint16_t>(usage & 0xFFFF);
                        HidGamepadLayout& L = b->layout;
                        if (page == 0x01) {  // Generic Desktop
                            switch (u) {
                            case 0x30: L.x = field; break;
                            case 0x31: L.y = field; break;
                            case 0x32: L.z = field; break;
                            case 0x33: L.rx = field; break;
                            case 0x34: L.ry = field; break;
                            case 0x35: L.rz = field; break;
                            case 0x39: L.hat = field; break;
                            default: break;
                            }
                        } else if (page == 0x09) {  // Button
                            if (L.button_count < MAX_HID_BUTTONS) {
                                L.buttons[L.button_count++] = field;
                            }
                        }
                    }
                }
                b->input_bits += report_size * report_count;
                break;
            }
            case 0xA:  // Collection
                if (collection_depth == 0 && udata == 0x01 && usage_count > 0) {
                    // Application collection: accept joystick (0x04), gamepad (0x05),
                    // and multi-axis controller (0x08) on the Generic Desktop page
                    uint32_t app_usage = usages[0];
                    if (app_usage == 0x00010004 || app_usage == 0x00010005 || app_usage == 0x00010008) {
                        gamepad_app_depth = collection_depth;
                    }
                }
                collection_depth++;
                break;
            case 0xC:  // End Collection
                collection_depth--;
                if (gamepad_app_depth >= 0 && collection_depth <= gamepad_app_depth) {
                    gamepad_app_depth = -1;
                }
                break;
            default:
                break;
            }
            // Local items only apply to the next Main item
            usage_count = 0;
            has_usage_range = false;
            usage_min = usage_max = 0;
            break;

        case 1:  // Global
            switch (tag) {
            case 0x0: usage_page = static_cast<uint16_t>(udata); break;
            case 0x1: logical_min = sdata; break;
            case 0x2: logical_max = sdata; break;
            case 0x7: report_size = udata; break;
            case 0x8:
                report_id = static_cast<uint8_t>(udata);
                parser.any_report_id = true;
                // Ensure the builder exists and knows the report stream is ID-prefixed
                if (LayoutBuilder* b = parser.builder_for(report_id)) {
                    b->layout.has_report_id = true;
                }
                break;
            case 0x9: report_count = udata; break;
            default: break;  // push/pop and units are irrelevant for offset computation
            }
            break;

        case 2:  // Local
            switch (tag) {
            case 0x0:  // Usage
                if (usage_count < MAX_LOCAL_USAGES) {
                    uint32_t usage = udata;
                    if (size < 4) usage |= static_cast<uint32_t>(usage_page) << 16;
                    usages[usage_count++] = usage;
                }
                break;
            case 0x1:  // Usage Minimum
                usage_min = (size < 4) ? udata : (udata & 0xFFFF);
                has_usage_range = true;
                break;
            case 0x2:  // Usage Maximum
                usage_max = (size < 4) ? udata : (udata & 0xFFFF);
                has_usage_range = true;
                break;
            default:
                break;
            }
            break;

        default:
            break;
        }
    }

    // Pick the most gamepad-like report: prefer one with X+Y axes, then most buttons
    LayoutBuilder* best = nullptr;
    int best_score = 0;
    for (int i = 0; i < parser.builder_count; ++i) {
        HidGamepadLayout& L = parser.builders[i].layout;
        int score = 0;
        if (L.x.present && L.y.present) score += 100;
        if (L.hat.present) score += 10;
        score += L.button_count;
        if (score > best_score) {
            best_score = score;
            best = &parser.builders[i];
        }
    }
    if (best == nullptr || best_score < 100) {
        // Without X/Y axes this is not something we can treat as a gamepad
        return false;
    }
    *out = best->layout;
    out->valid = true;
    return true;
}

// ---------------------------------------------------------------------------
// HID report decoding
// ---------------------------------------------------------------------------

uint32_t extract_bits(const uint8_t* report, uint16_t report_len, uint16_t bit_offset, uint8_t bit_size) {
    uint32_t value = 0;
    for (uint8_t i = 0; i < bit_size && i < 32; ++i) {
        uint16_t bit = bit_offset + i;
        uint16_t byte_idx = bit >> 3;
        if (byte_idx >= report_len) break;
        if (report[byte_idx] & (1u << (bit & 7))) {
            value |= 1u << i;
        }
    }
    return value;
}

// Returns the axis value normalized to [-1.0, +1.0]; HID axes have minimum = left/up
float decode_axis(const uint8_t* report, uint16_t report_len, const HidField& f) {
    if (!f.present || f.lmax <= f.lmin) return 0.0f;
    uint32_t raw = extract_bits(report, report_len, f.bit_offset, f.bit_size);
    int32_t value = static_cast<int32_t>(raw);
    if (f.lmin < 0 && f.bit_size < 32 && (raw & (1u << (f.bit_size - 1)))) {
        value = static_cast<int32_t>(raw | (~0u << f.bit_size));
    }
    float norm = static_cast<float>(value - f.lmin) / static_cast<float>(f.lmax - f.lmin);
    return std::max(-1.0f, std::min(1.0f, norm * 2.0f - 1.0f));
}

void decode_hat(const uint8_t* report, uint16_t report_len, const HidField& f,
                bool& up, bool& down, bool& left, bool& right) {
    up = down = left = right = false;
    if (!f.present) return;
    uint32_t raw = extract_bits(report, report_len, f.bit_offset, f.bit_size);
    int32_t idx = static_cast<int32_t>(raw) - f.lmin;
    switch (idx) {
    case 0: up = true; break;
    case 1: up = right = true; break;
    case 2: right = true; break;
    case 3: down = right = true; break;
    case 4: down = true; break;
    case 5: down = left = true; break;
    case 6: left = true; break;
    case 7: up = left = true; break;
    default: break;  // null / out of range = centered
    }
}

// ---------------------------------------------------------------------------
// Connected pad slots
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Nintendo Switch Pro Controller protocol
// ---------------------------------------------------------------------------

constexpr uint16_t NINTENDO_VID = 0x057e;

// Output report IDs
constexpr uint8_t SWPRO_OUT_SUBCMD   = 0x01;  // rumble + subcommand
constexpr uint8_t SWPRO_OUT_USB_CMD  = 0x80;  // USB-transport commands
// USB-transport commands (report 0x80) and their acks (report 0x81)
constexpr uint8_t SWPRO_USB_HANDSHAKE = 0x02;
constexpr uint8_t SWPRO_USB_FORCE_USB = 0x04;  // stop the Bluetooth fallback timeout
// Subcommands (in report 0x01) acknowledged by report 0x21
constexpr uint8_t SWPRO_SUBCMD_SET_REPORT_MODE = 0x03;
constexpr uint8_t SWPRO_SUBCMD_SET_PLAYER_LEDS = 0x30;
constexpr uint8_t SWPRO_SUBCMD_ENABLE_IMU      = 0x40;  // for future gyro support
// Input report IDs
constexpr uint8_t SWPRO_IN_SUBCMD_ACK = 0x21;
constexpr uint8_t SWPRO_IN_FULL       = 0x30;  // 60 Hz standard report (also 0x31-0x33)
constexpr uint8_t SWPRO_IN_SIMPLE     = 0x3F;  // "simple HID" mode: digital sticks only
constexpr uint8_t SWPRO_IN_USB_ACK    = 0x81;

// Init sequence, in order. Each step is resent on timeout and skipped after a few
// tries so third-party pads that ignore a command still come up.
enum class SwProStage : uint8_t { Handshake, ForceUsb, SetReportMode, SetPlayerLeds, Ready };

struct SwProCtl {
    SwProStage stage = SwProStage::Handshake;
    uint32_t last_tx_ms = 0;
    uint8_t attempts = 0;
    uint8_t packet_counter = 0;  // 4-bit sequence number required in report 0x01
};

struct HidSlot {
    bool used = false;
    uint8_t daddr = 0;
    uint8_t instance = 0;
    bool switch_pro = false;  // speaks the Nintendo protocol instead of the parsed layout
    SwProCtl swpro;
    HidGamepadLayout layout;
    PadState state;
};

struct XinputSlot {
    bool used = false;
    uint8_t daddr = 0;
    uint8_t instance = 0;
    PadState state;
};

HidSlot s_hid_slots[CFG_TUH_HID];
XinputSlot s_xinput_slots[CFG_TUH_XINPUT];

HidSlot* find_hid_slot(uint8_t daddr, uint8_t instance, bool allocate) {
    HidSlot* free_slot = nullptr;
    for (auto& slot : s_hid_slots) {
        if (slot.used && slot.daddr == daddr && slot.instance == instance) {
            return &slot;
        }
        if (!slot.used && free_slot == nullptr) {
            free_slot = &slot;
        }
    }
    if (allocate && free_slot != nullptr) {
        *free_slot = HidSlot{};
        free_slot->used = true;
        free_slot->daddr = daddr;
        free_slot->instance = instance;
        return free_slot;
    }
    return nullptr;
}

XinputSlot* find_xinput_slot(uint8_t daddr, uint8_t instance, bool allocate) {
    XinputSlot* free_slot = nullptr;
    for (auto& slot : s_xinput_slots) {
        if (slot.used && slot.daddr == daddr && slot.instance == instance) {
            return &slot;
        }
        if (!slot.used && free_slot == nullptr) {
            free_slot = &slot;
        }
    }
    if (allocate && free_slot != nullptr) {
        *free_slot = XinputSlot{};
        free_slot->used = true;
        free_slot->daddr = daddr;
        free_slot->instance = instance;
        return free_slot;
    }
    return nullptr;
}

// Combines every connected pad into one state and forwards it to the controller engine.
// Buttons and hat directions are OR-ed; sticks are summed (ControllerState clamps).
void merge_and_push() {
    if (s_controller == nullptr) return;

    PadState merged;
    auto merge_one = [&merged](const PadState& s) {
        merged.buttons |= s.buttons;
        merged.up |= s.up;
        merged.down |= s.down;
        merged.left |= s.left;
        merged.right |= s.right;
        merged.lx += s.lx;
        merged.ly += s.ly;
        merged.rx += s.rx;
        merged.ry += s.ry;
    };
    for (const auto& slot : s_hid_slots) {
        if (slot.used) merge_one(slot.state);
    }
    for (const auto& slot : s_xinput_slots) {
        if (slot.used) merge_one(slot.state);
    }

    auto clamp1 = [](float v) { return std::max(-1.0f, std::min(1.0f, v)); };
    s_controller->set_usb_host_state(merged.buttons,
                                     merged.up, merged.down, merged.left, merged.right,
                                     clamp1(merged.lx), clamp1(merged.ly),
                                     clamp1(merged.rx), clamp1(merged.ry));
}

}  // namespace

// ---------------------------------------------------------------------------
// Enumeration compatibility: full first device-descriptor read
// ---------------------------------------------------------------------------

// TinyUSB opens enumeration by reading only the first 8 bytes of the device descriptor
// (to learn the EP0 packet size), whereas PCs and the Switch read the whole descriptor.
// Some third-party pads (DragonRise 0079:181d "Switch compatible" pads, for example)
// choke on that short read and drop off the bus right after SET_ADDRESS. TinyUSB offers
// no hook for this, so the two host-controller calls that carry the request are wrapped
// at link time (see --wrap in CMakeLists.txt) and the request is widened to the full 18
// bytes: wLength in the SETUP packet, then the buffer length of the data stage that
// follows. The data lands in TinyUSB's 512-byte enumeration buffer, and devices with an
// 8-byte EP0 simply answer in three packets, which is what a PC host makes them do anyway.
extern "C" bool __real_hcd_setup_send(uint8_t rhport, uint8_t daddr, const uint8_t setup_packet[8]);
extern "C" bool __real_hcd_edpt_xfer(uint8_t rhport, uint8_t daddr, uint8_t ep_addr, uint8_t* buffer, uint16_t buflen);

bool s_widen_first_desc_read = false;

extern "C" bool __wrap_hcd_setup_send(uint8_t rhport, uint8_t daddr, const uint8_t setup_packet[8]) {
    // GET_DESCRIPTOR(DEVICE) to address 0 with wLength 8: the enumeration opener
    const bool is_short_dev_desc_read = daddr == 0 && setup_packet[0] == 0x80 && setup_packet[1] == 0x06 &&
                                        setup_packet[3] == 0x01 && setup_packet[6] == 8 && setup_packet[7] == 0;
    s_widen_first_desc_read = is_short_dev_desc_read;
    if (!is_short_dev_desc_read) {
        return __real_hcd_setup_send(rhport, daddr, setup_packet);
    }
    static uint8_t widened[8];
    std::memcpy(widened, setup_packet, 8);
    widened[6] = sizeof(tusb_desc_device_t);
    return __real_hcd_setup_send(rhport, daddr, widened);
}

extern "C" bool __wrap_hcd_edpt_xfer(uint8_t rhport, uint8_t daddr, uint8_t ep_addr, uint8_t* buffer, uint16_t buflen) {
    if (s_widen_first_desc_read && daddr == 0 && ep_addr == 0x80 && buflen == 8) {
        buflen = sizeof(tusb_desc_device_t);
        s_widen_first_desc_read = false;
    }
    return __real_hcd_edpt_xfer(rhport, daddr, ep_addr, buffer, buflen);
}

// ---------------------------------------------------------------------------
// Enumeration retry
// ---------------------------------------------------------------------------

// TinyUSB retries a failed enumeration transfer three times and then abandons the
// device for good, without resetting the port. Some pads (seen with a third-party
// Switch controller waking from sleep) fail the first control transfer's status stage
// and then ignore everything until they get a bus reset, so while a device is present
// on the wire and nothing is mounted, periodically simulate a re-plug.
constexpr uint32_t ENUM_RETRY_IDLE_MS = 1500;
bool s_device_mounted = false;
// Last time the host controller reported any activity (attach, removal, or a completed
// transfer); an enumeration that is still making progress keeps this fresh.
uint32_t s_last_host_activity_ms = 0;

void enum_retry_poll() {
    if (s_device_mounted || !hcd_port_connect_status(1)) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - s_last_host_activity_ms < ENUM_RETRY_IDLE_MS) return;
    s_last_host_activity_ms = now;
    UH_LOG("Device present but not enumerated; resetting port and retrying");
    // A control transfer the device never answered (endless NAKs) stays pending forever
    // and would make TinyUSB reject every later control request, so abort it first.
    for (uint8_t daddr = 0; daddr <= CFG_TUH_DEVICE_MAX; ++daddr) {
        tuh_edpt_abort_xfer(daddr, 0);
    }
    hcd_event_device_remove(1, false);
    hcd_event_device_attach(1, false);
}

// ---------------------------------------------------------------------------
// Nintendo Switch Pro Controller: init sequence and report parsing
// ---------------------------------------------------------------------------

constexpr uint32_t SWPRO_STEP_TIMEOUT_MS = 150;
constexpr uint8_t SWPRO_STEP_MAX_ATTEMPTS = 8;

bool swpro_send_usb_cmd(HidSlot* slot, uint8_t cmd) {
    return tuh_hid_send_report(slot->daddr, slot->instance, SWPRO_OUT_USB_CMD, &cmd, 1);
}

bool swpro_send_subcmd(HidSlot* slot, uint8_t subcmd, const uint8_t* args, uint8_t args_len) {
    // Report 0x01 layout: packet counter, 8 bytes rumble (neutral), subcommand, arguments
    uint8_t buf[10 + 8] = {0};
    buf[0] = slot->swpro.packet_counter;
    slot->swpro.packet_counter = (slot->swpro.packet_counter + 1) & 0x0F;
    static const uint8_t neutral_rumble[8] = {0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};
    std::memcpy(&buf[1], neutral_rumble, sizeof(neutral_rumble));
    buf[9] = subcmd;
    args_len = std::min<uint8_t>(args_len, sizeof(buf) - 10);
    std::memcpy(&buf[10], args, args_len);
    return tuh_hid_send_report(slot->daddr, slot->instance, SWPRO_OUT_SUBCMD, buf, 10 + args_len);
}

// Sends the command for the current init stage; called on entry and on timeout
void swpro_send_stage(HidSlot* slot) {
    SwProCtl& c = slot->swpro;
    bool sent = false;
    switch (c.stage) {
    case SwProStage::Handshake:
        sent = swpro_send_usb_cmd(slot, SWPRO_USB_HANDSHAKE);
        break;
    case SwProStage::ForceUsb:
        sent = swpro_send_usb_cmd(slot, SWPRO_USB_FORCE_USB);
        break;
    case SwProStage::SetReportMode: {
        uint8_t mode = SWPRO_IN_FULL;
        sent = swpro_send_subcmd(slot, SWPRO_SUBCMD_SET_REPORT_MODE, &mode, 1);
        break;
    }
    case SwProStage::SetPlayerLeds: {
        uint8_t leds = 0x01;  // player 1
        sent = swpro_send_subcmd(slot, SWPRO_SUBCMD_SET_PLAYER_LEDS, &leds, 1);
        break;
    }
    case SwProStage::Ready:
        return;
    }
    c.last_tx_ms = to_ms_since_boot(get_absolute_time());
    if (sent) {
        c.attempts++;
    }
}

void swpro_enter_stage(HidSlot* slot, SwProStage stage) {
    slot->swpro.stage = stage;
    slot->swpro.attempts = 0;
    if (stage == SwProStage::Ready) {
        UH_LOG("Switch Pro Controller %u/%u ready", slot->daddr, slot->instance);
        return;
    }
    swpro_send_stage(slot);
}

// Resends or skips stalled init steps. Runs from the host task between tuh_task() calls.
void swpro_poll() {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    for (auto& slot : s_hid_slots) {
        if (!slot.used || !slot.switch_pro || slot.swpro.stage == SwProStage::Ready) continue;
        if (now - slot.swpro.last_tx_ms < SWPRO_STEP_TIMEOUT_MS) continue;
        if (slot.swpro.attempts >= SWPRO_STEP_MAX_ATTEMPTS) {
            // Unacknowledged step: move on, the pad may not implement it
            swpro_enter_stage(&slot, static_cast<SwProStage>(static_cast<uint8_t>(slot.swpro.stage) + 1));
        } else {
            swpro_send_stage(&slot);
        }
    }
}

uint16_t swpro_buttons_from_full(const uint8_t* b) {
    // b points at the three button bytes (report bytes 3..5): right, shared, left
    uint16_t m = BTN_NONE;
    if (b[0] & 0x01) m |= BTN_Y;
    if (b[0] & 0x02) m |= BTN_X;
    if (b[0] & 0x04) m |= BTN_B;
    if (b[0] & 0x08) m |= BTN_A;
    if (b[0] & 0x40) m |= BTN_R;
    if (b[0] & 0x80) m |= BTN_ZR;
    if (b[1] & 0x01) m |= BTN_MINUS;
    if (b[1] & 0x02) m |= BTN_PLUS;
    if (b[1] & 0x04) m |= BTN_RSTICK;
    if (b[1] & 0x08) m |= BTN_LSTICK;
    if (b[1] & 0x10) m |= BTN_HOME;
    if (b[1] & 0x20) m |= BTN_CAPTURE;
    if (b[2] & 0x40) m |= BTN_L;
    if (b[2] & 0x80) m |= BTN_ZL;
    return m;
}

// 12-bit stick sample -> [-1, +1]. Without reading the factory calibration from the
// controller's SPI flash, assume the usual centre of 2048 and ~1400 counts of travel.
float swpro_stick(uint32_t raw) {
    float v = (static_cast<float>(raw) - 2048.0f) / 1400.0f;
    return std::max(-1.0f, std::min(1.0f, v));
}

// Standard full report (0x30-0x33): [id][timer][battery|conn][btn r][btn shared][btn l]
// [left stick 3 bytes][right stick 3 bytes][vibration][IMU samples from byte 13, when enabled]
void swpro_parse_full(HidSlot* slot, const uint8_t* r, uint16_t len) {
    if (len < 12) return;
    PadState st;
    st.buttons = swpro_buttons_from_full(&r[3]);
    st.down  = (r[5] & 0x01) != 0;
    st.up    = (r[5] & 0x02) != 0;
    st.right = (r[5] & 0x04) != 0;
    st.left  = (r[5] & 0x08) != 0;
    uint32_t lx = r[6] | ((r[7] & 0x0F) << 8);
    uint32_t ly = (r[7] >> 4) | (r[8] << 4);
    uint32_t rx = r[9] | ((r[10] & 0x0F) << 8);
    uint32_t ry = (r[10] >> 4) | (r[11] << 4);
    // The controller reports Y growing upward; the Switch HID report grows downward
    st.lx = swpro_stick(lx);
    st.ly = -swpro_stick(ly);
    st.rx = swpro_stick(rx);
    st.ry = -swpro_stick(ry);
    apply_radial_deadzone(st.lx, st.ly);
    apply_radial_deadzone(st.rx, st.ry);
    slot->state = st;
    merge_and_push();
}

// Simple HID report (0x3F): [id][btn lo][btn hi][hat][4 x uint16 LE stick axes, 8-way only]
void swpro_parse_simple(HidSlot* slot, const uint8_t* r, uint16_t len) {
    if (len < 12) return;
    PadState st;
    uint16_t b = r[1] | (r[2] << 8);
    if (b & 0x0001) st.buttons |= BTN_B;
    if (b & 0x0002) st.buttons |= BTN_A;
    if (b & 0x0004) st.buttons |= BTN_Y;
    if (b & 0x0008) st.buttons |= BTN_X;
    if (b & 0x0010) st.buttons |= BTN_L;
    if (b & 0x0020) st.buttons |= BTN_R;
    if (b & 0x0040) st.buttons |= BTN_ZL;
    if (b & 0x0080) st.buttons |= BTN_ZR;
    if (b & 0x0100) st.buttons |= BTN_MINUS;
    if (b & 0x0200) st.buttons |= BTN_PLUS;
    if (b & 0x0400) st.buttons |= BTN_LSTICK;
    if (b & 0x0800) st.buttons |= BTN_RSTICK;
    if (b & 0x1000) st.buttons |= BTN_HOME;
    if (b & 0x2000) st.buttons |= BTN_CAPTURE;
    HidField hat;
    hat.present = true;
    hat.bit_offset = 0;
    hat.bit_size = 8;
    hat.lmin = 0;
    decode_hat(&r[3], 1, hat, st.up, st.down, st.left, st.right);
    auto axis16 = [](const uint8_t* p) {
        return (static_cast<float>(p[0] | (p[1] << 8)) - 32768.0f) / 32767.0f;
    };
    st.lx = axis16(&r[4]);
    st.ly = axis16(&r[6]);
    st.rx = axis16(&r[8]);
    st.ry = axis16(&r[10]);
    slot->state = st;
    merge_and_push();
}

void swpro_handle_report(HidSlot* slot, const uint8_t* r, uint16_t len) {
    SwProCtl& c = slot->swpro;
    switch (r[0]) {
    case SWPRO_IN_USB_ACK:
        if (len >= 2 && c.stage == SwProStage::Handshake && r[1] == SWPRO_USB_HANDSHAKE) {
            swpro_enter_stage(slot, SwProStage::ForceUsb);
            // FORCE_USB is not acknowledged, so move straight on to the subcommands
            swpro_enter_stage(slot, SwProStage::SetReportMode);
        }
        break;
    case SWPRO_IN_SUBCMD_ACK:
        // Byte 13 = ack (bit 7 set), byte 14 = subcommand being acknowledged
        if (len >= 15 && (r[13] & 0x80)) {
            if (c.stage == SwProStage::SetReportMode && r[14] == SWPRO_SUBCMD_SET_REPORT_MODE) {
                swpro_enter_stage(slot, SwProStage::SetPlayerLeds);
            } else if (c.stage == SwProStage::SetPlayerLeds && r[14] == SWPRO_SUBCMD_SET_PLAYER_LEDS) {
                swpro_enter_stage(slot, SwProStage::Ready);
            }
        }
        // 0x21 reports carry the same button/stick payload as 0x30
        swpro_parse_full(slot, r, len);
        break;
    case SWPRO_IN_FULL:
    case 0x31:
    case 0x32:
    case 0x33:
        // Streaming full reports means the report mode is in effect, whether or not the
        // pad acknowledged it (some third-party pads stream 0x30 without any handshake)
        if (c.stage < SwProStage::SetPlayerLeds) {
            swpro_enter_stage(slot, SwProStage::SetPlayerLeds);
        }
        swpro_parse_full(slot, r, len);
        break;
    case SWPRO_IN_SIMPLE:
        swpro_parse_simple(slot, r, len);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// TinyUSB host callbacks (generic HID)
// ---------------------------------------------------------------------------

extern "C" {

// Every host-controller event counts as activity for the enumeration retry logic
void tuh_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr) {
    (void)rhport;
    (void)eventid;
    (void)in_isr;
    s_last_host_activity_ms = to_ms_since_boot(get_absolute_time());
}

void tuh_mount_cb(uint8_t daddr) {
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(daddr, &vid, &pid);
    UH_LOG("USB device attached: address %u, VID:PID %04x:%04x", daddr, vid, pid);
    s_device_mounted = true;
}

void tuh_umount_cb(uint8_t daddr) {
    UH_LOG("USB device removed: address %u", daddr);
    s_device_mounted = false;
}

void tuh_hid_mount_cb(uint8_t daddr, uint8_t idx, uint8_t const* desc_report, uint16_t desc_len) {
    uint8_t protocol = tuh_hid_interface_protocol(daddr, idx);
    if (protocol == HID_ITF_PROTOCOL_KEYBOARD || protocol == HID_ITF_PROTOCOL_MOUSE) {
        UH_LOG("Ignoring HID %s (address %u)", protocol == HID_ITF_PROTOCOL_KEYBOARD ? "keyboard" : "mouse", daddr);
        return;
    }

    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(daddr, &vid, &pid);

    if (vid == NINTENDO_VID) {
        // Pro Controller, Joy-Con grip, NSO pads, and third-party pads in Switch mode:
        // the HID descriptor does not describe the real reports, so use the Nintendo
        // protocol instead of the generic parser
        HidSlot* slot = find_hid_slot(daddr, idx, true);
        if (slot == nullptr) {
            LOG_W(TAG, "No free HID gamepad slot for device %u/%u", daddr, idx);
            return;
        }
        slot->switch_pro = true;
        slot->state = PadState{};
        UH_LOG("Switch Pro Controller connected: %04x:%04x (%u/%u), starting handshake", vid, pid, daddr, idx);
        if (!tuh_hid_receive_report(daddr, idx)) {
            LOG_W(TAG, "Failed to request HID report from device %u/%u", daddr, idx);
        }
        swpro_enter_stage(slot, SwProStage::Handshake);
        return;
    }

    HidGamepadLayout layout;
    if (!parse_hid_report_descriptor(desc_report, desc_len, &layout)) {
        UH_LOG("HID interface %u/%u is not a gamepad; ignoring", daddr, idx);
        return;
    }

    HidSlot* slot = find_hid_slot(daddr, idx, true);
    if (slot == nullptr) {
        LOG_W(TAG, "No free HID gamepad slot for device %u/%u", daddr, idx);
        return;
    }
    slot->layout = layout;
    slot->state = PadState{};

    UH_LOG("HID gamepad connected: %04x:%04x (%u buttons, hat:%d, report id:%d)",
           vid, pid, layout.button_count, layout.hat.present ? 1 : 0,
           layout.has_report_id ? layout.report_id : -1);

    if (!tuh_hid_receive_report(daddr, idx)) {
        LOG_W(TAG, "Failed to request HID report from device %u/%u", daddr, idx);
    }
}

void tuh_hid_umount_cb(uint8_t daddr, uint8_t idx) {
    HidSlot* slot = find_hid_slot(daddr, idx, false);
    if (slot != nullptr) {
        slot->used = false;
        merge_and_push();
        UH_LOG("HID gamepad disconnected: %u/%u", daddr, idx);
    }
}

void tuh_hid_report_received_cb(uint8_t daddr, uint8_t idx, uint8_t const* report, uint16_t len) {
    HidSlot* slot = find_hid_slot(daddr, idx, false);
    if (slot == nullptr || len == 0) {
        return;
    }

    if (slot->switch_pro) {
        swpro_handle_report(slot, report, len);
        tuh_hid_receive_report(daddr, idx);
        return;
    }

    const HidGamepadLayout& L = slot->layout;
    const uint8_t* data = report;
    uint16_t data_len = len;
    if (L.has_report_id) {
        // The first byte is the report ID; only decode the report we parsed the layout for
        if (report[0] != L.report_id) {
            tuh_hid_receive_report(daddr, idx);
            return;
        }
        data++;
        data_len--;
    }

    PadState st;
    st.lx = decode_axis(data, data_len, L.x);
    st.ly = decode_axis(data, data_len, L.y);
    // Right stick: prefer Z/Rz (DualShock, DualSense, most DirectInput pads), fall
    // back to Rx/Ry (some sticks and older pads)
    if (L.z.present || L.rz.present) {
        st.rx = decode_axis(data, data_len, L.z);
        st.ry = decode_axis(data, data_len, L.rz);
    } else {
        st.rx = decode_axis(data, data_len, L.rx);
        st.ry = decode_axis(data, data_len, L.ry);
    }
    apply_radial_deadzone(st.lx, st.ly);
    apply_radial_deadzone(st.rx, st.ry);

    decode_hat(data, data_len, L.hat, st.up, st.down, st.left, st.right);

    for (uint8_t i = 0; i < L.button_count; ++i) {
        const HidField& f = L.buttons[i];
        if (f.present && extract_bits(data, data_len, f.bit_offset, f.bit_size) != 0) {
            st.buttons |= kHidButtonMap[i];
        }
    }

    slot->state = st;
    merge_and_push();

    tuh_hid_receive_report(daddr, idx);
}

// ---------------------------------------------------------------------------
// TinyUSB host callbacks (XInput, via vendored tusb_xinput driver)
// ---------------------------------------------------------------------------

usbh_class_driver_t const* usbh_app_driver_get_cb(uint8_t* driver_count) {
    *driver_count = 1;
    return &usbh_xinput_driver;
}

void tuh_xinput_mount_cb(uint8_t daddr, uint8_t instance, const xinputh_interface_t* xinput_itf) {
    UH_LOG("XInput controller connected: %u/%u (type %d)", daddr, instance, xinput_itf->type);
    // An Xbox 360 wireless receiver reports controllers only after a connection packet
    // arrives on the IN pipe, so LED/rumble commands must wait until then
    if (xinput_itf->type == XBOX360_WIRELESS && !xinput_itf->connected) {
        tuh_xinput_receive_report(daddr, instance);
        return;
    }
    tuh_xinput_set_led(daddr, instance, 1, true);
    tuh_xinput_set_rumble(daddr, instance, 0, 0, true);
    tuh_xinput_receive_report(daddr, instance);
}

void tuh_xinput_umount_cb(uint8_t daddr, uint8_t instance) {
    XinputSlot* slot = find_xinput_slot(daddr, instance, false);
    if (slot != nullptr) {
        slot->used = false;
        merge_and_push();
    }
    UH_LOG("XInput controller disconnected: %u/%u", daddr, instance);
}

void tuh_xinput_report_received_cb(uint8_t daddr, uint8_t instance, xinputh_interface_t const* xid_itf, uint16_t len) {
    (void)len;
    if (xid_itf->last_xfer_result == XFER_RESULT_SUCCESS && xid_itf->connected && xid_itf->new_pad_data) {
        const xinput_gamepad_t* p = &xid_itf->pad;
        XinputSlot* slot = find_xinput_slot(daddr, instance, true);
        if (slot != nullptr) {
            PadState st;
            uint16_t w = p->wButtons;
            // Positional mapping: Xbox A/B/X/Y sit at Switch B/A/Y/X positions
            if (w & XINPUT_GAMEPAD_A) st.buttons |= BTN_B;
            if (w & XINPUT_GAMEPAD_B) st.buttons |= BTN_A;
            if (w & XINPUT_GAMEPAD_X) st.buttons |= BTN_Y;
            if (w & XINPUT_GAMEPAD_Y) st.buttons |= BTN_X;
            if (w & XINPUT_GAMEPAD_LEFT_SHOULDER) st.buttons |= BTN_L;
            if (w & XINPUT_GAMEPAD_RIGHT_SHOULDER) st.buttons |= BTN_R;
            if (w & XINPUT_GAMEPAD_BACK) st.buttons |= BTN_MINUS;
            if (w & XINPUT_GAMEPAD_START) st.buttons |= BTN_PLUS;
            if (w & XINPUT_GAMEPAD_GUIDE) st.buttons |= BTN_HOME;
            if (w & XINPUT_GAMEPAD_SHARE) st.buttons |= BTN_CAPTURE;
            if (w & XINPUT_GAMEPAD_LEFT_THUMB) st.buttons |= BTN_LSTICK;
            if (w & XINPUT_GAMEPAD_RIGHT_THUMB) st.buttons |= BTN_RSTICK;
            if (p->bLeftTrigger >= XINPUT_TRIGGER_THRESHOLD) st.buttons |= BTN_ZL;
            if (p->bRightTrigger >= XINPUT_TRIGGER_THRESHOLD) st.buttons |= BTN_ZR;
            st.up = (w & XINPUT_GAMEPAD_DPAD_UP) != 0;
            st.down = (w & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
            st.left = (w & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
            st.right = (w & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
            // XInput Y axes grow upward while the Switch report's grow downward
            st.lx = static_cast<float>(p->sThumbLX) / 32767.0f;
            st.ly = static_cast<float>(p->sThumbLY) / -32767.0f;
            st.rx = static_cast<float>(p->sThumbRX) / 32767.0f;
            st.ry = static_cast<float>(p->sThumbRY) / -32767.0f;
            auto clamp1 = [](float v) { return std::max(-1.0f, std::min(1.0f, v)); };
            st.lx = clamp1(st.lx);
            st.ly = clamp1(st.ly);
            st.rx = clamp1(st.rx);
            st.ry = clamp1(st.ry);
            apply_radial_deadzone(st.lx, st.ly);
            apply_radial_deadzone(st.rx, st.ry);

            slot->state = st;
            merge_and_push();
        }
    }
    tuh_xinput_receive_report(daddr, instance);
}

}  // extern "C"

// ---------------------------------------------------------------------------
// UsbHostInput class
// ---------------------------------------------------------------------------

UsbHostInput::UsbHostInput(ControllerState& controller, const UsbHostInputConfig& config)
    : controller_(controller), config_(config), initialized_(false) {}

UsbHostInput::~UsbHostInput() {}

void UsbHostInput::host_task_entry(void* param) {
    static_cast<UsbHostInput*>(param)->run_host_task();
}

void UsbHostInput::run_host_task() {
    // Pin to core 1: tuh_init() installs the timing-critical PIO-USB interrupt handlers on
    // the core it runs on, so keep both the init and subsequent host servicing on one
    // deterministic core, away from the bulk of the core-0 startup work.
    vTaskCoreAffinitySet(nullptr, 1u << 1);

    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = config_.dp_pin;

    // Pico-PIO-USB claims the PIO state machines, DMA channel, and hardware alarm named in
    // its configuration outright and panics if any of them is already taken. Its defaults
    // (PIO0 SM0-2, DMA channel 0, alarm 2) collide with the CYW43 Wi-Fi driver on W boards,
    // which has already grabbed the first free PIO state machine and DMA channels by the
    // time this task runs, so pick resources that are actually free at this point.

    // The library needs three state machines on one PIO block and the whole 32-slot
    // instruction memory (tx 5 + nrzi 10 + eop 17), with the tx program pinned at offset 0.
    // Only a PIO block nobody else has touched qualifies.
    int pio_num = -1;
    for (uint i = 0; i < NUM_PIOS; ++i) {
        PIO pio = pio_get_instance(i);
        bool sms_free = true;
        for (uint sm = 0; sm < NUM_PIO_STATE_MACHINES; ++sm) {
            if (pio_sm_is_claimed(pio, sm)) {
                sms_free = false;
                break;
            }
        }
        pio_program_t whole_memory = {};
        whole_memory.length = PIO_INSTRUCTION_COUNT;
        whole_memory.origin = -1;
        if (sms_free && pio_can_add_program(pio, &whole_memory)) {
            pio_num = static_cast<int>(i);
            break;
        }
    }
    if (pio_num < 0) {
        LOG_E(TAG, "No free PIO block for the USB host port; pass-through disabled");
        vTaskDelete(nullptr);
        return;
    }
    pio_cfg.pio_tx_num = static_cast<uint8_t>(pio_num);
    pio_cfg.pio_rx_num = static_cast<uint8_t>(pio_num);

    // The library claims the DMA channel itself, so reserve one only long enough to learn
    // its number and release it again; nothing else allocates DMA concurrently at this point.
    int dma_ch = dma_claim_unused_channel(true);
    dma_channel_unclaim(static_cast<uint>(dma_ch));
    pio_cfg.tx_ch = static_cast<uint8_t>(dma_ch);

    // The 1 ms SOF timer interrupt is delivered to the core that creates the alarm pool,
    // which is why this happens here after pinning to core 1.
    pio_cfg.alarm_pool = alarm_pool_create_with_unused_hardware_alarm(1);
    if (pio_cfg.alarm_pool == nullptr) {
        LOG_E(TAG, "No free hardware alarm for the USB host port; pass-through disabled");
        vTaskDelete(nullptr);
        return;
    }

    if (!tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg)) {
        LOG_E(TAG, "tuh_configure() failed");
        vTaskDelete(nullptr);
        return;
    }
    if (!tuh_init(1)) {
        LOG_E(TAG, "tuh_init() failed");
        vTaskDelete(nullptr);
        return;
    }
    LOG_I(TAG, "USB host port ready on GP%u (D+) / GP%u (D-) using PIO%d, DMA channel %d",
          config_.dp_pin, config_.dp_pin + 1, pio_num, dma_ch);
    s_last_host_activity_ms = to_ms_since_boot(get_absolute_time());

    while (true) {
        // Wake periodically so stalled Switch Pro init steps can be retried even when
        // the controller sends nothing (which is exactly the case before its handshake)
        tuh_task_ext(20, false);
        swpro_poll();
        enum_retry_poll();
    }
}

bool UsbHostInput::init() {
    if (!config_.enabled) {
        LOG_I(TAG, "USB host controller pass-through disabled by configuration");
        return false;
    }
    if (config_.dp_pin > 27) {
        LOG_E(TAG, "Invalid usb_host_dp_pin %u (need D+ <= 27 so D- fits on D+ + 1)", config_.dp_pin);
        return false;
    }

    s_instance = this;
    s_controller = &controller_;
    s_log_enabled = config_.log_enabled;
    s_deadzone = std::max(0, std::min(99, config_.deadzone_percent)) / 100.0f;

    BaseType_t res = xTaskCreate(host_task_entry, "usb_host", 4096, this,
                                 configMAX_PRIORITIES - 2, nullptr);
    if (res != pdPASS) {
        LOG_E(TAG, "Failed to create USB host task");
        return false;
    }

    initialized_ = true;
    return true;
}
