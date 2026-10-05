#ifndef USB_HID_H_
#define USB_HID_H_

#include <stdint.h>
#include <stdbool.h>

// TinyUSB glue: installs the USB device (esp_tinyusb runs tud_task() in its own task) and forwards
// the TinyUSB callbacks, which run in that task, to the bt_app task (see app_task.h). Code that
// includes btstack.h cannot include tusb.h (both define hid_report_type_t), so the bt_app side
// reaches TinyUSB through these functions.

// Samples PIN_USB_SERIAL_ENABLE into g_usb_serial_enabled. Call once at startup, before
// usb_hid_init() and before anything reports whether the serial port is on.
void usb_hid_read_serial_enable();

// Starts the USB device. Call once, from the bt_app task after the BTstack run loop has been
// initialized (the callbacks post to it).
void usb_hid_init();

bool usb_hid_mounted();

// Detaches from the host (before a reboot).
void usb_hid_disconnect();

// Sends the reply to the last VIAL request, if one is pending and the VIAL IN endpoint is free.
// Returns true if nothing is pending any more. bt_app task only.
bool usb_hid_flush_vial_reply();
// Queues a VIAL reply (32 bytes); bt_app task only.
void usb_hid_set_vial_reply(const uint8_t *reply);

#endif // USB_HID_H_
