#pragma once

#include <string>
#include <esp_http_server.h>
#include "mac_tracker.hpp"
#include "w5500_driver.hpp"
#include "status_led.hpp"
#include "wifi_manager.hpp"

class HttpServer {
public:
    HttpServer();
    ~HttpServer();

    bool start(int port, MacTracker* tracker, W5500Driver* w5500, StatusLed* led, WifiManager* wifi, const std::string& hostname);
    void stop();

private:
    static esp_err_t root_get_handler(httpd_req_t* req);
    static esp_err_t wake_handler(httpd_req_t* req);
    static esp_err_t status_get_handler(httpd_req_t* req);
    static esp_err_t purge_handler(httpd_req_t* req);
    static esp_err_t log_get_handler(httpd_req_t* req);

    httpd_handle_t server_;
    MacTracker* tracker_;
    W5500Driver* w5500_;
    StatusLed* led_;
    WifiManager* wifi_;
    std::string hostname_;
    bool running_;
};
