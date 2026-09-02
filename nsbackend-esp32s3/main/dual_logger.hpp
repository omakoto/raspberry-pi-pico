#pragma once

#include <string>
#include <cstdarg>
#include "esp_log.h"

// Initializes dual logging hook (routes ESP_LOG* to both UART0 and TinyUSB CDC)
void dual_logger_init();

// Prints a raw string or line to both UART0 and TinyUSB CDC
void dual_println(const std::string& str);
void dual_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
