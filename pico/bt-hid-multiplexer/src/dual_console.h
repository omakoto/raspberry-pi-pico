#ifndef DUAL_CONSOLE_H_
#define DUAL_CONSOLE_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

void dual_console_init();
void dual_console_update();
void dual_printf(const char *fmt, ...);
void dual_println(const char *str);
void print_welcome_banner();
void reboot_to_bootsel();

#endif // DUAL_CONSOLE_H_
