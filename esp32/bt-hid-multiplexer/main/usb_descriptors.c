#include <stdio.h>
#include "tusb.h"
#include "usb_descriptors.h"
#include "config.h"
#include "esp_mac.h"

// Endpoint assignments. The ESP32-S3 USB OTG controller has endpoint numbers 0-6 and five IN
// endpoints including EP0; with the serial port all four other IN endpoints are used.
#define EPNUM_CDC_NOTIF             0x81
#define EPNUM_CDC_OUT               0x02
#define EPNUM_CDC_IN                0x82
#define EPNUM_HID_KBD_MOUSE_OUT     0x03
#define EPNUM_HID_KBD_MOUSE_IN      0x83
#define EPNUM_HID_RAWHID_OUT        0x04
#define EPNUM_HID_RAWHID_IN         0x84

// Device Descriptor
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

bool g_usb_serial_enabled = false;

// Without the CDC serial port there is no interface association, so the device class is defined per
// interface.
tusb_desc_device_t const desc_device_hid_only = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

const tusb_desc_device_t *usb_device_descriptor(void) {
    return g_usb_serial_enabled ? &desc_device : &desc_device_hid_only;
}

// HID 0 Report Descriptor: Composite Keyboard + Mouse
uint8_t const desc_hid_kbd_mouse_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD( HID_REPORT_ID(REPORT_ID_KEYBOARD) ),
    TUD_HID_REPORT_DESC_MOUSE   ( HID_REPORT_ID(REPORT_ID_MOUSE) )
};

// HID 1 Report Descriptor: Vendor RawHID (VIAL WebHID Usage Page 0xFF60, Usage 0x0061)
uint8_t const desc_hid_rawhid_report[] = {
    HID_USAGE_PAGE_N ( 0xFF60, 2 )                     ,
    HID_USAGE        ( 0x61 )                          ,
    HID_COLLECTION   ( HID_COLLECTION_APPLICATION )    ,
      // Input Report: 32 bytes
      HID_USAGE       ( 0x62 )                         ,
      HID_LOGICAL_MIN ( 0x00 )                         ,
      HID_LOGICAL_MAX_N ( 0x00FF, 2 )                  ,
      HID_REPORT_SIZE ( 8 )                            ,
      HID_REPORT_COUNT( 32 )                           ,
      HID_INPUT       ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ) ,
      // Output Report: 32 bytes
      HID_USAGE       ( 0x63 )                         ,
      HID_LOGICAL_MIN ( 0x00 )                         ,
      HID_LOGICAL_MAX_N ( 0x00FF, 2 )                  ,
      HID_REPORT_SIZE ( 8 )                            ,
      HID_REPORT_COUNT( 32 )                           ,
      HID_OUTPUT      ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ) ,
    HID_COLLECTION_END
};

uint8_t const * tud_hid_descriptor_report_cb(uint8_t instance) {
    if (instance == 0) {
        return desc_hid_kbd_mouse_report;
    } else if (instance == 1) {
        return desc_hid_rawhid_report;
    }
    return NULL;
}

// Configuration Descriptor
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_INOUT_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

uint8_t const desc_configuration[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    // Interface 0 & 1: CDC ACM Serial
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),

    // Interface 2: HID Keyboard & Mouse
    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID_KBD_MOUSE, 5, HID_ITF_PROTOCOL_KEYBOARD, sizeof(desc_hid_kbd_mouse_report),
                             EPNUM_HID_KBD_MOUSE_OUT, EPNUM_HID_KBD_MOUSE_IN, 16, 1),

    // Interface 3: HID VIAL RawHID
    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID_RAWHID, 6, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_rawhid_report),
                             EPNUM_HID_RAWHID_OUT, EPNUM_HID_RAWHID_IN, 32, 1)
};

// The same two HID interfaces without the CDC serial port (interface numbers start at 0).
#define CONFIG_TOTAL_LEN_HID_ONLY  (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

uint8_t const desc_configuration_hid_only[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CONFIG_TOTAL_LEN_HID_ONLY, 0x00, 100),

    // Interface 0: HID Keyboard & Mouse
    TUD_HID_INOUT_DESCRIPTOR(0, 5, HID_ITF_PROTOCOL_KEYBOARD, sizeof(desc_hid_kbd_mouse_report),
                             EPNUM_HID_KBD_MOUSE_OUT, EPNUM_HID_KBD_MOUSE_IN, 16, 1),

    // Interface 1: HID VIAL RawHID
    TUD_HID_INOUT_DESCRIPTOR(1, 6, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_rawhid_report),
                             EPNUM_HID_RAWHID_OUT, EPNUM_HID_RAWHID_IN, 32, 1)
};

const uint8_t *usb_configuration_descriptor(void) {
    return g_usb_serial_enabled ? desc_configuration : desc_configuration_hid_only;
}

// String Descriptors (esp_tinyusb converts them to UTF-16; at most 31 characters each).
static char serial_str[13];

static const char *string_desc_arr[] = {
    (const char[]) { 0x09, 0x04 },          // 0: Supported language 0x0409 (English US)
    "omakoto",                               // 1: Manufacturer
    "ESP32-S3 BLE HID Multiplexer",          // 2: Product
    serial_str,                              // 3: Serial
    "ESP32-S3 Serial Console",               // 4: CDC Console
    "ESP32-S3 Keyboard/Mouse",               // 5: HID Composite
    "ESP32-S3 VIAL Configurator"             // 6: VIAL RawHID
};

const char **usb_string_descriptors(int *count) {
    if (serial_str[0] == '\0') {
        // The factory MAC address is unique per chip.
        uint8_t mac[6];
        esp_efuse_mac_get_default(mac);
        snprintf(serial_str, sizeof(serial_str), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    *count = sizeof(string_desc_arr) / sizeof(string_desc_arr[0]);
    return string_desc_arr;
}
