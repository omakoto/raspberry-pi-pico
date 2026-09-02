#include "serial_command_server.hpp"
#include <cstdio>
#include <cstring>
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "tinyusb_cdc_acm.h"
#include "tusb.h"
#include "dual_logger.hpp"

static const char* TAG = "SerialCmd";

SerialCommandServer::SerialCommandServer(ControllerState& controller, bool log_enabled, bool enable_echo)
    : controller_(controller),
      log_enabled_(log_enabled),
      enable_echo_(enable_echo),
      task_handle_(nullptr),
      running_(false) {}

SerialCommandServer::~SerialCommandServer() {
    stop();
}

bool SerialCommandServer::start() {
    if (running_) {
        return true;
    }

    // Install UART driver on UART0 if not already installed
    if (!uart_is_driver_installed(UART_NUM_0)) {
        uart_config_t uart_config = {
            .baud_rate = 115200,
            .data_bits = UART_DATA_8_BITS,
            .parity    = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .rx_flow_ctrl_thresh = 0,
            .source_clk = UART_SCLK_DEFAULT,
            .flags = {}
        };
        uart_param_config(UART_NUM_0, &uart_config);
        esp_err_t err = uart_driver_install(UART_NUM_0, 1024, 0, 0, nullptr, 0);
        if (err == ESP_OK) {
            uart_vfs_dev_use_driver(UART_NUM_0);
            ESP_LOGI(TAG, "UART0 command listener installed (115200 baud, D6-TX/D7-RX)");
        } else {
            ESP_LOGW(TAG, "Failed to install UART0 driver: %s", esp_err_to_name(err));
        }
    }

    running_ = true;
    BaseType_t ret = xTaskCreate(task_entry, "serial_cmd_task", 4096, this, tskIDLE_PRIORITY + 3, &task_handle_);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create serial command task");
        running_ = false;
        return false;
    }

    ESP_LOGI(TAG, "Serial command server started (listening on UART0 and USB CDC)");
    return true;
}

void SerialCommandServer::stop() {
    running_ = false;
    if (task_handle_ != nullptr) {
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
    }
}

void SerialCommandServer::task_entry(void* arg) {
    static_cast<SerialCommandServer*>(arg)->run_task();
}

void SerialCommandServer::run_task() {
    char uart_buf[128];
    char cdc_buf[128];

    while (running_) {
        controller_.check_scheduled();

        // 1. Check UART0 input
        if (uart_is_driver_installed(UART_NUM_0)) {
            int uart_len = uart_read_bytes(UART_NUM_0, uart_buf, sizeof(uart_buf), pdMS_TO_TICKS(10));
            if (uart_len > 0) {
                process_stream(uart_accum_, uart_buf, uart_len, false);
            }
        }

        // 2. Check USB CDC input
        if (tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0) && tud_mounted()) {
            size_t cdc_rx_len = 0;
            esp_err_t cdc_err = tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, reinterpret_cast<uint8_t*>(cdc_buf), sizeof(cdc_buf), &cdc_rx_len);
            if (cdc_err == ESP_OK && cdc_rx_len > 0) {
                process_stream(cdc_accum_, cdc_buf, cdc_rx_len, true);
            }
        }
    }

    vTaskDelete(nullptr);
}

void SerialCommandServer::process_stream(std::string& accum, const char* data, size_t len, bool is_cdc) {
    accum.append(data, len);

    while (!accum.empty()) {
        size_t delim_pos = accum.find_first_of("\r\n");
        if (delim_pos == std::string::npos) {
            break;
        }

        std::string line = accum.substr(0, delim_pos);

        // Consume both characters if part of a CRLF (\r\n) or LFCR (\n\r) sequence
        if (delim_pos + 1 < accum.size() &&
            ((accum[delim_pos] == '\r' && accum[delim_pos + 1] == '\n') ||
             (accum[delim_pos] == '\n' && accum[delim_pos + 1] == '\r'))) {
            accum.erase(0, delim_pos + 2);
        } else {
            accum.erase(0, delim_pos + 1);
        }

        if (!line.empty()) {
            if (log_enabled_) {
                dual_println(line);
            }

            if (enable_echo_) {
                if (is_cdc && tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0) && tud_mounted()) {
                    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, reinterpret_cast<const uint8_t*>(line.c_str()), line.length());
                    tinyusb_cdcacm_write_queue_char(TINYUSB_CDC_ACM_0, '\n');
                    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
                } else if (!is_cdc) {
                    std::printf("%s\n", line.c_str());
                    std::fflush(stdout);
                }
            }

            controller_.execute_command(line);
        }
    }
}
