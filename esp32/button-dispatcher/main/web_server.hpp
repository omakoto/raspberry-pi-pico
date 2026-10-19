#pragma once

#include <esp_http_server.h>

#include "dispatcher/api.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// The web server: serves the embedded single-page UI and hands the /api routes to dispatcher::Api.
//
// Requests the API marks as slow (they call out to the network) are detached from the server's single
// request thread with the asynchronous-request support and answered by a worker task, so the page stays
// responsive while, for example, "Run now" waits for a slow server.
class WebServer {
public:
    explicit WebServer(dispatcher::Api& api);
    ~WebServer();

    bool start(int port);
    void stop();

private:
    struct SlowJob;

    static esp_err_t api_handler(httpd_req_t* req);
    static void slow_worker_entry(void* arg);

    esp_err_t handle_api(httpd_req_t* req);
    void slow_worker_loop();

    dispatcher::Api& api_;
    httpd_handle_t server_;
    QueueHandle_t slow_queue_;
};
