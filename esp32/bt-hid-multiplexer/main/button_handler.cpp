#include "button_handler.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "platform.h"

bool ButtonHandler::last_raw_state_ = true;
bool ButtonHandler::debounced_state_ = true;
uint32_t ButtonHandler::last_debounce_time_ = 0;
uint32_t ButtonHandler::press_start_time_ = 0;
bool ButtonHandler::long_press_fired_ = false;
bool ButtonHandler::extra_long_press_fired_ = false;

void ButtonHandler::init() {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << PIN_PAIR_BUTTON;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;  // Active LOW: resting HIGH, pressed LOW
    gpio_config(&io);
    // Allow internal pull-up to charge pin capacitance before reading baseline
    esp_rom_delay_us(2000);

    last_raw_state_ = gpio_get_level((gpio_num_t)PIN_PAIR_BUTTON);
    debounced_state_ = last_raw_state_;
    last_debounce_time_ = platform_now_ms();
    press_start_time_ = 0;
    long_press_fired_ = false;
    extra_long_press_fired_ = false;
}

ButtonEvent ButtonHandler::update() {
    uint32_t now = platform_now_ms();
    bool raw = gpio_get_level((gpio_num_t)PIN_PAIR_BUTTON);
    ButtonEvent event = BUTTON_EVENT_NONE;

    if (raw != last_raw_state_) {
        last_debounce_time_ = now;
        last_raw_state_ = raw;
    }

    if ((now - last_debounce_time_) > 30) { // 30ms debounce window
        if (debounced_state_ != raw) {
            debounced_state_ = raw;

            if (!debounced_state_) {
                // Button transitioned to PRESSED (LOW)
                press_start_time_ = now;
                long_press_fired_ = false;
                extra_long_press_fired_ = false;
            } else {
                // Button transitioned to RELEASED (HIGH)
                if (!long_press_fired_ && !extra_long_press_fired_ && press_start_time_ > 0) {
                    uint32_t duration = now - press_start_time_;
                    press_start_time_ = 0;
                    if (duration >= 50 && duration < 1500) {
                        event = BUTTON_EVENT_SHORT_PRESS;
                    }
                }
            }
        }
    }

    // Check ongoing holds while button is held pressed
    if (!debounced_state_) {
        uint32_t duration = now - press_start_time_;
        if (duration >= 8000 && !extra_long_press_fired_) {
            extra_long_press_fired_ = true;
            event = BUTTON_EVENT_EXTRA_LONG_PRESS_RESET;
        } else if (duration >= 2000 && !long_press_fired_ && !extra_long_press_fired_) {
            long_press_fired_ = true;
            event = BUTTON_EVENT_LONG_PRESS_PAIR;
        }
    }

    return event;
}
