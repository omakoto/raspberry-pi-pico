#include "w5500_driver.hpp"
#include <cstdio>
#include <cstring>
#include "esp_log.h"
#include "esp_event.h"
#include "driver/spi_master.h"
#include "esp_eth_mac_spi.h"

static const char* TAG = "W5500Driver";

static bool parse_mac(const std::string& mac_str, uint8_t mac_out[6]) {
    unsigned int bytes[6];
    if (sscanf(mac_str.c_str(), "%x:%x:%x:%x:%x:%x",
               &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4], &bytes[5]) == 6) {
        for (int i = 0; i < 6; ++i) {
            mac_out[i] = static_cast<uint8_t>(bytes[i]);
        }
        return true;
    }
    return false;
}

W5500Driver::W5500Driver()
    : status_led_(nullptr),
      eth_handle_(nullptr),
      eth_netif_(nullptr),
      glue_handle_(nullptr),
      isr_installed_(false) {}

W5500Driver::~W5500Driver() {
    if (eth_handle_ != nullptr) {
        esp_eth_stop(eth_handle_);
        if (glue_handle_ != nullptr) {
            esp_eth_del_netif_glue(glue_handle_);
            glue_handle_ = nullptr;
        }
        esp_eth_driver_uninstall(eth_handle_);
        eth_handle_ = nullptr;
    }
    if (eth_netif_ != nullptr) {
        esp_netif_destroy(eth_netif_);
        eth_netif_ = nullptr;
    }
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

    // Assert RSTn LOW for 10ms, then deassert HIGH and wait 160ms for PLL stabilization
    gpio_set_level(config_.rst_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(config_.rst_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(config_.rst_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(160));
    ESP_LOGI(TAG, "W5500 hardware reset completed and PLL stabilized.");
}

bool W5500Driver::init(const W5500Config& config, StatusLed* status_led, IpCallback ip_callback) {
    config_ = config;
    status_led_ = status_led;
    ip_callback_ = ip_callback;

    if (status_led_) {
        status_led_->set_state(LedState::INITIALIZING);
    }

    // 1. Hardware Reset (if reset pin configured >= 0)
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
    spi_device_interface_config_t devcfg = {};
    devcfg.mode = 0;
    devcfg.clock_speed_hz = 14 * 1000 * 1000; // 14 MHz
    devcfg.spics_io_num = config_.cs_pin;
    devcfg.queue_size = 20;

    // 4. Configure W5500 MAC
    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &devcfg);
    if (config_.int_pin >= 0) {
        ESP_LOGI(TAG, "Configuring W5500 in Interrupt mode on GPIO %d", config_.int_pin);
        esp_err_t isr_err = gpio_install_isr_service(0);
        if (isr_err == ESP_OK) {
            isr_installed_ = true;
        } else if (isr_err == ESP_ERR_INVALID_STATE) {
            // Already installed by system or earlier component
            isr_installed_ = true;
        } else {
            ESP_LOGW(TAG, "gpio_install_isr_service returned: %s", esp_err_to_name(isr_err));
        }
        w5500_config.int_gpio_num = config_.int_pin;
        w5500_config.poll_period_ms = 0;
    } else {
        uint32_t poll_ms = (config_.poll_period_ms > 0) ? config_.poll_period_ms : 5;
        ESP_LOGI(TAG, "Configuring W5500 in Polling mode (period: %lu ms)", poll_ms);
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
    phy_config.reset_gpio_num = -1; // Reset already executed in reset_hardware()
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

    // Set custom MAC address if configured
    uint8_t mac_bytes[6] = {};
    if (parse_mac(config_.mac_address, mac_bytes)) {
        err = esp_eth_ioctl(eth_handle_, ETH_CMD_S_MAC_ADDR, mac_bytes);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Configured custom MAC address: %s", config_.mac_address.c_str());
        } else {
            ESP_LOGW(TAG, "Failed to set custom MAC address: %s", esp_err_to_name(err));
        }
    }

    // 7. Register Event Handlers
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &W5500Driver::eth_event_handler, this));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &W5500Driver::got_ip_event_handler, this));

    // 8. Attach to TCP/IP Stack via esp_netif
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    eth_netif_ = esp_netif_new(&netif_cfg);
    if (!eth_netif_) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        return false;
    }

    if (!config_.hostname.empty()) {
        esp_netif_set_hostname(eth_netif_, config_.hostname.c_str());
    }

    glue_handle_ = esp_eth_new_netif_glue(eth_handle_);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif_, glue_handle_));

    // 9. Start Ethernet Driver
    ESP_LOGI(TAG, "Checking physical Ethernet link state...");
    if (status_led_) {
        status_led_->set_state(LedState::ETH_LINK_DOWN);
    }

    err = esp_eth_start(eth_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_start failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "W5500 driver started successfully.");
    return true;
}

void W5500Driver::eth_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* self = static_cast<W5500Driver*>(arg);
    self->on_eth_event(event_id, event_data);
}

void W5500Driver::got_ip_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* self = static_cast<W5500Driver*>(arg);
    self->on_got_ip(event_data);
}

void W5500Driver::on_eth_event(int32_t event_id, void* event_data) {
    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Ethernet cable connected (link is UP)! Requesting DHCP lease...");
            if (status_led_) {
                status_led_->set_state(LedState::ETH_DHCP_WAIT);
            }
            break;

        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Warning: Ethernet link lost. Waiting for cable reconnection...");
            if (status_led_) {
                status_led_->set_state(LedState::ETH_LINK_DOWN);
            }
            break;

        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Ethernet interface started.");
            break;

        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet interface stopped.");
            break;

        default:
            break;
    }
}

void W5500Driver::on_got_ip(void* event_data) {
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);
    const esp_netif_ip_info_t* ip_info = &event->ip_info;

    uint8_t mac[6] = {};
    esp_eth_ioctl(eth_handle_, ETH_CMD_G_MAC_ADDR, mac);

    esp_netif_dns_info_t dns_info = {};
    esp_netif_get_dns_info(eth_netif_, ESP_NETIF_DNS_MAIN, &dns_info);

    const char* hostname = nullptr;
    esp_netif_get_hostname(eth_netif_, &hostname);

    printf("------------------------------------------------\n");
    printf("Ethernet Connected!\n");
    printf("  MAC Address:  %02X:%02X:%02X:%02X:%02X:%02X\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    printf("  IP Address:   " IPSTR "\n", IP2STR(&ip_info->ip));
    printf("  Subnet Mask:  " IPSTR "\n", IP2STR(&ip_info->netmask));
    printf("  Gateway:      " IPSTR "\n", IP2STR(&ip_info->gw));
    printf("  DNS Server:   " IPSTR "\n", IP2STR(&dns_info.ip.u_addr.ip4));
    printf("  Hostname:     %s\n", hostname ? hostname : "w5500-test");
    printf("------------------------------------------------\n");

    if (ip_callback_) {
        ip_callback_(*ip_info);
    }
}
