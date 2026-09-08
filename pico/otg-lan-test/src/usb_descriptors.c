/*
 * USB descriptors for otg-lan-test.
 *
 * Composite device, three functions:
 *   - virtual Ethernet adapter (interfaces 0-1), offered as two USB configurations:
 *       configuration 1: RNDIS    -- Windows and Linux pick this one
 *       configuration 2: CDC-ECM  -- macOS does not do RNDIS and picks this one
 *     The host picks whichever configuration it has a driver for; both carry the same
 *     traffic and TinyUSB's ecm_rndis_device driver handles either.
 *   - CDC ACM serial console (interfaces 2-3): stdio logs; setting 1200 baud reboots
 *     the board into BOOTSEL (handled by pico_stdio_usb).
 *   - Raspberry Pi reset interface (interface 4): lets `picotool reboot` work
 *     (custom class driver in pico_stdio_usb's reset_interface.c).
 */

#include <string.h>

#include "pico/unique_id.h"
#include "pico/usb_reset_interface.h"
#include "tusb.h"

// Ethernet MAC address the *host* sees on its side of the link. Filled in from the
// flash unique ID at startup so two boards on one PC do not collide. It is declared
// (extern) by TinyUSB's net driver, which reports it to the host over RNDIS/ECM.
uint8_t tud_network_mac_address[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00};

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_INTERFACE,
    STRID_MAC,
    STRID_CDC,
    STRID_RESET,
};

enum {
    ITF_NUM_NET_CTRL = 0,
    ITF_NUM_NET_DATA,
    ITF_NUM_CDC_CTRL,
    ITF_NUM_CDC_DATA,
    ITF_NUM_RESET,
    ITF_NUM_TOTAL,
};

enum {
    CONFIG_ID_RNDIS = 0,
    CONFIG_ID_ECM = 1,
    CONFIG_ID_COUNT,
};

// Endpoint numbers. RP2040/RP2350 allow IN and OUT to share a number.
#define EPNUM_NET_NOTIF 0x81
#define EPNUM_NET_OUT   0x02
#define EPNUM_NET_IN    0x82
#define EPNUM_CDC_NOTIF 0x83
#define EPNUM_CDC_OUT   0x04
#define EPNUM_CDC_IN    0x84

// The VID/PID follow the TinyUSB example convention (0xCafe, 0x4000 | class bitmap:
// bit 5 = network, bit 0 = CDC). Nothing on the host keys off them: RNDIS, ECM and CDC
// are matched by class/subclass/protocol, and picotool matches the reset interface.
#define USB_VID 0xCafe
#define USB_PID 0x4021

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    // IAD device class so the host groups the control + data interfaces as one function.
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = CONFIG_ID_COUNT,
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&desc_device;
}

// Vendor-specific interface with no endpoints; picotool finds it by class/subclass/protocol
// and sends a control request to it. Same layout as the SDK's stdio_usb_descriptors.c.
#define TUD_RPI_RESET_DESC_LEN 9
#define TUD_RPI_RESET_DESCRIPTOR(_itfnum, _stridx) \
    9, TUSB_DESC_INTERFACE, _itfnum, 0, 0, TUSB_CLASS_VENDOR_SPECIFIC, RESET_INTERFACE_SUBCLASS, \
        RESET_INTERFACE_PROTOCOL, _stridx,

// Interfaces shared by both configurations, appended after the network function.
#define COMMON_DESCRIPTORS                                                                                   \
    /* Interface number, string index, EP notification address and size, EP data address (out, in) and */ \
    /* size. */                                                                                            \
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_CTRL, STRID_CDC, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),  \
    TUD_RPI_RESET_DESCRIPTOR(ITF_NUM_RESET, STRID_RESET)

#define COMMON_DESC_LEN        (TUD_CDC_DESC_LEN + TUD_RPI_RESET_DESC_LEN)
#define RNDIS_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_RNDIS_DESC_LEN + COMMON_DESC_LEN)
#define ECM_CONFIG_TOTAL_LEN   (TUD_CONFIG_DESC_LEN + TUD_CDC_ECM_DESC_LEN + COMMON_DESC_LEN)

