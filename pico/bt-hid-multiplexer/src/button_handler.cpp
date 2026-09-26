#include "button_handler.h"
#include "hardware/gpio.h"
#include "pico/time.h"

bool ButtonHandler::last_raw_state_ = true;
bool ButtonHandler::debounced_state_ = true;
uint32_t ButtonHandler::last_debounce_time_ = 0;
uint32_t ButtonHandler::press_start_time_ = 0;
bool ButtonHandler::long_press_fired_ = false;
bool ButtonHandler::extra_long_press_fired_ = false;

void ButtonHandler::init() {
    gpio_init(PIN_PAIR_BUTTON);
    gpio_set_dir(PIN_PAIR_BUTTON, GPIO_IN);
    gpio_pull_up(PIN_PAIR_BUTTON); // Active LOW: resting HIGH, pressed LOW

    last_raw_state_ = gpio_get(PIN_PAIR_BUTTON);
    debounced_state_ = last_raw_state_;
    last_debounce_time_ = to_ms_since_boot(get_absolute_time());
    press_start_time_ = 0;
    long_press_fired_ = false;
    extra_long_press_fired_ = false;
}

ButtonEvent ButtonHandler::update() {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    bool raw = gpio_get(PIN_PAIR_BUTTON);
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
                if (!long_press_fired_ && !extra_long_press_fired_) {
                    uint32_t duration = now - press_start_time_;
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
