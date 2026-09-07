#include <cstdio>
#include <string>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "status_led.hpp"
#include "config_manager.hpp"
#include "usb_msc.hpp"
#include "dual_logger.hpp"
#include "w5500_driver.hpp"
#include "tcp_server.hpp"
#include "mdns_service.hpp"

static const char* TAG = "Main";

#ifndef CONFIG_W5500_HOSTNAME
#define CONFIG_W5500_HOSTNAME "w5500-test"
#endif

#ifndef CONFIG_W5500_TCP_PORT
#define CONFIG_W5500_TCP_PORT 10110
#endif

#ifndef CONFIG_W5500_MAC_ADDR
#define CONFIG_W5500_MAC_ADDR "DE:AD:BE:EF:FE:ED"
#endif

#ifndef CONFIG_W5500_SPI_SCLK_GPIO
#define CONFIG_W5500_SPI_SCLK_GPIO 7
#endif

#ifndef CONFIG_W5500_SPI_MOSI_GPIO
#define CONFIG_W5500_SPI_MOSI_GPIO 9
#endif

#ifndef CONFIG_W5500_SPI_MISO_GPIO
#define CONFIG_W5500_SPI_MISO_GPIO 8
#endif

#ifndef CONFIG_W5500_SPI_CS_GPIO
#define CONFIG_W5500_SPI_CS_GPIO 4
#endif

#ifndef CONFIG_W5500_RST_GPIO
#define CONFIG_W5500_RST_GPIO 3
#endif

#ifndef CONFIG_W5500_INT_GPIO
#define CONFIG_W5500_INT_GPIO 2
#endif

// Only consulted when spi_int is -1 (polling mode); interrupt mode ignores it.
#ifndef CONFIG_W5500_POLL_MS
#define CONFIG_W5500_POLL_MS 2
#endif

#ifndef CONFIG_W5500_STATUS_LED_GPIO
#define CONFIG_W5500_STATUS_LED_GPIO 21
#endif

static StatusLed s_status_led(static_cast<gpio_num_t>(CONFIG_W5500_STATUS_LED_GPIO));
static ConfigManager s_config_mgr;
static UsbMsc s_usb_msc;
static W5500Driver s_w5500_driver;
static TcpServer s_tcp_server;
static MdnsService s_mdns_service;

static std::string s_active_hostname = CONFIG_W5500_HOSTNAME;
static int s_active_tcp_port = CONFIG_W5500_TCP_PORT;

static void on_ip_acquired(const esp_netif_ip_info_t& ip_info) {
    char ip_str[32];
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));

    // Send UDP broadcast announcement to prime ARP and switch forwarding tables
    TcpServer::send_announcement();

    // Start mDNS responder service
    s_mdns_service.init(s_active_hostname, s_active_tcp_port);

    // Start TCP Echo Server
    s_tcp_server.start(s_active_tcp_port, &s_status_led);

    ESP_LOGI(TAG, "TCP Echo Server ready!");
    ESP_LOGI(TAG, "  Connect via IP:   nc %s %d", ip_str, s_active_tcp_port);
    ESP_LOGI(TAG, "  Connect via mDNS: nc %s.local %d", s_active_hostname.c_str(), s_active_tcp_port);
}

extern "C" void app_main(void) {
    // 1. Initialize Dual Logger (routes logs to both UART0 and TinyUSB CDC ACM)
    dual_logger_init();

    ESP_LOGI(TAG, "Starting W5500 LAN TCP Server on Seeed Studio XIAO ESP32-S3...");

    // 2. Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 3. Initialize Status LED
    s_status_led.init();

    // 4. Mount Wear Levelling FATFS and expose via TinyUSB MSC
    if (s_config_mgr.init("/spiflash", "storage")) {
        // Expose FATFS wear-levelling partition as USB Mass Storage on Native USB OTG port
        s_usb_msc.init(s_config_mgr.get_wl_handle());
        s_config_mgr.load("/spiflash/config.toml", "/spiflash/config-override.toml");
    } else {
        ESP_LOGW(TAG, "FATFS mount skipped or failed; using default configuration");
    }

    s_active_hostname = s_config_mgr.get_string("hostname", CONFIG_W5500_HOSTNAME);
    s_active_tcp_port = s_config_mgr.get_int("tcp_port", CONFIG_W5500_TCP_PORT);

    // 4. Initialize TCP/IP stack and default event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 5. Configure W5500 hardware parameters
    W5500Config cfg = {};
    cfg.sclk_pin = static_cast<gpio_num_t>(s_config_mgr.get_int("spi_sck", CONFIG_W5500_SPI_SCLK_GPIO));
    cfg.mosi_pin = static_cast<gpio_num_t>(s_config_mgr.get_int("spi_mosi", CONFIG_W5500_SPI_MOSI_GPIO));
    cfg.miso_pin = static_cast<gpio_num_t>(s_config_mgr.get_int("spi_miso", CONFIG_W5500_SPI_MISO_GPIO));
    cfg.cs_pin = static_cast<gpio_num_t>(s_config_mgr.get_int("spi_cs", CONFIG_W5500_SPI_CS_GPIO));
    cfg.rst_pin = static_cast<gpio_num_t>(s_config_mgr.get_int("spi_reset", CONFIG_W5500_RST_GPIO));
    cfg.int_pin = static_cast<gpio_num_t>(s_config_mgr.get_int("spi_int", CONFIG_W5500_INT_GPIO));
    cfg.poll_period_ms = s_config_mgr.get_int("poll_period_ms", CONFIG_W5500_POLL_MS);
    cfg.spi_speed_mhz = s_config_mgr.get_int("spi_speed_mhz", 25);
    cfg.int_diag = s_config_mgr.get_bool("int_diag", false);
    cfg.hostname = s_active_hostname;
    cfg.mac_address = s_config_mgr.get_string("mac", CONFIG_W5500_MAC_ADDR);

    ESP_LOGI(TAG, "Configuration: hostname='%s', tcp_port=%d, mac='%s'",
             cfg.hostname.c_str(), s_active_tcp_port, cfg.mac_address.c_str());
    ESP_LOGI(TAG, "SPI Pins: SCK=GPIO%d, MOSI=GPIO%d, MISO=GPIO%d, CS=GPIO%d, RST=GPIO%d, INT=GPIO%d",
             cfg.sclk_pin, cfg.mosi_pin, cfg.miso_pin, cfg.cs_pin, cfg.rst_pin, cfg.int_pin);

    // 6. Initialize W5500 Ethernet Driver
    if (!s_w5500_driver.init(cfg, &s_status_led, on_ip_acquired)) {
        ESP_LOGE(TAG, "Failed to initialize W5500 Ethernet driver");
        return;
    }
}
