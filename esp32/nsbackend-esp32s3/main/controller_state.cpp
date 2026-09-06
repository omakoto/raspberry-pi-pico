#include "controller_state.hpp"
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <sstream>
#include "esp_timer.h"

static constexpr int64_t DEFAULT_AUTO_RELEASE_US = 50000; // 50ms

ControllerState::ControllerState(GamepadHid& gamepad)
    : gamepad_(gamepad),
      buttons_(BTN_NONE),
      dpad_up_(false),
      dpad_down_(false),
      dpad_left_(false),
      dpad_right_(false),
      gpio_buttons_(BTN_NONE),
      gpio_dpad_up_(false),
      gpio_dpad_down_(false),
      gpio_dpad_left_(false),
      gpio_dpad_right_(false),
      lx_(0.0f),
      ly_(0.0f),
      rx_(0.0f),
      ry_(0.0f) {}

ControllerState::~ControllerState() {}

void ControllerState::reset_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    buttons_ = BTN_NONE;
    dpad_up_ = false;
    dpad_down_ = false;
    dpad_left_ = false;
    dpad_right_ = false;
    lx_ = 0.0f;
    ly_ = 0.0f;
    rx_ = 0.0f;
    ry_ = 0.0f;
    scheduled_commands_.clear();
    sync_report();
}

void ControllerState::set_button(uint16_t mask, bool active) {
    if (active) {
        buttons_ |= mask;
    } else {
        buttons_ &= ~mask;
    }
}

void ControllerState::set_gpio_state(uint16_t gpio_buttons, bool up, bool down, bool left, bool right) {
    std::lock_guard<std::mutex> lock(mutex_);
    gpio_buttons_ = gpio_buttons;
    gpio_dpad_up_ = up;
    gpio_dpad_down_ = down;
    gpio_dpad_left_ = left;
    gpio_dpad_right_ = right;
    sync_report();
}

void ControllerState::sync_report() {
    // Resolves hat direction with opposing cancellation
    bool up = dpad_up_ || gpio_dpad_up_;
    bool down = dpad_down_ || gpio_dpad_down_;
    bool left = dpad_left_ || gpio_dpad_left_;
    bool right = dpad_right_ || gpio_dpad_right_;

    if (up && down) {
        up = down = false;
    }
    if (left && right) {
        left = right = false;
    }

    uint8_t hat = HAT_CENTER;
    if (up) {
        if (right) hat = HAT_UP_RIGHT;
        else if (left) hat = HAT_UP_LEFT;
        else hat = HAT_UP;
    } else if (down) {
        if (right) hat = HAT_DOWN_RIGHT;
        else if (left) hat = HAT_DOWN_LEFT;
        else hat = HAT_DOWN;
    } else if (right) {
        hat = HAT_RIGHT;
    } else if (left) {
        hat = HAT_LEFT;
    }

    // Map analog floats [-1.0 .. +1.0] to byte [0 .. 255] with 128 neutral center
    auto to_byte = [](float val) -> uint8_t {
        int rounded = static_cast<int>(std::round(128.0f + (val * 127.0f)));
        return static_cast<uint8_t>(std::max(0, std::min(255, rounded)));
    };

    uint8_t lx_byte = to_byte(lx_);
    uint8_t ly_byte = to_byte(ly_);
    uint8_t rx_byte = to_byte(rx_);
    uint8_t ry_byte = to_byte(ry_);

    uint16_t merged_buttons = buttons_ | gpio_buttons_;

    gamepad_.send_report(merged_buttons, hat, lx_byte, ly_byte, rx_byte, ry_byte);
}

