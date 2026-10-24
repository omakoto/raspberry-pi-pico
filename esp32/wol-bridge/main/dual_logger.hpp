#pragma once

#include <string>

// Console on UART0 (TX GPIO43 / RX GPIO44, 115200 8N1) and the TinyUSB CDC ACM serial port.
// All stdout and ESP_LOG* output goes into a log ring (see log_ring.hpp) that survives reboots, and
// both ports accept the commands listed by "help".

// Installs the UART driver and routes stdout / ESP_LOG* into the log ring. Call first thing in
// app_main, before any task of this firmware is created.
void dual_logger_init();

// Starts the console task, which drains the log ring to both ports and runs typed commands. Call
// once TinyUSB is installed.
void dual_logger_start_console();

// Called by the "status" command; prints a one-line health summary.
void dual_logger_set_status_callback(void (*callback)());

// Why this run started and what is known about the previous run, as printed at boot.
std::string dual_logger_boot_report();

// Formatted print helper that outputs to both ports.
void dual_printf(const char* fmt, ...);

// String print helper that outputs to both ports with a newline.
void dual_println(const std::string& str);
