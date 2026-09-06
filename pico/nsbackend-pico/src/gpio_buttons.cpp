/*
 * Physical GPIO Button Manager Implementation for nsbackend-pico.
 * Samples pins at 10ms with 15ms debounce.
 */

#include "gpio_buttons.hpp"
#include <cstdio>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include "dual_logger.hpp"

static const char* TAG = "GpioButtons";
static constexpr int64_t DEBOUNCE_US = 15000; // 15ms debounce

GpioButtonManager::GpioButtonManager(ControllerState& controller)
    : controller_(controller),
      task_handle_(nullptr),
      running_(false) {

    // Raspberry Pi Pico Pin definitions:
    // GP0..GP5 for primary buttons and D-Pad, GP10 for L+R
    buttons_ = {
        {0,  "GP0",  ButtonAction::BUTTON,     "a",     BTN_A,          false, true, 0},
        {1,  "GP1",  ButtonAction::DPAD_DOWN,  "pd",    0,              false, true, 0},
        {2,  "GP2",  ButtonAction::DPAD_LEFT,  "pl",    0,              false, true, 0},
        {3,  "GP3",  ButtonAction::DPAD_RIGHT, "pr",    0,              false, true, 0},
        {4,  "GP4",  ButtonAction::DPAD_UP,    "pu",    0,              false, true, 0},
        {5,  "GP5",  ButtonAction::BUTTON,     "b",     BTN_B,          false, true, 0},
        {10, "GP10", ButtonAction::BUTTON,     "l1 r1", BTN_L | BTN_R, false, true, 0},
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
    int64_t now_us = static_cast<int64_t>(time_us_64());

    for (auto& btn : buttons_) {
        gpio_init(btn.pin);
        gpio_set_dir(btn.pin, GPIO_IN);
        gpio_pull_up(btn.pin);

        bool initial_level = gpio_get(btn.pin);
        btn.last_raw = initial_level;
        btn.is_active = !btn.last_raw;
        btn.last_change_us = now_us;

        LOG_I(TAG, "GPIO button mapped: %s (GP%u) -> %s", btn.name.c_str(), static_cast<unsigned>(btn.pin), btn.cmd_name.c_str());
    }

    apply_to_controller();

    running_ = true;
    BaseType_t res = xTaskCreate(task_entry, "gpio_btn_task", 2048, this, tskIDLE_PRIORITY + 2, &task_handle_);
    if (res != pdPASS) {
        LOG_E(TAG, "Failed to create GPIO button task");
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
    int64_t now_us = static_cast<int64_t>(time_us_64());
    bool state_changed = false;

    for (auto& btn : buttons_) {
        bool raw = gpio_get(btn.pin);
        if (raw != btn.last_raw) {
            btn.last_raw = raw;
            btn.last_change_us = now_us;
        }

        if ((now_us - btn.last_change_us) >= DEBOUNCE_US) {
            bool active = !raw; // Active LOW with pull-up
            if (active != btn.is_active) {
                btn.is_active = active;
                if (btn.is_active) {
                    dual_println(btn.cmd_name);
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
