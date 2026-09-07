/*
 * TinyUSB Configuration for nsbackend-pico.
 * Enables Composite USB Device:
 * - Interface 0: HORI Pokken Gamepad HID
 * - Interface 1 & 2: CDC ACM Serial Console
 * - Interface 3: Mass Storage Class (MSC) for config FAT partition
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

// Port configuration (Device mode)
#define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_DEVICE)

// Enabled Device Mode
#define CFG_TUD_ENABLED             1
#define CFG_TUD_MAX_SPEED           OPT_MODE_DEFAULT_SPEED
#define CFG_TUD_ENDPOINT0_SIZE      64

// Class Drivers
#define CFG_TUD_HID                 1
#define CFG_TUD_CDC                 1
#define CFG_TUD_MSC                 1

// HID Configuration
#define CFG_TUD_HID_EP_BUFSIZE      16

// CDC Configuration
#define CFG_TUD_CDC_RX_BUFSIZE      1024
#define CFG_TUD_CDC_TX_BUFSIZE      2048
#define CFG_TUD_CDC_EP_BUFSIZE      64

// MSC Configuration
#define CFG_TUD_MSC_EP_BUFSIZE      512

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
