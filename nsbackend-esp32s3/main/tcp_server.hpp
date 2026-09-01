#pragma once

#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "controller_state.hpp"
#include "status_led.hpp"

class TcpServer {
public:
    TcpServer(int port, ControllerState& controller, StatusLed& led, bool log_enabled = true, bool enable_echo = true);
    ~TcpServer();

    bool start();
    void stop();

private:
    static void task_entry(void* arg);
    void run_server();
    void handle_client(int client_sock);

    int port_;
    ControllerState& controller_;
    StatusLed& led_;
    bool log_enabled_;
    bool enable_echo_;
    TaskHandle_t task_handle_;
    std::atomic<bool> running_;
};
