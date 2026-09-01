#include "status_led.hpp"
#include <cmath>
#include "esp_log.h"
#include "esp_timer.h"

static const char* TAG = "StatusLed";

StatusLed::StatusLed(gpio_num_t pin, bool active_low)
    : pin_(pin),
      active_low_(active_low),
      current_state_(LedState::INITIALIZING),
      task_handle_(nullptr),
      running_(false) {}

StatusLed::~StatusLed() {
    running_ = false;
    if (task_handle_ != nullptr) {
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
    }
    set_raw(false);
}

bool StatusLed::init() {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin_),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LED GPIO %d: %s", pin_, esp_err_to_name(ret));
        return false;
    }

    set_raw(true);
    running_ = true;

    BaseType_t res = xTaskCreate(task_entry, "status_led_task", 2048, this, tskIDLE_PRIORITY + 1, &task_handle_);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create status LED task");
        return false;
    }

    ESP_LOGI(TAG, "Status LED initialized on GPIO %d (active_low=%d)", pin_, active_low_);
    return true;
}

void StatusLed::set_state(LedState state) {
    current_state_.store(state, std::memory_order_relaxed);
}

LedState StatusLed::get_state() const {
    return current_state_.load(std::memory_order_relaxed);
}

void StatusLed::set_raw(bool on) {
    int level = active_low_ ? (!on ? 1 : 0) : (on ? 1 : 0);
    gpio_set_level(pin_, level);
}

void StatusLed::task_entry(void* arg) {
    static_cast<StatusLed*>(arg)->run_task();
}

void StatusLed::run_task() {
    LedState last_state = LedState::INITIALIZING;
    int64_t state_start_us = esp_timer_get_time();

    while (running_) {
        LedState state = current_state_.load(std::memory_order_relaxed);
        if (state != last_state) {
            last_state = state;
            state_start_us = esp_timer_get_time();
        }

        int64_t now_us = esp_timer_get_time();
        double elapsed_s = static_cast<double>(now_us - state_start_us) / 1000000.0;

        switch (state) {
            case LedState::INITIALIZING:
                set_raw(true);
                break;

            case LedState::WAITING_CLIENT: {
                // 3 blinks (0.2s ON, 0.2s OFF, 0.2s ON, 0.2s OFF, 0.2s ON) - 0.5s pause (Cycle: 1.5s)
                double cycle = std::fmod(elapsed_s, 1.5);
                if (cycle < 0.2) {
                    set_raw(true);
                } else if (cycle < 0.4) {
                    set_raw(false);
                } else if (cycle < 0.6) {
                    set_raw(true);
                } else if (cycle < 0.8) {
                    set_raw(false);
                } else if (cycle < 1.0) {
                    set_raw(true);
                } else {
                    set_raw(false);
                }
                break;
            }

            case LedState::CLIENT_CONNECTED: {
                // Heartbeat: 1.0s ON, 1.0s OFF (Cycle: 2.0s)
                double cycle = std::fmod(elapsed_s, 2.0);
                set_raw(cycle < 1.0);
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
