#ifndef APP_TASK_H_
#define APP_TASK_H_

#include <stdint.h>

// The bt_app task runs the BTstack run loop. BTstack is not thread-safe, so this task owns all
// application state (BTstack, multiplexer, keymap, device bindings, VIAL); other tasks hand work to
// it with the app_post_*() functions below, which may be called from any task (DESIGN.md §5).
void app_task_start();

// Posting functions. Each queues its payload and schedules a handler on the bt_app task; a handler
// that is already scheduled is not scheduled twice, so payloads are queued or kept as "latest value".
void app_post_host_leds(uint8_t leds);           // keyboard LED output report from the host
void app_post_vial_request(const uint8_t *req);  // 32-byte VIAL RawHID request
void app_post_usb_ready();                       // a HID IN transfer completed
void app_post_usb_state_changed();               // USB mounted / unmounted
void app_post_console_line(const char *line);    // console command line (NUL-terminated)

// Shows a short message on the OLED; bt_app task only.
void app_show_toast(const char *msg, uint32_t duration_ms = 3000);

// Stage codes for LogRing::stage(): which handler the bt_app task is running (0 = idle).
enum AppStage : uint16_t {
    STAGE_IDLE = 0,
    STAGE_USB_EVENT = 1,
    STAGE_CONSOLE = 2,
    STAGE_REPORT_FLUSH = 3,
    STAGE_BUTTON = 4,
    STAGE_TIMER = 5,
    STAGE_UI_PUBLISH = 6,
    STAGE_VIAL = 7,
    STAGE_STORAGE = 8,
    STAGE_HANGTEST = 99,
};

#endif // APP_TASK_H_
