// Host-test stub of hardware/sync.h: no interrupts to disable.
#pragma once
#include <stdint.h>
inline uint32_t save_and_disable_interrupts() { return 0; }
inline void restore_interrupts(uint32_t) {}
