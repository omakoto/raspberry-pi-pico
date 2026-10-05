// Host-test stub of main/platform.h with a settable clock.
#pragma once
#include <stdint.h>
#define PLATFORM_NOINIT
extern uint32_t g_now_ms;
inline uint32_t platform_now_ms() { return g_now_ms; }
inline void platform_critical_enter() {}
inline void platform_critical_exit() {}
