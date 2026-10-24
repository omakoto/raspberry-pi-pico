#include "http_server.hpp"
#include <cstdio>
#include <cstring>
#include <sstream>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "dual_logger.hpp"
#include "log_ring.hpp"

static const char* TAG = "HttpServer";

HttpServer::HttpServer()
    : server_(nullptr),
      tracker_(nullptr),
      w5500_(nullptr),
      led_(nullptr),
      wifi_(nullptr),
      running_(false) {}

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::start(int port, MacTracker* tracker, W5500Driver* w5500, StatusLed* led, WifiManager* wifi, const std::string& hostname) {
    stop();

    tracker_ = tracker;
    w5500_ = w5500;
    led_ = led;
    wifi_ = wifi;
    hostname_ = hostname;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_uri_handlers = 8;
    config.stack_size = 8192;

    ESP_LOGI(TAG, "Starting HTTP server on port %d...", port);
    esp_err_t ret = httpd_start(&server_, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return false;
    }

    // Register URI handlers
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = &HttpServer::root_get_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &root_uri);

    httpd_uri_t wake_get_uri = {
        .uri = "/wake",
        .method = HTTP_GET,
        .handler = &HttpServer::wake_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &wake_get_uri);

    httpd_uri_t wake_post_uri = {
        .uri = "/wake",
        .method = HTTP_POST,
        .handler = &HttpServer::wake_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &wake_post_uri);

    httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = &HttpServer::status_get_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &status_uri);

    // Purge is POST-only so that link previews and browser prefetching can't wipe learned MACs.
    httpd_uri_t purge_post_uri = {
        .uri = "/purge",
        .method = HTTP_POST,
        .handler = &HttpServer::purge_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &purge_post_uri);

    httpd_uri_t log_uri = {
        .uri = "/log",
        .method = HTTP_GET,
        .handler = &HttpServer::log_get_handler,
        .user_ctx = this
    };
    httpd_register_uri_handler(server_, &log_uri);

    running_ = true;
    ESP_LOGI(TAG, "HTTP Server listening at http://%s.local:%d/", hostname_.c_str(), port);
    return true;
}

void HttpServer::stop() {
    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
    }
    running_ = false;
}

