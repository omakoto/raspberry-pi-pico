#include "w5500_driver.hpp"
#include <cstdio>
#include <cstring>
#include "esp_log.h"
#include "esp_event.h"
#include "driver/spi_master.h"
#include "esp_eth_mac_spi.h"

static const char* TAG = "W5500Driver";

namespace {

uint16_t calc_ip_checksum(const void* vdata, size_t length) {
    const uint8_t* data = static_cast<const uint8_t*>(vdata);
    uint32_t sum = 0;
    for (size_t i = 0; i < length - 1; i += 2) {
        sum += (data[i] << 8) | data[i + 1];
    }
    if (length & 1) {
        sum += data[length - 1] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return static_cast<uint16_t>(~sum);
}

} // namespace

W5500Driver::W5500Driver()
    : tracker_(nullptr),
      led_(nullptr),
      eth_handle_(nullptr),
      link_up_(false),
      isr_installed_(false),
      initialized_(false) {}

W5500Driver::~W5500Driver() {
    stop();
}

void W5500Driver::reset_hardware() {
    if (config_.rst_pin < 0) {
        return;
    }

    ESP_LOGI(TAG, "Executing hardware reset pulse on W5500 (RST GPIO %d)...", config_.rst_pin);
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << config_.rst_pin);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    // Pulse RSTn LOW for 10ms, then deassert HIGH and wait 160ms for PLL stabilization
    gpio_set_level(static_cast<gpio_num_t>(config_.rst_pin), 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(static_cast<gpio_num_t>(config_.rst_pin), 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(static_cast<gpio_num_t>(config_.rst_pin), 1);
    vTaskDelay(pdMS_TO_TICKS(160));
    ESP_LOGI(TAG, "W5500 hardware reset completed and PLL stabilized.");
}

bool W5500Driver::init(const W5500Config& config, MacTracker* tracker, StatusLed* led) {
    config_ = config;
    tracker_ = tracker;
    led_ = led;

    // 1. Hardware Reset
    reset_hardware();

    // 2. Initialize SPI Bus (SPI2_HOST / FSPI)
    ESP_LOGI(TAG, "Initializing SPI bus: SCLK=GPIO%d, MOSI=GPIO%d, MISO=GPIO%d, CS=GPIO%d",
             config_.sclk_pin, config_.mosi_pin, config_.miso_pin, config_.cs_pin);

    spi_bus_config_t buscfg = {};
    buscfg.miso_io_num = config_.miso_pin;
    buscfg.mosi_io_num = config_.mosi_pin;
    buscfg.sclk_io_num = config_.sclk_pin;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = 4000;

    esp_err_t err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return false;
    }

    // 3. Configure SPI Device Interface
    int speed_mhz = (config_.spi_speed_mhz > 0) ? config_.spi_speed_mhz : 25;
    ESP_LOGI(TAG, "Setting SPI clock speed: %d MHz", speed_mhz);
    spi_device_interface_config_t devcfg = {};
    devcfg.mode = 0;
    devcfg.clock_speed_hz = speed_mhz * 1000 * 1000;
    devcfg.spics_io_num = config_.cs_pin;
    devcfg.queue_size = 20;

    // 4. Configure W5500 MAC
    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &devcfg);
    bool use_interrupt = config_.int_pin >= 0;
    if (use_interrupt) {
        esp_err_t isr_err = gpio_install_isr_service(0);
        if (isr_err == ESP_OK || isr_err == ESP_ERR_INVALID_STATE) {
            isr_installed_ = true;
        } else {
            ESP_LOGW(TAG, "gpio_install_isr_service failed (%s); falling back to polling mode",
                     esp_err_to_name(isr_err));
            use_interrupt = false;
        }
    }

