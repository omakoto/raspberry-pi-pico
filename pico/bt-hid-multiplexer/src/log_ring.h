#ifndef LOG_RING_H_
#define LOG_RING_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Console output goes through one ring buffer: any context (main loop or the Bluetooth interrupt
// context) appends to it without ever blocking, and only the main loop drains it to the UART and the
// USB serial port. That keeps slow or absent consoles from stalling Bluetooth and USB, and keeps the
// two contexts from writing to the (not thread-safe) USB serial FIFO at the same time.
//
// The ring lives in RAM that a watchdog reset does not clear, so after the watchdog has rebooted a
// hung board the end of the previous run's log and a trail of breadcrumbs (the last Bluetooth/flash
// events and the main loop stage) can still be read, see previousLog().
#define LOG_RING_SIZE 8192
#define LOG_CRUMB_COUNT 16

// Breadcrumb codes: the high nibble says where it came from, the rest is an event code.
#define CRUMB_HCI(event)     (0x1000 | (event))   // BTstack HCI event
#define CRUMB_SM(event)      (0x2000 | (event))   // BTstack security manager event
#define CRUMB_GATT(event)    (0x3000 | (event))   // HID over GATT client event
#define CRUMB_CLASSIC(event) (0x4000 | (event))   // classic HID host event
#define CRUMB_FLASH_KEYMAP_BEGIN   0x5001
#define CRUMB_FLASH_KEYMAP_END     0x5002
#define CRUMB_FLASH_BINDINGS_BEGIN 0x5003
#define CRUMB_FLASH_BINDINGS_END   0x5004
#define CRUMB_PAIRING_START        0x6001
#define CRUMB_PAIRING_STOP         0x6002

class LogRing {
public:
    // Non-blocking output sinks, implemented by the console (and by a fake in the host test).
    struct Sinks {
        uint32_t (*uart_space)();                              // bytes writable right now
        void (*uart_write)(const uint8_t *data, uint32_t len);
        bool (*cdc_ready)();                                   // USB serial open on the host
        uint32_t (*cdc_space)();
        void (*cdc_write)(const uint8_t *data, uint32_t len);
        void (*pump)();                                        // services USB while flush() waits
        uint32_t (*now_ms)();
    };

    // Call first thing in main. was_watchdog_reset: the board was rebooted by the watchdog.
    static void init(const Sinks *sinks, bool was_watchdog_reset);

    // Appends text; safe from any context, never blocks, drops the oldest text when full.
    static void write(const char *data, size_t len);

    // Moves pending text to the sinks without blocking; call from the main loop.
    static void drain();

    // Drains until everything pending has gone out or the timeout passes (e.g. before a reboot).
    static void flush(uint32_t timeout_ms);

    // Like write(), but drains in between so that text longer than the ring is not lost.
    static void writeLong(const char *data, size_t len);

    // Records what the firmware is doing, so that a hang can be located afterwards.
    static void breadcrumb(uint16_t code);
    static void stage(uint16_t stage);        // main loop position
    static void heartbeat(uint32_t uptime_ms); // proof of life; stored with the previous-run data

    // The previous run, if its log survived (a power-on boot leaves nothing).
    static bool previousRunValid();
    static bool previousRunWasWatchdog();
    static const uint8_t *previousLog(uint32_t *len);
    static uint32_t previousUptimeMs();
    static uint16_t previousStage();
    static uint8_t previousBreadcrumbs(uint16_t out[LOG_CRUMB_COUNT]);  // oldest first
};

#endif // LOG_RING_H_
