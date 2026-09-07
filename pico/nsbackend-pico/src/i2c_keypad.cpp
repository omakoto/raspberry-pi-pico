/*
 * I2C PCF8574/PCF8574A Matrix Keypad Implementation for nsbackend-pico.
 * Performs matrix scanning on a PCF8574 expander and maps keys to Nintendo Switch controls.
 */

#include "i2c_keypad.hpp"
#include <cstdio>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "dual_logger.hpp"

static const char* TAG = "I2CKeyPad";
static const char DEFAULT_KEYMAP_4x4[] = "123A456B789C*0#D";

I2cKeypadManager::I2cKeypadManager(ControllerState& controller, const I2cKeypadConfig& config)
    : controller_(controller),
      config_(config),
      i2c_inst_(nullptr),
      task_handle_(nullptr),
      device_found_(false) {}

I2cKeypadManager::~I2cKeypadManager() {
    if (task_handle_ != nullptr) {
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
    }
}

bool I2cKeypadManager::probe_address(uint8_t addr) {
    uint8_t test_byte = 0xF0;
    // Timeout of 5ms prevents blocking if bus is unconnected or pull-ups are missing
    int ret = i2c_write_timeout_us(i2c_inst_, addr, &test_byte, 1, false, 5000);
    return (ret == 1);
}