    if (use_interrupt) {
        ESP_LOGI(TAG, "Configuring W5500 in Interrupt mode on GPIO %d", config_.int_pin);
        w5500_config.int_gpio_num = config_.int_pin;
        w5500_config.poll_period_ms = 0;
    } else {
        uint32_t poll_ms = (config_.poll_period_ms > 0) ? config_.poll_period_ms : 5;
        ESP_LOGI(TAG, "Configuring W5500 in Polling mode (period: %lu ms)", static_cast<unsigned long>(poll_ms));
        w5500_config.int_gpio_num = -1;
        w5500_config.poll_period_ms = poll_ms;
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t* mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    if (!mac) {
        ESP_LOGE(TAG, "esp_eth_mac_new_w5500 failed");
        return false;
    }

    // 5. Configure W5500 PHY
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = -1;
    phy_config.reset_gpio_num = -1;
    esp_eth_phy_t* phy = esp_eth_phy_new_w5500(&phy_config);
    if (!phy) {
        ESP_LOGE(TAG, "esp_eth_phy_new_w5500 failed");
        mac->del(mac);
        return false;
    }

    // 6. Install Ethernet Driver
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    err = esp_eth_driver_install(&eth_config, &eth_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_driver_install failed: %s", esp_err_to_name(err));
        mac->del(mac);
        phy->del(phy);
        return false;
    }

    // 7. Set custom MAC address
    err = esp_eth_ioctl(eth_handle_, ETH_CMD_S_MAC_ADDR, config_.own_mac);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Configured W5500 MAC: %s", MacTracker::format_mac(config_.own_mac).c_str());
    } else {
        ESP_LOGW(TAG, "Failed to set W5500 MAC: %s", esp_err_to_name(err));
    }

    // 8. Enable Promiscuous Mode to sniff all packets from PC
    bool promisc = true;
    err = esp_eth_ioctl(eth_handle_, ETH_CMD_S_PROMISCUOUS, &promisc);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Promiscuous mode enabled on W5500.");
    } else {
        ESP_LOGW(TAG, "Setting promiscuous mode returned: %s", esp_err_to_name(err));
    }

    // 9. Install custom raw packet input path callback
    esp_eth_update_input_path(eth_handle_, &W5500Driver::packet_input_cb, this);

    // 10. Register Ethernet Events
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &W5500Driver::eth_event_handler, this));

    initialized_ = true;
    return true;
}

bool W5500Driver::start() {
    if (!initialized_ || !eth_handle_) {
        return false;
    }
    esp_err_t err = esp_eth_start(eth_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_start failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "W5500 Ethernet sniffer started successfully.");
    return true;
}

void W5500Driver::stop() {
    if (eth_handle_) {
        esp_eth_stop(eth_handle_);
        esp_eth_driver_uninstall(eth_handle_);
        eth_handle_ = nullptr;
    }
    initialized_ = false;
}

bool W5500Driver::is_link_up() const {
    return link_up_.load(std::memory_order_relaxed);
}

void W5500Driver::eth_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* self = static_cast<W5500Driver*>(arg);
    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Ethernet link UP (PC connected)!");
            self->link_up_.store(true, std::memory_order_relaxed);
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Ethernet link DOWN (PC disconnected or unpowered)!");
            self->link_up_.store(false, std::memory_order_relaxed);
            break;
        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Ethernet driver started.");
            break;
        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet driver stopped.");
            self->link_up_.store(false, std::memory_order_relaxed);
            break;
        default:
            break;
    }
}

