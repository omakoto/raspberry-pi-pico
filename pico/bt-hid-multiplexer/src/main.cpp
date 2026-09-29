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

        // Flush any remaining accumulated mouse/trackpad movement
        Multiplexer::flushMouse();

        // Check Push Button Events
        ButtonEvent btn_ev = ButtonHandler::update();
        if (btn_ev == BUTTON_EVENT_SHORT_PRESS) {
            printf("[Button] Short press detected.\n");
            show_toast("Refreshed status", 1500);
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

        // Periodic SSD1306 UI Display Refresh (10 Hz, starts after splash screen ends)
        if (now >= splash_end_ms && (now - s_last_display_update_ms >= STATUS_UPDATE_INTERVAL_MS)) {
            s_last_display_update_ms = now;

            const char *toast = (now < s_toast_expiry_ms) ? s_toast_msg : "";
            SSD1306::renderStatus(
                BleHidHost::isConnected(),
                BleHidHost::getConnectedDeviceName(),
                VirtualMatrix::getActiveLayer(),
                BleHidHost::isScanning(),
                BleHidHost::getActivePasskey(),
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
