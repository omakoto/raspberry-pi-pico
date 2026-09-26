#include "logger.h"
#include <Arduino.h>
#include <stdio.h>
#include <stdarg.h>

void logger_init(uint32_t baud) {
    Serial.begin(baud);
    Serial1.begin(baud);
}

void logger_printf(const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (Serial) {
        Serial.print(buf);
    }
    Serial1.print(buf);
}

void logger_println(const char *msg) {
    if (Serial) {
        Serial.println(msg);
    }
    Serial1.println(msg);
}

void logger_println() {
    if (Serial) {
        Serial.println();
    }
    Serial1.println();
}

void logger_print(const char *msg) {
    if (Serial) {
        Serial.print(msg);
    }
    Serial1.print(msg);
}

void logger_flush() {
    if (Serial) {
        Serial.flush();
    }
    Serial1.flush();
}
