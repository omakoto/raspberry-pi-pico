#include <cstdio>
#include <string>
#include <vector>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config_manager.hpp"
#include "dual_logger.hpp"
#include "usb_msc.hpp"
#include "status_led.hpp"
#include "mac_tracker.hpp"
#include "w5500_driver.hpp"
#include "wifi_manager.hpp"
#include "mdns_service.hpp"
#include "http_server.hpp"

static const char* TAG = "Main";

#include "esp_netif.h"
#include "esp_event.h"

extern "C" void app_main() {
    // 1. Dual logger initialization (UART0 + TinyUSB CDC ACM)
    dual_logger_init();
    ESP_LOGI(TAG, "Starting ESP32 Wake-on-LAN Bridge...");

    // 2. Initialize NVS (needed for Wi-Fi and MAC address persistence)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing and re-initializing NVS partition...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize TCP/IP stack and default system event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 3. Mount FATFS Wear-Levelling storage and load configuration
    static ConfigManager config;
    if (config.init()) {
        config.load();
    } else {
        ESP_LOGW(TAG, "Running with compiled defaults (no FATFS config available)");
    }

    // 4. Initialize Status LED
    int led_gpio = config.get_int("led_pin", 21);
    bool led_active_low = config.get_bool("led_active_low", true);
    static StatusLed status_led(static_cast<gpio_num_t>(led_gpio), led_active_low);
    status_led.init();
    status_led.set_state(LedState::INITIALIZING);

    // 5. Initialize TinyUSB Composite Device (CDC ACM Console + MSC Flash Drive)
    static UsbMsc usb_msc;
    usb_msc.init(config.get_wl_handle());

    // 6. Configure W5500 Ethernet SPI
    W5500Config eth_cfg;
    eth_cfg.sclk_pin = config.get_int("spi_sck", 7);
    eth_cfg.mosi_pin = config.get_int("spi_mosi", 9);
    eth_cfg.miso_pin = config.get_int("spi_miso", 8);
    eth_cfg.cs_pin = config.get_int("spi_cs", 4);
    eth_cfg.rst_pin = config.get_int("spi_reset", 3);
    eth_cfg.int_pin = config.get_int("spi_int", 2);
    eth_cfg.poll_period_ms = config.get_int("poll_period_ms", 2);
    eth_cfg.spi_speed_mhz = config.get_int("spi_speed_mhz", 25);
    eth_cfg.int_diag = config.get_bool("int_diag", false);
    bool log_rx_packets = config.get_bool("log_rx_packets", false);
    eth_cfg.log_rx_packets = log_rx_packets;

    std::string mac_str = config.get_string("mac", "DE:AD:BE:EF:FE:ED");
    MacTracker::parse_mac(mac_str, eth_cfg.own_mac);

    // 7. Initialize MAC Tracker
    std::vector<std::string> static_macs = config.get_string_list("static_macs");
    static MacTracker mac_tracker;
    mac_tracker.init(eth_cfg.own_mac, static_macs, log_rx_packets);

    // 8. Start W5500 Sniffer Driver
    static W5500Driver w5500;
    if (w5500.init(eth_cfg, &mac_tracker, &status_led)) {
        w5500.start();
    } else {
        ESP_LOGE(TAG, "Failed to initialize W5500 Ethernet driver!");
    }

    // 9. Initialize Wi-Fi Manager
    std::vector<std::pair<std::string, std::string>> ap_list;
    std::string primary_ssid = config.get_string("wifi_ssid", "");
    std::string primary_pass = config.get_string("wifi_password", "");
    if (!primary_ssid.empty() && primary_ssid != "YOUR_WIFI_SSID") {
        ap_list.emplace_back(primary_ssid, primary_pass);
    }
    for (int i = 1; i <= 9; ++i) {
        std::string key_s = "wifi_ssid" + std::to_string(i);
        std::string key_p = "wifi_password" + std::to_string(i);
        std::string s = config.get_string(key_s, "");
        std::string p = config.get_string(key_p, "");
        if (!s.empty()) {
            ap_list.emplace_back(s, p);
        }
    }

    static WifiManager wifi(ap_list);
    wifi.init();

    // Spawn Wi-Fi connection task
    xTaskCreate([](void* arg) {
        wifi.connect(&status_led);
        vTaskDelete(nullptr);
    }, "wifi_task", 4096, nullptr, 5, nullptr);

    // Wait until Wi-Fi is connected
    ESP_LOGI(TAG, "Waiting for Wi-Fi connection...");
    while (!wifi.is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    std::string hostname = config.get_string("hostname", "wol");
    int http_port = config.get_int("http_port", 80);

    // 10. Start mDNS Service (e.g. wol.local)
    static MdnsService mdns;
    mdns.init(hostname, http_port);

    // 11. Start HTTP Server (http://wol.local/wake)
    static HttpServer http_server;
    http_server.start(http_port, &mac_tracker, &w5500, &status_led, &wifi, hostname);

    status_led.set_state(LedState::READY);
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, " ESP32 Wake-on-LAN Bridge Ready!");
    ESP_LOGI(TAG, " Wi-Fi IP:    %s", wifi.get_ip_address().c_str());
    ESP_LOGI(TAG, " Web URL:     http://%s.local:%d/", hostname.c_str(), http_port);
    ESP_LOGI(TAG, " Wake URL:    http://%s.local:%d/wake", hostname.c_str(), http_port);
    ESP_LOGI(TAG, "=================================================");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
