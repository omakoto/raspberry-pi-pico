/*
 * Switch-facing USB gamepad device implementation.
 * Provides the HORI Pokken composite descriptors and report transmission, and routes
 * descriptors, output reports and the report stream to procon_device when the firmware
 * is configured to impersonate a Nintendo Pro Controller.
 */

#include "gamepad_hid.hpp"
#include <cstring>
#include <algorithm>
#include "FreeRTOS.h"
#include "task.h"
#include "tusb.h"
#include "pico/bootrom.h"
#include "dual_logger.hpp"
#include "procon_device.hpp"

static const char* TAG = "GamepadHid";

// HORI Pokken Controller 8-byte HID Report Descriptor
static const uint8_t switch_hid_report_descriptor[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x05,        // Usage (Game Pad)
    0xA1, 0x01,        // Collection (Application)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x35, 0x00,        //   Physical Minimum (0)
    0x45, 0x01,        //   Physical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x10,        //   Report Count (16 buttons)
    0x05, 0x09,        //   Usage Page (Button)
    0x19, 0x01,        //   Usage Minimum (0x01)
    0x29, 0x10,        //   Usage Maximum (0x10)
    0x81, 0x02,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x05, 0x01,        //   Usage Page (Generic Desktop Ctrls)
    0x25, 0x07,        //   Logical Maximum (7)
    0x46, 0x3B, 0x01,  //   Physical Maximum (315)
    0x75, 0x04,        //   Report Size (4)
    0x95, 0x01,        //   Report Count (1)
    0x65, 0x14,        //   Unit (Eng Rot:Angular Pos)
    0x09, 0x39,        //   Usage (Hat switch)
    0x81, 0x42,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,Null State)
    0x65, 0x00,        //   Unit (None)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x04,        //   Report Size (4)
    0x81, 0x01,        //   Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x46, 0xFF, 0x00,  //   Physical Maximum (255)
    0x09, 0x30,        //   Usage (X - Left Stick X)
    0x09, 0x31,        //   Usage (Y - Left Stick Y)
    0x09, 0x32,        //   Usage (Z - Right Stick X)
    0x09, 0x35,        //   Usage (Rz - Right Stick Y)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x04,        //   Report Count (4)
    0x81, 0x02,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x01,        //   Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0xC0               // End Collection
};

// Device Descriptor
// Use MISC class with IAD protocol for composite USB device (HID + CDC + MSC)
static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x0F0D,
    .idProduct          = 0x0092,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

// Configuration Descriptor (Composite HID + CDC ACM + MSC)
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)
static const uint8_t desc_configuration[] = {
    // Config number, interface count (4: HID + CDC*2 + MSC), string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 4, 0, TUSB_DESC_TOTAL_LEN, 0, 500),
    // Interface 0: HID Gamepad (EP 0x81 IN, 16 bytes, interval 5ms)
    TUD_HID_DESCRIPTOR(0, 4, false, sizeof(switch_hid_report_descriptor), 0x81, 16, 5),
    // Interface 1 & 2: CDC ACM Serial (Notif EP 0x82, Data OUT EP 0x03, Data IN EP 0x83)
    TUD_CDC_DESCRIPTOR(1, 5, 0x82, 8, 0x03, 0x83, 64),
    // Interface 3: MSC Storage (EP Out 0x04, EP In 0x84, EP Size 64)
    TUD_MSC_DESCRIPTOR(3, 6, 0x04, 0x84, 64),
};

// String Descriptors
static const char* string_descriptors[] = {
    (const char[]){0x09, 0x04}, // 0: English (0x0409)
    "HORI CO.,LTD.",            // 1: Manufacturer
    "POKKEN CONTROLLER",        // 2: Product
    "000000000001",             // 3: Serial Number
    "Switch Gamepad HID",       // 4: Interface (HID)
    "Pico CDC Console",         // 5: Interface (CDC)
    "Pico MSC Storage",         // 6: Interface (MSC)
};

static uint16_t s_desc_str[64];
static GamepadHid* s_gamepad_instance = nullptr;
static GamepadIdentity s_identity = GamepadIdentity::Pokken;

static bool is_procon() {
    return s_identity == GamepadIdentity::ProCon;
}

