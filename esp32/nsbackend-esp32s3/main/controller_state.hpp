#pragma once

#include <string>
#include <vector>
#include <mutex>
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

    // I2C matrix keypad input interface
    void set_keypad_state(uint16_t keypad_buttons, bool up, bool down, bool left, bool right);

private:
    void set_button(uint16_t mask, bool active);

    GamepadHid& gamepad_;
    std::mutex mutex_;

    // TCP Command State
    uint16_t buttons_;
    bool dpad_up_;
    bool dpad_down_;
    bool dpad_left_;
    bool dpad_right_;

    // Keypad Input State
    uint16_t keypad_buttons_;
    bool keypad_dpad_up_;
    bool keypad_dpad_down_;
    bool keypad_dpad_left_;
    bool keypad_dpad_right_;

    // Analog Sticks [-1.0 .. 1.0]
    float lx_;
    float ly_;
    float rx_;
    float ry_;

    std::vector<ScheduledCommand> scheduled_commands_;
};
