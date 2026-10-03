#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "tusb.h"

#include "config.h"
#include "ssd1306.h"
#include "button_handler.h"
#include "multiplexer.h"
#include "virtual_matrix.h"
#include "vial_server.h"
#include "ble_hid_host.h"
#include "usb_descriptors.h"
#include "dual_console.h"

// Track LED heartbeat state
static bool s_led_state = false;
static uint32_t s_last_heartbeat_ms = 0;
static uint32_t s_last_display_update_ms = 0;
static char s_toast_msg[32] = {0};
static uint32_t s_toast_expiry_ms = 0;

static void show_toast(const char *msg, uint32_t duration_ms = 3000) {
    snprintf(s_toast_msg, sizeof(s_toast_msg), "%s", msg);
    s_toast_expiry_ms = to_ms_since_boot(get_absolute_time()) + duration_ms;
}

int main() {
    stdio_init_all();
    dual_console_init();

    // 1. Initialize Display early for startup feedback
    SSD1306::init();
#if PICO_RP2350
    const char *board_name = "Board: Pico 2 W (RP2350)";
#else
    const char *board_name = "Board: Pico W (RP2040)";
#endif
    SSD1306::renderBootSplash(board_name, "Firmware: v1.0.0");

    // 2. Initialize GPIO button
    ButtonHandler::init();

    // 3. Initialize Keymap and VIAL engine
    VialServer::init();

    // 4. Initialize Multi-device Multiplexer
    Multiplexer::init();

    // 5. Initialize TinyUSB Device Stack
    tusb_init();

    // 6. Initialize CYW43 wireless controller and BTstack integration
    if (cyw43_arch_init() != 0) {
        printf("[System] Failed to initialize cyw43_arch!\n");
        SSD1306::drawString("CYW43 INIT FAILED", 4, 48, true, false);
        SSD1306::show();
        while (1) tight_loop_contents();
    }

    // 7. Initialize BLE HID Central Host (powers on radio)
    BleHidHost::init();
    print_welcome_banner();

    // Hold boot splash screen on display for 2.0 seconds from startup
    const uint32_t splash_end_ms = to_ms_since_boot(get_absolute_time()) + 2000;
    s_last_heartbeat_ms = to_ms_since_boot(get_absolute_time());
    s_last_display_update_ms = splash_end_ms;

    while (true) {
        uint32_t now = to_ms_since_boot(get_absolute_time());

        // Service TinyUSB Device stack and Dual Console (USB CDC + Hardware UART0)
        tud_task();
        dual_console_update();

        // Flush any pending keyboard reports or accumulated mouse/trackpad movement
        Multiplexer::flushKeyboard();
        Multiplexer::flushMouse();

        // Check Push Button Events
        ButtonEvent btn_ev = ButtonHandler::update();
        if (btn_ev == BUTTON_EVENT_SHORT_PRESS) {
            if (BleHidHost::isScanning()) {
                printf("[Button] Short press: Stopping BLE scan.\n");
                BleHidHost::stopScan();
                show_toast("BLE Scan Stopped", 1500);
            } else {
                printf("[Button] Short press: Starting BLE scan.\n");
                BleHidHost::startScan();
                show_toast("BLE Scan Started", 1500);
            }
        } else if (btn_ev == BUTTON_EVENT_LONG_PRESS_PAIR) {
            printf("[Button] Long press: Entering pairing mode.\n");
            BleHidHost::startScan();
            show_toast("BLE Scan Started", 3000);
        } else if (btn_ev == BUTTON_EVENT_EXTRA_LONG_PRESS_RESET) {
            printf("[Button] Extra long press: Resetting bonds and keymap!\n");
            BleHidHost::clearBonds();
            VirtualMatrix::resetKeymap();
            show_toast("Factory Reset Done", 4000);
        }

        // Reverse Lock LED sync (Host PC CapsLock/NumLock -> Connected BLE keyboard)
        if (Multiplexer::hasLedsChanged()) {
            uint8_t leds = Multiplexer::getHostLeds();
            BleHidHost::sendHostLeds(leds);
            Multiplexer::acknowledgeLeds();
        }

        // Heartbeat LED (1 Hz blink rate: 500ms on, 500ms off)
        if (now - s_last_heartbeat_ms >= HEARTBEAT_INTERVAL_MS) {
            s_last_heartbeat_ms = now;
            s_led_state = !s_led_state;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, s_led_state);
        }

        // Ensure background scanning is active whenever there are unconnected bonded devices
        static uint32_t s_last_scan_check_ms = 0;
        if (now - s_last_scan_check_ms >= 1000) {
            s_last_scan_check_ms = now;
            if (BleHidHost::hasUnconnectedBonds() && !BleHidHost::isScanning()) {
                BleHidHost::startScan();
            }
        }

        // SSD1306 UI Display Refresh: update on status changes or at 1 Hz periodic interval
        // to avoid blocking I2C bus transfers (which stall USB and BLE polling)
        static uint8_t s_last_ble_count = 0xFF;
        static char s_last_dev_name[64] = {0};
        static int s_last_active_layer = -1;
        static bool s_last_is_scanning = false;
        static uint32_t s_last_passkey = 0;
        static bool s_last_has_toast = false;

        uint8_t cur_ble_count = BleHidHost::getConnectedCount();
        bool cur_ble_connected = (cur_ble_count > 0);
        const char *cur_dev_name = BleHidHost::getConnectedDeviceName();
        int cur_active_layer = VirtualMatrix::getActiveLayer();
        bool cur_is_scanning = BleHidHost::isScanning();
        uint32_t cur_passkey = BleHidHost::getActivePasskey();
        bool cur_has_toast = (now < s_toast_expiry_ms);

        bool state_changed = (cur_ble_count != s_last_ble_count) ||
                             (strcmp(cur_dev_name, s_last_dev_name) != 0) ||
                             (cur_active_layer != s_last_active_layer) ||
                             (cur_is_scanning != s_last_is_scanning) ||
                             (cur_passkey != s_last_passkey) ||
                             (cur_has_toast != s_last_has_toast);

        // SSD1306::show() is a ~25 ms blocking I2C transfer, and TinyUSB only releases the HID IN
        // endpoint from tud_task() in this loop, so every redraw delays the next HID report by up
        // to that long. Redraw only when the content changed; the 10 s refresh is just a safety
        // net against a glitched display.
        if (now >= splash_end_ms && (state_changed || (now - s_last_display_update_ms >= 10000))) {
            s_last_display_update_ms = now;
            s_last_ble_count = cur_ble_count;
            strncpy(s_last_dev_name, cur_dev_name, sizeof(s_last_dev_name) - 1);
            s_last_dev_name[sizeof(s_last_dev_name) - 1] = '\0';
            s_last_active_layer = cur_active_layer;
            s_last_is_scanning = cur_is_scanning;
            s_last_passkey = cur_passkey;
            s_last_has_toast = cur_has_toast;

            const char *toast = cur_has_toast ? s_toast_msg : "";
            SSD1306::renderStatus(
                cur_ble_connected,
                cur_dev_name,
                cur_active_layer,
                cur_is_scanning,
                cur_passkey,
                toast
            );
        }
    }

    return 0;
}

// --------------------------------------------------------------------+
// TinyUSB HID Callbacks
// --------------------------------------------------------------------+

// Invoked when received GET_REPORT control request
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen) {
    (void) instance;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) reqlen;
    return 0;
}

// Invoked when received SET_REPORT control request or received data on OUT endpoint
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize) {
    (void) report_id;
    (void) report_type;

    if (instance == 0) {
        // HID Instance 0: Keyboard Output Report (Caps Lock, Num Lock LEDs)
        if (bufsize >= 1) {
            Multiplexer::setHostLeds(buffer[0]);
        }
    } else if (instance == 1) {
        // HID Instance 1: VIAL RawHID WebHID report
        if (bufsize >= 32) {
            uint8_t out_buf[32];
            VialServer::handleRawReport(buffer, out_buf);
            tud_hid_n_report(1, 0, out_buf, 32);
        }
    }
}
