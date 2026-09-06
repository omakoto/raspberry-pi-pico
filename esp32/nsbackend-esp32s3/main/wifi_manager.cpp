#include "wifi_manager.hpp"
#include <cstring>
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_timer.h"

static const char* TAG = "WifiManager";

static constexpr int WIFI_CONNECTED_BIT = BIT0;
static constexpr int WIFI_FAIL_BIT      = BIT1;

const char* WifiManager::get_disconnect_reason_str(uint8_t reason) {
    switch (reason) {
        case 1: return "Unspecified failure (General connection error)";
        case 2: return "Auth expired (AP timed out during authentication - check 2.4 GHz band or MAC filtering)";
        case 3: return "Auth leave (Disconnected by AP)";
        case 4: return "Assoc expired (AP timed out during association)";
        case 5: return "Assoc too many (AP rejected connection: Max client limit reached)";
        case 6: return "Not authenticated";
        case 7: return "Not associated";
        case 8: return "Assoc leave (Disassociated by AP)";
        case 15: return "4-way handshake timeout (WRONG PASSWORD, weak signal, or WPA3/PMF mismatch)";
        case 201: return "No AP found (SSID not found - verify 2.4 GHz band is active)";
        case 202: return "Auth failed (WRONG PASSWORD)";
        case 204: return "Handshake timeout (WRONG PASSWORD or weak signal)";
        case 205: return "Connection failed (General radio/handshake failure or weak signal)";
        default: return "Unknown disconnect reason";
    }
}

WifiManager::WifiManager(const std::vector<std::pair<std::string, std::string>>& ap_list)
    : ap_list_(ap_list),
      netif_sta_(nullptr),
      wifi_event_group_(nullptr),
      connected_(false),
      last_disconnect_reason_(0),
      initialized_(false) {}

WifiManager::~WifiManager() {
    if (wifi_event_group_ != nullptr) {
        vEventGroupDelete(wifi_event_group_);
    }
}

bool WifiManager::init() {
    wifi_event_group_ = xEventGroupCreate();
    if (wifi_event_group_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi event group");
        return false;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    netif_sta_ = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiManager::event_handler, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiManager::event_handler, this, nullptr));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    initialized_ = true;
    ESP_LOGI(TAG, "Wi-Fi subsystem initialized");
    return true;
}

void WifiManager::event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* self = static_cast<WifiManager*>(arg);
    if (event_base == WIFI_EVENT) {
        self->handle_wifi_event(event_id, event_data);
    } else if (event_base == IP_EVENT) {
        self->handle_ip_event(event_id, event_data);
    }
}

void WifiManager::handle_wifi_event(int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        auto* event = static_cast<wifi_event_sta_disconnected_t*>(event_data);
        last_disconnect_reason_ = event->reason;
        connected_.store(false, std::memory_order_relaxed);
        xEventGroupSetBits(wifi_event_group_, WIFI_FAIL_BIT);
        xEventGroupClearBits(wifi_event_group_, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d: %s)", event->reason, get_disconnect_reason_str(event->reason));
    }
}

void WifiManager::handle_ip_event(int32_t event_id, void* event_data) {
    if (event_id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        char ip_str[32];
        esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
        ip_address_ = ip_str;
        connected_.store(true, std::memory_order_relaxed);
        xEventGroupSetBits(wifi_event_group_, WIFI_CONNECTED_BIT);
        xEventGroupClearBits(wifi_event_group_, WIFI_FAIL_BIT);
        ESP_LOGI(TAG, "Connected to Wi-Fi successfully! IP: %s", ip_address_.c_str());
    }
}

bool WifiManager::is_connected() const {
    return connected_.load(std::memory_order_relaxed);
}

std::string WifiManager::get_ip_address() const {
    return ip_address_;
}

std::vector<std::string> WifiManager::get_configured_ssids() const {
    std::vector<std::string> ssids;
    for (const auto& ap : ap_list_) {
        ssids.push_back(ap.first);
    }
    return ssids;
}

