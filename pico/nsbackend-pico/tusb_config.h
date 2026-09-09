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

// CFG_TUSB_OS is supplied on the compiler command line by the Pico SDK, driven by the
// TINYUSB_OPT_OS variable set in CMakeLists.txt. It must stay OPT_OS_FREERTOS: with the
// OPT_OS_PICO osal, osal_queue_receive() ignores its timeout and returns immediately, which
// turns the USB device task into an unyielding spin that starves every other FreeRTOS task.

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

// ---------------------------------------------------------------------------
// Host mode (rhport 1): PIO-USB bit-banged full-speed port on two GPIOs,
// used as a USB-A host port for controller pass-through (HID gamepads + XInput).
//
// CFG_TUSB_RHPORT1_MODE is intentionally NOT defined: defining it would make the
// legacy no-argument tusb_init() call bring up the host stack before
// tuh_configure() has supplied the PIO-USB pin configuration (whose library
// default, GPIO0, collides with Button A). The host stack is instead started
// explicitly with tuh_init(1) from the USB host task after tuh_configure().
// ---------------------------------------------------------------------------
#define CFG_TUH_ENABLED             1
#define CFG_TUH_RPI_PIO_USB         1
#define CFG_TUH_MAX_SPEED           OPT_MODE_FULL_SPEED

// Only needed so TinyUSB's BSP family.c (linked via tinyusb_board) compiles with
// CFG_TUH_RPI_PIO_USB enabled. This firmware never calls board_init(); the actual pins
// come from config.toml and are applied via tuh_configure() in usb_host_input.cpp.
#define BOARD_TUH_RHPORT            1
#ifndef PICO_DEFAULT_PIO_USB_DP_PIN
#define PICO_DEFAULT_PIO_USB_DP_PIN 16
#endif

// Support a hub so controllers behind a small USB hub / dock also work
#define CFG_TUH_HUB                 1
#define CFG_TUH_DEVICE_MAX          (CFG_TUH_HUB ? 4 : 1)
#define CFG_TUH_ENUMERATION_BUFSIZE 512

// Host class drivers: generic HID (DirectInput pads, DualShock/DualSense, ...)
// and XInput (Xbox controllers, via vendored tusb_xinput app driver)
#define CFG_TUH_HID                 4
#define CFG_TUH_HID_EPIN_BUFSIZE    64
#define CFG_TUH_HID_EPOUT_BUFSIZE   64
#define CFG_TUH_XINPUT              2

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
