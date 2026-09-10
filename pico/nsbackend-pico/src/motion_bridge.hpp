/*
 * Motion bridge for nsbackend-pico.
 *
 * Hands the latest IMU block (three accelerometer + gyroscope samples, 36 bytes in the
 * Pro Controller's own report layout) from the USB host task, which reads it off an
 * attached Pro Controller, to the USB device task, which forwards it to the Switch in
 * the emulated Pro Controller's reports. A seqlock keeps the two tasks lock-free: the
 * writer never blocks, and the reader retries until it sees a consistent copy.
 */

#pragma once

#include <cstdint>

constexpr int MOTION_BLOCK_SIZE = 36;

// Stores a new IMU block together with the timer byte of the report it came from
// (called from the USB host task).
void motion_bridge_write(uint8_t report_timer, const uint8_t* imu_block);

// Copies the latest IMU block into out (and its report timer into *timer). Returns false
// when nothing has been written for longer than max_age_ms (or ever), in which case the
// outputs are left untouched.
bool motion_bridge_read(uint8_t* out, uint8_t* timer, uint32_t max_age_ms);

// Number of blocks written so far; a change means a new block is available.
uint32_t motion_bridge_version();

// IMU configuration requested by the console (subcommand 0x41 arguments: gyro range,
// accelerometer range, gyro sample rate, accelerometer filter), to be applied to the
// attached controller so both ends use the same scale. Written by the USB device task,
// read by the USB host task.
constexpr int IMU_CONFIG_SIZE = 4;
void motion_bridge_set_imu_config(const uint8_t* args);
// Returns the config version (0 = never set) and copies the arguments when non-zero.
uint32_t motion_bridge_get_imu_config(uint8_t* args);
