#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

// Device port configuration (Native USB)
#define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_DEVICE)

// Enabled Device Mode
#define CFG_TUD_ENABLED             1
#define CFG_TUD_MAX_SPEED           OPT_MODE_DEFAULT_SPEED
#define CFG_TUD_ENDPOINT0_SIZE      64

// Class Drivers: CDC ACM for debug console, 2 HID instances (Keyboard+Mouse, VIAL RawHID)
#define CFG_TUD_CDC                 1
#define CFG_TUD_HID                 2

// HID Configuration
// Must equal the VIAL RawHID endpoint size (32). TinyUSB arms the OUT endpoint for this many bytes
// and a transfer only completes at a short packet or when the buffer is full, so a larger value
// leaves every 32-byte VIAL request pending until the next one arrives.
#define CFG_TUD_HID_EP_BUFSIZE      32

// CDC Configuration
#define CFG_TUD_CDC_RX_BUFSIZE      1024
#define CFG_TUD_CDC_TX_BUFSIZE      2048
#define CFG_TUD_CDC_EP_BUFSIZE      64

#ifdef __cplusplus
}
#endif

#endif // _TUSB_CONFIG_H_
