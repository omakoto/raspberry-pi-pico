#include "gpio_buttons.hpp"
#include <cstdio>
#include "esp_log.h"
#include "esp_timer.h"

static const char* TAG = "GpioButtons";
static constexpr int64_t DEBOUNCE_US = 15000; // 15ms debounce

GpioButtonManager::GpioButtonManager(ControllerState& controller)
    : controller_(controller),
      task_handle_(nullptr),
      running_(false) {

    // XIAO ESP32-S3 Pin definitions matching nsbackend-pico
    buttons_ = {
        {GPIO_NUM_1,  "D0", ButtonAction::BUTTON,     "a",     BTN_A,             false, true, 0},
        {GPIO_NUM_2,  "D1", ButtonAction::DPAD_DOWN,  "pd",    0,                 false, true, 0},
        {GPIO_NUM_3,  "D2", ButtonAction::DPAD_LEFT,  "pl",    0,                 false, true, 0},
        {GPIO_NUM_4,  "D3", ButtonAction::DPAD_RIGHT, "pr",    0,                 false, true, 0},
        {GPIO_NUM_5,  "D4", ButtonAction::DPAD_UP,    "pu",    0,                 false, true, 0},
        {GPIO_NUM_6,  "D5",  ButtonAction::BUTTON,     "b",     BTN_B,             false, true, 0},
        {GPIO_NUM_9,  "D10", ButtonAction::BUTTON,     "l1 r1", BTN_L | BTN_R,    false, true, 0},
    };
}

GpioButtonManager::~GpioButtonManager() {
    running_ = false;
    if (task_handle_ != nullptr) {
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
    }
}

bool GpioButtonManager::init() {
    for (auto& btn : buttons_) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << btn.pin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };

        esp_err_t ret = gpio_config(&io_conf);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to configure GPIO %d (%s): %s", btn.pin, btn.name.c_str(), esp_err_to_name(ret));
            continue;
        }

        int initial_level = gpio_get_level(btn.pin);
        btn.last_raw = (initial_level != 0);
        btn.is_active = !btn.last_raw;
        btn.last_change_us = esp_timer_get_time();

        ESP_LOGI(TAG, "GPIO button mapped: %s (GPIO %d) -> %s", btn.name.c_str(), btn.pin, btn.cmd_name.c_str());
    }

    apply_to_controller();

    running_ = true;
    BaseType_t res = xTaskCreate(task_entry, "gpio_btn_task", 3072, this, tskIDLE_PRIORITY + 2, &task_handle_);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create GPIO button task");
        return false;
    }

    return true;
}

void GpioButtonManager::task_entry(void* arg) {
    static_cast<GpioButtonManager*>(arg)->run_task();
}

void GpioButtonManager::run_task() {
    while (running_) {
        update_inputs();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void GpioButtonManager::update_inputs() {
    int64_t now_us = esp_timer_get_time();
    bool state_changed = false;

    for (auto& btn : buttons_) {
        bool raw = (gpio_get_level(btn.pin) != 0);
        if (raw != btn.last_raw) {
            btn.last_raw = raw;
            btn.last_change_us = now_us;
        }

        if ((now_us - btn.last_change_us) >= DEBOUNCE_US) {
            bool active = !raw; // Active LOW
            if (active != btn.is_active) {
                btn.is_active = active;
                if (btn.is_active) {
                    std::printf("%s\n", btn.cmd_name.c_str());
                    std::fflush(stdout);
                }
                state_changed = true;
            }
        }
    }

    if (state_changed) {
        apply_to_controller();
    }
}

void GpioButtonManager::apply_to_controller() {
    uint16_t gpio_btn_mask = BTN_NONE;
    bool dpad_up = false;
    bool dpad_down = false;
    bool dpad_left = false;
    bool dpad_right = false;

    for (const auto& btn : buttons_) {
        if (btn.is_active) {
            switch (btn.action) {
                case ButtonAction::BUTTON:
                    gpio_btn_mask |= btn.mask;
                    break;
                case ButtonAction::DPAD_UP:
                    dpad_up = true;
                    break;
                case ButtonAction::DPAD_DOWN:
                    dpad_down = true;
                    break;
                case ButtonAction::DPAD_LEFT:
                    dpad_left = true;
                    break;
                case ButtonAction::DPAD_RIGHT:
                    dpad_right = true;
                    break;
            }
        }
    }

    controller_.set_gpio_state(gpio_btn_mask, dpad_up, dpad_down, dpad_left, dpad_right);
}
