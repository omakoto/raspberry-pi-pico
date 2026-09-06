/*
 * HORI Pokken Controller USB HID & Composite Device Implementation.
 * Provides TinyUSB composite descriptors and report transmission.
 */

#include "gamepad_hid.hpp"
#include <cstring>
#include <algorithm>
#include "FreeRTOS.h"
#include "task.h"
#include "tusb.h"
#include "dual_logger.hpp"

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

// ---------------------------------------------------------------------------
// TinyUSB Standard Descriptor Callbacks
// ---------------------------------------------------------------------------

extern "C" {

uint8_t const* tud_descriptor_device_cb(void) {
    return (uint8_t const*)&desc_device;
}

uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    size_t char_count = 0;

    if (index == 0) {
        std::memcpy(&s_desc_str[1], string_descriptors[0], 2);
        char_count = 1;
    } else {
        if (index >= sizeof(string_descriptors) / sizeof(string_descriptors[0])) {
            return nullptr;
        }
        const char* str = string_descriptors[index];
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
    return switch_hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}

} // extern "C"

// ---------------------------------------------------------------------------
// GamepadHid Class Implementation
// ---------------------------------------------------------------------------

GamepadHid::GamepadHid() : initialized_(false) {
    std::memset(&last_report_, 0, sizeof(last_report_));
    last_report_.hat = HAT_CENTER;
    last_report_.lx = 128;
    last_report_.ly = 128;
    last_report_.rx = 128;
    last_report_.ry = 128;
}

GamepadHid::~GamepadHid() {}

void GamepadHid::usb_task_entry(void* param) {
    static_cast<GamepadHid*>(param)->run_usb_task();
}

void GamepadHid::run_usb_task() {
    while (true) {
        tud_task();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

bool GamepadHid::init() {
    LOG_I(TAG, "Initializing TinyUSB Composite Device (Switch Gamepad HID + CDC + MSC)...");

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

    if (std::memcmp(&report, &last_report_, sizeof(SwitchReport)) != 0) {
        if (tud_mounted() && tud_hid_ready()) {
            tud_hid_report(0, &report, sizeof(SwitchReport));
            last_report_ = report;
        }
    }
}
