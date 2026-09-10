/*
 * Main Application Lifecycle for nsbackend-pico.
 * Initializes hardware, USB composite HID/CDC/MSC, Wi-Fi networking, mDNS, and servers.
 */

#include <cstdio>
#include <cstring>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/uart.h"
#include "FreeRTOS.h"
#include "task.h"
#include "tusb.h"

#include "config_manager.hpp"
#include "status_led.hpp"
#include "gamepad_hid.hpp"
#include "controller_state.hpp"
#include "gpio_buttons.hpp"
#include "i2c_keypad.hpp"
#include "dual_logger.hpp"
#include "serial_command_server.hpp"
#include "usb_host_input.hpp"

#if defined(NSBACKEND_HAS_WIFI) && NSBACKEND_HAS_WIFI
#include "wifi_manager.hpp"
#include "mdns_service.hpp"
#include "tcp_server.hpp"
#endif

static const char* TAG = "Main";

static void supervisor_task(void* param) {
    (void)param;

    LOG_I(TAG, "Starting Nintendo Switch Controller Backend (nsbackend-pico)...");

    // 1. Initialize FATFS and load configuration
    ConfigManager config;
    if (config.init()) {
        config.load();
    }

    std::string hostname = config.get_string("hostname", "nscon");
    int tcp_port = config.get_int("tcp_port", 10100);
    bool log_enabled = config.get_bool("log", true);
    bool enable_echo = config.get_bool("enable_echo", true);
    bool led_active_low = config.get_bool("pico_led_active_low", config.get_bool("led_active_low", false));

    // 2. Initialize Status LED
    StatusLed status_led(25, led_active_low);
    status_led.init();
    status_led.set_state(LedState::INITIALIZING);

    // 3. Initialize the Switch-facing USB device: HORI Pokken pad (composite with CDC console
    //    and MSC storage) or a Nintendo Pro Controller (HID-only unless procon_composite)
    std::string identity_name = config.get_string("switch_identity", "pokken");
    GamepadIdentity identity = (identity_name == "procon") ? GamepadIdentity::ProCon : GamepadIdentity::Pokken;
    bool procon_composite = config.get_bool("procon_composite", false);
    GamepadHid gamepad;
    if (!gamepad.init(identity, procon_composite)) {
        LOG_E(TAG, "Failed to initialize USB device interface");
    }

    // 4. Initialize Controller State Engine
    ControllerState controller(gamepad);
    controller.reset_all();

    // 5. Initialize Physical GPIO Buttons
    GpioButtonManager gpio_buttons(controller);
    gpio_buttons.init();

    // 6. Initialize I2C Matrix Keypad (PCF8574)
    I2cKeypadConfig keypad_config;
    keypad_config.enabled = config.get_bool("i2c_keypad_enabled", true);
    keypad_config.log_enabled = log_enabled;
    keypad_config.sda_pin = static_cast<uint8_t>(config.get_int("i2c_sda_pin", 20));
    keypad_config.scl_pin = static_cast<uint8_t>(config.get_int("i2c_scl_pin", 21));
    keypad_config.address = static_cast<uint8_t>(config.get_int("i2c_address", 0x20));
    keypad_config.reverse_row = config.get_bool("i2c_reverse_row", true);
    keypad_config.reverse_col = config.get_bool("i2c_reverse_col", true);
    keypad_config.debounce_ms = static_cast<uint32_t>(config.get_int("i2c_debounce_ms", 20));

    I2cKeypadManager keypad(controller, keypad_config);
    if (keypad_config.enabled) {
        keypad.init();
        keypad.start();
    }

    // 7. Initialize USB Host Port (PIO-USB on USB-A receptacle, controller pass-through)
    UsbHostInputConfig usb_host_config;
    usb_host_config.enabled = config.get_bool("usb_host_enabled", true);
    usb_host_config.dp_pin = static_cast<uint8_t>(config.get_int("usb_host_dp_pin", 16));
    usb_host_config.deadzone_percent = config.get_int("usb_host_deadzone_percent", 10);
    usb_host_config.log_enabled = log_enabled;
    UsbHostInput usb_host(controller, usb_host_config);
    usb_host.init();

    // 8. Start Serial Command Server (accepts commands on UART0 GP12/GP13 and USB CDC)
    SerialCommandServer serial_server(controller, log_enabled, enable_echo);
    serial_server.start();

#if defined(NSBACKEND_HAS_WIFI) && NSBACKEND_HAS_WIFI
    // 9. Initialize Wi-Fi Manager
    auto ap_list = config.get_wifi_ap_list();
    WifiManager wifi(ap_list);
    wifi.init();

    LOG_I(TAG, "Connecting to Wi-Fi...");
    wifi.connect(&status_led);

    while (!wifi.is_connected()) {
        status_led.set_state(LedState::WIFI_RECONNECTING);
        wifi.connect(&status_led);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    status_led.set_state(LedState::SETTING_UP_TCP);
    MdnsService mdns;
    mdns.init(hostname, tcp_port);

    // 10. Start TCP Command Server
    LOG_I(TAG, "Initializing TCP Server on port %d...", tcp_port);
    TcpServer tcp_server(tcp_port, controller, status_led, log_enabled, enable_echo);
    if (!tcp_server.start()) {
        LOG_E(TAG, "Failed to start TCP command server on port %d!", tcp_port);
    }

    status_led.set_state(LedState::WAITING_CLIENT);
    LOG_I(TAG, "nsbackend-pico running. Hostname: %s.local:%d", hostname.c_str(), tcp_port);

    // Supervisor loop: monitors Wi-Fi connection health
    while (true) {
        if (!wifi.is_connected()) {
            LOG_W(TAG, "Wi-Fi disconnected. Reconnecting...");
            status_led.set_state(LedState::WIFI_RECONNECTING);
            wifi.connect(&status_led);
            if (wifi.is_connected()) {
                status_led.set_state(LedState::SETTING_UP_TCP);
                mdns.init(hostname, tcp_port);
                if (!tcp_server.is_listening()) {
                    tcp_server.start();
                }
                status_led.set_state(LedState::WAITING_CLIENT);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#else
    status_led.set_state(LedState::WAITING_CLIENT);
    LOG_I(TAG, "nsbackend-pico running on non-W board (Serial UART0 GP12/GP13 & USB CDC active)");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
}

extern "C" void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    (void)xTask;
    const char prefix[] = "\r\n[E][FreeRTOS] Stack overflow in task: ";
    uart_write_blocking(uart0, reinterpret_cast<const uint8_t*>(prefix), sizeof(prefix) - 1);
    if (pcTaskName != nullptr) {
        uart_write_blocking(uart0, reinterpret_cast<const uint8_t*>(pcTaskName), std::strlen(pcTaskName));
    }
    uart_write_blocking(uart0, reinterpret_cast<const uint8_t*>("\r\n"), 2);

    if (tud_mounted()) {
        tud_cdc_n_write(0, "\r\n[E][FreeRTOS] Stack overflow in task: ", 40);
        if (pcTaskName != nullptr) {
            tud_cdc_n_write(0, pcTaskName, static_cast<uint32_t>(std::strlen(pcTaskName)));
        }
        tud_cdc_n_write(0, "\r\n", 2);
        tud_cdc_n_write_flush(0);
    }
    while (true) {
        tight_loop_contents();
    }
}

extern "C" void vApplicationMallocFailedHook(void) {
    const char msg[] = "\r\n[E][FreeRTOS] Heap allocation failed!\r\n";
    uart_write_blocking(uart0, reinterpret_cast<const uint8_t*>(msg), sizeof(msg) - 1);

    if (tud_mounted()) {
        tud_cdc_n_write(0, "\r\n[E][FreeRTOS] Heap allocation failed!\r\n", 41);
        tud_cdc_n_write_flush(0);
    }
    while (true) {
        tight_loop_contents();
    }
}

int main() {
    // The PIO-USB host port derives full-speed USB timing from clk_sys with PIO clock
    // dividers, which only come out exact when clk_sys is a multiple of 12 MHz. Run at
    // 120 MHz on every board (instead of the 125/150 MHz defaults) and do it before any
    // clock-dependent peripheral (UART, USB, Wi-Fi) is initialized.
    set_sys_clock_khz(120000, true);

    stdio_init_all();
    dual_logger_init();

    BaseType_t res = xTaskCreate(supervisor_task, "supervisor", 4096, nullptr, tskIDLE_PRIORITY + 2, nullptr);
    if (res != pdPASS) {
        std::printf("Failed to create supervisor task\n");
        return -1;
    }

    vTaskStartScheduler();

    while (true) {
        tight_loop_contents();
    }
    return 0;
}
