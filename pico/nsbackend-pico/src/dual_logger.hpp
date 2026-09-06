/*
 * Dual Logger for nsbackend-pico.
 * Outputs formatted logs and command stream to both hardware UART0 and TinyUSB CDC console.
 */

#pragma once

#include <string>
#include <cstdarg>
#include <cstdio>

void dual_logger_init();
void dual_println(const std::string& str);
void dual_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#define LOG_I(tag, fmt, ...) dual_printf("[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define LOG_W(tag, fmt, ...) dual_printf("[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define LOG_E(tag, fmt, ...) dual_printf("[E][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define LOG_D(tag, fmt, ...) dual_printf("[D][%s] " fmt "\n", tag, ##__VA_ARGS__)
