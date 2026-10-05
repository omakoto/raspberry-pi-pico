#include "app_task.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"

#include "btstack.h"
#include "btstack_port_esp32.h"
#include "btstack_run_loop.h"

#include "ble_hid_host.h"
#include "button_handler.h"
#include "config.h"
#include "device_bindings.h"
#include "dual_console.h"
#include "log_ring.h"
#include "macros.h"
#include "multiplexer.h"
#include "platform.h"
#include "usb_descriptors.h"
#include "usb_hid.h"
#include "vial_server.h"
#include "ui_task.h"
#include "virtual_matrix.h"

// The task watchdog is fed only from this BTstack timer, so a wedged run loop (in BTstack or in any
// handler running on it) stops the feeding and reboots the board after CONFIG_ESP_TASK_WDT_TIMEOUT_S.
#define HEARTBEAT_INTERVAL_MS 1000

// Keymap edits are written to flash once they have been quiet for a while (VirtualMatrix), checked
// at this interval.
#define KEYMAP_SAVE_CHECK_MS 100

// Background scanning is (re)started at this interval whenever a bonded device is not connected or
// pairing mode is on.
#define SCAN_CHECK_INTERVAL_MS 1000

// Button polling (debouncing and long-press timing are in ButtonHandler).
#define BUTTON_POLL_MS 10

// The OLED state is recomputed at this interval and handed to the UI task when it changed.
#define UI_PUBLISH_MS 20

// VIAL requests waiting for the bt_app task. The configurator sends one request and waits for its
// reply, so a few entries are plenty.
#define VIAL_QUEUE_LENGTH 4

// Console lines waiting for the bt_app task.
#define CONSOLE_QUEUE_LENGTH 4

static btstack_timer_source_t s_heartbeat_timer;
static btstack_timer_source_t s_keymap_save_timer;
static btstack_timer_source_t s_scan_check_timer;
static btstack_timer_source_t s_button_timer;
static btstack_timer_source_t s_ui_timer;

// Short message shown on the OLED.
static char s_toast_msg[32];
static uint32_t s_toast_expiry_ms = 0;

void app_show_toast(const char *msg, uint32_t duration_ms) {
    snprintf(s_toast_msg, sizeof(s_toast_msg), "%s", msg);
    s_toast_expiry_ms = platform_now_ms() + duration_ms;
}

static QueueHandle_t s_vial_queue;
static QueueHandle_t s_console_queue;

// Set once the BTstack run loop exists; before that, posted work cannot be scheduled. Only console
// input can arrive that early (USB starts later, from the bt_app task) and is dropped.
static volatile bool s_run_loop_ready = false;
static volatile uint8_t s_host_leds;

// Marks the bt_app handler that is running, for the previous-run report after a watchdog reset.
class StageScope {
public:
    explicit StageScope(uint16_t stage) { LogRing::stage(stage); }
    ~StageScope() { LogRing::stage(STAGE_IDLE); }
};

// --------------------------------------------------------------------+
// Handlers (bt_app task)
// --------------------------------------------------------------------+

static void maybe_reboot_to_bootloader() {
    // After the reply to the VIA "jump to bootloader" command has gone out.
    if (!VialServer::bootloaderRequested() || !usb_hid_flush_vial_reply()) return;
    reboot_to_download_mode();
}

// Handles queued VIAL requests, one at a time: the next one only once the previous reply is out.
static void process_vial_requests() {
    uint8_t request[RAWHID_REPORT_SIZE];
    while (usb_hid_flush_vial_reply() && xQueueReceive(s_vial_queue, request, 0) == pdTRUE) {
        uint8_t reply[RAWHID_REPORT_SIZE];
        VialServer::handleRawReport(request, reply);
        usb_hid_set_vial_reply(reply);
    }
    maybe_reboot_to_bootloader();
}

static void on_vial_request(void *context) {
    (void) context;
    StageScope stage(STAGE_VIAL);
    process_vial_requests();
}

static void on_usb_ready(void *context) {
    (void) context;
    StageScope stage(STAGE_REPORT_FLUSH);
    Multiplexer::flushKeyboard();
    Multiplexer::flushMouse();
    process_vial_requests();
}

static void on_host_leds(void *context) {
    (void) context;
    StageScope stage(STAGE_USB_EVENT);
    // Reverse Lock LED sync: host Caps/Num/Scroll Lock state to the connected keyboards.
    Multiplexer::setHostLeds(s_host_leds);
    if (Multiplexer::hasLedsChanged()) {
        BleHidHost::sendHostLeds(Multiplexer::getHostLeds());
        Multiplexer::acknowledgeLeds();
    }
}

