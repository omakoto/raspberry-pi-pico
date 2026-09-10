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

// Stores a new IMU block (called from the USB host task).
void motion_bridge_write(const uint8_t* imu_block);

// Copies the latest IMU block into out. Returns false when nothing has been written for
// longer than max_age_ms (or ever), in which case out is left untouched.
bool motion_bridge_read(uint8_t* out, uint32_t max_age_ms);
