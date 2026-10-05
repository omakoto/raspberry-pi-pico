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

#include "config.h"
#include "device_bindings.h"
#include "log_ring.h"
#include "multiplexer.h"
#include "platform.h"
#include "usb_descriptors.h"
#include "usb_hid.h"
#include "vial_server.h"
#include "virtual_matrix.h"

// The task watchdog is fed only from this BTstack timer, so a wedged run loop (in BTstack or in any
// handler running on it) stops the feeding and reboots the board after CONFIG_ESP_TASK_WDT_TIMEOUT_S.
#define HEARTBEAT_INTERVAL_MS 1000

// Keymap edits are written to flash once they have been quiet for a while (VirtualMatrix), checked
// at this interval.
#define KEYMAP_SAVE_CHECK_MS 100

// VIAL requests waiting for the bt_app task. The configurator sends one request and waits for its
// reply, so a few entries are plenty.
#define VIAL_QUEUE_LENGTH 4

static btstack_timer_source_t s_heartbeat_timer;
static btstack_timer_source_t s_keymap_save_timer;
static btstack_packet_callback_registration_t s_hci_event_registration;

static QueueHandle_t s_vial_queue;
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
    printf("[System] Rebooting into download mode...\n");
    usb_hid_disconnect();
    vTaskDelay(pdMS_TO_TICKS(150));
    platform_reboot_to_download_mode();
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
    Multiplexer::setHostLeds(s_host_leds);
    if (Multiplexer::hasLedsChanged()) {
        printf("[USB] Host LEDs 0x%02X\n", Multiplexer::getHostLeds());
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

static btstack_context_callback_registration_t s_vial_request_cb = {nullptr, &on_vial_request, nullptr};
static btstack_context_callback_registration_t s_usb_ready_cb = {nullptr, &on_usb_ready, nullptr};
static btstack_context_callback_registration_t s_host_leds_cb = {nullptr, &on_host_leds, nullptr};
static btstack_context_callback_registration_t s_usb_state_cb = {nullptr, &on_usb_state_changed, nullptr};

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
    }
    btstack_run_loop_set_timer(ts, KEYMAP_SAVE_CHECK_MS);
    btstack_run_loop_add_timer(ts);
}

static void start_timer(btstack_timer_source_t *ts, void (*handler)(btstack_timer_source_t *), uint32_t ms) {
    btstack_run_loop_set_timer_handler(ts, handler);
    btstack_run_loop_set_timer(ts, ms);
    btstack_run_loop_add_timer(ts);
}

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void) channel;
    (void) size;
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) == BTSTACK_EVENT_STATE) {
        uint8_t state = btstack_event_state_get_state(packet);
        if (state == HCI_STATE_WORKING) {
            bd_addr_t addr;
            gap_local_bd_addr(addr);
            printf("[BT] Controller ready, address %s\n", bd_addr_to_str(addr));
        }
    }
}

// Resolves a multiplexer device index to the Bluetooth address of the connected device.
static bool device_address(uint8_t dev_idx, uint8_t addr[6]) {
    // No Bluetooth HID host yet: no device is ever connected.
    (void) dev_idx;
    (void) addr;
    return false;
}

// Runs on the bt_app task before the run loop starts.
static void bt_app_setup() {
    // Keymap (loads NVS), device bindings and multiplexer state, before USB can deliver requests.
    VialServer::init();
    DeviceBindings::init(device_address);
    Multiplexer::init();

    usb_hid_init();

    s_hci_event_registration.callback = &hci_packet_handler;
    hci_add_event_handler(&s_hci_event_registration);

    start_timer(&s_heartbeat_timer, &on_heartbeat, HEARTBEAT_INTERVAL_MS);
    start_timer(&s_keymap_save_timer, &on_keymap_save_check, KEYMAP_SAVE_CHECK_MS);

    hci_power_control(HCI_POWER_ON);
}

static void bt_app_task(void *arg) {
    (void) arg;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    // Sets up the VHCI transport, the run loop (bound to this task), and the TLV / LE device DB in NVS.
    btstack_init();
    bt_app_setup();
    btstack_run_loop_execute();  // never returns
}

void app_task_start() {
    s_vial_queue = xQueueCreate(VIAL_QUEUE_LENGTH, RAWHID_REPORT_SIZE);
    xTaskCreatePinnedToCore(bt_app_task, "bt_app", BT_APP_TASK_STACK_SIZE, nullptr,
                            BT_APP_TASK_PRIORITY, nullptr, BT_APP_TASK_CORE);
}
