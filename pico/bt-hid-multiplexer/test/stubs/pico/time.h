// Host-test stub of pico/time.h with a settable clock.
#pragma once
#include <stdint.h>
typedef uint64_t absolute_time_t;
extern uint32_t g_now_ms;
inline absolute_time_t get_absolute_time() { return g_now_ms; }
inline uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)t; }
