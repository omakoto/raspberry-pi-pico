#include "status_led.hpp"
#include "esp_log.h"

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
}

bool StatusLed::init() {
    if (pin_ < 0) {
        ESP_LOGI(TAG, "Status LED disabled (pin < 0)");
        return true;
    }

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << pin_);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LED GPIO %d: %s", pin_, esp_err_to_name(err));
        return false;
    }

    set_raw(true); // Default to ON during boot
    running_ = true;

    BaseType_t ret = xTaskCreate(task_entry, "status_led", 2048, this, 2, &task_handle_);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create status LED task");
        return false;
    }

    return true;
}

void StatusLed::set_state(LedState state) {
    current_state_ = state;
}

LedState StatusLed::get_state() const {
    return current_state_.load();
}

void StatusLed::set_raw(bool on) {
    if (pin_ < 0) return;
    int level = active_low_ ? (on ? 0 : 1) : (on ? 1 : 0);
    gpio_set_level(pin_, level);
}

void StatusLed::task_entry(void* arg) {
    auto* self = static_cast<StatusLed*>(arg);
    self->run_task();
}

void StatusLed::run_task() {
    while (running_) {
        LedState state = current_state_.load();
        switch (state) {
            case LedState::INITIALIZING:
                set_raw(true);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;

            case LedState::ETH_LINK_DOWN:
                // Rapid blink (0.1s ON, 0.1s OFF)
                set_raw(true);
                vTaskDelay(pdMS_TO_TICKS(100));
                set_raw(false);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;

            case LedState::ETH_DHCP_WAIT:
                // Link up, waiting for DHCP (0.1s ON, 0.9s OFF)
                set_raw(true);
                vTaskDelay(pdMS_TO_TICKS(100));
                set_raw(false);
                vTaskDelay(pdMS_TO_TICKS(900));
                break;

            case LedState::WAITING_CLIENT:
                // Slow blink (0.5s ON, 0.5s OFF)
                set_raw(true);
                vTaskDelay(pdMS_TO_TICKS(500));
                set_raw(false);
                vTaskDelay(pdMS_TO_TICKS(500));
                break;

            case LedState::CLIENT_CONNECTED:
                // Heartbeat pulse (0.9s ON, 0.1s OFF)
                set_raw(true);
                vTaskDelay(pdMS_TO_TICKS(900));
                set_raw(false);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
        }
    }
}
