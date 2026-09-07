/*
 * TCP Command Server Implementation for nsbackend-pico.
 * Handles client connections and line-based command execution over Wi-Fi.
 */

#include "tcp_server.hpp"

#if defined(NSBACKEND_HAS_WIFI) && NSBACKEND_HAS_WIFI

#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "dual_logger.hpp"

static const char* TAG = "TcpServer";

TcpServer::TcpServer(int port, ControllerState& controller, StatusLed& led, bool log_enabled, bool enable_echo)
    : port_(port),
      controller_(controller),
      led_(led),
      log_enabled_(log_enabled),
      enable_echo_(enable_echo),
      task_handle_(nullptr),
      running_(false),
      listening_(false),
      listen_sock_(-1) {}

TcpServer::~TcpServer() {
    stop();
}

bool TcpServer::start() {
    stop();
    running_ = true;
    listening_ = false;

    BaseType_t res = xTaskCreate(task_entry, "tcp_server_task", 2048, this, tskIDLE_PRIORITY + 3, &task_handle_);
    if (res != pdPASS) {
        LOG_E(TAG, "Failed to create TCP server task (res=%d, free_heap=%u)",
              static_cast<int>(res), static_cast<unsigned>(xPortGetFreeHeapSize()));
        return false;
    }

    LOG_I(TAG, "TCP Server task created for port %d", port_);
    return true;
}

void TcpServer::stop() {
    running_ = false;
    listening_ = false;
    int sock = listen_sock_.exchange(-1);
    if (sock >= 0) {
        closesocket(sock);
    }
    if (task_handle_ != nullptr) {
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
    }
}

void TcpServer::task_entry(void* arg) {
    static_cast<TcpServer*>(arg)->run_server();
}

void TcpServer::run_server() {
    LOG_I(TAG, "run_server: initializing TCP socket...");
    struct sockaddr_in dest_addr = {};
    dest_addr.sin_len = sizeof(dest_addr);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port_);
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        LOG_E(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(nullptr);
        return;
    }
    LOG_I(TAG, "run_server: socket %d created, binding to port %d...", listen_sock, port_);

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    int err = bind(listen_sock, reinterpret_cast<struct sockaddr*>(&dest_addr), sizeof(dest_addr));
    if (err != 0) {
        LOG_E(TAG, "Socket unable to bind to port %d: errno %d", port_, errno);
        closesocket(listen_sock);
        vTaskDelete(nullptr);
        return;
    }
    LOG_I(TAG, "run_server: bound to port %d, listening...", port_);

    err = listen(listen_sock, 2);
    if (err != 0) {
        LOG_E(TAG, "Error occurred during listen: errno %d", errno);
        closesocket(listen_sock);
        vTaskDelete(nullptr);
        return;
    }

    listen_sock_.store(listen_sock);
    listening_ = true;
    LOG_I(TAG, "TCP Server listening on port %d...", port_);

    while (running_) {
        struct sockaddr_in source_addr = {};
        socklen_t addr_len = sizeof(source_addr);

        int client_sock = accept(listen_sock, reinterpret_cast<struct sockaddr*>(&source_addr), &addr_len);
        if (client_sock < 0) {
            if (!running_) {
                break;
            }
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            LOG_W(TAG, "Unable to accept connection: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        char addr_str[32];
        inet_ntoa_r(source_addr.sin_addr, addr_str, sizeof(addr_str) - 1);
        LOG_I(TAG, "Client connected from %s:%d", addr_str, ntohs(source_addr.sin_port));

        led_.set_state(LedState::CLIENT_CONNECTED);
        handle_client(client_sock);

        LOG_I(TAG, "Client disconnected, resetting controller state");
        controller_.reset_all();
        led_.set_state(LedState::WAITING_CLIENT);
    }

    int sock = listen_sock_.exchange(-1);
    if (sock >= 0) {
        closesocket(sock);
    }
    listening_ = false;
    vTaskDelete(nullptr);
}

void TcpServer::handle_client(int client_sock) {
    // 20ms timeout for responsive streaming and auto-releases
    struct timeval tv = { .tv_sec = 0, .tv_usec = 20000 };
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int nodelay = 1;
    setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    char rx_buf[256];
    std::string stream_accum;

    while (running_) {
        controller_.check_scheduled();

        int len = recv(client_sock, rx_buf, sizeof(rx_buf) - 1, 0);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
                continue;
            }
            LOG_W(TAG, "recv failed: errno %d", errno);
            break;
        }

        if (len == 0) {
            LOG_I(TAG, "Connection closed by client (EOF)");
            break;
        }

        rx_buf[len] = '\0';
        stream_accum.append(rx_buf, len);

        while (!stream_accum.empty()) {
            size_t delim_pos = stream_accum.find_first_of("\r\n");
            if (delim_pos == std::string::npos) {
                break;
            }

            std::string line = stream_accum.substr(0, delim_pos);

            // Consume both characters if part of a CRLF or LFCR sequence
            if (delim_pos + 1 < stream_accum.size() &&
                ((stream_accum[delim_pos] == '\r' && stream_accum[delim_pos + 1] == '\n') ||
                 (stream_accum[delim_pos] == '\n' && stream_accum[delim_pos + 1] == '\r'))) {
                stream_accum.erase(0, delim_pos + 2);
            } else {
                stream_accum.erase(0, delim_pos + 1);
            }

            if (!line.empty()) {
                if (log_enabled_) {
                    dual_println(line);
                }

                if (enable_echo_) {
                    std::string echo_resp = line + "\n";
                    send(client_sock, echo_resp.c_str(), echo_resp.length(), 0);
                }

                controller_.execute_command(line);
            }
        }
    }

    closesocket(client_sock);
}

#endif // NSBACKEND_HAS_WIFI
