#ifndef USB_DESCRIPTORS_H_
#define USB_DESCRIPTORS_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    REPORT_ID_KEYBOARD = 1,
    REPORT_ID_MOUSE    = 2,
    REPORT_ID_CONSUMER = 3,  // 16-bit consumer page usage (media and browser keys)
    REPORT_ID_SYSTEM   = 4   // 1 = power down, 2 = sleep, 3 = wake up
};

enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_HID_KBD_MOUSE,
    ITF_NUM_HID_RAWHID,
    ITF_NUM_TOTAL
};

// Whether the USB device includes the CDC serial port. Must be set before the descriptors are read
// (usb_hid_init): the USB descriptors cannot change while the device is attached.
extern bool g_usb_serial_enabled;

// RawHID buffer size for VIAL protocol transfers
#define RAWHID_REPORT_SIZE 32

#ifdef __cplusplus
}
#endif

#endif // USB_DESCRIPTORS_H_
