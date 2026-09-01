#pragma once

#include <atomic>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum class LedState : int {
    INITIALIZING = 1,     // Startup: Constantly ON
    WAITING_CLIENT = 2,   // 3 blinks (0.2s on/off x 3, 0.5s pause)
    CLIENT_CONNECTED = 3  // Heartbeat (1.0s on, 1.0s off)
};

class StatusLed {
public:
    StatusLed(gpio_num_t pin = GPIO_NUM_21, bool active_low = true);
    ~StatusLed();

    bool init();
    void set_state(LedState state);
    LedState get_state() const;

private:
    static void task_entry(void* arg);
    void run_task();
    void set_raw(bool on);

    gpio_num_t pin_;
    bool active_low_;
    std::atomic<LedState> current_state_;
    TaskHandle_t task_handle_;
    std::atomic<bool> running_;
};
