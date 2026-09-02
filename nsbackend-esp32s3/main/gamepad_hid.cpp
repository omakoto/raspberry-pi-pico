#include "gamepad_hid.hpp"
#include <cstring>
#include "esp_log.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_msc.h"
#include "class/hid/hid_device.h"
#include "class/cdc/cdc_device.h"
#include "class/msc/msc_device.h"

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

// String Descriptors
static const char* switch_string_descriptor[] = {
    (const char[]){0x09, 0x04}, // 0: English (0x0409)
    "HORI CO.,LTD.",            // 1: Manufacturer
    "POKKEN CONTROLLER",        // 2: Product
    "000000000001",             // 3: Serial Number
    "Switch Gamepad HID",       // 4: Interface (HID)
    "ESP32-S3 CDC Console",     // 5: Interface (CDC)
    "ESP32-S3 MSC Storage",     // 6: Interface (MSC)
};

// Configuration Descriptor (Composite HID Gamepad + CDC ACM + MSC Storage)
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)
static const uint8_t switch_configuration_descriptor[] = {
    // Config number, interface count (4: 1 HID + 2 CDC + 1 MSC), string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 4, 0, TUSB_DESC_TOTAL_LEN, 0, 500),
    // Interface 0: HID Gamepad (EP 0x81 IN, 16 bytes, interval 5ms)
    TUD_HID_DESCRIPTOR(0, 4, false, sizeof(switch_hid_report_descriptor), 0x81, 16, 5),
    // Interface 1 & 2: CDC ACM Serial (Notif EP 0x82, Data OUT EP 0x03, Data IN EP 0x83)
    TUD_CDC_DESCRIPTOR(1, 5, 0x82, 8, 0x03, 0x83, 64),
    // Interface 3: MSC Storage (EP Out 0x04, EP In 0x84, EP Size 64)
    TUD_MSC_DESCRIPTOR(3, 6, 0x04, 0x84, 64),
};

// Custom Device Descriptor for HORI Pokken Controller with IAD (Composite Device)
static const tusb_desc_device_t switch_device_descriptor = {
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

// TinyUSB Callbacks
extern "C" {

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return switch_hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

} // extern "C"

GamepadHid::GamepadHid() : initialized_(false) {
    std::memset(&last_report_, 0, sizeof(last_report_));
    last_report_.hat = HAT_CENTER;
    last_report_.lx = 128;
    last_report_.ly = 128;
    last_report_.rx = 128;
    last_report_.ry = 128;
}

GamepadHid::~GamepadHid() {}

bool GamepadHid::init(wl_handle_t wl_handle) {
    ESP_LOGI(TAG, "Initializing TinyUSB Composite Device (Switch Gamepad HID + CDC Serial + MSC Storage)...");

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &switch_device_descriptor;
    tusb_cfg.descriptor.full_speed_config = switch_configuration_descriptor;
    tusb_cfg.descriptor.string = switch_string_descriptor;
    tusb_cfg.descriptor.string_count = sizeof(switch_string_descriptor) / sizeof(switch_string_descriptor[0]);

#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = switch_configuration_descriptor;
#endif

    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install TinyUSB driver: %s", esp_err_to_name(ret));
        return false;
    }

    // Initialize CDC ACM for serial console output
    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = nullptr,
        .callback_rx_wanted_char = nullptr,
        .callback_line_state_changed = nullptr,
        .callback_line_coding_changed = nullptr
    };
    esp_err_t cdc_ret = tinyusb_cdcacm_init(&acm_cfg);
    if (cdc_ret == ESP_OK) {
        ESP_LOGI(TAG, "TinyUSB CDC ACM serial interface initialized");
    } else {
        ESP_LOGW(TAG, "Failed to initialize TinyUSB CDC ACM: %s", esp_err_to_name(cdc_ret));
    }

    // Initialize MSC Storage if valid Wear Levelling handle is provided
    if (wl_handle != WL_INVALID_HANDLE) {
        const tinyusb_msc_storage_config_t msc_cfg = {
            .medium = {
                .wl_handle = wl_handle
            },
            .fat_fs = {
                .base_path = nullptr,
                .config = {
                    .format_if_mount_failed = false,
                    .max_files = 2,
                    .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
                    .disk_status_check_enable = false,
                    .use_one_fat = false
                },
                .do_not_format = true,
                .format_flags = 0
            },
            .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB
        };
        tinyusb_msc_storage_handle_t msc_handle;
        esp_err_t msc_ret = tinyusb_msc_new_storage_spiflash(&msc_cfg, &msc_handle);
        if (msc_ret == ESP_OK) {
            ESP_LOGI(TAG, "TinyUSB MSC Storage initialized for FATFS partition");
        } else {
            ESP_LOGW(TAG, "Failed to initialize TinyUSB MSC Storage: %s", esp_err_to_name(msc_ret));
        }
    }

    initialized_ = true;
    ESP_LOGI(TAG, "TinyUSB Composite Gamepad + CDC + MSC driver installed successfully");
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
