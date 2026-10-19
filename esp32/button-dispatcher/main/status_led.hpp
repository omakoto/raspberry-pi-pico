#pragma once

#include <atomic>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum class LedState : int {
    INITIALIZING = 1,        // Solid ON during boot
    WIFI_CONNECTING = 2,     // Fast blink (0.2s ON, 0.2s OFF)
    WIFI_RECONNECTING = 3,   // Double blink
    READY = 4,               // Idle heartbeat (0.05s ON, 1.95s OFF)
    PRESS = 5,          // Rapid strobe, a press was heard (0.05s ON, 0.05s OFF)
};

class StatusLed {
public:
    explicit StatusLed(gpio_num_t pin = GPIO_NUM_21, bool active_low = true);
    ~StatusLed();

    bool init();
    void set_state(LedState state);
    LedState get_state() const;
    void pulse(uint32_t duration_ms = 100);

private:
    static void task_entry(void* arg);
    void run_task();
    void set_raw(bool on);

    gpio_num_t pin_;
    bool active_low_;
    std::atomic<LedState> current_state_;
    std::atomic<uint32_t> pulse_until_ms_;
    TaskHandle_t task_handle_;
    std::atomic<bool> running_;
};
