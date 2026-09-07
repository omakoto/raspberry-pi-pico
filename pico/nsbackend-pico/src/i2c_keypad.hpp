/*
 * I2C PCF8574/PCF8574A Matrix Keypad Driver for nsbackend-pico.
 * Scans 4x4 matrix keypads over I2C and translates key events into Switch controller inputs.
 */

#pragma once

#include <cstdint>
#include "FreeRTOS.h"
#include "task.h"
#include "hardware/i2c.h"
#include "controller_state.hpp"

// Keypad Scan Constants
constexpr int KEYPAD_NOKEY = 16;
constexpr int KEYPAD_FAIL = 17;

struct I2cKeypadConfig {
    bool enabled = true;
    bool log_enabled = true;
    uint8_t sda_pin = 20;
    uint8_t scl_pin = 21;
    uint8_t address = 0x20;
    bool reverse_row = true;
    bool reverse_col = true;
    uint32_t debounce_ms = 20;
};

class I2cKeypadManager {
public:
    I2cKeypadManager(ControllerState& controller, const I2cKeypadConfig& config);
    ~I2cKeypadManager();

    bool init();
    void start();

    // Converts a matrix key character to its canonical console command string
    static const char* key_to_command_name(char c);

private:
    static void keypad_task_entry(void* param);
    void run_task();

    bool probe_address(uint8_t addr);
    int scan_matrix();
    char key_to_char(int key) const;
    void map_char_to_controller(char c, uint16_t& buttons, bool& up, bool& down, bool& left, bool& right) const;

    ControllerState& controller_;
    I2cKeypadConfig config_;
    i2c_inst_t* i2c_inst_;
    TaskHandle_t task_handle_;
    bool device_found_;
};