esp_err_t HttpServer::root_get_handler(httpd_req_t* req) {
    auto* self = static_cast<HttpServer*>(req->user_ctx);

    bool link_up = self->w5500_ ? self->w5500_->is_link_up() : false;
    bool purge_armed = self->tracker_ ? self->tracker_->is_purge_armed() : false;
    std::string ip_str = self->wifi_ ? self->wifi_->get_ip_address() : "0.0.0.0";
    auto entries = self->tracker_ ? self->tracker_->get_all_entries() : std::vector<MacEntry>();

    int64_t now_ms = esp_timer_get_time() / 1000;

    auto recent_pkts = self->tracker_ ? self->tracker_->get_recent_packets() : std::vector<CapturedPacket>();
    uint32_t total_pkts = self->tracker_ ? self->tracker_->get_total_rx_packets() : 0;

    std::stringstream html;
    html << "<!DOCTYPE html><html><head><meta charset='utf-8'>"
         << "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         << "<title>ESP32 Wake-on-LAN Bridge</title>"
         << "<style>"
         << "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#f0f2f5;color:#1c1e21;margin:0;padding:20px;}"
         << ".container{max-width:720px;margin:0 auto;background:#fff;padding:24px;border-radius:12px;box-shadow:0 4px 16px rgba(0,0,0,0.08);}"
         << "h1{margin-top:0;font-size:22px;color:#111827;display:flex;align-items:center;gap:8px;}"
         << ".cards{display:grid;grid-template-columns:1fr 1fr 1fr;gap:12px;margin:18px 0;}"
         << ".card{padding:12px;background:#f8fafc;border-radius:8px;border:1px solid #e2e8f0;}"
         << ".card-label{font-size:11px;color:#64748b;text-transform:uppercase;font-weight:600;}"
         << ".card-val{font-size:14px;font-weight:700;margin-top:4px;}"
         << ".badge{display:inline-block;padding:2px 8px;border-radius:12px;font-size:12px;font-weight:600;}"
         << ".badge-ok{background:#dcfce7;color:#166534;}"
         << ".badge-warn{background:#fef9c3;color:#854d0e;}"
         << ".badge-err{background:#fee2e2;color:#991b1b;}"
         << ".actions{display:flex;gap:10px;margin:20px 0;}"
         << ".btn{flex:1;padding:12px;border:none;border-radius:8px;font-weight:600;font-size:15px;cursor:pointer;text-align:center;text-decoration:none;display:inline-block;}"
         << ".btn-wake{background:#2563eb;color:#fff;}"
         << ".btn-wake:hover{background:#1d4ed8;}"
         << ".btn-clear{background:#e2e8f0;color:#334155;}"
         << ".btn-clear:hover{background:#cbd5e1;}"
         << "table{width:100%;border-collapse:collapse;margin-top:10px;font-size:13px;}"
         << "th,td{padding:8px 10px;text-align:left;border-bottom:1px solid #e2e8f0;}"
         << "th{background:#f8fafc;color:#475569;font-weight:600;}"
         << "code{background:#f1f5f9;padding:2px 6px;border-radius:4px;font-family:monospace;font-size:12px;}"
         << "</style></head><body><div class='container'>"
         << "<h1><span>\u26A1</span> ESP32 Wake-on-LAN Bridge</h1>"
         << "<div class='cards'>"
         << "<div class='card'><div class='card-label'>Ethernet Link</div><div class='card-val'>"
         << (link_up ? "<span class='badge badge-ok'>LINK UP</span>" : "<span class='badge badge-err'>LINK DOWN</span>")
         << "</div></div>"
         << "<div class='card'><div class='card-label'>Purge State</div><div class='card-val'>"
         << (purge_armed ? "<span class='badge badge-warn'>ARMED (Next RX)</span>" : "<span class='badge badge-ok'>IDLE (Tracking)</span>")
         << "</div></div>"
         << "<div class='card'><div class='card-label'>Total Rx Packets</div><div class='card-val'>" << total_pkts << "</div></div>"
         << "<div class='card'><div class='card-label'>Wi-Fi IP</div><div class='card-val'>" << ip_str << "</div></div>"
         << "<div class='card'><div class='card-label'>Hostname</div><div class='card-val'>" << self->hostname_ << ".local</div></div>"
         << "<div class='card'><div class='card-label'>Tracked MACs</div><div class='card-val'>" << entries.size() << "</div></div>"
         << "</div>"
         << "<div class='actions'>"
         << "<form method='POST' action='/wake' style='flex:2;'><button class='btn btn-wake' type='submit'>\u26A1 Send Wake-on-LAN</button></form>"
         << "<form method='POST' action='/purge' style='flex:1;'><button class='btn btn-clear' type='submit'>Purge Learned</button></form>"
         << "</div>"
         << "<h3>Tracked MAC Addresses (" << entries.size() << ")</h3>";

    if (entries.empty()) {
        html << "<p style='color:#64748b;font-style:italic;'>No MAC addresses recorded yet. Connect LAN to hub or PC.</p>";
    } else {
        html << "<table><thead><tr><th>MAC Address</th><th>Type</th><th>Packets</th><th>Last Seen</th></tr></thead><tbody>";
        for (const auto& e : entries) {
            int64_t sec_ago = (now_ms - e.last_seen_ms) / 1000;
            html << "<tr><td><code>" << e.mac_str << "</code></td>"
                 << "<td>" << (e.is_static ? "<span class='badge badge-warn'>Static</span>" : "<span class='badge badge-ok'>Learned</span>") << "</td>"
                 << "<td>" << e.packet_count << "</td>"
                 << "<td>" << sec_ago << "s ago</td></tr>";
        }
        html << "</tbody></table>";
    }

    html << "<h3 style='margin-top:24px;'>Recent Captured Ethernet Frames (" << recent_pkts.size() << ")</h3>";
    if (recent_pkts.empty()) {
        html << "<p style='color:#64748b;font-style:italic;'>No packets captured yet.</p>";
    } else {
        html << "<table><thead><tr><th>Time</th><th>Source MAC</th><th>Destination MAC</th><th>Type</th><th>Size</th></tr></thead><tbody>";
        for (auto it = recent_pkts.rbegin(); it != recent_pkts.rend(); ++it) {
            int64_t sec_ago = (now_ms - it->timestamp_ms) / 1000;
            char type_str[10];
            snprintf(type_str, sizeof(type_str), "0x%04X", it->ethertype);
            html << "<tr><td>" << sec_ago << "s ago</td>"
                 << "<td><code>" << it->src_mac << "</code></td>"
                 << "<td><code>" << it->dst_mac << "</code></td>"
                 << "<td><code>" << type_str << "</code></td>"
                 << "<td>" << it->len << " B</td></tr>";
        }
        html << "</tbody></table>";
    }

    html << "<hr style='margin-top:24px;border:none;border-top:1px solid #e2e8f0;'>"
         << "<p style='font-size:12px;color:#94a3b8;'>Direct endpoint: <code>GET /wake</code> | JSON: <code>GET /status</code> | <a href='/log'>Log</a> (includes the previous run)</p>"
         << "</div></body></html>";

    std::string s = html.str();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, s.data(), s.size());
}

