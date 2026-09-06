#pragma once

#include <atomic>
#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "controller_state.hpp"

class SerialCommandServer {
public:
    SerialCommandServer(ControllerState& controller, bool log_enabled = true, bool enable_echo = true);
    ~SerialCommandServer();

    bool start();
    void stop();

private:
    static void task_entry(void* arg);
    void run_task();
    void process_stream(std::string& accum, const char* data, size_t len, bool is_cdc);

    ControllerState& controller_;
    bool log_enabled_;
    bool enable_echo_;
    TaskHandle_t task_handle_;
    std::atomic<bool> running_;
    std::string uart_accum_;
    std::string cdc_accum_;
};
