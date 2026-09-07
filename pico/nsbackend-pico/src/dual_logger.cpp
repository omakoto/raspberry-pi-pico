/*
 * Dual Logger Implementation for nsbackend-pico.
 * Directs log messages to stdout (UART0) and USB CDC ACM serial console.
 */

#include "dual_logger.hpp"
#include <vector>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "tusb.h"

static SemaphoreHandle_t s_log_mutex = nullptr;

// Translates newline characters to CRLF line endings for USB CDC ACM output
static void cdc_write_crlf(const char* buf, size_t len) {
    if (!tud_mounted() || !tud_cdc_n_connected(0) || len == 0) {
        return;
    }

    size_t last = 0;
    for (size_t i = 0; i < len; ++i) {
        if (buf[i] == '\n' && (i == 0 || buf[i - 1] != '\r')) {
            if (i > last) {
                tud_cdc_n_write(0, buf + last, static_cast<uint32_t>(i - last));
            }
            tud_cdc_n_write(0, "\r\n", 2);
            last = i + 1;
        }
    }
    if (last < len) {
        tud_cdc_n_write(0, buf + last, static_cast<uint32_t>(len - last));
    }
    tud_cdc_n_write_flush(0);
}

// Translates newline characters to CRLF line endings for hardware UART0 output on GP12
static void uart_write_crlf(const char* buf, size_t len) {
    if (len == 0) {
        return;
    }

    size_t last = 0;
    for (size_t i = 0; i < len; ++i) {
        if (buf[i] == '\n' && (i == 0 || buf[i - 1] != '\r')) {
            if (i > last) {
                uart_write_blocking(uart0, reinterpret_cast<const uint8_t*>(buf + last), i - last);
            }
            uart_write_blocking(uart0, reinterpret_cast<const uint8_t*>("\r\n"), 2);
            last = i + 1;
        }
    }
    if (last < len) {
        uart_write_blocking(uart0, reinterpret_cast<const uint8_t*>(buf + last), len - last);
    }
}

static void dual_vprintf(const char* fmt, va_list args) {
    bool scheduler_running = (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED);
    bool in_isr = portCHECK_IF_IN_ISR();
    bool taken = false;
    if (scheduler_running && s_log_mutex != nullptr && !in_isr) {
        taken = (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(500)) == pdTRUE);
    }

    va_list args_copy;
    va_copy(args_copy, args);

    char stack_buf[256];
    int len = std::vsnprintf(stack_buf, sizeof(stack_buf), fmt, args);
    if (len > 0) {
        if (len < static_cast<int>(sizeof(stack_buf))) {
            // 1. Output to hardware UART0 on GP12
            uart_write_crlf(stack_buf, static_cast<size_t>(len));
            // 2. Output to USB CDC ACM when host is mounted
            if (tud_mounted() && (taken || !scheduler_running)) {
                cdc_write_crlf(stack_buf, static_cast<size_t>(len));
            }
        } else {
            std::vector<char> dyn_buf(len + 1);
            std::vsnprintf(dyn_buf.data(), dyn_buf.size(), fmt, args_copy);
            // 1. Output to hardware UART0 on GP12
            uart_write_crlf(dyn_buf.data(), static_cast<size_t>(len));
            // 2. Output to USB CDC ACM when host is mounted
            if (tud_mounted() && (taken || !scheduler_running)) {
                cdc_write_crlf(dyn_buf.data(), static_cast<size_t>(len));
            }
        }
    }
    va_end(args_copy);

    if (taken) {
        xSemaphoreGive(s_log_mutex);
    }
}

void dual_logger_init() {
    if (s_log_mutex == nullptr) {
        s_log_mutex = xSemaphoreCreateMutex();
    }

    // Configure hardware UART0 on GP12 (TX) and GP13 (RX) at 115200 baud 8N1
    uart_init(uart0, 115200);
    gpio_set_function(12, GPIO_FUNC_UART);
    gpio_set_function(13, GPIO_FUNC_UART);
    uart_set_hw_flow(uart0, false, false);
    uart_set_format(uart0, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(uart0, true);
}

void dual_println(const std::string& str) {
    bool scheduler_running = (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED);
    bool in_isr = portCHECK_IF_IN_ISR();
    bool taken = false;
    if (scheduler_running && s_log_mutex != nullptr && !in_isr) {
        taken = (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(500)) == pdTRUE);
    }

    // 1. Output to hardware UART0 on GP12
    uart_write_blocking(uart0, reinterpret_cast<const uint8_t*>(str.c_str()), str.length());
    uart_write_blocking(uart0, reinterpret_cast<const uint8_t*>("\r\n"), 2);

    // 2. Output to USB CDC ACM when host terminal is actively connected
    if (tud_mounted() && tud_cdc_n_connected(0) && (taken || !scheduler_running)) {
        tud_cdc_n_write(0, str.c_str(), static_cast<uint32_t>(str.length()));
        tud_cdc_n_write(0, "\r\n", 2);
        tud_cdc_n_write_flush(0);
    }

    if (taken) {
        xSemaphoreGive(s_log_mutex);
    }
}

void dual_printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    dual_vprintf(fmt, args);
    va_end(args);
}