std::set<std::string> WifiManager::scan_networks() {
    ESP_LOGI(TAG, "Scanning visible Wi-Fi networks...");
    std::set<std::string> visible_ssids;

    wifi_scan_config_t scan_config = {};
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_config.scan_time.active.min = 100;
    scan_config.scan_time.active.max = 300;

    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(ret));
        return visible_ssids;
    }

    uint16_t num_ap = 0;
    esp_wifi_scan_get_ap_num(&num_ap);
    if (num_ap == 0) {
        ESP_LOGI(TAG, "  No visible Wi-Fi networks found.");
        return visible_ssids;
    }

    std::vector<wifi_ap_record_t> ap_records(num_ap);
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&num_ap, ap_records.data()));

    for (uint16_t i = 0; i < num_ap; ++i) {
        std::string ssid_str = reinterpret_cast<const char*>(ap_records[i].ssid);
        ESP_LOGI(TAG, "  [AP] SSID: '%s', RSSI: %d dBm, Ch: %d",
                 ssid_str.empty() ? "<hidden>" : ssid_str.c_str(),
                 ap_records[i].rssi,
                 ap_records[i].primary);
        if (!ssid_str.empty()) {
            visible_ssids.insert(ssid_str);
        }
    }

    return visible_ssids;
}

bool WifiManager::attempt_connect(const std::string& ssid, const std::string& password) {
    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID: '%s' (password length: %u)...", ssid.c_str(), (unsigned)password.length());

    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password), password.c_str(), sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    xEventGroupClearBits(wifi_event_group_, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    ESP_ERROR_CHECK(esp_wifi_disconnect());
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group_,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(10000)
    );

    if (bits & WIFI_CONNECTED_BIT) {
        return true;
    }

    ESP_LOGW(TAG, "Wi-Fi connection to '%s' failed: %s (code %d)",
             ssid.c_str(), get_disconnect_reason_str(last_disconnect_reason_), last_disconnect_reason_);
    return false;
}

void WifiManager::connect(StatusLed* led) {
    if (is_connected()) {
        ESP_LOGI(TAG, "Already connected to Wi-Fi. IP: %s", ip_address_.c_str());
        return;
    }

    if (ap_list_.empty()) {
        ESP_LOGW(TAG, "No Wi-Fi SSIDs configured.");
        if (led != nullptr) {
            led->set_state(LedState::WIFI_RECONNECTING);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
        return;
    }

    if (led != nullptr) {
        led->set_state(LedState::WIFI_CONNECTING);
    }

    // Step 1: Fast direct attempt on primary AP
    size_t last_tried_idx = 0;
    if (attempt_connect(ap_list_[0].first, ap_list_[0].second)) {
        return;
    }

    // Step 2: Scan and round-robin fallback
    while (!is_connected()) {
        std::set<std::string> visible_ssids = scan_networks();

        std::vector<size_t> candidates;
        size_t num_aps = ap_list_.size();
        for (size_t step = 1; step <= num_aps; ++step) {
            size_t idx = (last_tried_idx + step) % num_aps;
            if (visible_ssids.find(ap_list_[idx].first) != visible_ssids.end()) {
                candidates.push_back(idx);
            }
        }

        if (candidates.empty()) {
            ESP_LOGW(TAG, "No configured Wi-Fi APs visible in scan. Retrying in 3 seconds...");
            if (led != nullptr) {
                led->set_state(LedState::WIFI_RECONNECTING);
            }
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        for (size_t idx : candidates) {
            last_tried_idx = idx;
            if (led != nullptr) {
                led->set_state(LedState::WIFI_CONNECTING);
            }
            if (attempt_connect(ap_list_[idx].first, ap_list_[idx].second)) {
                return;
            }
        }

        ESP_LOGW(TAG, "All visible Wi-Fi candidates failed. Retrying scan in 3 seconds...");
        if (led != nullptr) {
            led->set_state(LedState::WIFI_RECONNECTING);
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
