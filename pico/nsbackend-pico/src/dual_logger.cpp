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

static void dual_vprintf(const char* fmt, va_list args) {
    bool scheduler_running = (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED);
    if (scheduler_running && s_log_mutex != nullptr) {
        xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    }

    va_list args_copy;
    va_copy(args_copy, args);

    // 1. Output to stdout (UART0)
    std::vprintf(fmt, args);
    std::fflush(stdout);

    // 2. Output to USB CDC ACM if host is mounted and connected
    if (tud_mounted() && tud_cdc_n_connected(0)) {
        char stack_buf[256];
        int len = std::vsnprintf(stack_buf, sizeof(stack_buf), fmt, args_copy);
        if (len > 0) {
            if (len < static_cast<int>(sizeof(stack_buf))) {
                tud_cdc_n_write(0, stack_buf, static_cast<uint32_t>(len));
            } else {
                std::vector<char> dyn_buf(len + 1);
                va_list args_dyn;
                va_copy(args_dyn, args_copy);
                std::vsnprintf(dyn_buf.data(), dyn_buf.size(), fmt, args_dyn);
                va_end(args_dyn);
                tud_cdc_n_write(0, dyn_buf.data(), static_cast<uint32_t>(len));
            }
            tud_cdc_n_write_flush(0);
        }
    }
    va_end(args_copy);

    if (scheduler_running && s_log_mutex != nullptr) {
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
    if (scheduler_running && s_log_mutex != nullptr) {
        xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    }

    std::printf("%s\n", str.c_str());
    std::fflush(stdout);

    if (tud_mounted() && tud_cdc_n_connected(0)) {
        tud_cdc_n_write(0, str.c_str(), static_cast<uint32_t>(str.length()));
        tud_cdc_n_write_char(0, '\n');
        tud_cdc_n_write_flush(0);
    }

    if (scheduler_running && s_log_mutex != nullptr) {
        xSemaphoreGive(s_log_mutex);
    }
}

void dual_printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    dual_vprintf(fmt, args);
    va_end(args);
}