esp_err_t HttpServer::wake_handler(httpd_req_t* req) {
    auto* self = static_cast<HttpServer*>(req->user_ctx);

    if (self->led_) {
        self->led_->set_state(LedState::WOL_FIRING);
    }

    auto entries = self->tracker_ ? self->tracker_->get_all_entries() : std::vector<MacEntry>();
    bool link_up = self->w5500_ ? self->w5500_->is_link_up() : false;

    size_t dispatched = 0;
    if (self->w5500_) {
        dispatched = self->w5500_->send_wol_all(entries);
    }

    // Arm purge on next RX as requested: after sending WOL packet and when we receive any packet,
    // we discard all old entries and start over with that single address.
    if (self->tracker_) {
        self->tracker_->arm_purge_on_next_rx();
    }

    if (self->led_) {
        self->led_->set_state(LedState::READY);
    }

    // Check if client expects JSON
    char buf[128];
    bool wants_json = false;
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK && strstr(buf, "format=json")) {
        wants_json = true;
    }
    if (httpd_req_get_hdr_value_str(req, "Accept", buf, sizeof(buf)) == ESP_OK && strstr(buf, "application/json")) {
        wants_json = true;
    }

    if (wants_json) {
        const char* status = "ok";
        if (dispatched == 0) {
            status = !link_up ? "link_down" : "no_targets";
            // A non-2xx status lets scripts detect the failure (e.g. with curl -f).
            httpd_resp_set_status(req, "503 Service Unavailable");
        }
        std::stringstream json;
        json << "{\"status\":\"" << status << "\",\"dispatched\":" << dispatched
             << ",\"link_up\":" << (link_up ? "true" : "false")
             << ",\"purge_armed\":true,\"macs\":[";
        for (size_t i = 0; i < entries.size(); ++i) {
            if (i > 0) json << ",";
            json << "\"" << entries[i].mac_str << "\"";
        }
        json << "]}";
        std::string s = json.str();
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, s.data(), s.size());
    }

    // Redirect back to root dashboard
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, nullptr, 0);
}

esp_err_t HttpServer::status_get_handler(httpd_req_t* req) {
    auto* self = static_cast<HttpServer*>(req->user_ctx);

    bool link_up = self->w5500_ ? self->w5500_->is_link_up() : false;
    bool purge_armed = self->tracker_ ? self->tracker_->is_purge_armed() : false;
    std::string ip_str = self->wifi_ ? self->wifi_->get_ip_address() : "0.0.0.0";
    auto entries = self->tracker_ ? self->tracker_->get_all_entries() : std::vector<MacEntry>();
    auto recent_pkts = self->tracker_ ? self->tracker_->get_recent_packets() : std::vector<CapturedPacket>();
    uint32_t total_pkts = self->tracker_ ? self->tracker_->get_total_rx_packets() : 0;

    std::stringstream json;
    json << "{\"hostname\":\"" << self->hostname_ << "\""
         << ",\"wifi_ip\":\"" << ip_str << "\""
         << ",\"ethernet_link\":\"" << (link_up ? "UP" : "DOWN") << "\""
         << ",\"purge_armed\":" << (purge_armed ? "true" : "false")
         << ",\"mac_count\":" << entries.size()
         << ",\"total_packets\":" << total_pkts
         << ",\"entries\":[";

    for (size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) json << ",";
        json << "{\"mac\":\"" << entries[i].mac_str << "\""
             << ",\"is_static\":" << (entries[i].is_static ? "true" : "false")
             << ",\"packets\":" << entries[i].packet_count << "}";
    }
    json << "],\"recent_packets\":[";
    for (size_t i = 0; i < recent_pkts.size(); ++i) {
        if (i > 0) json << ",";
        json << "{\"src\":\"" << recent_pkts[i].src_mac << "\""
             << ",\"dst\":\"" << recent_pkts[i].dst_mac << "\""
             << ",\"type\":" << recent_pkts[i].ethertype
             << ",\"len\":" << recent_pkts[i].len << "}";
    }
    json << "]}";

    std::string s = json.str();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, s.data(), s.size());
}

esp_err_t HttpServer::purge_handler(httpd_req_t* req) {
    auto* self = static_cast<HttpServer*>(req->user_ctx);

    if (self->tracker_) {
        self->tracker_->clear_learned();
    }

    // Redirect back to root
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, nullptr, 0);
}

// ESP_LOG* colors its lines with ANSI escape sequences, which a browser would show as garbage.
static std::string strip_ansi(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\x1b' && i + 1 < in.size() && in[i + 1] == '[') {
            i += 2;
            while (i < in.size() && !(in[i] >= '@' && in[i] <= '~')) ++i;
            continue;
        }
        out += in[i];
    }
    return out;
}

esp_err_t HttpServer::log_get_handler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    std::string head = dual_logger_boot_report() + "\n\n===== Log of this run =====\n";
    httpd_resp_send_chunk(req, head.data(), head.size());
    std::string current = strip_ansi(LogRing::snapshot());
    httpd_resp_send_chunk(req, current.data(), current.size());
    if (LogRing::previous_valid()) {
        static const char prev_head[] = "\n===== Log of the previous run (end) =====\n";
        httpd_resp_send_chunk(req, prev_head, sizeof(prev_head) - 1);
        std::string prev = strip_ansi(LogRing::previous_log());
        httpd_resp_send_chunk(req, prev.data(), prev.size());
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
}
