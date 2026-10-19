#pragma once

#include <esp_http_server.h>

#include "dispatcher/api.hpp"

// The web server: serves the embedded single-page UI and hands the /api routes to dispatcher::Api.
//
// Every request is answered on the server's own thread, including the ones that call out to the network
// ("Run now", SwitchBot lists): the page waits for them anyway, so plain synchronous handlers are enough and
// avoid esp_http_server's asynchronous-request machinery.
class WebServer {
public:
    explicit WebServer(dispatcher::Api& api);
    ~WebServer();

    bool start(int port);
    void stop();

private:
    static esp_err_t api_handler(httpd_req_t* req);

    esp_err_t handle_api(httpd_req_t* req);

    dispatcher::Api& api_;
    httpd_handle_t server_;
};
