#include <cstdio>
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config_manager.hpp"
#include "status_led.hpp"
#include "gamepad_hid.hpp"
#include "controller_state.hpp"
#include "gpio_buttons.hpp"
#include "wifi_manager.hpp"
#include "mdns_service.hpp"
#include "tcp_server.hpp"
#include "dual_logger.hpp"
#include "serial_command_server.hpp"

static const char* TAG = "Main";

extern "C" void app_main(void) {
    // Initialize dual logging (UART0 + TinyUSB CDC)
    dual_logger_init();

    ESP_LOGI(TAG, "Starting Nintendo Switch Controller TCP Backend (nsbackend-esp32s3)...");

    // Initialize NVS (required for Wi-Fi stack)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize FATFS and load configuration
    ConfigManager config;
    if (config.init()) {
        config.load();
    }

    std::string hostname = config.get_string("hostname", "nscon");
    int tcp_port = config.get_int("tcp_port", 10100);
    bool log_enabled = config.get_bool("log", true);
    bool enable_echo = config.get_bool("enable_echo", true);
    bool led_active_low = config.get_bool("led_active_low", true);

    // Initialize Status LED (Turn ON during initialization)
    StatusLed status_led(GPIO_NUM_21, led_active_low);
    status_led.init();
    status_led.set_state(LedState::INITIALIZING);

    // Initialize TinyUSB Composite Interface (HID Gamepad + CDC Console + MSC Storage)
    GamepadHid gamepad;
    if (!gamepad.init(config.get_wl_handle())) {
        ESP_LOGE(TAG, "Failed to initialize USB Composite interface");
    }

    // Initialize Controller State Engine
    ControllerState controller(gamepad);
    controller.reset_all();

    // Initialize Physical GPIO Buttons
    GpioButtonManager gpio_buttons(controller);
    gpio_buttons.init();

    // Start Serial Command Server (accepts commands on UART0 D6/D7 and USB CDC immediately)
    SerialCommandServer serial_server(controller, log_enabled, enable_echo);
    serial_server.start();

    // Initialize Wi-Fi Manager
    auto ap_list = config.get_wifi_ap_list();
    WifiManager wifi(ap_list);
    wifi.init();

    ESP_LOGI(TAG, "Connecting to Wi-Fi...");
    wifi.connect(&status_led);

    // If initial connection was not established (e.g. no SSIDs or not visible), maintain rapid strobe
    while (!wifi.is_connected()) {
        status_led.set_state(LedState::WIFI_RECONNECTING);
        wifi.connect(&status_led);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    status_led.set_state(LedState::SETTING_UP_TCP);
    MdnsService mdns;
    mdns.init(hostname, tcp_port);

    // Start TCP Command Server
    TcpServer tcp_server(tcp_port, controller, status_led, log_enabled, enable_echo);
    tcp_server.start();

    status_led.set_state(LedState::WAITING_CLIENT);
    ESP_LOGI(TAG, "nsbackend-esp32s3 running. Hostname: %s.local:%d", hostname.c_str(), tcp_port);

    // Main supervisor loop: monitors Wi-Fi connection
    while (true) {
        if (!wifi.is_connected()) {
            ESP_LOGW(TAG, "Wi-Fi disconnected. Reconnecting...");
            status_led.set_state(LedState::WIFI_RECONNECTING);
            wifi.connect(&status_led);
            if (wifi.is_connected()) {
                status_led.set_state(LedState::SETTING_UP_TCP);
                mdns.init(hostname, tcp_port);
                status_led.set_state(LedState::WAITING_CLIENT);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
