#include <Arduino.h>
#include "config.h"
#include "multiplexer.h"
#include "usb_host_manager.h"
#include "vial_server.h"
#include "logger.h"

static elapsedMicros s_mouse_flush_timer;
static elapsedMillis s_led_timer;

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWriteFast(LED_BUILTIN, HIGH);

    logger_init(115200);
    delay(200);

    logger_println("\n==================================================");
    logger_println("     Teensy 4.1 USB HID Multiplexer & VIAL");
    logger_println("==================================================");

    Multiplexer::init();
    VialServer::init();
    UsbHostManager::init();

    logger_println("[Init] Multiplexer engine ready.");
    logger_println("[Init] Connect peripherals to USB Host port via hub.");
    logger_println("[Init] Configure remapping at https://vial.rocks via WebHID.\n");

    s_mouse_flush_timer = 0;
    s_led_timer = 0;
}

void loop() {
    // 1. Service downstream USB Host (keyboards, mice, hubs)
    UsbHostManager::poll();

    // 2. Service upstream VIAL / VIA WebHID and Serial CLI commands
    VialServer::poll();

    // 3. Rate-decoupled smooth mouse reporting (1000 Hz / 1ms interval)
    if (s_mouse_flush_timer >= MOUSE_FLUSH_INTERVAL_US) {
        Multiplexer::flushMouse();
        s_mouse_flush_timer = 0;
    }

    // 4. Heartbeat LED indicator
    if (s_led_timer >= LED_HEARTBEAT_INTERVAL_MS) {
        digitalToggleFast(LED_BUILTIN);
        s_led_timer = 0;
    }
}
