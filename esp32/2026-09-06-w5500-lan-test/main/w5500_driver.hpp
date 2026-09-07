#pragma once

#include <string>
#include <functional>
#include "esp_eth.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "status_led.hpp"

struct W5500Config {
    gpio_num_t sclk_pin;
    gpio_num_t mosi_pin;
    gpio_num_t miso_pin;
    gpio_num_t cs_pin;
    gpio_num_t rst_pin;
    gpio_num_t int_pin;
    uint32_t poll_period_ms;
    int spi_speed_mhz;
    bool int_diag;
    std::string hostname;
    std::string mac_address;
};

class W5500Driver {
public:
    using IpCallback = std::function<void(const esp_netif_ip_info_t&)>;

    W5500Driver();
    ~W5500Driver();

    bool init(const W5500Config& config, StatusLed* status_led, IpCallback ip_callback);
    void reset_hardware();

    esp_eth_handle_t get_eth_handle() const { return eth_handle_; }
    esp_netif_t* get_netif() const { return eth_netif_; }

private:
    static void eth_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    static void got_ip_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

    void on_eth_event(int32_t event_id, void* event_data);
    void on_got_ip(void* event_data);

    W5500Config config_;
    StatusLed* status_led_;
    IpCallback ip_callback_;
    esp_eth_handle_t eth_handle_;
    esp_netif_t* eth_netif_;
    esp_eth_netif_glue_handle_t glue_handle_;
    bool isr_installed_;
};
