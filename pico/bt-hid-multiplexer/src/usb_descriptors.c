#include "tusb.h"
#include "usb_descriptors.h"
#include "pico/unique_id.h"

// Endpoint assignments
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
    .idVendor           = 0x2E8A, // Raspberry Pi
    .idProduct          = 0x000C, // Multiplexer Device
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

uint8_t const * tud_descriptor_device_cb(void) {
    return (uint8_t const *) &desc_device;
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

uint8_t const * tud_descriptor_configuration_cb(uint8_t index) {
    (void) index;
    return desc_configuration;
}

// String Descriptors
static char serial_str[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];

static char const* string_desc_arr[] = {
    (const char[]) { 0x09, 0x04 },          // 0: Supported language 0x0409 (English US)
    "Raspberry Pi",                          // 1: Manufacturer
    "Pico 2 W BLE HID Multiplexer",          // 2: Product
    serial_str,                              // 3: Serial
    "Pico 2 W Serial Console",               // 4: CDC Console
    "Pico 2 W Keyboard/Mouse",               // 5: HID Composite
    "Pico 2 W VIAL Configurator"             // 6: VIAL RawHID
};

static uint16_t _desc_str[64];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void) langid;

    if (serial_str[0] == '\0') {
        pico_get_unique_board_id_string(serial_str, sizeof(serial_str));
    }

    uint8_t count;
    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        count = 1;
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) return NULL;
        const char* str = string_desc_arr[index];
        count = strlen(str);
        if (count > 63) count = 63;
        for (uint8_t i = 0; i < count; i++) {
            _desc_str[1 + i] = str[i];
        }
    }

    _desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * count + 2);
    return _desc_str;
}
