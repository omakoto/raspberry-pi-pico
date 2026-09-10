/*
 * Motion bridge implementation (see motion_bridge.hpp).
 */

#include "motion_bridge.hpp"

#include <cstring>
#include "pico/stdlib.h"

namespace {

volatile uint32_t s_seq = 0;          // odd while a write is in progress
uint8_t s_block[MOTION_BLOCK_SIZE];
volatile uint32_t s_written_ms = 0;
volatile bool s_valid = false;

}  // namespace

void motion_bridge_write(const uint8_t* imu_block) {
    s_seq = s_seq + 1;  // odd: readers back off
    __asm volatile("dmb" ::: "memory");
    std::memcpy(s_block, imu_block, MOTION_BLOCK_SIZE);
    s_written_ms = to_ms_since_boot(get_absolute_time());
    s_valid = true;
    __asm volatile("dmb" ::: "memory");
    s_seq = s_seq + 1;  // even: consistent
}

bool motion_bridge_read(uint8_t* out, uint32_t max_age_ms) {
    if (!s_valid) return false;
    uint8_t copy[MOTION_BLOCK_SIZE];
    uint32_t written = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint32_t before = s_seq;
        if (before & 1u) continue;
        __asm volatile("dmb" ::: "memory");
        std::memcpy(copy, s_block, MOTION_BLOCK_SIZE);
        written = s_written_ms;
        __asm volatile("dmb" ::: "memory");
        if (s_seq == before) {
            if (to_ms_since_boot(get_absolute_time()) - written > max_age_ms) return false;
            std::memcpy(out, copy, MOTION_BLOCK_SIZE);
            return true;
        }
    }
    return false;
}
