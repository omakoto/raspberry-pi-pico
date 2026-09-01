#include "mdns_service.hpp"
#include "mdns.h"
#include "esp_log.h"

static const char* TAG = "MdnsService";

MdnsService::MdnsService() : initialized_(false), tcp_port_(10100) {}

MdnsService::~MdnsService() {
    stop();
}

bool MdnsService::init(const std::string& hostname, int tcp_port) {
    stop();

    hostname_ = hostname;
    tcp_port_ = tcp_port;

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MDNS Init failed: %d", err);
        return false;
    }

    ESP_ERROR_CHECK(mdns_hostname_set(hostname_.c_str()));
    ESP_LOGI(TAG, "MDNS hostname set to: %s.local", hostname_.c_str());

    ESP_ERROR_CHECK(mdns_instance_name_set("Nintendo Switch Controller Backend"));

    mdns_service_add(nullptr, "_nscon", "_tcp", tcp_port_, nullptr, 0);
    ESP_LOGI(TAG, "MDNS service registered: _nscon._tcp on port %d", tcp_port_);

    initialized_ = true;
    return true;
}

void MdnsService::stop() {
    if (initialized_) {
        mdns_free();
        initialized_ = false;
    }
}
