#include "tcp_server.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include "esp_log.h"
#include "esp_timer.h"

static const char* TAG = "TcpServer";

// TCP keepalive tuning for the client connection: start probing after this many idle
// seconds, then send up to kKeepaliveCount probes this many seconds apart. A peer that
// answers none of them is declared dead after roughly 25 seconds.
static constexpr int kKeepaliveIdleSec = 10;
static constexpr int kKeepaliveIntervalSec = 5;
static constexpr int kKeepaliveCount = 3;

TcpServer::TcpServer()
    : port_(10110),
      status_led_(nullptr),
      task_handle_(nullptr),
      running_(false),
      listen_sock_(-1) {}

TcpServer::~TcpServer() {
    stop();
}

void TcpServer::send_announcement() {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        return;
    }

    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    struct sockaddr_in dest_addr = {};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(9);
    dest_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    char msg = 0;
    sendto(sock, &msg, 1, 0, reinterpret_cast<struct sockaddr*>(&dest_addr), sizeof(dest_addr));
    close(sock);
}

bool TcpServer::start(int port, StatusLed* status_led) {
    stop();

    port_ = port;
    status_led_ = status_led;
    running_ = true;

    BaseType_t ret = xTaskCreate(task_entry, "tcp_server", 8192, this, 5, &task_handle_);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TCP server task");
        running_ = false;
        return false;
    }

    return true;
}

void TcpServer::stop() {
    running_ = false;
    if (listen_sock_ >= 0) {
        close(listen_sock_);
        listen_sock_ = -1;
    }
    if (task_handle_ != nullptr) {
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
    }
}

void TcpServer::task_entry(void* arg) {
    auto* self = static_cast<TcpServer*>(arg);
    self->run_task();
}

