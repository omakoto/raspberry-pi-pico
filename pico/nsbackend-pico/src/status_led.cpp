/*
 * Status LED Controller Implementation for nsbackend-pico.
 * Drives blink patterns for CYW43 Wi-Fi LED or standard Pico GPIO 25.
 */

#include "status_led.hpp"
#include <cmath>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include "dual_logger.hpp"

#if defined(PICO_CYW43_SUPPORTED) && PICO_CYW43_SUPPORTED
#include "pico/cyw43_arch.h"
#endif

static const char* TAG = "StatusLed";

StatusLed::StatusLed(uint32_t pin, bool active_low)
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
#if !(defined(PICO_CYW43_SUPPORTED) && PICO_CYW43_SUPPORTED)
    gpio_init(pin_);
    gpio_set_dir(pin_, GPIO_OUT);
#endif

    set_raw(true);
    running_ = true;

    BaseType_t res = xTaskCreate(task_entry, "status_led_task", 1024, this, tskIDLE_PRIORITY + 1, &task_handle_);
    if (res != pdPASS) {
        LOG_E(TAG, "Failed to create status LED task");
        return false;
    }

    LOG_I(TAG, "Status LED initialized (active_low=%d)", active_low_);
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

#if defined(PICO_CYW43_SUPPORTED) && PICO_CYW43_SUPPORTED
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, level);
#else
    gpio_put(pin_, level);
#endif
}

void StatusLed::task_entry(void* arg) {
    static_cast<StatusLed*>(arg)->run_task();
}

void StatusLed::run_task() {
    LedState last_state = LedState::INITIALIZING;
    int64_t state_start_us = static_cast<int64_t>(time_us_64());

    while (running_) {
        LedState state = current_state_.load(std::memory_order_relaxed);
        if (state != last_state) {
            last_state = state;
            state_start_us = static_cast<int64_t>(time_us_64());
        }

        int64_t now_us = static_cast<int64_t>(time_us_64());
        double elapsed_s = static_cast<double>(now_us - state_start_us) / 1000000.0;

        switch (state) {
            case LedState::INITIALIZING:
                set_raw(true);
                break;

            case LedState::WIFI_CONNECTING: {
                // 0.1s ON, 1.0s OFF (Cycle: 1.1s)
                double cycle = std::fmod(elapsed_s, 1.1);
                set_raw(cycle < 0.1);
                break;
            }

            case LedState::SETTING_UP_TCP: {
                // 0.1s ON, 0.1s OFF, 0.1s ON, 1.0s OFF (Cycle: 1.3s)
                double cycle = std::fmod(elapsed_s, 1.3);
                set_raw(cycle < 0.1 || (cycle >= 0.2 && cycle < 0.3));
                break;
            }

            case LedState::WAITING_CLIENT: {
                // 0.5s ON, 0.5s OFF (Cycle: 1.0s)
                double cycle = std::fmod(elapsed_s, 1.0);
                set_raw(cycle < 0.5);
                break;
            }

            case LedState::CLIENT_CONNECTED: {
                // Heartbeat: 1.0s ON, 1.0s OFF (Cycle: 2.0s)
                double cycle = std::fmod(elapsed_s, 2.0);
                set_raw(cycle < 1.0);
                break;
            }

            case LedState::WIFI_RECONNECTING: {
                // Rapid strobe: 0.1s ON, 0.1s OFF (Cycle: 0.2s)
                double cycle = std::fmod(elapsed_s, 0.2);
                set_raw(cycle < 0.1);
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
