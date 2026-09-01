#include "tcp_server.hpp"
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char* TAG = "TcpServer";

TcpServer::TcpServer(int port, ControllerState& controller, StatusLed& led, bool log_enabled, bool enable_echo)
    : port_(port),
      controller_(controller),
      led_(led),
      log_enabled_(log_enabled),
      enable_echo_(enable_echo),
      task_handle_(nullptr),
      running_(false) {}

TcpServer::~TcpServer() {
    stop();
}

bool TcpServer::start() {
    stop();
    running_ = true;

    BaseType_t res = xTaskCreate(task_entry, "tcp_server_task", 4096, this, tskIDLE_PRIORITY + 3, &task_handle_);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TCP server task");
        return false;
    }

    ESP_LOGI(TAG, "TCP Server task created for port %d", port_);
    return true;
}

void TcpServer::stop() {
    running_ = false;
    if (task_handle_ != nullptr) {
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
    }
}

void TcpServer::task_entry(void* arg) {
    static_cast<TcpServer*>(arg)->run_server();
}

void TcpServer::run_server() {
    struct sockaddr_in dest_addr = {};
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port_);

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(nullptr);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    int err = bind(listen_sock, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(listen_sock);
        vTaskDelete(nullptr);
        return;
    }

    err = listen(listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        close(listen_sock);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "TCP Server listening on port %d...", port_);

    while (running_) {
        led_.set_state(LedState::WAITING_CLIENT);

        struct sockaddr_in source_addr = {};
        socklen_t addr_len = sizeof(source_addr);

        // Set listen socket timeout for periodic auto-release checking
        struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };
        setsockopt(listen_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        int client_sock = accept(listen_sock, (struct sockaddr*)&source_addr, &addr_len);
        if (client_sock < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
                controller_.check_scheduled();
                continue;
            }
            ESP_LOGW(TAG, "Unable to accept connection: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        char addr_str[32];
        inet_ntoa_r(source_addr.sin_addr, addr_str, sizeof(addr_str) - 1);
        ESP_LOGI(TAG, "Client connected from %s:%d", addr_str, ntohs(source_addr.sin_port));

        led_.set_state(LedState::CLIENT_CONNECTED);
        handle_client(client_sock);

        ESP_LOGI(TAG, "Client disconnected, resetting controller state");
        controller_.reset_all();
    }

    close(listen_sock);
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
            ESP_LOGW(TAG, "recv failed: errno %d", errno);
            break;
        }

        if (len == 0) {
            ESP_LOGI(TAG, "Connection closed by client (EOF)");
            break;
        }

        rx_buf[len] = '\0';
        stream_accum.append(rx_buf, len);

        size_t newline_pos;
        while ((newline_pos = stream_accum.find('\n')) != std::string::npos) {
            std::string line = stream_accum.substr(0, newline_pos);
            stream_accum.erase(0, newline_pos + 1);

            // Strip trailing carriage return if present
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (!line.empty()) {
                if (log_enabled_) {
                    std::printf("%s\n", line.c_str());
                    std::fflush(stdout);
                }

                if (enable_echo_) {
                    std::string echo_resp = line + "\n";
                    send(client_sock, echo_resp.c_str(), echo_resp.length(), 0);
                }

                controller_.execute_command(line);
            }
        }
    }

    close(client_sock);
}