bool I2cKeypadManager::init() {
    if (!config_.enabled) {
        LOG_I(TAG, "I2C Keypad is disabled by configuration");
        return true;
    }

    // Determine hardware I2C instance based on SDA pin assignment
    i2c_inst_ = ((config_.sda_pin >> 1) & 1) ? i2c1 : i2c0;

    // Standard mode 100 kHz provides reliable communication with PCF8574
    i2c_init(i2c_inst_, 100000);

    gpio_set_function(config_.sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(config_.scl_pin, GPIO_FUNC_I2C);
    gpio_pull_up(config_.sda_pin);
    gpio_pull_up(config_.scl_pin);

    LOG_I(TAG, "Initializing on SDA GP%d, SCL GP%d at 100kHz", config_.sda_pin, config_.scl_pin);

    // Probe configured address first
    if (probe_address(config_.address)) {
        device_found_ = true;
        LOG_I(TAG, "PCF8574 detected at address 0x%02X", config_.address);
    } else if (config_.address == 0x20 && probe_address(0x38)) {
        // Fallback check for PCF8574A address variant (0x38..0x3F)
        config_.address = 0x38;
        device_found_ = true;
        LOG_I(TAG, "PCF8574A detected at alternative address 0x38");
    } else {
        LOG_W(TAG, "PCF8574 not detected at address 0x%02X (will retry in background)", config_.address);
        device_found_ = false;
    }

    return true;
}

void I2cKeypadManager::start() {
    if (!config_.enabled) {
        return;
    }

    BaseType_t res = xTaskCreate(keypad_task_entry, "i2c_keypad", 2048, this, tskIDLE_PRIORITY + 2, &task_handle_);
    if (res != pdPASS) {
        LOG_E(TAG, "Failed to create I2C keypad FreeRTOS task");
    }
}

void I2cKeypadManager::keypad_task_entry(void* param) {
    auto* self = static_cast<I2cKeypadManager*>(param);
    self->run_task();
}

int I2cKeypadManager::scan_matrix() {
    // Step 1: Read columns with rows held LOW (write 0xF0)
    uint8_t write_val = 0xF0;
    int ret = i2c_write_timeout_us(i2c_inst_, config_.address, &write_val, 1, false, 5000);
    if (ret != 1) {
        return KEYPAD_FAIL;
    }

    uint8_t col_byte = 0xFF;
    ret = i2c_read_timeout_us(i2c_inst_, config_.address, &col_byte, 1, false, 5000);
    if (ret != 1) {
        return KEYPAD_FAIL;
    }

    uint8_t col_val = (col_byte >> 4) & 0x0F;
    if (col_val == 0x0F) {
        // No key pressed
        return KEYPAD_NOKEY;
    }

    int col = -1;
    if (col_val == 0x0E) col = 0;
    else if (col_val == 0x0D) col = 1;
    else if (col_val == 0x0B) col = 2;
    else if (col_val == 0x07) col = 3;
    else {
        // Multiple columns low or bus noise
        write_val = 0xF0;
        i2c_write_timeout_us(i2c_inst_, config_.address, &write_val, 1, false, 5000);
        return KEYPAD_FAIL;
    }

    // Step 2: Read rows with columns held LOW (write 0x0F)
    write_val = 0x0F;
    ret = i2c_write_timeout_us(i2c_inst_, config_.address, &write_val, 1, false, 5000);
    if (ret != 1) {
        return KEYPAD_FAIL;
    }

    uint8_t row_byte = 0xFF;
    ret = i2c_read_timeout_us(i2c_inst_, config_.address, &row_byte, 1, false, 5000);
    if (ret != 1) {
        return KEYPAD_FAIL;
    }

    uint8_t row_val = row_byte & 0x0F;
    if (row_val == 0x0F) {
        write_val = 0xF0;
        i2c_write_timeout_us(i2c_inst_, config_.address, &write_val, 1, false, 5000);
        return KEYPAD_NOKEY;
    }

    int row = -1;
    if (row_val == 0x0E) row = 0;
    else if (row_val == 0x0D) row = 1;
    else if (row_val == 0x0B) row = 2;
    else if (row_val == 0x07) row = 3;
    else {
        write_val = 0xF0;
        i2c_write_timeout_us(i2c_inst_, config_.address, &write_val, 1, false, 5000);
        return KEYPAD_FAIL;
    }

    // Step 3: Reset bus back to idle state
    write_val = 0xF0;
    i2c_write_timeout_us(i2c_inst_, config_.address, &write_val, 1, false, 5000);

    // Apply row/column reversal if configured
    if (config_.reverse_col) {
        col = 3 - col;
    }
    if (config_.reverse_row) {
        row = 3 - row;
    }

    return (row * 4) + col;
}

char I2cKeypadManager::key_to_char(int key) const {
    if (key >= 0 && key < 16) {
        return DEFAULT_KEYMAP_4x4[key];
    }
    return '\0';
}

void I2cKeypadManager::map_char_to_controller(
    char c, uint16_t& buttons, bool& up, bool& down, bool& left, bool& right) const {
    buttons = BTN_NONE;
    up = down = left = right = false;

    switch (c) {
        // Directional Pad
        case '2': up = true; break;
        case '4': left = true; break;
        case '6': right = true; break;
        case '8': down = true; break;

        // Bumpers and Triggers
        case '1': buttons |= BTN_L; break;
        case '3': buttons |= BTN_R; break;
        case '7': buttons |= BTN_ZL; break;
        case '9': buttons |= BTN_ZR; break;

        // System Buttons
        case '*': buttons |= BTN_MINUS; break;
        case '#': buttons |= BTN_PLUS; break;
        case '0': buttons |= BTN_HOME; break;

        // Face Buttons
        case 'A': case 'a': buttons |= BTN_A; break;
        case 'B': case 'b': buttons |= BTN_B; break;
        case 'C': case 'c': buttons |= BTN_X; break;
        case 'D': case 'd': buttons |= BTN_Y; break;

        // Key '5' is unassigned
        case '5':
        default:
            break;
    }
}

void I2cKeypadManager::run_task() {
    int stable_key = KEYPAD_NOKEY;
    int candidate_key = KEYPAD_NOKEY;
    uint32_t candidate_count = 0;
    constexpr uint32_t REQUIRED_CONSECUTIVE = 2; // 2 consecutive consistent reads for debounce

    while (true) {
        // If device was not detected at boot, periodically attempt reconnection
        if (!device_found_) {
            if (probe_address(config_.address) || probe_address(0x38)) {
                device_found_ = true;
                LOG_I(TAG, "PCF8574 connected and online at address 0x%02X", config_.address);
            } else {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        int raw_key = scan_matrix();

        if (raw_key == KEYPAD_FAIL) {
            // Bus communication issue or conflicting multi-touch
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (raw_key == candidate_key) {
            if (candidate_count < REQUIRED_CONSECUTIVE) {
                candidate_count++;
            }
        } else {
            candidate_key = raw_key;
            candidate_count = 1;
        }

        if (candidate_count >= REQUIRED_CONSECUTIVE && candidate_key != stable_key) {
            stable_key = candidate_key;

            uint16_t buttons = BTN_NONE;
            bool up = false;
            bool down = false;
            bool left = false;
            bool right = false;

            if (stable_key != KEYPAD_NOKEY) {
                char ch = key_to_char(stable_key);
                map_char_to_controller(ch, buttons, up, down, left, right);
                LOG_D(TAG, "Keypad event: key index %d ('%c')", stable_key, ch);
            }

            controller_.set_keypad_state(buttons, up, down, left, right);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
