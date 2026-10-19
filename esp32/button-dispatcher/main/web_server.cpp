#include "web_server.hpp"

#include <arpa/inet.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <sys/socket.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

static const char* TAG = "WebServer";

// The gzip-compressed UI files, embedded by main/CMakeLists.txt (symbol names follow the file names).
#define EMBEDDED_FILE(symbol)                                                         \
    extern const uint8_t symbol##_start[] asm("_binary_" #symbol "_start");           \
    extern const uint8_t symbol##_end[] asm("_binary_" #symbol "_end");
EMBEDDED_FILE(index_html_gz)
EMBEDDED_FILE(app_js_gz)
EMBEDDED_FILE(style_css_gz)
EMBEDDED_FILE(favicon_svg_gz)

namespace {

constexpr size_t kMaxRequestBody = 32 * 1024;   // an imported config is the largest thing the page sends

struct StaticFile {
    const char* uri;
    const char* content_type;
    const uint8_t* start;
    const uint8_t* end;
};

const StaticFile kStaticFiles[] = {
    {"/", "text/html; charset=utf-8", index_html_gz_start, index_html_gz_end},
    {"/static/app.js", "text/javascript; charset=utf-8", app_js_gz_start, app_js_gz_end},
    {"/static/style.css", "text/css; charset=utf-8", style_css_gz_start, style_css_gz_end},
    {"/static/favicon.svg", "image/svg+xml", favicon_svg_gz_start, favicon_svg_gz_end},
};

const char* reason_for(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default: return "OK";
    }
}

// Headers for every response: the page loads only its own scripts and styles, so nothing from elsewhere
// is allowed, and it must not be framed or sniffed.
void set_security_headers(httpd_req_t* req, bool is_api) {
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'self'; frame-ancestors 'none'");
    httpd_resp_set_hdr(req, "Cache-Control", is_api ? "no-store" : "no-cache");
}

esp_err_t static_file_handler(httpd_req_t* req) {
    const auto* file = static_cast<const StaticFile*>(req->user_ctx);
    set_security_headers(req, false);
    httpd_resp_set_type(req, file->content_type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, reinterpret_cast<const char*>(file->start), file->end - file->start);
}

esp_err_t not_found_handler(httpd_req_t* req, httpd_err_code_t) {
    set_security_headers(req, true);
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"error\": \"not found\"}", HTTPD_RESP_USE_STRLEN);
}

// Sends an API response. httpd keeps pointers to the strings given to httpd_resp_set_*, so everything
// here must stay alive until httpd_resp_send returns.
esp_err_t send_api_response(httpd_req_t* req, const dispatcher::Response& response) {
    const std::string status_line = std::to_string(response.status) + " " + reason_for(response.status);
    httpd_resp_set_status(req, status_line.c_str());
    httpd_resp_set_type(req, response.content_type.c_str());
    set_security_headers(req, true);
    for (const auto& header : response.headers) httpd_resp_set_hdr(req, header.first.c_str(), header.second.c_str());
    return httpd_resp_send(req, response.body.data(), response.body.size());
}

dispatcher::Response error_response(int status, const char* message) {
    dispatcher::Response response;
    response.status = status;
    response.body = std::string("{\"error\": \"") + message + "\"}";
    return response;
}

std::string header_value(httpd_req_t* req, const char* name) {
    const size_t length = httpd_req_get_hdr_value_len(req, name);
    if (length == 0) return "";
    std::string value(length + 1, '\0');
    if (httpd_req_get_hdr_value_str(req, name, value.data(), value.size()) != ESP_OK) return "";
    value.resize(length);
    return value;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string percent_decode(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') {
            out += ' ';
        } else if (text[i] == '%' && i + 2 < text.size() && hex_digit(text[i + 1]) >= 0 && hex_digit(text[i + 2]) >= 0) {
            out += static_cast<char>(hex_digit(text[i + 1]) * 16 + hex_digit(text[i + 2]));
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

std::string peer_address(httpd_req_t* req) {
    sockaddr_in6 addr = {};
    socklen_t length = sizeof(addr);
    const int fd = httpd_req_to_sockfd(req);
    if (fd < 0 || getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &length) != 0) return "";
    char text[INET6_ADDRSTRLEN] = {};
    if (addr.sin6_family == AF_INET) {
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&addr)->sin_addr, text, sizeof(text));
    } else {
        inet_ntop(AF_INET6, &addr.sin6_addr, text, sizeof(text));
    }
    return text;
}

