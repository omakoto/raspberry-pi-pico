#include "usb_hid.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

#include "app_task.h"
#include "config.h"
#include "usb_descriptors.h"

// TinyUSB task: on the core without the BT controller, above the console and UI tasks, so that
// endpoint completions are handled at once.
#define USB_TASK_CORE      1
#define USB_TASK_PRIORITY  20
#define USB_TASK_STACK     4096

// Descriptors for esp_tinyusb (tinyusb_config_t.descriptor), matching g_usb_serial_enabled; defined
// in usb_descriptors.c.
extern "C" {
const tusb_desc_device_t *usb_device_descriptor(void);
const uint8_t *usb_configuration_descriptor(void);
const char **usb_string_descriptors(int *count);
}

static volatile bool s_mounted = false;

// Reply to the last VIAL request. The IN endpoint can still be busy when the request arrives, and
// a dropped reply leaves the web configurator waiting forever, so unsent replies are retried when the
// endpoint becomes free.
static uint8_t s_vial_reply[RAWHID_REPORT_SIZE];
static bool s_vial_reply_pending = false;

static void usb_event_cb(tinyusb_event_t *event, void *arg) {
    (void) arg;
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        s_mounted = true;
        app_post_usb_state_changed();
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        s_mounted = false;
        app_post_usb_state_changed();
    }
}

void usb_hid_init() {
    // The USB serial port is only part of the device when PIN_USB_SERIAL_ENABLE is grounded at boot
    // (or when forced at build time).
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << PIN_USB_SERIAL_ENABLE;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);
    esp_rom_delay_us(200);  // let the pull-up settle before sampling
#ifdef USB_SERIAL_ALWAYS
    g_usb_serial_enabled = true;
#else
    g_usb_serial_enabled = gpio_get_level((gpio_num_t) PIN_USB_SERIAL_ENABLE) == 0;
#endif

    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG(usb_event_cb);
    cfg.task.size = USB_TASK_STACK;
    cfg.task.priority = USB_TASK_PRIORITY;
    cfg.task.xCoreID = USB_TASK_CORE;
    cfg.descriptor.device = usb_device_descriptor();
    cfg.descriptor.full_speed_config = usb_configuration_descriptor();
    cfg.descriptor.string = usb_string_descriptors(&cfg.descriptor.string_count);
    esp_err_t err = tinyusb_driver_install(&cfg);
    if (err != ESP_OK) {
        printf("[USB] tinyusb_driver_install failed: %s\n", esp_err_to_name(err));
        return;
    }
    printf("[USB] Device started (serial port %s)\n", g_usb_serial_enabled ? "on" : "off");
}

void usb_hid_disconnect() {
    tud_disconnect();
}

bool usb_hid_mounted() {
    return s_mounted;
}

void usb_hid_set_vial_reply(const uint8_t *reply) {
    memcpy(s_vial_reply, reply, sizeof(s_vial_reply));
    s_vial_reply_pending = true;
}

bool usb_hid_flush_vial_reply() {
    if (s_vial_reply_pending && tud_hid_n_ready(1) &&
        tud_hid_n_report(1, 0, s_vial_reply, sizeof(s_vial_reply))) {
        s_vial_reply_pending = false;
    }
    return !s_vial_reply_pending;
}

// --------------------------------------------------------------------+
// TinyUSB HID callbacks (TinyUSB task)
// --------------------------------------------------------------------+

extern "C" uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                                          uint8_t *buffer, uint16_t reqlen) {
    (void) instance;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) reqlen;
    return 0;
}

// Invoked for SET_REPORT control requests and for data on an OUT endpoint.
extern "C" void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                                      uint8_t const *buffer, uint16_t bufsize) {
    (void) report_type;
    if (instance == 0) {
        // Keyboard LED output report. A SET_REPORT control request arrives with its report ID
        // stripped; data on the interrupt OUT endpoint (which Linux uses when there is one) arrives
        // with report_id 0 and the report ID still in the first byte.
        if (report_id == REPORT_ID_KEYBOARD && bufsize >= 1) {
            app_post_host_leds(buffer[0]);
        } else if (report_id == 0 && bufsize >= 2 && buffer[0] == REPORT_ID_KEYBOARD) {
            app_post_host_leds(buffer[1]);
        }
    } else if (instance == 1) {
        // VIAL RawHID request
        if (bufsize >= RAWHID_REPORT_SIZE) {
            app_post_vial_request(buffer);
        }
    }
}

extern "C" void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len) {
    (void) instance;
    (void) report;
    (void) len;
    app_post_usb_ready();
}

// A failed transfer frees the endpoint too; whatever was waiting for it is sent now.
extern "C" void tud_hid_report_failed_cb(uint8_t instance, hid_report_type_t report_type, uint8_t const *report,
                                         uint16_t xferred_bytes) {
    (void) instance;
    (void) report_type;
    (void) report;
    (void) xferred_bytes;
    app_post_usb_ready();
}
