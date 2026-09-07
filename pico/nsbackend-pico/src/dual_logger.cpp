/*
 * Dual Logger Implementation for nsbackend-pico.
 * Directs log messages to stdout (UART0) and USB CDC ACM serial console.
 */

#include "dual_logger.hpp"
#include <vector>
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "tusb.h"

static SemaphoreHandle_t s_log_mutex = nullptr;

// Translates newline characters to CRLF line endings for USB CDC ACM output
static void cdc_write_crlf(const char* buf, size_t len) {
    if (!tud_mounted() || len == 0) {
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

static void dual_vprintf(const char* fmt, va_list args) {
    bool scheduler_running = (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED);
    bool in_isr = portCHECK_IF_IN_ISR();
    bool taken = false;
    if (scheduler_running && s_log_mutex != nullptr && !in_isr) {
        taken = (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(500)) == pdTRUE);
    }

    va_list args_copy;
    va_copy(args_copy, args);

    // 1. Output to stdout (UART0)
    std::vprintf(fmt, args);
    std::fflush(stdout);

    // 2. Output to USB CDC ACM when host is mounted and lock is held
    if (tud_mounted() && (taken || !scheduler_running)) {
        char stack_buf[256];
        int len = std::vsnprintf(stack_buf, sizeof(stack_buf), fmt, args_copy);
        if (len > 0) {
            if (len < static_cast<int>(sizeof(stack_buf))) {
                cdc_write_crlf(stack_buf, static_cast<size_t>(len));
            } else {
                std::vector<char> dyn_buf(len + 1);
                va_list args_dyn;
                va_copy(args_dyn, args_copy);
                std::vsnprintf(dyn_buf.data(), dyn_buf.size(), fmt, args_dyn);
                va_end(args_dyn);
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
}

void dual_println(const std::string& str) {
    bool scheduler_running = (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED);
    bool in_isr = portCHECK_IF_IN_ISR();
    bool taken = false;
    if (scheduler_running && s_log_mutex != nullptr && !in_isr) {
        taken = (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(500)) == pdTRUE);
    }

    std::printf("%s\n", str.c_str());
    std::fflush(stdout);

    if (tud_mounted() && (taken || !scheduler_running)) {
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