void TcpServer::run_task() {
    char rx_buffer[1024];

    while (running_) {
        struct sockaddr_in server_addr = {};
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        server_addr.sin_port = htons(port_);

        listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (listen_sock_ < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d (%s)", errno, strerror(errno));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int opt = 1;
        setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        // Set 1-second timeout on accept to periodically run background announcements
        struct timeval accept_tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(listen_sock_, SOL_SOCKET, SO_RCVTIMEO, &accept_tv, sizeof(accept_tv));

        int err = bind(listen_sock_, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr));
        if (err != 0) {
            ESP_LOGE(TAG, "Socket unable to bind on port %d: errno %d (%s)", port_, errno, strerror(errno));
            close(listen_sock_);
            listen_sock_ = -1;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        err = listen(listen_sock_, 1);
        if (err != 0) {
            ESP_LOGE(TAG, "Error occurred during listen: errno %d (%s)", errno, strerror(errno));
            close(listen_sock_);
            listen_sock_ = -1;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        ESP_LOGI(TAG, "TCP Echo Server listening on port %d", port_);
        if (status_led_) {
            status_led_->set_state(LedState::WAITING_CLIENT);
        }

        int64_t last_announce_us = esp_timer_get_time();

        while (running_) {
            struct sockaddr_in source_addr = {};
            socklen_t addr_len = sizeof(source_addr);
            int client_sock = accept(listen_sock_, reinterpret_cast<struct sockaddr*>(&source_addr), &addr_len);

            if (client_sock < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Periodic 3-second UDP announcement while waiting for client
                    int64_t now_us = esp_timer_get_time();
                    if (now_us - last_announce_us >= 3000000) {
                        last_announce_us = now_us;
                        send_announcement();
                    }
                    continue;
                }
                ESP_LOGE(TAG, "Accept error: errno %d (%s)", errno, strerror(errno));
                break;
            }

            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &source_addr.sin_addr, client_ip, sizeof(client_ip));
            int client_port = ntohs(source_addr.sin_port);
            ESP_LOGI(TAG, "Client connected from %s:%d", client_ip, client_port);

            if (status_led_) {
                status_led_->set_state(LedState::CLIENT_CONNECTED);
            }

            // Wake out of recv() periodically so that a stop() request is noticed promptly.
            struct timeval client_tv = { .tv_sec = 5, .tv_usec = 0 };
            setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &client_tv, sizeof(client_tv));

            // Only one client is served at a time, so a peer that disappears without closing
            // the connection - a yanked cable, a crashed host, a network outage - would
            // otherwise hold this task in recv() forever and no further client could ever be
            // accepted. Nothing is transmitted while waiting for input, so the stack has no
            // other way to discover the peer is gone: keepalive probes supply that traffic
            // and make recv() fail, which returns this task to accept().
            int keepalive_enable = 1;
            int keepalive_idle = kKeepaliveIdleSec;
            int keepalive_interval = kKeepaliveIntervalSec;
            int keepalive_count = kKeepaliveCount;
            if (setsockopt(client_sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive_enable, sizeof(keepalive_enable)) != 0 ||
                setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepalive_idle, sizeof(keepalive_idle)) != 0 ||
                setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepalive_interval, sizeof(keepalive_interval)) != 0 ||
                setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPCNT, &keepalive_count, sizeof(keepalive_count)) != 0) {
                ESP_LOGW(TAG, "Failed to enable TCP keepalive for %s:%d: errno %d (%s)",
                         client_ip, client_port, errno, strerror(errno));
            }

            while (running_) {
                int len = recv(client_sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
                if (len < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        // Socket read timeout: yield briefly and keep waiting
                        vTaskDelay(pdMS_TO_TICKS(10));
                        continue;
                    }
                    if (errno == ETIMEDOUT) {
                        ESP_LOGW(TAG, "Client %s:%d stopped responding to keepalive; dropping connection",
                                 client_ip, client_port);
                    } else if (errno == ECONNRESET || errno == ENOTCONN || errno == ESHUTDOWN || errno == ECONNABORTED) {
                        ESP_LOGI(TAG, "Client %s:%d closed connection", client_ip, client_port);
                    } else {
                        ESP_LOGW(TAG, "Socket receive error from %s:%d: errno %d (%s)", client_ip, client_port, errno, strerror(errno));
                    }
                    break;
                }

                if (len == 0) {
                    ESP_LOGI(TAG, "Client %s:%d disconnected", client_ip, client_port);
                    break;
                }

                rx_buffer[len] = '\0';

                // Check if buffer contains printable UTF-8 / ASCII
                bool is_printable = true;
                for (int i = 0; i < len; ++i) {
                    unsigned char c = static_cast<unsigned char>(rx_buffer[i]);
                    if (c < 32 && c != '\r' && c != '\n' && c != '\t') {
                        is_printable = false;
                        break;
                    }
                }

                if (is_printable) {
                    // Trim trailing newline for clean single-line log formatting
                    char print_buf[1024];
                    strncpy(print_buf, rx_buffer, sizeof(print_buf) - 1);
                    print_buf[sizeof(print_buf) - 1] = '\0';
                    size_t plen = strlen(print_buf);
                    while (plen > 0 && (print_buf[plen - 1] == '\r' || print_buf[plen - 1] == '\n')) {
                        print_buf[--plen] = '\0';
                    }
                    ESP_LOGI(TAG, "[%s:%d] Received (%d bytes): %s", client_ip, client_port, len, print_buf);
                } else {
                    ESP_LOGI(TAG, "[%s:%d] Received (%d bytes binary data)", client_ip, client_port, len);
                }

                // Echo the received data back to client
                ESP_LOGI(TAG, "[%s:%d] Echoing %d bytes...", client_ip, client_port, len);
                int sent = send(client_sock, rx_buffer, len, 0);
                if (sent < 0) {
                    ESP_LOGE(TAG, "[%s:%d] Error echoing data: errno %d (%s)", client_ip, client_port, errno, strerror(errno));
                    break;
                }
                ESP_LOGI(TAG, "[%s:%d] Echoed %d bytes.", client_ip, client_port, sent);
            }

            close(client_sock);
            ESP_LOGI(TAG, "Connection with %s:%d finished.", client_ip, client_port);

            if (status_led_) {
                status_led_->set_state(LedState::WAITING_CLIENT);
            }
        }

        if (listen_sock_ >= 0) {
            close(listen_sock_);
            listen_sock_ = -1;
        }
    }
}
