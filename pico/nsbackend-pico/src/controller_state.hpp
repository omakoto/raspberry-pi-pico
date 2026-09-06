/*
 * Controller State Engine for nsbackend-pico.
 * Parses commands, maintains button/stick/dpad state, and handles timed releases.
 */

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "FreeRTOS.h"
#include "semphr.h"
#include "gamepad_hid.hpp"

struct ScheduledCommand {
    int64_t target_time_us;
    std::string command;
};

class ControllerState {
public:
    explicit ControllerState(GamepadHid& gamepad);
    ~ControllerState();

    void reset_all();
    void sync_report();
    void execute_command(const std::string& cmd_line);
    void check_scheduled();

    // Direct GPIO button input interface
    void set_gpio_state(uint16_t gpio_buttons, bool up, bool down, bool left, bool right);

private:
    void set_button(uint16_t mask, bool active);

    GamepadHid& gamepad_;
    SemaphoreHandle_t mutex_;

    // TCP / Serial Command State
    uint16_t buttons_;
    bool dpad_up_;
    bool dpad_down_;
    bool dpad_left_;
    bool dpad_right_;

    // GPIO Button State
    uint16_t gpio_buttons_;
    bool gpio_dpad_up_;
    bool gpio_dpad_down_;
    bool gpio_dpad_left_;
    bool gpio_dpad_right_;

    // Analog Sticks [-1.0 .. 1.0]
    float lx_;
    float ly_;
    float rx_;
    float ry_;

    std::vector<ScheduledCommand> scheduled_commands_;
};
