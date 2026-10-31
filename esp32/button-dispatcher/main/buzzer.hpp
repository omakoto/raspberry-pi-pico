#pragma once

#include <mutex>

#include "dispatcher/beeps.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// The acknowledgement buzzer: a passive buzzer (no oscillator of its own) driven straight from one GPIO with
// a square wave from the LEDC peripheral, at one fixed pitch. Beeps are scheduled by dispatcher::BeepScheduler
// and sounded by a small task, so beep() returns at once and can be called from any task.
class Buzzer {
public:
    static constexpr int kDefaultFrequencyHz = 880;

    // A pin below 0 disables the buzzer.
    explicit Buzzer(int pin, int frequency_hz = kDefaultFrequencyHz);

    bool init();
    void beep(dispatcher::Beep kind, int count);

private:
    static void task_entry(void* arg);
    void run_task();
    void set_tone(bool on);

    int pin_;
    int frequency_hz_;
    bool enabled_;
    std::mutex mutex_;  // guards scheduler_ and keeps one sequence's pulses together in the queue
    dispatcher::BeepScheduler scheduler_;
    QueueHandle_t queue_;
};