// Builds the core's Request from an esp_http_server request. Returns false, with `response` set, if the
// request cannot be handled (unsupported method, body too large, read error).
bool build_request(httpd_req_t* req, dispatcher::Request* request, dispatcher::Response* response) {
    switch (req->method) {
        case HTTP_GET: request->method = "GET"; break;
        case HTTP_POST: request->method = "POST"; break;
        case HTTP_PUT: request->method = "PUT"; break;
        case HTTP_DELETE: request->method = "DELETE"; break;
        default: *response = error_response(405, "method not allowed"); return false;
    }
    const std::string uri = req->uri;
    const size_t question = uri.find('?');
    request->path = uri.substr(0, question);
    if (question != std::string::npos) {
        size_t start = question + 1;
        while (start <= uri.size()) {
            size_t end = uri.find('&', start);
            if (end == std::string::npos) end = uri.size();
            const std::string pair = uri.substr(start, end - start);
            if (!pair.empty()) {
                const size_t equals = pair.find('=');
                request->query[percent_decode(pair.substr(0, equals))] =
                    equals == std::string::npos ? "" : percent_decode(pair.substr(equals + 1));
            }
            start = end + 1;
        }
    }
    request->cookie = header_value(req, "Cookie");
    request->csrf = header_value(req, dispatcher::kCsrfHeaderName);
    request->client = peer_address(req);

    if (req->content_len > kMaxRequestBody) {
        *response = error_response(413, "request too large");
        return false;
    }
    request->body.resize(req->content_len);
    size_t received = 0;
    while (received < req->content_len) {
        const int count = httpd_req_recv(req, request->body.data() + received, req->content_len - received);
        if (count == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (count <= 0) {
            *response = error_response(400, "could not read the request body");
            return false;
        }
        received += static_cast<size_t>(count);
    }
    return true;
}

}  // namespace

WebServer::WebServer(dispatcher::Api& api) : api_(api), server_(nullptr) {}

WebServer::~WebServer() { stop(); }

bool WebServer::start(int port) {
    stop();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_uri_handlers = 16;
    // Requests that call out to the network ("Run now", SwitchBot lists) run on this thread too, and a TLS
    // handshake needs the room.
    config.stack_size = 12 * 1024;
    config.uri_match_fn = httpd_uri_match_wildcard;
    // Free the oldest idle connection when a new one arrives, so a browser that keeps several
    // connections open cannot lock the page out.
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting HTTP server on port %d...", port);
    esp_err_t ret = httpd_start(&server_, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        server_ = nullptr;
        return false;
    }

    for (const StaticFile& file : kStaticFiles) {
        httpd_uri_t uri = {};
        uri.uri = file.uri;
        uri.method = HTTP_GET;
        uri.handler = static_file_handler;
        uri.user_ctx = const_cast<StaticFile*>(&file);
        httpd_register_uri_handler(server_, &uri);
    }
    for (httpd_method_t method : {HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_DELETE}) {
        httpd_uri_t uri = {};
        uri.uri = "/api/*";
        uri.method = method;
        uri.handler = api_handler;
        uri.user_ctx = this;
        httpd_register_uri_handler(server_, &uri);
    }
    httpd_register_err_handler(server_, HTTPD_404_NOT_FOUND, not_found_handler);
    return true;
}

void WebServer::stop() {
    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
    }
}

esp_err_t WebServer::api_handler(httpd_req_t* req) { return static_cast<WebServer*>(req->user_ctx)->handle_api(req); }

esp_err_t WebServer::handle_api(httpd_req_t* req) {
    dispatcher::Request request;
    dispatcher::Response response;
    if (!build_request(req, &request, &response)) {
        // The body may be unread, so the connection cannot be reused for another request.
        httpd_resp_set_hdr(req, "Connection", "close");
        return send_api_response(req, response);
    }

    if (req->method != HTTP_GET) ESP_LOGI(TAG, "%s %s received", request.method.c_str(), request.path.c_str());
    const int64_t start_us = esp_timer_get_time();
    const dispatcher::Response answer = api_.handle(request);
    const int64_t handled_us = esp_timer_get_time();
    const esp_err_t sent = send_api_response(req, answer);
    ESP_LOGI(TAG, "%s %s -> %d: handled in %lld ms, sent in %lld ms (err %d)", request.method.c_str(),
             request.path.c_str(), answer.status, static_cast<long long>((handled_us - start_us) / 1000),
             static_cast<long long>((esp_timer_get_time() - handled_us) / 1000), static_cast<int>(sent));
    return sent;
}
