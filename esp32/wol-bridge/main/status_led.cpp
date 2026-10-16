#include "status_led.hpp"
#include "esp_timer.h"
#include "esp_log.h"

static const char* TAG = "StatusLed";

StatusLed::StatusLed(gpio_num_t pin, bool active_low)
    : pin_(pin),
      active_low_(active_low),
      current_state_(LedState::INITIALIZING),
      pulse_until_ms_(0),
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
    if (pin_ < 0) {
        return false;
    }

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << pin_);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure Status LED GPIO %d: %s", pin_, esp_err_to_name(err));
        return false;
    }

    set_raw(false);
    running_ = true;

    BaseType_t ret = xTaskCreate(task_entry, "status_led", 2048, this, 2, &task_handle_);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn Status LED task");
        return false;
    }

    return true;
}

void StatusLed::set_state(LedState state) {
    current_state_.store(state, std::memory_order_relaxed);
}

LedState StatusLed::get_state() const {
    return current_state_.load(std::memory_order_relaxed);
}

void StatusLed::pulse(uint32_t duration_ms) {
    uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    pulse_until_ms_.store(now_ms + duration_ms, std::memory_order_relaxed);
}

void StatusLed::set_raw(bool on) {
    if (pin_ < 0) return;
    gpio_set_level(pin_, (active_low_ ? !on : on) ? 1 : 0);
}

void StatusLed::task_entry(void* arg) {
    static_cast<StatusLed*>(arg)->run_task();
}

void StatusLed::run_task() {
    uint32_t step = 0;
    while (running_) {
        uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        uint32_t pulse_until = pulse_until_ms_.load(std::memory_order_relaxed);

        if (now_ms < pulse_until) {
            set_raw(true);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        LedState state = current_state_.load(std::memory_order_relaxed);
        switch (state) {
            case LedState::INITIALIZING:
                set_raw(true);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;

            case LedState::WIFI_CONNECTING:
                // Fast blink 0.2s ON / 0.2s OFF
                set_raw((step % 2) == 0);
                vTaskDelay(pdMS_TO_TICKS(200));
                step++;
                break;

            case LedState::WIFI_RECONNECTING:
                // Slower pulse 0.5s ON / 0.5s OFF
                set_raw((step % 2) == 0);
                vTaskDelay(pdMS_TO_TICKS(500));
                step++;
                break;

            case LedState::READY:
                // Gentle heartbeat pulse: 50ms ON every 2 seconds (40 steps of 50ms)
                set_raw((step % 40) == 0);
                vTaskDelay(pdMS_TO_TICKS(50));
                step++;
                break;

            case LedState::WOL_FIRING:
                // Rapid strobe 50ms ON / 50ms OFF
                set_raw((step % 2) == 0);
                vTaskDelay(pdMS_TO_TICKS(50));
                step++;
                break;
        }
    }
}
