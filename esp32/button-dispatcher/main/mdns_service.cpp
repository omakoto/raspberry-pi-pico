#include "mdns_service.hpp"
#include "mdns.h"
#include "esp_log.h"

static const char* TAG = "MdnsService";

MdnsService::MdnsService() : initialized_(false), http_port_(80) {}

MdnsService::~MdnsService() {
    stop();
}

bool MdnsService::init(const std::string& hostname, int http_port) {
    stop();

    hostname_ = hostname;
    http_port_ = http_port;

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MDNS Init failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_ERROR_CHECK(mdns_hostname_set(hostname_.c_str()));
    ESP_LOGI(TAG, "MDNS hostname set to: %s.local", hostname_.c_str());

    ESP_ERROR_CHECK(mdns_instance_name_set("BLE Button Dispatcher"));

    mdns_service_add(nullptr, "_http", "_tcp", http_port_, nullptr, 0);
    ESP_LOGI(TAG, "MDNS service registered: _http._tcp on port %d", http_port_);

    initialized_ = true;
    return true;
}

void MdnsService::stop() {
    if (initialized_) {
        mdns_free();
        initialized_ = false;
    }
}
