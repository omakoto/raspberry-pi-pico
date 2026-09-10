/*
 * Motion bridge implementation (see motion_bridge.hpp).
 */

#include "motion_bridge.hpp"

#include <cstring>
#include "pico/stdlib.h"

namespace {

volatile uint32_t s_seq = 0;          // odd while a write is in progress
uint8_t s_block[MOTION_BLOCK_SIZE];
volatile uint8_t s_timer = 0;
volatile uint32_t s_written_ms = 0;
volatile uint32_t s_version = 0;

volatile uint32_t s_cfg_seq = 0;
uint8_t s_cfg[IMU_CONFIG_SIZE];
volatile uint32_t s_cfg_version = 0;

}  // namespace

void motion_bridge_write(uint8_t report_timer, const uint8_t* imu_block) {
    s_seq = s_seq + 1;  // odd: readers back off
    __asm volatile("dmb" ::: "memory");
    std::memcpy(s_block, imu_block, MOTION_BLOCK_SIZE);
    s_timer = report_timer;
    s_written_ms = to_ms_since_boot(get_absolute_time());
    s_version = s_version + 1;
    __asm volatile("dmb" ::: "memory");
    s_seq = s_seq + 1;  // even: consistent
}

bool motion_bridge_read(uint8_t* out, uint8_t* timer, uint32_t max_age_ms) {
    if (s_version == 0) return false;
    uint8_t copy[MOTION_BLOCK_SIZE];
    uint8_t t = 0;
    uint32_t written = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint32_t before = s_seq;
        if (before & 1u) continue;
        __asm volatile("dmb" ::: "memory");
        std::memcpy(copy, s_block, MOTION_BLOCK_SIZE);
        t = s_timer;
        written = s_written_ms;
        __asm volatile("dmb" ::: "memory");
        if (s_seq == before) {
            if (to_ms_since_boot(get_absolute_time()) - written > max_age_ms) return false;
            std::memcpy(out, copy, MOTION_BLOCK_SIZE);
            *timer = t;
            return true;
        }
    }
    return false;
}

volatile uint8_t s_mode = 0;
volatile uint32_t s_mode_version = 0;

void motion_bridge_set_imu_mode(uint8_t mode) {
    s_mode = mode;
    __asm volatile("dmb" ::: "memory");
    s_mode_version = s_mode_version + 1;
}

uint32_t motion_bridge_get_imu_mode(uint8_t* mode) {
    uint32_t version = s_mode_version;
    __asm volatile("dmb" ::: "memory");
    if (version != 0) *mode = s_mode;
    return version;
}

uint32_t motion_bridge_version() {
    return s_version;
}

void motion_bridge_set_imu_config(const uint8_t* args) {
    s_cfg_seq = s_cfg_seq + 1;
    __asm volatile("dmb" ::: "memory");
    std::memcpy(s_cfg, args, IMU_CONFIG_SIZE);
    s_cfg_version = s_cfg_version + 1;
    __asm volatile("dmb" ::: "memory");
    s_cfg_seq = s_cfg_seq + 1;
}

uint32_t motion_bridge_get_imu_config(uint8_t* args) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint32_t before = s_cfg_seq;
        if (before & 1u) continue;
        __asm volatile("dmb" ::: "memory");
        uint8_t copy[IMU_CONFIG_SIZE];
        std::memcpy(copy, s_cfg, IMU_CONFIG_SIZE);
        uint32_t version = s_cfg_version;
        __asm volatile("dmb" ::: "memory");
        if (s_cfg_seq == before) {
            if (version != 0) std::memcpy(args, copy, IMU_CONFIG_SIZE);
            return version;
        }
    }
    return 0;
}