static void on_usb_state_changed(void *context) {
    (void) context;
    StageScope stage(STAGE_USB_EVENT);
    bool mounted = usb_hid_mounted();
    printf("[USB] %s\n", mounted ? "Mounted" : "Unmounted");
    if (mounted) {
        // Input that arrived while the host was away.
        Multiplexer::flushKeyboard();
        Multiplexer::flushMouse();
    }
}

static void on_console_line(void *context) {
    (void) context;
    char line[CONSOLE_LINE_MAX];
    while (xQueueReceive(s_console_queue, line, 0) == pdTRUE) {
        StageScope stage(STAGE_CONSOLE);
        dual_console_handle_command(line);
    }
}

static btstack_context_callback_registration_t s_vial_request_cb = {nullptr, &on_vial_request, nullptr};
static btstack_context_callback_registration_t s_usb_ready_cb = {nullptr, &on_usb_ready, nullptr};
static btstack_context_callback_registration_t s_host_leds_cb = {nullptr, &on_host_leds, nullptr};
static btstack_context_callback_registration_t s_usb_state_cb = {nullptr, &on_usb_state_changed, nullptr};
static btstack_context_callback_registration_t s_console_line_cb = {nullptr, &on_console_line, nullptr};

// --------------------------------------------------------------------+
// Posting (any task)
// --------------------------------------------------------------------+

void app_post_host_leds(uint8_t leds) {
    s_host_leds = leds;
    btstack_run_loop_execute_on_main_thread(&s_host_leds_cb);
}

void app_post_vial_request(const uint8_t *req) {
    if (xQueueSend(s_vial_queue, req, 0) != pdTRUE) {
        printf("[Vial] Request dropped (queue full)\n");
    }
    btstack_run_loop_execute_on_main_thread(&s_vial_request_cb);
}

void app_post_console_line(const char *line) {
    if (!s_run_loop_ready) return;
    char buf[CONSOLE_LINE_MAX];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    if (xQueueSend(s_console_queue, buf, 0) != pdTRUE) {
        printf("[Console] Busy, command dropped: %s\n", buf);
    }
    btstack_run_loop_execute_on_main_thread(&s_console_line_cb);
}

void app_post_usb_ready() {
    btstack_run_loop_execute_on_main_thread(&s_usb_ready_cb);
}

void app_post_usb_state_changed() {
    btstack_run_loop_execute_on_main_thread(&s_usb_state_cb);
}

// --------------------------------------------------------------------+
// Timers and setup (bt_app task)
// --------------------------------------------------------------------+

static void on_heartbeat(btstack_timer_source_t *ts) {
    esp_task_wdt_reset();
    LogRing::heartbeat(platform_now_ms());
    btstack_run_loop_set_timer(ts, HEARTBEAT_INTERVAL_MS);
    btstack_run_loop_add_timer(ts);
}

static void on_keymap_save_check(btstack_timer_source_t *ts) {
    {
        StageScope stage(STAGE_STORAGE);
        VirtualMatrix::flushPendingSave();
        MacroStore::flushPendingSave();
    }
    btstack_run_loop_set_timer(ts, KEYMAP_SAVE_CHECK_MS);
    btstack_run_loop_add_timer(ts);
}

static void on_scan_check(btstack_timer_source_t *ts) {
    {
        StageScope stage(STAGE_TIMER);
        if ((BleHidHost::hasUnconnectedBonds() || BleHidHost::isPairingMode()) && !BleHidHost::isScanning()) {
            BleHidHost::startScan();
        }
    }
    btstack_run_loop_set_timer(ts, SCAN_CHECK_INTERVAL_MS);
    btstack_run_loop_add_timer(ts);
}

static void on_button_poll(btstack_timer_source_t *ts) {
    // Also moves a macro on that is waiting out a delay.
    Multiplexer::poll();
    ButtonEvent btn_ev = ButtonHandler::update();
    if (btn_ev != BUTTON_EVENT_NONE) {
        StageScope stage(STAGE_BUTTON);
        if (btn_ev == BUTTON_EVENT_SHORT_PRESS) {
            if (BleHidHost::isPairingMode()) {
                printf("[Button] Short press: Stopping pairing mode.\n");
                BleHidHost::stopPairingMode();
                app_show_toast("Pairing Stopped", 1500);
            } else {
                printf("[Button] Short press: Starting pairing mode (60s).\n");
                BleHidHost::startPairingMode();
                app_show_toast("Pairing Mode (60s)", 1500);
            }
        } else if (btn_ev == BUTTON_EVENT_LONG_PRESS_PAIR) {
            printf("[Button] Long press: Starting pairing mode (60s).\n");
            BleHidHost::startPairingMode();
            app_show_toast("Pairing Mode (60s)", 3000);
        } else if (btn_ev == BUTTON_EVENT_EXTRA_LONG_PRESS_RESET) {
            printf("[Button] Extra long press: Resetting bonds and keymap!\n");
            BleHidHost::clearBonds();
            VirtualMatrix::resetKeymap();
            app_show_toast("Factory Reset Done", 4000);
        }
    }
    btstack_run_loop_set_timer(ts, BUTTON_POLL_MS);
    btstack_run_loop_add_timer(ts);
}

