#include "app_task.h"

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"

#include "btstack.h"
#include "btstack_port_esp32.h"
#include "btstack_run_loop.h"

#include "config.h"
#include "platform.h"

// The task watchdog is fed only from this BTstack timer, so a wedged run loop (in BTstack or in any
// handler running on it) stops the feeding and reboots the board after CONFIG_ESP_TASK_WDT_TIMEOUT_S.
#define HEARTBEAT_INTERVAL_MS 1000

static btstack_timer_source_t s_heartbeat_timer;
static btstack_packet_callback_registration_t s_hci_event_registration;

static void on_heartbeat(btstack_timer_source_t *ts) {
    esp_task_wdt_reset();
    btstack_run_loop_set_timer(ts, HEARTBEAT_INTERVAL_MS);
    btstack_run_loop_add_timer(ts);
}

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void) channel;
    (void) size;
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) == BTSTACK_EVENT_STATE) {
        uint8_t state = btstack_event_state_get_state(packet);
        printf("[BT] HCI state %u at %lu ms\n", state, (unsigned long) platform_now_ms());
        if (state == HCI_STATE_WORKING) {
            bd_addr_t addr;
            gap_local_bd_addr(addr);
            printf("[BT] Controller ready, address %s\n", bd_addr_to_str(addr));
        }
    }
}

// Runs on the bt_app task before the run loop starts.
static void bt_app_setup() {
    s_hci_event_registration.callback = &hci_packet_handler;
    hci_add_event_handler(&s_hci_event_registration);

    btstack_run_loop_set_timer_handler(&s_heartbeat_timer, &on_heartbeat);
    btstack_run_loop_set_timer(&s_heartbeat_timer, HEARTBEAT_INTERVAL_MS);
    btstack_run_loop_add_timer(&s_heartbeat_timer);

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
    xTaskCreatePinnedToCore(bt_app_task, "bt_app", BT_APP_TASK_STACK_SIZE, nullptr,
                            BT_APP_TASK_PRIORITY, nullptr, BT_APP_TASK_CORE);
}
