#pragma once

#include <atomic>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum class LedState : int {
    INITIALIZING = 1,        // Startup / Hardware init: Solid ON
    WIFI_CONNECTING = 2,     // Wi-Fi connecting: 0.1s ON, 1.0s OFF
    SETTING_UP_TCP = 3,      // Wi-Fi connected, setting up TCP: 0.1s ON, 0.1s OFF, 0.1s ON, 1.0s OFF
    WAITING_CLIENT = 4,      // Waiting for client: 0.5s ON, 0.5s OFF
    CLIENT_CONNECTED = 5,    // Client connected: 1.0s ON, 1.0s OFF
    WIFI_RECONNECTING = 6,   // Wi-Fi failed / reconnecting: Rapid strobe (0.1s ON, 0.1s OFF)
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
