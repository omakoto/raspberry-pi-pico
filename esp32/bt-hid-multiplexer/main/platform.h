#ifndef PLATFORM_H_
#define PLATFORM_H_

#include <stdint.h>

// Small platform layer used by the code ported from the Pico firmware in place of the Pico SDK, so
// that the ported sources stay close to the originals and the host tests can stub it.

// Milliseconds since boot.
uint32_t platform_now_ms();

// Human readable reason of the last reset, and whether it was a watchdog or panic reset (the cases
// in which the previous run's log is worth showing).
const char *platform_reset_reason_str();
bool platform_reset_was_crash();

#endif // PLATFORM_H_
