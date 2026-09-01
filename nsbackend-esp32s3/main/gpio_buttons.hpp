#pragma once

#include <vector>
#include <string>
#include <atomic>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "controller_state.hpp"

enum class ButtonAction {
    BUTTON,
    DPAD_UP,
    DPAD_DOWN,
    DPAD_LEFT,
    DPAD_RIGHT
};

struct GpioButtonSpec {
    gpio_num_t pin;
    std::string name;
    ButtonAction action;
    std::string cmd_name;
    uint16_t mask;
    bool is_active;
    bool last_raw;
    int64_t last_change_us;
};

class GpioButtonManager {
public:
    explicit GpioButtonManager(ControllerState& controller);
    ~GpioButtonManager();

    bool init();

private:
    static void task_entry(void* arg);
    void run_task();
    void update_inputs();
    void apply_to_controller();

    ControllerState& controller_;
    std::vector<GpioButtonSpec> buttons_;
    TaskHandle_t task_handle_;
    std::atomic<bool> running_;
};