static const uint8_t rndis_configuration[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(CONFIG_ID_RNDIS + 1, ITF_NUM_TOTAL, 0, RNDIS_CONFIG_TOTAL_LEN, 0, 100),
    // Interface number, string index, EP notification address and size, EP data address (out, in) and size.
    TUD_RNDIS_DESCRIPTOR(ITF_NUM_NET_CTRL, STRID_INTERFACE, EPNUM_NET_NOTIF, 8, EPNUM_NET_OUT, EPNUM_NET_IN,
                         CFG_TUD_NET_ENDPOINT_SIZE),
    COMMON_DESCRIPTORS
};

static const uint8_t ecm_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(CONFIG_ID_ECM + 1, ITF_NUM_TOTAL, 0, ECM_CONFIG_TOTAL_LEN, 0, 100),
    // Interface number, description string index, MAC address string index, EP notification address and
    // size, EP data address (out, in) and size, max segment size.
    TUD_CDC_ECM_DESCRIPTOR(ITF_NUM_NET_CTRL, STRID_INTERFACE, STRID_MAC, EPNUM_NET_NOTIF, 64, EPNUM_NET_OUT,
                           EPNUM_NET_IN, CFG_TUD_NET_ENDPOINT_SIZE, CFG_TUD_NET_MTU),
    COMMON_DESCRIPTORS
};

TU_VERIFY_STATIC(sizeof(rndis_configuration) == RNDIS_CONFIG_TOTAL_LEN, "RNDIS config length mismatch");
TU_VERIFY_STATIC(sizeof(ecm_configuration) == ECM_CONFIG_TOTAL_LEN, "ECM config length mismatch");

static const uint8_t *const configuration_arr[CONFIG_ID_COUNT] = {
    [CONFIG_ID_RNDIS] = rndis_configuration,
    [CONFIG_ID_ECM] = ecm_configuration,
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    return (index < CONFIG_ID_COUNT) ? configuration_arr[index] : NULL;
}

static const char *const string_desc_arr[] = {
    [STRID_LANGID] = (const char[]){0x09, 0x04},  // English (0x0409)
    [STRID_MANUFACTURER] = "omakoto",
    [STRID_PRODUCT] = "otg-lan-test USB Ethernet",
    [STRID_SERIAL] = NULL,  // generated from the flash unique ID
    [STRID_INTERFACE] = "otg-lan-test Network Interface",
    [STRID_MAC] = NULL,  // generated from tud_network_mac_address
    [STRID_CDC] = "otg-lan-test Serial Console",
    [STRID_RESET] = "Reset",
};

static uint16_t _desc_str[32 + 1];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    size_t chr_count = 0;

    switch (index) {
    case STRID_LANGID:
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
        break;

    case STRID_SERIAL: {
        char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
        pico_get_unique_board_id_string(serial, sizeof(serial));
        chr_count = strlen(serial);
        for (size_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = (uint16_t)serial[i];
        }
        break;
    }

    case STRID_MAC:
        // CDC-ECM carries the MAC address as a 12-hex-digit string descriptor.
        for (size_t i = 0; i < sizeof(tud_network_mac_address); i++) {
            _desc_str[1 + chr_count++] = "0123456789ABCDEF"[(tud_network_mac_address[i] >> 4) & 0xf];
            _desc_str[1 + chr_count++] = "0123456789ABCDEF"[(tud_network_mac_address[i] >> 0) & 0xf];
        }
        break;

    default:
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) return NULL;
        const char *str = string_desc_arr[index];
        if (str == NULL) return NULL;
        chr_count = strlen(str);
        size_t const max_count = sizeof(_desc_str) / sizeof(_desc_str[0]) - 1;
        if (chr_count > max_count) chr_count = max_count;
        for (size_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = (uint16_t)str[i];
        }
        break;
    }

    // First byte is length (including header), second byte is string type.
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}
