#include "dual_logger.hpp"
#include <cstdio>
#include <cstdarg>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "tinyusb_cdc_acm.h"
#include "tusb.h"

static SemaphoreHandle_t s_log_mutex = nullptr;

static int dual_vprintf(const char* fmt, va_list args) {
    bool in_isr = xPortInIsrContext();
    if (!in_isr && s_log_mutex != nullptr) {
        xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    }

    va_list args_copy;
    va_copy(args_copy, args);

    // 1. Output to UART0 (stdout)
    int ret = vprintf(fmt, args);
    std::fflush(stdout);

    // 2. Output to USB CDC ACM if initialized and USB host is mounted
    if (tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0) && tud_mounted()) {
        char stack_buf[256];
        int len = vsnprintf(stack_buf, sizeof(stack_buf), fmt, args_copy);
        if (len > 0) {
            if (len < static_cast<int>(sizeof(stack_buf))) {
                tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, reinterpret_cast<const uint8_t*>(stack_buf), len);
            } else {
                std::vector<char> dyn_buf(len + 1);
                va_list args_dyn;
                va_copy(args_dyn, args_copy);
                vsnprintf(dyn_buf.data(), dyn_buf.size(), fmt, args_dyn);
                va_end(args_dyn);
                tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, reinterpret_cast<const uint8_t*>(dyn_buf.data()), len);
            }
            tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
        }
    }
    va_end(args_copy);

    if (!in_isr && s_log_mutex != nullptr) {
        xSemaphoreGive(s_log_mutex);
    }

    return ret;
}

void dual_logger_init() {
    if (s_log_mutex == nullptr) {
        s_log_mutex = xSemaphoreCreateMutex();
    }
    esp_log_set_vprintf(dual_vprintf);
}

void dual_println(const std::string& str) {
    bool in_isr = xPortInIsrContext();
    if (!in_isr && s_log_mutex != nullptr) {
        xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    }

    // 1. Output to UART0
    std::printf("%s\n", str.c_str());
    std::fflush(stdout);

    // 2. Output to USB CDC ACM
    if (tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0) && tud_mounted()) {
        tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, reinterpret_cast<const uint8_t*>(str.c_str()), str.length());
        tinyusb_cdcacm_write_queue_char(TINYUSB_CDC_ACM_0, '\n');
        tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    }

    if (!in_isr && s_log_mutex != nullptr) {
        xSemaphoreGive(s_log_mutex);
    }
}

void dual_printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    dual_vprintf(fmt, args);
    va_end(args);
}
