#include "ui_task.h"

#include <stdio.h>
#include <string.h>

#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "platform.h"
#include "oled.h"

#define UI_TASK_CORE      1
#define UI_TASK_PRIORITY  2
#define UI_TASK_STACK     4096

// The task wakes up at least this often even when nothing changes: for the end of the boot splash,
// the safety-net redraw, the screen timeout, and the task watchdog (5 s).
#define UI_POLL_MS         1000
// How long the boot splash stays up.
#define SPLASH_MS          2000
// The display is redrawn on every change; this full redraw is only a safety net against a glitched
// display.
#define REFRESH_MS         10000
// The panel is turned off this long after the screen last changed or the last input (against
// burn-in), and back on with the next change or input. It is checked on the UI_POLL_MS wake-ups.
#define SCREEN_TIMEOUT_MS  60000

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

    bool display = Oled::init();
    if (!display) {
        // No display to draw on. The task is suspended rather than deleted, because ui_publish() keeps
        // notifying it, which is harmless for a suspended task but not for a deleted one.
        esp_task_wdt_delete(nullptr);
        while (true) vTaskSuspend(nullptr);
    }
    char version[32];
    snprintf(version, sizeof(version), "Firmware: v%s", FIRMWARE_VERSION);
    Oled::renderBootSplash(nullptr, version);
    const uint32_t splash_end_ms = platform_now_ms() + SPLASH_MS;

    UiSnapshot shown = {};
    bool shown_valid = false;
    uint32_t last_draw_ms = 0;
    uint32_t last_change_ms = 0;
    bool screen_off = false;

    while (true) {
        esp_task_wdt_reset();
        // Wake up on a new snapshot, or after UI_POLL_MS.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(UI_POLL_MS));
        uint32_t now = platform_now_ms();

        UiSnapshot cur;
        bool valid;
        portENTER_CRITICAL(&s_lock);
        cur = s_shared;
        valid = s_shared_valid;
        portEXIT_CRITICAL(&s_lock);
        if (!valid) continue;

        if (now < splash_end_ms) continue;
        // Input keeps the display on and wakes it up, but alone does not redraw it while it is on,
        // which a moving mouse would otherwise do on every snapshot.
        bool input = shown_valid && cur.input_count != shown.input_count;
        shown.input_count = cur.input_count;
        bool changed = !shown_valid || memcmp(&cur, &shown, sizeof(cur)) != 0;
        if (changed || input) {
            last_change_ms = now;
        }
        bool redraw = screen_off ? (changed || input) : (changed || now - last_draw_ms >= REFRESH_MS);
        if (!redraw) {
            if (!screen_off && now - last_change_ms >= SCREEN_TIMEOUT_MS) {
                Oled::setDisplayOn(false);
                screen_off = true;
            }
            continue;
        }
        shown = cur;
        shown_valid = true;
        last_draw_ms = now;
        Oled::renderStatus(cur.connected_count, cur.device_name, cur.active_layer, cur.pairing,
                           cur.passkey, cur.toast, cur.usb_mounted);
        // Turned on after the redraw, so that the old image does not flash up first.
        if (screen_off) {
            Oled::setDisplayOn(true);
            screen_off = false;
        }
    }
}

void ui_task_start() {
    xTaskCreatePinnedToCore(ui_task, "ui", UI_TASK_STACK, nullptr, UI_TASK_PRIORITY, &s_task, UI_TASK_CORE);
}
