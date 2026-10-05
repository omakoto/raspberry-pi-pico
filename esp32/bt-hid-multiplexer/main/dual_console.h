#ifndef DUAL_CONSOLE_H_
#define DUAL_CONSOLE_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Longest console input line, including the terminating NUL.
#define CONSOLE_LINE_MAX 128

// Installs the UART driver and routes stdout / ESP_LOG into the log ring. Call first thing in
// app_main, before any task of this firmware is created.
void dual_console_init();
// Starts the console task, which drains the log ring to UART0 and the USB serial port and collects
// input lines from both.
void dual_console_start_task();
// Runs one console command; bt_app task only.
void dual_console_handle_command(const char *cmd);
// USB serial events, called from the TinyUSB task.
void dual_console_cdc_line_coding(uint32_t bit_rate);
void dual_console_cdc_line_state(bool dtr);

void dual_printf(const char *fmt, ...);
void dual_println(const char *str);
void print_welcome_banner();
void reboot_to_download_mode();
// Prints what is known about the previous run (see LogRing); full = also its log text.
void print_previous_run_report(bool full);

#endif // DUAL_CONSOLE_H_