esp_err_t W5500Driver::packet_input_cb(esp_eth_handle_t eth_handle, uint8_t* buffer, uint32_t length, void* priv) {
    auto* self = static_cast<W5500Driver*>(priv);
    if (self && length >= 14 && buffer) {
        const uint8_t* dst_mac = buffer;
        const uint8_t* src_mac = buffer + 6;
        uint16_t ethertype = (static_cast<uint16_t>(buffer[12]) << 8) | buffer[13];

        char src_str[20], dst_str[20];
        snprintf(src_str, sizeof(src_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5]);
        snprintf(dst_str, sizeof(dst_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 dst_mac[0], dst_mac[1], dst_mac[2], dst_mac[3], dst_mac[4], dst_mac[5]);

        if (self->config_.log_rx_packets) {
            ESP_LOGI(TAG, "LAN Rx: len=%lu | src=%s | dst=%s | type=0x%04X",
                     static_cast<unsigned long>(length), src_str, dst_str, ethertype);
        }

        if (self->tracker_) {
            self->tracker_->record_packet(src_mac, dst_mac, ethertype, length);
            if (self->tracker_->on_frame_received(src_mac)) {
                // Pulse status LED when a new MAC is learned or wake purge triggers
                if (self->led_) {
                    self->led_->pulse(150);
                }
            }
        }
    }

    // Always free the receive buffer allocated by ESP-IDF SPI Ethernet driver
    if (buffer) {
        free(buffer);
    }
    return ESP_OK;
}

bool W5500Driver::send_wol(const uint8_t target_mac[6]) {
    if (!eth_handle_ || !is_link_up()) {
        ESP_LOGW(TAG, "Cannot send WoL packet: Ethernet link is DOWN.");
        return false;
    }

    ESP_LOGI(TAG, "Sending WoL Magic Packet to target: %s",
             MacTracker::format_mac(target_mac).c_str());

    // 1. Build Layer 2 WoL frame (EtherType 0x0842)
    // Frame size: 14 header + 102 magic payload = 116 bytes
    uint8_t l2_frame[116];
    // Dst MAC: Broadcast FF:FF:FF:FF:FF:FF
    memset(l2_frame, 0xFF, 6);
    // Src MAC: W5500 MAC
    memcpy(l2_frame + 6, config_.own_mac, 6);
    // EtherType: 0x0842 (Wake-on-LAN)
    l2_frame[12] = 0x08;
    l2_frame[13] = 0x42;
    // Magic Payload: 6x 0xFF followed by 16x target MAC
    memset(l2_frame + 14, 0xFF, 6);
    for (int i = 0; i < 16; ++i) {
        memcpy(l2_frame + 14 + 6 + (i * 6), target_mac, 6);
    }

    // 2. Build UDP Broadcast WoL frame (EtherType 0x0800, UDP Port 9)
    // Frame size: 14 eth + 20 ip + 8 udp + 102 payload = 144 bytes
    uint8_t udp_frame[144];
    // Dst MAC: Broadcast
    memset(udp_frame, 0xFF, 6);
    // Src MAC: W5500 MAC
    memcpy(udp_frame + 6, config_.own_mac, 6);
    // EtherType: 0x0800 (IPv4)
    udp_frame[12] = 0x08;
    udp_frame[13] = 0x00;

    // IPv4 Header (20 bytes)
    uint8_t* ip = udp_frame + 14;
    ip[0] = 0x45; // Version 4, IHL 5
    ip[1] = 0x00; // DSCP / ECN
    uint16_t ip_total_len = 20 + 8 + 102;
    ip[2] = (ip_total_len >> 8) & 0xFF;
    ip[3] = ip_total_len & 0xFF;
    ip[4] = 0x12; ip[5] = 0x34; // Identification
    ip[6] = 0x00; ip[7] = 0x00; // Flags / Frag
    ip[8] = 64;   // TTL
    ip[9] = 17;   // Protocol 17 = UDP
    ip[10] = 0x00; ip[11] = 0x00; // Checksum placeholder
    // Src IP: 0.0.0.0
    memset(ip + 12, 0, 4);
    // Dst IP: 255.255.255.255
    memset(ip + 16, 0xFF, 4);
    uint16_t checksum = calc_ip_checksum(ip, 20);
    ip[10] = (checksum >> 8) & 0xFF;
    ip[11] = checksum & 0xFF;

    // UDP Header (8 bytes)
    uint8_t* udp = ip + 20;
    udp[0] = 0x27; udp[1] = 0x10; // Source port: 10000
    udp[2] = 0x00; udp[3] = 0x09; // Destination port: 9 (Discard/WoL)
    uint16_t udp_len = 8 + 102;
    udp[4] = (udp_len >> 8) & 0xFF;
    udp[5] = udp_len & 0xFF;
    udp[6] = 0x00; udp[7] = 0x00; // Checksum 0 (optional for IPv4 UDP)

    // Magic Payload into UDP packet
    uint8_t* payload = udp + 8;
    memset(payload, 0xFF, 6);
    for (int i = 0; i < 16; ++i) {
        memcpy(payload + 6 + (i * 6), target_mac, 6);
    }

    // Transmit frames 3 times with brief pause for guaranteed delivery
    for (int burst = 0; burst < 3; ++burst) {
        esp_eth_transmit(eth_handle_, l2_frame, sizeof(l2_frame));
        vTaskDelay(pdMS_TO_TICKS(10));
        esp_eth_transmit(eth_handle_, udp_frame, sizeof(udp_frame));
        vTaskDelay(pdMS_TO_TICKS(15));
    }

    return true;
}

size_t W5500Driver::send_wol_all(const std::vector<MacEntry>& targets) {
    if (!is_link_up()) {
        ESP_LOGW(TAG, "Cannot send WoL: Ethernet physical link is DOWN.");
        return 0;
    }

    size_t count = 0;
    for (const auto& entry : targets) {
        if (send_wol(entry.mac)) {
            count++;
        }
    }
    return count;
}