void ControllerState::execute_command(const std::string& cmd_line) {
    std::string line = cmd_line;
    auto hash_pos = line.find('#');
    if (hash_pos != std::string::npos) {
        line = line.substr(0, hash_pos);
    }

    std::stringstream ss(line);
    std::vector<std::string> tokens;
    std::string token;
    while (ss >> token) {
        tokens.push_back(token);
    }

    if (tokens.empty()) {
        return;
    }

    size_t cmd_idx = 0;
    double duration_s = 0.0;

    // Check for optional leading duration e.g. "0.05 a 1"
    if (!tokens[0].empty() && (std::isdigit(static_cast<unsigned char>(tokens[0][0])) || tokens[0][0] == '.')) {
        char* end_ptr = nullptr;
        double parsed_dur = std::strtod(tokens[0].c_str(), &end_ptr);
        if (end_ptr != tokens[0].c_str()) {
            duration_s = parsed_dur;
            cmd_idx = 1;
        }
    }

    if (cmd_idx >= tokens.size()) {
        return;
    }

    std::string cmd = tokens[cmd_idx];
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](unsigned char c){ return std::tolower(c); });

    bool auto_release = false;
    float arg = 1.0f;

    if (tokens.size() > cmd_idx + 1) {
        char* end_ptr = nullptr;
        float parsed_arg = std::strtof(tokens[cmd_idx + 1].c_str(), &end_ptr);
        if (end_ptr != tokens[cmd_idx + 1].c_str()) {
            arg = std::max(-1.0f, std::min(1.0f, parsed_arg));
        } else {
            arg = 1.0f;
        }
    } else {
        arg = 1.0f;
        auto_release = true;
    }

    bool is_active = (std::abs(arg) >= 0.5f);

    std::lock_guard<std::mutex> lock(mutex_);

    // Buttons
    if (cmd == "a") set_button(BTN_A, is_active);
    else if (cmd == "b") set_button(BTN_B, is_active);
    else if (cmd == "x") set_button(BTN_X, is_active);
    else if (cmd == "y") set_button(BTN_Y, is_active);
    else if (cmd == "h") set_button(BTN_HOME, is_active);
    else if (cmd == "c") set_button(BTN_CAPTURE, is_active);
    else if (cmd == "m" || cmd == "-") set_button(BTN_MINUS, is_active);
    else if (cmd == "p" || cmd == "+") set_button(BTN_PLUS, is_active);
    else if (cmd == "l1") set_button(BTN_L, is_active);
    else if (cmd == "l2") set_button(BTN_ZL, is_active);
    else if (cmd == "r1") set_button(BTN_R, is_active);
    else if (cmd == "r2") set_button(BTN_ZR, is_active);
    else if (cmd == "lp") set_button(BTN_LSTICK, is_active);
    else if (cmd == "rp") set_button(BTN_RSTICK, is_active);

    // D-Pad
    else if (cmd == "pu") dpad_up_ = is_active;
    else if (cmd == "pd") dpad_down_ = is_active;
    else if (cmd == "pl") dpad_left_ = is_active;
    else if (cmd == "pr") dpad_right_ = is_active;
    else if (cmd == "pur") { dpad_up_ = is_active; dpad_right_ = is_active; }
    else if (cmd == "pul") { dpad_up_ = is_active; dpad_left_ = is_active; }
    else if (cmd == "pdr") { dpad_down_ = is_active; dpad_right_ = is_active; }
    else if (cmd == "pdl") { dpad_down_ = is_active; dpad_left_ = is_active; }
    else if (cmd == "px") {
        if (arg >= 0.5f) { dpad_right_ = true; dpad_left_ = false; }
        else if (arg <= -0.5f) { dpad_left_ = true; dpad_right_ = false; }
        else { dpad_left_ = false; dpad_right_ = false; }
    }
    else if (cmd == "py") {
        if (arg >= 0.5f) { dpad_up_ = true; dpad_down_ = false; }
        else if (arg <= -0.5f) { dpad_down_ = true; dpad_up_ = false; }
        else { dpad_up_ = false; dpad_down_ = false; }
    }

    // Left Stick
    else if (cmd == "lx") lx_ = arg;
    else if (cmd == "ly") ly_ = arg;
    else if (cmd == "lu") { lx_ = 0.0f; ly_ = is_active ? -1.0f : 0.0f; }
    else if (cmd == "ld") { lx_ = 0.0f; ly_ = is_active ? 1.0f : 0.0f; }
    else if (cmd == "ll") { lx_ = is_active ? -1.0f : 0.0f; ly_ = 0.0f; }
    else if (cmd == "lr") { lx_ = is_active ? 1.0f : 0.0f; ly_ = 0.0f; }
    else if (cmd == "lur") { lx_ = is_active ? 1.0f : 0.0f; ly_ = is_active ? -1.0f : 0.0f; }
    else if (cmd == "lul") { lx_ = is_active ? -1.0f : 0.0f; ly_ = is_active ? -1.0f : 0.0f; }
    else if (cmd == "ldr") { lx_ = is_active ? 1.0f : 0.0f; ly_ = is_active ? 1.0f : 0.0f; }
    else if (cmd == "ldl") { lx_ = is_active ? -1.0f : 0.0f; ly_ = is_active ? 1.0f : 0.0f; }

    // Right Stick
    else if (cmd == "rx") rx_ = arg;
    else if (cmd == "ry") ry_ = arg;
    else if (cmd == "ru") { rx_ = 0.0f; ry_ = is_active ? -1.0f : 0.0f; }
    else if (cmd == "rd") { rx_ = 0.0f; ry_ = is_active ? 1.0f : 0.0f; }
    else if (cmd == "rl") { rx_ = is_active ? -1.0f : 0.0f; ry_ = 0.0f; }
    else if (cmd == "rr") { rx_ = is_active ? 1.0f : 0.0f; ry_ = 0.0f; }
    else if (cmd == "rur") { rx_ = is_active ? 1.0f : 0.0f; ry_ = is_active ? -1.0f : 0.0f; }
    else if (cmd == "rul") { rx_ = is_active ? -1.0f : 0.0f; ry_ = is_active ? -1.0f : 0.0f; }
    else if (cmd == "rdr") { rx_ = is_active ? 1.0f : 0.0f; ry_ = is_active ? 1.0f : 0.0f; }
    else if (cmd == "rdl") { rx_ = is_active ? -1.0f : 0.0f; ry_ = is_active ? 1.0f : 0.0f; }

    // Reset/Clear
    else if (cmd == "clear" || cmd == "reset" || cmd == "release") {
        buttons_ = BTN_NONE;
        dpad_up_ = dpad_down_ = dpad_left_ = dpad_right_ = false;
        lx_ = ly_ = rx_ = ry_ = 0.0f;
    }
    else {
        return;
    }

    sync_report();

    // Auto-release scheduling
    if (auto_release) {
        int64_t delay_us = (duration_s > 0.0) ? static_cast<int64_t>(duration_s * 1000000.0) : DEFAULT_AUTO_RELEASE_US;
        int64_t target_time = esp_timer_get_time() + delay_us;
        scheduled_commands_.push_back({target_time, cmd + " 0"});
    }
}

void ControllerState::check_scheduled() {
    int64_t now_us = esp_timer_get_time();
    std::vector<std::string> ready_cmds;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (scheduled_commands_.empty()) {
            return;
        }

        std::vector<ScheduledCommand> remaining;
        for (const auto& item : scheduled_commands_) {
            if (now_us >= item.target_time_us) {
                ready_cmds.push_back(item.command);
            } else {
                remaining.push_back(item);
            }
        }
        scheduled_commands_ = std::move(remaining);
    }

    for (const auto& cmd : ready_cmds) {
        execute_command(cmd);
    }
}
