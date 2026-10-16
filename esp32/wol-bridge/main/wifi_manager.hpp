#pragma once

#include <string>
#include <vector>
#include <set>
#include <utility>
#include <atomic>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "status_led.hpp"

class WifiManager {
public:
    explicit WifiManager(const std::vector<std::pair<std::string, std::string>>& ap_list);
    ~WifiManager();

    bool init();
    void connect(StatusLed* led = nullptr);
    bool is_connected() const;
    std::string get_ip_address() const;
    std::vector<std::string> get_configured_ssids() const;
    std::set<std::string> scan_networks();

private:
    static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    void handle_wifi_event(int32_t event_id, void* event_data);
    void handle_ip_event(int32_t event_id, void* event_data);
    bool attempt_connect(const std::string& ssid, const std::string& password);
    static const char* get_disconnect_reason_str(uint8_t reason);

    std::vector<std::pair<std::string, std::string>> ap_list_;
    esp_netif_t* netif_sta_;
    EventGroupHandle_t wifi_event_group_;
    std::atomic<bool> connected_;
    std::string ip_address_;
    uint8_t last_disconnect_reason_;
    bool initialized_;
};
