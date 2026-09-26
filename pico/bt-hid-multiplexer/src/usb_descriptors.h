#ifndef USB_DESCRIPTORS_H_
#define USB_DESCRIPTORS_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    REPORT_ID_KEYBOARD = 1,
    REPORT_ID_MOUSE    = 2
};

enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_HID_KBD_MOUSE,
    ITF_NUM_HID_RAWHID,
    ITF_NUM_TOTAL
};

// RawHID buffer size for VIAL protocol transfers
#define RAWHID_REPORT_SIZE 32

#ifdef __cplusplus
}
#endif

#endif // USB_DESCRIPTORS_H_
