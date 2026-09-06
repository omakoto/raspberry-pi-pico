/*
 * Wi-Fi Manager for nsbackend-pico.
 * Handles CYW43 initialization, multi-AP connection with scan fallback, and auto-reconnection.
 */

#pragma once

#if defined(NSBACKEND_HAS_WIFI) && NSBACKEND_HAS_WIFI

#include <string>
#include <vector>
#include <set>
#include <utility>
#include <atomic>
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
    bool attempt_connect(const std::string& ssid, const std::string& password);
    static int scan_result_cb(void* env, const struct _cyw43_ev_scan_result_t* result);

    std::vector<std::pair<std::string, std::string>> ap_list_;
    std::atomic<bool> connected_;
    std::string ip_address_;
    bool initialized_;
};

#endif // NSBACKEND_HAS_WIFI
