#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// All console output goes through one ring buffer: any task appends to it without ever blocking,
// and only the console task drains it to UART0 and the USB serial port. That keeps a slow or absent
// terminal from stalling the Wi-Fi, Ethernet and HTTP tasks.
//
// The ring lives in RAM that a software, panic or watchdog reset does not clear, so after the board
// has rebooted the end of the previous run's log can still be read (see previous_log()). Whether it
// survives the RST button depends on the SRAM keeping its content; unplugging the board loses it.
class LogRing {
public:
    static constexpr size_t SIZE = 16384;

    // Non-blocking output sinks, implemented by the console.
    struct Sinks {
        size_t (*uart_space)();                       // bytes writable right now
        void (*uart_write)(const uint8_t* data, size_t len);
        bool (*cdc_ready)();                          // USB serial port open on the host
        size_t (*cdc_space)();
        void (*cdc_write)(const uint8_t* data, size_t len);
    };

    // Call first thing in app_main. Keeps the previous run's log if the RAM still holds it.
    static void init(const Sinks* sinks);

    // Appends text; safe from any context including ISRs, never blocks, drops the oldest text when full.
    static void write(const char* data, size_t len);

    // Moves pending text to the sinks without blocking; call from the console task only.
    static void drain();

    // Records proof of life, so that the previous run's report can say how long it ran.
    static void heartbeat(uint32_t uptime_ms);

    // A copy of what the ring currently holds, oldest first.
    static std::string snapshot();

    // The previous run, if its log survived.
    static bool previous_valid();
    static std::string previous_log();
    static uint32_t previous_uptime_ms();
};