static void on_ui_publish(btstack_timer_source_t *ts) {
    {
        StageScope stage(STAGE_UI_PUBLISH);
        UiSnapshot snap = {};
        snap.connected_count = BleHidHost::getConnectedCount();
        // The device used last; if it is not connected (or none has sent input yet), the device that
        // connected last.
        uint8_t name_dev = DeviceBindings::lastActiveDevice();
        uint8_t addr[6];
        if (name_dev == DeviceBindings::NO_DEVICE || !BleHidHost::getSlotAddress(name_dev, addr)) {
            name_dev = BleHidHost::getMostRecentlyConnectedSlot();
        }
        if (name_dev != 0xFF) {
            snprintf(snap.device_name, sizeof(snap.device_name), "%s", BleHidHost::getConnectedDeviceName(name_dev));
        }
        // The layer of the device that was used last (its bound layer, or a layer a layer key selected).
        snap.active_layer = VirtualMatrix::getEffectiveLayer(DeviceBindings::lastActiveDevice());
        snap.pairing = BleHidHost::isPairingMode();
        snap.passkey = BleHidHost::getActivePasskey();
        if ((int32_t)(s_toast_expiry_ms - platform_now_ms()) > 0) {
            snprintf(snap.toast, sizeof(snap.toast), "%s", s_toast_msg);
            snap.toast_expiry_ms = s_toast_expiry_ms;
        }
        snap.usb_mounted = usb_hid_mounted();
        ui_publish(snap);
    }
    btstack_run_loop_set_timer(ts, UI_PUBLISH_MS);
    btstack_run_loop_add_timer(ts);
}

static void start_timer(btstack_timer_source_t *ts, void (*handler)(btstack_timer_source_t *), uint32_t ms) {
    btstack_run_loop_set_timer_handler(ts, handler);
    btstack_run_loop_set_timer(ts, ms);
    btstack_run_loop_add_timer(ts);
}

// Resolves a multiplexer device index to the Bluetooth address of the connected device.
static bool device_address(uint8_t dev_idx, uint8_t addr[6]) {
    return BleHidHost::getSlotAddress(dev_idx, addr);
}

// Runs on the bt_app task before the run loop starts.
static void bt_app_setup() {
    // Keymap (loads NVS), device bindings and multiplexer state, before USB can deliver requests.
    VialServer::init();
    DeviceBindings::init(device_address);
    MacroStore::init();
    Multiplexer::init();
    ButtonHandler::init();

    usb_hid_init();

    start_timer(&s_heartbeat_timer, &on_heartbeat, HEARTBEAT_INTERVAL_MS);
    start_timer(&s_keymap_save_timer, &on_keymap_save_check, KEYMAP_SAVE_CHECK_MS);
    start_timer(&s_scan_check_timer, &on_scan_check, SCAN_CHECK_INTERVAL_MS);
    start_timer(&s_button_timer, &on_button_poll, BUTTON_POLL_MS);
    start_timer(&s_ui_timer, &on_ui_publish, UI_PUBLISH_MS);

    // Sets up L2CAP, SM, GATT client and HIDS client, loads the bonds and powers the controller on.
    BleHidHost::init();
}

static void bt_app_task(void *arg) {
    (void) arg;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    // Sets up the VHCI transport, the run loop (bound to this task), and the TLV / LE device DB in NVS.
    btstack_init();
    s_run_loop_ready = true;
    bt_app_setup();
    btstack_run_loop_execute();  // never returns
}

void app_task_start() {
    s_vial_queue = xQueueCreate(VIAL_QUEUE_LENGTH, RAWHID_REPORT_SIZE);
    s_console_queue = xQueueCreate(CONSOLE_QUEUE_LENGTH, CONSOLE_LINE_MAX);
    xTaskCreatePinnedToCore(bt_app_task, "bt_app", BT_APP_TASK_STACK_SIZE, nullptr,
                            BT_APP_TASK_PRIORITY, nullptr, BT_APP_TASK_CORE);
}
