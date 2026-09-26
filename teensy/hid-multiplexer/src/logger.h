#ifndef LOGGER_H_
#define LOGGER_H_

#include <stdint.h>
#include <stddef.h>

void logger_init(uint32_t baud = 115200);
void logger_printf(const char *fmt, ...);
void logger_println(const char *msg);
void logger_println();
void logger_print(const char *msg);
void logger_flush();

#endif // LOGGER_H_
