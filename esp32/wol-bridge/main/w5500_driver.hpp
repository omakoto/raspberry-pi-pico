#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include "esp_eth.h"
#include "driver/gpio.h"
#include "mac_tracker.hpp"
#include "status_led.hpp"

struct W5500Config {
    int sclk_pin = 7;
    int mosi_pin = 9;
    int miso_pin = 8;
    int cs_pin = 4;
    int rst_pin = 3;
    int int_pin = 2;
    int poll_period_ms = 2;
    int spi_speed_mhz = 25;
    bool int_diag = false;
    bool log_rx_packets = false;
    uint8_t own_mac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};
};

class W5500Driver {
public:
    W5500Driver();
    ~W5500Driver();

    bool init(const W5500Config& config, MacTracker* tracker, StatusLed* led);
    bool start();
    void stop();

    bool is_link_up() const;
    const uint8_t* get_own_mac() const { return config_.own_mac; }

    // Sends WoL Magic Packet (both Raw L2 and UDP port 9) to a single target MAC
    bool send_wol(const uint8_t target_mac[6]);

    // Sends WoL Magic Packets to all tracked MACs
    size_t send_wol_all(const std::vector<MacEntry>& targets);

private:
    void reset_hardware();
    static esp_err_t packet_input_cb(esp_eth_handle_t eth_handle, uint8_t* buffer, uint32_t length, void* priv);
    static void eth_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

    W5500Config config_;
    MacTracker* tracker_;
    StatusLed* led_;
    esp_eth_handle_t eth_handle_;
    std::atomic<bool> link_up_;
    bool isr_installed_;
    bool initialized_;
};
