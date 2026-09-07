/*
 * TCP Command Server for nsbackend-pico.
 * Accepts TCP socket connections on port 10100 to receive gamepad commands over Wi-Fi.
 */

#pragma once

#if defined(NSBACKEND_HAS_WIFI) && NSBACKEND_HAS_WIFI

#include <atomic>
#include "FreeRTOS.h"
#include "task.h"
#include "controller_state.hpp"
#include "status_led.hpp"

class TcpServer {
public:
    TcpServer(int port, ControllerState& controller, StatusLed& led, bool log_enabled = true, bool enable_echo = true);
    ~TcpServer();

    bool start();
    void stop();
    bool is_listening() const { return listening_.load(); }
    int get_port() const { return port_; }

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
    std::atomic<bool> listening_;
    std::atomic<int> listen_sock_;
};

#endif // NSBACKEND_HAS_WIFI