// ---------------------------------------------------------------------------
// TinyUSB Standard Descriptor & Device Callbacks
// ---------------------------------------------------------------------------

extern "C" {

uint8_t const* tud_descriptor_device_cb(void) {
    if (is_procon()) return procon::device_descriptor();
    return (uint8_t const*)&desc_device;
}

uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    if (is_procon()) return procon::configuration_descriptor();
    return desc_configuration;
}

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    size_t char_count = 0;

    if (index == 0) {
        std::memcpy(&s_desc_str[1], string_descriptors[0], 2);
        char_count = 1;
    } else {
        const char* str = nullptr;
        if (is_procon()) {
            str = procon::string_descriptor(index);
        } else if (index < sizeof(string_descriptors) / sizeof(string_descriptors[0])) {
            str = string_descriptors[index];
        }
        if (str == nullptr) {
            return nullptr;
        }
        char_count = std::strlen(str);
        if (char_count > 63) {
            char_count = 63;
        }
        for (size_t i = 0; i < char_count; ++i) {
            s_desc_str[1 + i] = str[i];
        }
    }

    s_desc_str[0] = static_cast<uint16_t>((TUSB_DESC_STRING << 8) | (2 * char_count + 2));
    return s_desc_str;
}

uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    if (is_procon()) return procon::report_descriptor();
    return switch_hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen) {
    (void)instance; (void)report_id;
    if (is_procon()) {
        return 0;  // the Pro Controller protocol runs over the interrupt endpoints only
    }
    if (report_type == HID_REPORT_TYPE_INPUT) {
        SwitchReport rep = {
            .buttons = BTN_NONE,
            .hat = HAT_CENTER,
            .lx = 128,
            .ly = 128,
            .rx = 128,
            .ry = 128,
            .vendor = 0x00
        };
        if (s_gamepad_instance) {
            rep = s_gamepad_instance->get_current_report();
        }
        uint16_t copy_len = std::min(reqlen, static_cast<uint16_t>(sizeof(SwitchReport)));
        std::memcpy(buffer, &rep, copy_len);
        return copy_len;
    }
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize) {
    (void)instance; (void)report_type;
    if (!is_procon()) {
        return;
    }
    // Data from the OUT endpoint arrives as the raw packet (report_id 0, ID in buffer[0]);
    // a control SET_REPORT may strip the ID, so put it back for the parser
    if (report_id == 0 || (bufsize > 0 && buffer[0] == report_id)) {
        procon::on_output_report(buffer, bufsize);
    } else {
        uint8_t packet[CFG_TUD_HID_EP_BUFSIZE];
        packet[0] = report_id;
        uint16_t n = std::min<uint16_t>(bufsize, CFG_TUD_HID_EP_BUFSIZE - 1);
        std::memcpy(&packet[1], buffer, n);
        procon::on_output_report(packet, n + 1);
    }
}

void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const* p_line_coding) {
    if (itf == 0 && p_line_coding->bit_rate == 1200) {
        reboot_to_bootsel();
    }
}

void tud_mount_cb(void) {
    if (is_procon()) procon::on_mount();
    if (s_gamepad_instance) {
        s_gamepad_instance->on_mount();
    }
}

void tud_umount_cb(void) {
    if (is_procon()) procon::on_umount();
    if (s_gamepad_instance) {
        s_gamepad_instance->on_umount();
    }
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const* report, uint16_t len) {
    (void)instance; (void)report; (void)len;
    if (s_gamepad_instance) {
        s_gamepad_instance->on_report_complete();
    }
}

} // extern "C"

static volatile bool s_bootsel_reboot_requested = false;

void reboot_to_bootsel() {
    s_bootsel_reboot_requested = true;
}

// ---------------------------------------------------------------------------
// GamepadHid Class Implementation
// ---------------------------------------------------------------------------

GamepadHid::GamepadHid()
    : current_report_{}, last_report_{}, report_sent_(false), initialized_(false), identity_(GamepadIdentity::Pokken) {
    current_report_ = {
        .buttons = BTN_NONE,
        .hat = HAT_CENTER,
        .lx = 128,
        .ly = 128,
        .rx = 128,
        .ry = 128,
        .vendor = 0x00
    };
    std::memset(&last_report_, 0xFF, sizeof(last_report_));
}

GamepadHid::~GamepadHid() {}

