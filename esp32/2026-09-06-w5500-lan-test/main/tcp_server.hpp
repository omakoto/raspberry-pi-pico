#pragma once

#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "status_led.hpp"

class TcpServer {
public:
    TcpServer();
    ~TcpServer();

    bool start(int port, StatusLed* status_led);
    void stop();

    static void send_announcement();

private:
    static void task_entry(void* arg);
    void run_task();

    int port_;
    StatusLed* status_led_;
    TaskHandle_t task_handle_;
    std::atomic<bool> running_;
    int listen_sock_;
};
