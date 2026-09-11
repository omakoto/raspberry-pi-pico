/*
 * I2C PCF8574/PCF8574A Matrix Keypad Implementation for nsbackend-esp32s3.
 * Performs matrix scanning on a PCF8574 expander and maps keys to Nintendo Switch controls.
 */

#include "i2c_keypad.hpp"
#include <cstdio>
#include "esp_log.h"
#include "dual_logger.hpp"

static const char* TAG = "I2CKeyPad";
static const char DEFAULT_KEYMAP_4x4[] = "123A456B789C*0#D";

I2cKeypadManager::I2cKeypadManager(ControllerState& controller, const I2cKeypadConfig& config)
    : controller_(controller),
      config_(config),
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
    esp_err_t ret = i2c_master_write_to_device(config_.port, addr, &test_byte, 1, pdMS_TO_TICKS(50));
    return (ret == ESP_OK);
}

bool I2cKeypadManager::init() {
    if (!config_.enabled) {
        ESP_LOGI(TAG, "I2C Keypad is disabled by configuration");
        return true;
    }

    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = config_.sda_pin;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_io_num = config_.scl_pin;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = 100000;
    conf.clk_flags = 0;

    esp_err_t err = i2c_param_config(config_.port, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config failed: %s", esp_err_to_name(err));
        return false;
    }

    err = i2c_driver_install(config_.port, conf.mode, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Initializing on SDA GPIO%d, SCL GPIO%d at 100kHz", config_.sda_pin, config_.scl_pin);

    // Probe configured address first
    if (probe_address(config_.address)) {
        device_found_ = true;
        ESP_LOGI(TAG, "PCF8574 detected at address 0x%02X", config_.address);
    } else if (config_.address == 0x20 && probe_address(0x38)) {
        // Fallback check for PCF8574A address variant (0x38..0x3F)
        config_.address = 0x38;
        device_found_ = true;
        ESP_LOGI(TAG, "PCF8574A detected at alternative address 0x38");
    } else {
        ESP_LOGW(TAG, "PCF8574 not detected at address 0x%02X (will retry in background)", config_.address);
        device_found_ = false;
    }

    return true;
}

void I2cKeypadManager::start() {
    if (!config_.enabled) {
        return;
    }

    BaseType_t res = xTaskCreate(keypad_task_entry, "i2c_keypad", 4096, this, tskIDLE_PRIORITY + 2, &task_handle_);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create I2C keypad FreeRTOS task");
    }
}

void I2cKeypadManager::keypad_task_entry(void* param) {
    auto* self = static_cast<I2cKeypadManager*>(param);
    self->run_task();
}

int I2cKeypadManager::scan_matrix() {
    // Step 1: Read columns with rows held LOW (write 0xF0)
    uint8_t write_val = 0xF0;
    esp_err_t ret = i2c_master_write_to_device(config_.port, config_.address, &write_val, 1, pdMS_TO_TICKS(20));
    if (ret != ESP_OK) {
        return KEYPAD_FAIL;
    }

    uint8_t col_byte = 0xFF;
    ret = i2c_master_read_from_device(config_.port, config_.address, &col_byte, 1, pdMS_TO_TICKS(20));
    if (ret != ESP_OK) {
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
        i2c_master_write_to_device(config_.port, config_.address, &write_val, 1, pdMS_TO_TICKS(20));
        return KEYPAD_FAIL;
    }

    // Step 2: Read rows with columns held LOW (write 0x0F)
    write_val = 0x0F;
    ret = i2c_master_write_to_device(config_.port, config_.address, &write_val, 1, pdMS_TO_TICKS(20));
    if (ret != ESP_OK) {
        return KEYPAD_FAIL;
    }

    uint8_t row_byte = 0xFF;
    ret = i2c_master_read_from_device(config_.port, config_.address, &row_byte, 1, pdMS_TO_TICKS(20));
    if (ret != ESP_OK) {
        return KEYPAD_FAIL;
    }

    uint8_t row_val = row_byte & 0x0F;
    if (row_val == 0x0F) {
        write_val = 0xF0;
        i2c_master_write_to_device(config_.port, config_.address, &write_val, 1, pdMS_TO_TICKS(20));
        return KEYPAD_NOKEY;
    }

    int row = -1;
    if (row_val == 0x0E) row = 0;
    else if (row_val == 0x0D) row = 1;
    else if (row_val == 0x0B) row = 2;
    else if (row_val == 0x07) row = 3;
    else {
        write_val = 0xF0;
        i2c_master_write_to_device(config_.port, config_.address, &write_val, 1, pdMS_TO_TICKS(20));
        return KEYPAD_FAIL;
    }

    // Step 3: Reset bus back to idle state
    write_val = 0xF0;
    i2c_master_write_to_device(config_.port, config_.address, &write_val, 1, pdMS_TO_TICKS(20));

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

const char* I2cKeypadManager::key_to_command_name(char c) {
    switch (c) {
        // Directional Pad
        case '2': return "pu [Up]";
        case '4': return "pl [Left]";
        case '6': return "pr [Right]";
        case '8': return "pd [Down]";

        // Bumpers and Triggers
        case '1': return "l1 [L]";
        case '3': return "r1 [R]";
        case '7': return "l2 [ZL]";
        case '9': return "r2 [ZR]";

        // System Buttons
        case '*': return "m [Minus]";
        case '#': return "p [Plus]";
        case '0': return "h [Home]";

        // Face Buttons
        case 'A': case 'a': return "a [A]";
        case 'B': case 'b': return "b [B]";
        case 'C': case 'c': return "x [X]";
        case 'D': case 'd': return "y [Y]";

        // Key '5' is unassigned
        case '5':
        default:
            return nullptr;
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
                ESP_LOGI(TAG, "PCF8574 connected and online at address 0x%02X", config_.address);
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
                ESP_LOGD(TAG, "Keypad event: key index %d ('%c')", stable_key, ch);

                if (config_.log_enabled) {
                    const char* cmd_name = key_to_command_name(ch);
                    if (cmd_name != nullptr) {
                        dual_println(cmd_name);
                    }
                }
            }

            controller_.set_keypad_state(buttons, up, down, left, right);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
