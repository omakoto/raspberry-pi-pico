#ifndef PLATFORM_H_
#define PLATFORM_H_

#include <stdint.h>
#include "esp_attr.h"

// Small platform layer used by the code ported from the Pico firmware in place of the Pico SDK, so
// that the ported sources stay close to the originals and the host tests can stub it. It lives in a
// directory of its own so that test/stubs/platform.h can take its place in the host tests (a quoted
// #include looks in the including file's directory first).

// Placement for data that must survive a software, panic or watchdog reset (not cleared at startup).
#define PLATFORM_NOINIT __NOINIT_ATTR

// Short critical section usable from any task on either core (a spinlock). Keep it to a few memcpys.
void platform_critical_enter();
void platform_critical_exit();

// Reboots into the ROM download mode, in which esptool can flash the board over the native USB port
// (USB-Serial-JTAG) or UART0.
void platform_reboot_to_download_mode();

// Milliseconds since boot.
uint32_t platform_now_ms();

// Human readable reason of the last reset, and whether it was a watchdog or panic reset (the cases
// in which the previous run's log is worth showing).
const char *platform_reset_reason_str();
bool platform_reset_was_crash();

#endif // PLATFORM_H_
