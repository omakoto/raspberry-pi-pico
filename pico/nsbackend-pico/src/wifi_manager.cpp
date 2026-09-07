/*
 * Wi-Fi Manager Implementation for nsbackend-pico.
 * Drives CYW43 Wi-Fi connection, scanning, and multi-AP failover.
 */

#include "wifi_manager.hpp"

#if defined(NSBACKEND_HAS_WIFI) && NSBACKEND_HAS_WIFI

#include <cstring>
#include "pico/stdlib.h"
#include "pico/error.h"
#include "pico/cyw43_arch.h"
#include "cyw43.h"
#include "cyw43_ll.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "dual_logger.hpp"

static const char* TAG = "WifiManager";

WifiManager::WifiManager(const std::vector<std::pair<std::string, std::string>>& ap_list)
    : ap_list_(ap_list),
      connected_(false),
      initialized_(false) {}

WifiManager::~WifiManager() {}

bool WifiManager::init() {
    if (initialized_) {
        return true;
    }

    if (!cyw43_is_initialized(&cyw43_state)) {
        if (cyw43_arch_init() != 0) {
            LOG_E(TAG, "Failed to initialize CYW43 architecture");
            return false;
        }
    }

    cyw43_arch_enable_sta_mode();
    initialized_ = true;
    LOG_I(TAG, "CYW43 Wi-Fi subsystem initialized in STA mode");
    return true;
}

int WifiManager::scan_result_cb(void* env, const struct _cyw43_ev_scan_result_t* result) {
    if (result != nullptr && env != nullptr) {
        auto* found_set = static_cast<std::set<std::string>*>(env);
        if (result->ssid_len > 0) {
            std::string ssid(reinterpret_cast<const char*>(result->ssid), result->ssid_len);
            found_set->insert(ssid);
        }
    }
    return 0;
}

std::set<std::string> WifiManager::scan_networks() {
    LOG_I(TAG, "Scanning visible Wi-Fi networks...");
    std::set<std::string> visible_ssids;

    cyw43_wifi_scan_options_t scan_options = {};
    int ret = cyw43_wifi_scan(&cyw43_state, &scan_options, &visible_ssids, scan_result_cb);
    if (ret != 0) {
        LOG_W(TAG, "cyw43_wifi_scan start returned error: %d", ret);
        return visible_ssids;
    }

    uint32_t start_ms = to_ms_since_boot(get_absolute_time());
    while (cyw43_wifi_scan_active(&cyw43_state) && (to_ms_since_boot(get_absolute_time()) - start_ms < 5000)) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (visible_ssids.empty()) {
        LOG_I(TAG, "  No visible Wi-Fi networks found.");
    }

    return visible_ssids;
}

bool WifiManager::is_connected() const {
    int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    return (status == CYW43_LINK_UP);
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

const char* WifiManager::error_to_string(int err) {
    switch (err) {
        case PICO_OK:
            return "Success";
        case PICO_ERROR_GENERIC:
            return "Generic error";
        case PICO_ERROR_TIMEOUT:
            return "Timed out waiting for AP";
        case PICO_ERROR_NO_DATA:
            return "No data";
        case PICO_ERROR_NOT_PERMITTED:
            return "Operation not permitted";
        case PICO_ERROR_INVALID_ARG:
            return "Invalid argument";
        case PICO_ERROR_IO:
            return "Hardware I/O error";
        case PICO_ERROR_BADAUTH:
            return "Authentication failed (incorrect password or credentials)";
        case PICO_ERROR_CONNECT_FAILED:
            return "Connection failed (handshake failed or rejected by AP)";
        case PICO_ERROR_INSUFFICIENT_RESOURCES:
            return "Insufficient resources (out of memory)";
        case PICO_ERROR_INVALID_STATE:
            return "Invalid state";
        case PICO_ERROR_NOT_FOUND:
            return "Network / SSID not found";
        default:
            return "Unknown error";
    }
}

const char* WifiManager::link_status_to_string(int status) {
    switch (status) {
        case CYW43_LINK_DOWN:
            return "Link down";
        case CYW43_LINK_JOIN:
            return "Joined Wi-Fi";
        case CYW43_LINK_NOIP:
            return "Connected, waiting for IP";
        case CYW43_LINK_UP:
            return "Link up";
        case CYW43_LINK_FAIL:
            return "Connection failed";
        case CYW43_LINK_NONET:
            return "SSID not found";
        case CYW43_LINK_BADAUTH:
            return "Authentication failed";
        default:
            return "Unknown status";
    }
}

bool WifiManager::attempt_connect(const std::string& ssid, const std::string& password) {
    LOG_I(TAG, "Connecting to Wi-Fi SSID: '%s' (password length: %u)...", ssid.c_str(), static_cast<unsigned>(password.length()));

    // Try standard WPA2-AES first (standard across modern APs)
    uint32_t auth = password.empty() ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_AES_PSK;
    int ret = cyw43_arch_wifi_connect_timeout_ms(ssid.c_str(), password.c_str(), auth, 10000);

    // If WPA2-AES failed and network has a password, attempt WPA2/WPA mixed mode fallback
    if (ret != 0 && !password.empty()) {
        LOG_I(TAG, "Retrying '%s' with mixed WPA2/WPA auth mode...", ssid.c_str());
        ret = cyw43_arch_wifi_connect_timeout_ms(ssid.c_str(), password.c_str(), CYW43_AUTH_WPA2_MIXED_PSK, 10000);
    }

    if (ret == 0) {
        const ip4_addr_t* addr = netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
        char ip_str[32];
        ip4addr_ntoa_r(addr, ip_str, sizeof(ip_str));
        ip_address_ = ip_str;
        connected_.store(true, std::memory_order_relaxed);
        // Disable Wi-Fi power saving mode completely to eliminate latency and packet loss
        cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
        LOG_I(TAG, "Connected to Wi-Fi successfully! IP: %s (no-power-save PM active)", ip_address_.c_str());
        return true;
    }

    int link_status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    LOG_W(TAG, "Wi-Fi connection to '%s' failed: %s (error %d, link: %s)",
          ssid.c_str(), error_to_string(ret), ret, link_status_to_string(link_status));
    connected_.store(false, std::memory_order_relaxed);
    return false;
}

void WifiManager::connect(StatusLed* led) {
    if (is_connected()) {
        LOG_I(TAG, "Already connected to Wi-Fi. IP: %s", ip_address_.c_str());
        return;
    }

    if (ap_list_.empty()) {
        LOG_W(TAG, "No Wi-Fi SSIDs configured.");
        if (led != nullptr) {
            led->set_state(LedState::WIFI_RECONNECTING);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
        return;
    }

    if (led != nullptr) {
        led->set_state(LedState::WIFI_CONNECTING);
    }

    // Try each configured AP in round-robin sequence
    while (!is_connected()) {
        for (size_t idx = 0; idx < ap_list_.size(); ++idx) {
            if (led != nullptr) {
                led->set_state(LedState::WIFI_CONNECTING);
            }
            if (attempt_connect(ap_list_[idx].first, ap_list_[idx].second)) {
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        LOG_W(TAG, "All configured Wi-Fi APs failed to connect. Retrying in 3 seconds...");
        if (led != nullptr) {
            led->set_state(LedState::WIFI_RECONNECTING);
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

#endif // NSBACKEND_HAS_WIFI
