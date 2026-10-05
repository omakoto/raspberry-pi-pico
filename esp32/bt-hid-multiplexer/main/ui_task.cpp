#include "ui_task.h"

#include <stdio.h>
#include <string.h>

#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "platform.h"
#include "ssd1306.h"
#include "status_led.h"

#define UI_TASK_CORE      1
#define UI_TASK_PRIORITY  2
#define UI_TASK_STACK     4096

// How long the boot splash stays up.
#define SPLASH_MS          2000
// The display is redrawn on every change; this full redraw is only a safety net against a glitched
// display.
#define REFRESH_MS         10000

static TaskHandle_t s_task = nullptr;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static UiSnapshot s_shared;        // guarded by s_lock
static bool s_shared_valid = false;
static UiSnapshot s_last_published;  // bt_app task only

void ui_publish(const UiSnapshot &snapshot) {
    if (s_shared_valid && memcmp(&snapshot, &s_last_published, sizeof(snapshot)) == 0) return;
    s_last_published = snapshot;
    portENTER_CRITICAL(&s_lock);
    s_shared = snapshot;
    s_shared_valid = true;
    portEXIT_CRITICAL(&s_lock);
    if (s_task) xTaskNotifyGive(s_task);
}

static void ui_task(void *arg) {
    (void) arg;
    esp_task_wdt_add(nullptr);

    status_led_init();
    bool display = SSD1306::init();
    if (display) {
        char version[32];
        snprintf(version, sizeof(version), "Firmware: v%s", FIRMWARE_VERSION);
        SSD1306::renderBootSplash("Board: " BOARD_NAME, version);
    }
    const uint32_t splash_end_ms = platform_now_ms() + SPLASH_MS;

    UiSnapshot shown = {};
    bool shown_valid = false;
    bool shown_toast = false;
    uint32_t last_draw_ms = 0;
    bool led_on = false;
    uint32_t last_led_toggle_ms = 0;

    while (true) {
        esp_task_wdt_reset();
        // Wake up on a new snapshot, or often enough for the LED blink.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PAIRING_LED_BLINK_INTERVAL_MS / 2));
        uint32_t now = platform_now_ms();

        UiSnapshot cur;
        bool valid;
        portENTER_CRITICAL(&s_lock);
        cur = s_shared;
        valid = s_shared_valid;
        portEXIT_CRITICAL(&s_lock);
        if (!valid) continue;

        // Pairing Mode LED indicator: rapid-blinks at 5 Hz (100 ms on, 100 ms off) during pairing
        // mode; OFF otherwise.
        if (cur.pairing) {
            if (now - last_led_toggle_ms >= PAIRING_LED_BLINK_INTERVAL_MS) {
                last_led_toggle_ms = now;
                led_on = !led_on;
                status_led_set(led_on);
            }
        } else if (led_on) {
            led_on = false;
            status_led_set(false);
        }

        if (!display || now < splash_end_ms) continue;
        bool has_toast = cur.toast[0] != '\0' && (int32_t)(cur.toast_expiry_ms - now) > 0;
        bool changed = !shown_valid || memcmp(&cur, &shown, sizeof(cur)) != 0 || has_toast != shown_toast;
        if (changed || now - last_draw_ms >= REFRESH_MS) {
            shown = cur;
            shown_valid = true;
            shown_toast = has_toast;
            last_draw_ms = now;
            SSD1306::renderStatus(cur.connected_count > 0, cur.device_name, cur.active_layer, cur.pairing,
                                  cur.passkey, has_toast ? cur.toast : "", cur.usb_mounted);
        }
    }
}

void ui_task_start() {
    xTaskCreatePinnedToCore(ui_task, "ui", UI_TASK_STACK, nullptr, UI_TASK_PRIORITY, &s_task, UI_TASK_CORE);
}