void GamepadHid::usb_task_entry(void* param) {
    static_cast<GamepadHid*>(param)->run_usb_task();
}

void GamepadHid::run_usb_task() {
    while (true) {
        // Sleep on the TinyUSB event queue that the USB ISR posts to, so USB transactions are
        // serviced with no polling latency. The 10ms cap only bounds how long a BOOTSEL reboot
        // request waits when the bus is idle.
        // In Pro Controller mode the periodic report needs a finer wake-up so its 15 ms
        // cadence is kept even when the bus is otherwise idle.
        tud_task_ext(is_procon() ? 5 : 10, false);

        if (s_bootsel_reboot_requested) {
            // Allow in-flight USB transactions (such as control ACK or serial echo) to complete
            vTaskDelay(pdMS_TO_TICKS(50));
            // Explicitly disconnect USB PHY to inform host before jumping to bootrom
            tud_disconnect();
            vTaskDelay(pdMS_TO_TICKS(150));
            reset_usb_boot(0, 0);
        }

        if (is_procon()) {
            procon::tick();
            continue;
        }

        // Transmit initial neutral report once host finishes mounting and endpoint is ready
        if (!report_sent_ && tud_mounted() && tud_hid_ready()) {
            if (tud_hid_report(0, &current_report_, sizeof(SwitchReport))) {
                last_report_ = current_report_;
                report_sent_ = true;
                LOG_I(TAG, "Sent initial neutral HID report to host");
            }
        }
    }
}

bool GamepadHid::init(GamepadIdentity identity, bool composite) {
    identity_ = identity;
    s_identity = identity;
    s_gamepad_instance = this;
    if (identity == GamepadIdentity::ProCon) {
        LOG_I(TAG, "Initializing TinyUSB device as Nintendo Pro Controller%s...", composite ? " (+ CDC + MSC)" : "");
        procon::init(composite);
    } else {
        LOG_I(TAG, "Initializing TinyUSB Composite Device (Switch Gamepad HID + CDC + MSC)...");
    }

    if (!tusb_init()) {
        LOG_E(TAG, "tusb_init() failed");
        return false;
    }

    BaseType_t res = xTaskCreate(usb_task_entry, "usb_task", 2048, this, configMAX_PRIORITIES - 1, nullptr);
    if (res != pdPASS) {
        LOG_E(TAG, "Failed to create USB device task");
        return false;
    }

    initialized_ = true;
    LOG_I(TAG, "TinyUSB composite driver and task started successfully");
    return true;
}

bool GamepadHid::is_mounted() const {
    return tud_mounted();
}

void GamepadHid::send_report(uint16_t buttons, uint8_t hat, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry) {
    SwitchReport report = {
        .buttons = buttons,
        .hat = hat,
        .lx = lx,
        .ly = ly,
        .rx = rx,
        .ry = ry,
        .vendor = 0x00
    };
    current_report_ = report;

    if (is_procon()) {
        // The Pro Controller stream is generated periodically by the USB task
        procon::InputState in;
        in.buttons = buttons;
        in.hat = hat;
        in.lx = lx;
        in.ly = ly;
        in.rx = rx;
        in.ry = ry;
        procon::set_input(in);
        return;
    }

    if (!report_sent_ || std::memcmp(&report, &last_report_, sizeof(SwitchReport)) != 0) {
        if (tud_mounted() && tud_hid_ready()) {
            if (tud_hid_report(0, &report, sizeof(SwitchReport))) {
                last_report_ = report;
                report_sent_ = true;
            }
        }
    }
}

void GamepadHid::on_mount() {
    report_sent_ = false;
}

void GamepadHid::on_umount() {
    report_sent_ = false;
}

void GamepadHid::on_report_complete() {
    if (is_procon()) return;
    // If state changed while the previous transfer was in flight, transmit updated report
    if (!report_sent_ || std::memcmp(&current_report_, &last_report_, sizeof(SwitchReport)) != 0) {
        if (tud_mounted() && tud_hid_ready()) {
            if (tud_hid_report(0, &current_report_, sizeof(SwitchReport))) {
                last_report_ = current_report_;
                report_sent_ = true;
            }
        }
    }
}

SwitchReport GamepadHid::get_current_report() const {
    return current_report_;
}
