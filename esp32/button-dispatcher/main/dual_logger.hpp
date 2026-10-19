#pragma once

#include <string>

// Initializes the dual-output logging system to dispatch ESP_LOG* messages to both UART0 and TinyUSB CDC ACM.
void dual_logger_init();

// Formatted print helper that outputs to both UART0 and TinyUSB CDC ACM.
void dual_printf(const char* fmt, ...);

// String print helper that outputs to both UART0 and TinyUSB CDC ACM with a newline.
void dual_println(const std::string& str);
