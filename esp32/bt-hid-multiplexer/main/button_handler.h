#ifndef BUTTON_HANDLER_H_
#define BUTTON_HANDLER_H_

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

enum ButtonEvent {
    BUTTON_EVENT_NONE = 0,
    BUTTON_EVENT_SHORT_PRESS,
    BUTTON_EVENT_LONG_PRESS_PAIR,
    BUTTON_EVENT_EXTRA_LONG_PRESS_RESET
};

class ButtonHandler {
public:
    static void init();
    static ButtonEvent update();

private:
    static bool last_raw_state_;
    static bool debounced_state_;
    static uint32_t last_debounce_time_;
    static uint32_t press_start_time_;
    static bool long_press_fired_;
    static bool extra_long_press_fired_;
};

#endif // BUTTON_HANDLER_H_
