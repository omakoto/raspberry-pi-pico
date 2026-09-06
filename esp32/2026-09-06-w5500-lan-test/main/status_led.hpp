#pragma once

#include <atomic>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum class LedState : int {
    INITIALIZING = 1,        // Startup / Hardware reset: Solid ON
    ETH_LINK_DOWN = 2,       // Ethernet cable disconnected: Rapid strobe (0.1s ON, 0.1s OFF)
    ETH_DHCP_WAIT = 3,       // Link UP, waiting for DHCP lease: 0.1s ON, 0.9s OFF
    WAITING_CLIENT = 4,      // TCP Server listening, waiting for client: 0.5s ON, 0.5s OFF
    CLIENT_CONNECTED = 5,    // Client connected and streaming: Solid ON
};

class StatusLed {
public:
    explicit StatusLed(gpio_num_t pin = GPIO_NUM_21, bool active_low = true);
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
