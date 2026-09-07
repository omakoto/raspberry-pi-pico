/*
 * Serial Command Server Implementation for nsbackend-pico.
 * Reads commands from UART0 (GP12/GP13) and USB CDC ACM.
 */

#include "serial_command_server.hpp"
#include <cstdio>
#include <cstring>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "tusb.h"
#include "dual_logger.hpp"
#include "gamepad_hid.hpp"
#if defined(NSBACKEND_HAS_WIFI) && NSBACKEND_HAS_WIFI
#include "pico/cyw43_arch.h"
#include "cyw43.h"
#include "lwip/ip4_addr.h"
#include "wifi_manager.hpp"
#endif

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

    // Configure UART0 on GP12 (TX) and GP13 (RX)
    uart_init(uart0, 115200);
    gpio_set_function(12, GPIO_FUNC_UART);
    gpio_set_function(13, GPIO_FUNC_UART);
    uart_set_hw_flow(uart0, false, false);
    uart_set_format(uart0, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(uart0, true);

    LOG_I(TAG, "UART0 command listener configured (115200 baud, GP12-TX / GP13-RX)");

    running_ = true;
    BaseType_t ret = xTaskCreate(task_entry, "serial_cmd_task", 2048, this, tskIDLE_PRIORITY + 3, &task_handle_);
    if (ret != pdPASS) {
        LOG_E(TAG, "Failed to create serial command task");
        running_ = false;
        return false;
    }

    LOG_I(TAG, "Serial command server started (listening on UART0 and USB CDC)");
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
        size_t uart_count = 0;
        while (uart_is_readable(uart0) && uart_count < sizeof(uart_buf)) {
            uart_buf[uart_count++] = static_cast<char>(uart_getc(uart0));
        }
        if (uart_count > 0) {
            process_stream(uart_accum_, uart_buf, uart_count, false);
        }

        // 2. Check USB CDC input
        if (tud_mounted() && tud_cdc_n_available(0)) {
            uint32_t cdc_rx_len = tud_cdc_n_read(0, cdc_buf, sizeof(cdc_buf));
            if (cdc_rx_len > 0) {
                process_stream(cdc_accum_, cdc_buf, cdc_rx_len, true);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
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

        // Consume both characters if part of a CRLF or LFCR sequence
        if (delim_pos + 1 < accum.size() &&
            ((accum[delim_pos] == '\r' && accum[delim_pos + 1] == '\n') ||
             (accum[delim_pos] == '\n' && accum[delim_pos + 1] == '\r'))) {
            accum.erase(0, delim_pos + 2);
        } else {
            accum.erase(0, delim_pos + 1);
        }

        if (!line.empty()) {
            if (log_enabled_) {
                dual_println(ControllerState::format_command_for_log(line));
            }

            if (enable_echo_) {
                if (is_cdc && tud_mounted()) {
                    tud_cdc_n_write(0, line.c_str(), static_cast<uint32_t>(line.length()));
                    tud_cdc_n_write(0, "\r\n", 2);
                    tud_cdc_n_write_flush(0);
                } else if (!is_cdc) {
                    uart_write_blocking(uart0, reinterpret_cast<const uint8_t*>(line.c_str()), line.length());
                    uart_write_blocking(uart0, reinterpret_cast<const uint8_t*>("\r\n"), 2);
                }
            }

            if (line == "reboot bootloader" || line == "bootloader" || line == "bootsel") {
                dual_println("Rebooting to USB bootloader...");
                reboot_to_bootsel();
                continue;
            }

            if (line == "status" || line == "info" || line == "netstat") {
#if defined(NSBACKEND_HAS_WIFI) && NSBACKEND_HAS_WIFI
                const ip4_addr_t* addr = netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
                const ip4_addr_t* mask = netif_ip4_netmask(&cyw43_state.netif[CYW43_ITF_STA]);
                const ip4_addr_t* gw = netif_ip4_gw(&cyw43_state.netif[CYW43_ITF_STA]);
                int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
                char ip_buf[16] = "0.0.0.0", mask_buf[16] = "0.0.0.0", gw_buf[16] = "0.0.0.0";
                if (addr) ip4addr_ntoa_r(addr, ip_buf, sizeof(ip_buf));
                if (mask) ip4addr_ntoa_r(mask, mask_buf, sizeof(mask_buf));
                if (gw) ip4addr_ntoa_r(gw, gw_buf, sizeof(gw_buf));
                dual_printf("Wi-Fi: %s (link code %d)\n", WifiManager::link_status_to_string(link), link);
                dual_printf("IPv4: %s  Mask: %s  GW: %s\n", ip_buf, mask_buf, gw_buf);
#endif
                dual_printf("FreeRTOS Heap: %u bytes free (min ever: %u bytes)\n",
                            static_cast<unsigned>(xPortGetFreeHeapSize()),
                            static_cast<unsigned>(xPortGetMinimumEverFreeHeapSize()));
                continue;
            }

            if (line == "help" || line == "?") {
                dual_println("Commands: status, info, netstat, bootloader, help, or controller input (e.g. a, b, x, y, h)");
                continue;
            }

            controller_.execute_command(line);
        }
    }
}
