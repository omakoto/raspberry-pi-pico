/*
 * TinyUSB configuration for otg-lan-test.
 *
 * Composite device: a virtual Ethernet adapter, a CDC ACM serial console (stdio logs,
 * and the 1200-baud BOOTSEL trick), a mass storage device (32 KB FAT12 config drive on
 * flash) and the Raspberry Pi reset interface (picotool reboot). The Ethernet function is
 * offered as two USB configurations: RNDIS
 * (configuration 1, what Windows and Linux pick) and CDC-ECM (configuration 2, what
 * macOS picks).
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

// CFG_TUSB_MCU and CFG_TUSB_OS are supplied on the command line by the Pico SDK.

// pico_stdio_usb's reset_interface.c reads its PICO_STDIO_USB_* settings through
// tusb_config.h (the SDK's own tusb_config.h does this include too), so pull them in here.
#include "pico/stdio_usb.h"

#define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_DEVICE)

#define CFG_TUD_ENABLED             1
#define CFG_TUD_MAX_SPEED           OPT_MODE_DEFAULT_SPEED
#define CFG_TUD_ENDPOINT0_SIZE      64

// Network class. TinyUSB has two mutually exclusive drivers: ECM/RNDIS and NCM.
#define CFG_TUD_ECM_RNDIS           1
#define CFG_TUD_NCM                 0

// CDC ACM serial console used by pico_stdio_usb.
#define CFG_TUD_CDC                 1
#define CFG_TUD_CDC_RX_BUFSIZE      256
#define CFG_TUD_CDC_TX_BUFSIZE      1024
#define CFG_TUD_CDC_EP_BUFSIZE      64

// Mass storage: the 32 KB FAT12 config drive (msc_flash_disk.c). One sector per transfer.
#define CFG_TUD_MSC                 1
#define CFG_TUD_MSC_EP_BUFSIZE      512

// No other classes. (The reset interface is a custom class driver, not a TinyUSB class.)
#define CFG_TUD_HID                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
