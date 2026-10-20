#pragma once

#include <mutex>

#include "dispatcher/beeps.hpp"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// The acknowledgement buzzer: an active buzzer (it has its own oscillator, so one fixed pitch) switched by a
// transistor from one GPIO. Beeps are scheduled by dispatcher::BeepScheduler and sounded by a small task, so
// beep() returns at once and can be called from any task.
class Buzzer {
public:
    // A pin below 0 disables the buzzer. `active_low` is for a buzzer (such as a ready-made module with a PNP
    // transistor) that sounds when the pin is low.
    explicit Buzzer(int pin, bool active_low = true);

    bool init();
    void beep(dispatcher::Beep kind, int count);

private:
    static void task_entry(void* arg);
    void run_task();
    void set_raw(bool on);

    int pin_;
    bool active_low_;
    bool enabled_;
    std::mutex mutex_;  // guards scheduler_ and keeps one sequence's pulses together in the queue
    dispatcher::BeepScheduler scheduler_;
    QueueHandle_t queue_;
};
