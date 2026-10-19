#include "esp_platform.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"

static const char* TAG = "EspPlatform";

namespace {

constexpr const char* kStatePath = "/state";
constexpr const char* kStatePartition = "state";
constexpr int kWorkerStackBytes = 10 * 1024;   // a TLS handshake needs several KB of stack
constexpr int kWorkQueueLength = 16;
// The wall clock counts from 1970 until SNTP sets it; anything before this is "not set yet".
constexpr int64_t kEarliestPlausibleUnixTime = 1735689600;  // 2025-01-01

const char* reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 422: return "Unprocessable Entity";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "";
    }
}

struct BodyCollector {
    std::string* body;
    size_t limit;
};

esp_err_t http_event_handler(esp_http_client_event_t* event) {
    if (event->event_id == HTTP_EVENT_ON_DATA && event->user_data != nullptr && event->data_len > 0) {
        auto* collector = static_cast<BodyCollector*>(event->user_data);
        if (collector->body->size() < collector->limit) {
            const size_t room = collector->limit - collector->body->size();
            collector->body->append(static_cast<const char*>(event->data),
                                    std::min(static_cast<size_t>(event->data_len), room));
        }
    }
    return ESP_OK;
}

esp_http_client_method_t to_client_method(const std::string& method) {
    if (method == "POST") return HTTP_METHOD_POST;
    if (method == "PUT") return HTTP_METHOD_PUT;
    if (method == "DELETE") return HTTP_METHOD_DELETE;
    return HTTP_METHOD_GET;
}

std::string read_file(const std::string& path, bool* ok) {
    std::string text;
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        *ok = false;
        return text;
    }
    char buffer[512];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, count);
    fclose(file);
    *ok = true;
    return text;
}

}  // namespace

EspPlatform::EspPlatform() : state_wl_(WL_INVALID_HANDLE), state_mounted_(false), work_queue_(nullptr) {}

bool EspPlatform::init() {
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,  // the first boot finds an empty partition
        .max_files = 4,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(kStatePath, kStatePartition, &mount_config, &state_wl_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount the state partition: %s (settings will not be saved)", esp_err_to_name(err));
    } else {
        state_mounted_ = true;
    }

    work_queue_ = xQueueCreate(kWorkQueueLength, sizeof(std::function<void()>*));
    if (work_queue_ == nullptr ||
        xTaskCreate(worker_entry, "dispatch_work", kWorkerStackBytes, this, 4, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start the background worker");
        return false;
    }
    return state_mounted_;
}

double EspPlatform::monotonic_s() const { return static_cast<double>(esp_timer_get_time()) / 1e6; }

bool EspPlatform::wall_clock_valid() const { return wall_time_ms() / 1000 >= kEarliestPlausibleUnixTime; }

int64_t EspPlatform::wall_time_ms() const {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

void EspPlatform::random_bytes(uint8_t* out, size_t length) { esp_fill_random(out, length); }

std::string EspPlatform::hmac_sha256(const std::string& key, const std::string& message) {
    unsigned char out[32];
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (mbedtls_md_hmac(info, reinterpret_cast<const unsigned char*>(key.data()), key.size(),
                        reinterpret_cast<const unsigned char*>(message.data()), message.size(), out) != 0) {
        ESP_LOGE(TAG, "HMAC-SHA256 failed");
        return std::string(32, '\0');
    }
    return std::string(reinterpret_cast<const char*>(out), sizeof(out));
}

std::string EspPlatform::pbkdf2_hmac_sha256(const std::string& password, const std::string& salt, int iterations,
                                            size_t output_length) {
    std::string out(output_length, '\0');
    const int64_t started_us = esp_timer_get_time();
    const int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(
        MBEDTLS_MD_SHA256, reinterpret_cast<const unsigned char*>(password.data()), password.size(),
        reinterpret_cast<const unsigned char*>(salt.data()), salt.size(), static_cast<unsigned int>(iterations),
        static_cast<uint32_t>(output_length), reinterpret_cast<unsigned char*>(out.data()));
    if (rc != 0) {
        ESP_LOGE(TAG, "PBKDF2 failed: -0x%04x", -rc);
        return std::string(output_length, '\0');
    }
    ESP_LOGI(TAG, "PBKDF2 with %d iterations took %lld ms", iterations,
             static_cast<long long>((esp_timer_get_time() - started_us) / 1000));
    return out;
}

bool EspPlatform::load_document(const std::string& name, std::string* text) {
    if (!state_mounted_) return false;
    const std::string path = std::string(kStatePath) + "/" + name;
    const std::string temp = path + ".tmp";
    bool ok = false;
    *text = read_file(path, &ok);
    if (ok) return true;
    // A save that was interrupted after the old file was removed but before the new one was renamed leaves
    // only the complete temporary file; finish that rename.
    struct stat info;
    if (stat(temp.c_str(), &info) == 0 && rename(temp.c_str(), path.c_str()) == 0) {
        ESP_LOGW(TAG, "Recovered %s from an interrupted save", name.c_str());
        *text = read_file(path, &ok);
    }
    return ok;
}

bool EspPlatform::save_document(const std::string& name, const std::string& text) {
    if (!state_mounted_) return false;
    const std::string path = std::string(kStatePath) + "/" + name;
    const std::string temp = path + ".tmp";
    FILE* file = fopen(temp.c_str(), "wb");
    if (file == nullptr) {
        ESP_LOGE(TAG, "Cannot create %s: errno %d", temp.c_str(), errno);
        return false;
    }
    const bool written = fwrite(text.data(), 1, text.size(), file) == text.size() && fflush(file) == 0 &&
                         fsync(fileno(file)) == 0;
    fclose(file);
    if (!written) {
        ESP_LOGE(TAG, "Cannot write %s", temp.c_str());
        remove(temp.c_str());
        return false;
    }
    // FAT cannot rename over an existing file, so the old one goes first; the complete temporary file
    // covers the short gap (see load_document).
    remove(path.c_str());
    if (rename(temp.c_str(), path.c_str()) != 0) {
        ESP_LOGE(TAG, "Cannot rename %s: errno %d", temp.c_str(), errno);
        return false;
    }
    return true;
}

dispatcher::HttpResponse EspPlatform::http_request(const std::string& method, const std::string& url,
                                                   const dispatcher::HeaderList& headers, const std::string& body,
                                                   double timeout_s, size_t max_response_bytes) {
    dispatcher::HttpResponse result;
    BodyCollector collector{&result.body, max_response_bytes};

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = to_client_method(method);
    config.timeout_ms = static_cast<int>(timeout_s * 1000);
    config.event_handler = http_event_handler;
    config.user_data = &collector;
    config.crt_bundle_attach = esp_crt_bundle_attach;  // verifies https servers against the bundled roots
    config.buffer_size = 2048;
    config.buffer_size_tx = 1024;
    config.max_redirection_count = 10;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        result.failure = dispatcher::HttpResponse::Failure::kConnect;
        return result;
    }
    for (const auto& header : headers) esp_http_client_set_header(client, header.first.c_str(), header.second.c_str());
    if (!body.empty()) esp_http_client_set_post_field(client, body.data(), static_cast<int>(body.size()));

    const esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        const int socket_errno = esp_http_client_get_errno(client);
        const bool timed_out = err == ESP_ERR_TIMEOUT || err == ESP_ERR_HTTP_EAGAIN || socket_errno == ETIMEDOUT ||
                               socket_errno == EAGAIN || socket_errno == EWOULDBLOCK;
        result.failure = timed_out ? dispatcher::HttpResponse::Failure::kTimeout
                                   : dispatcher::HttpResponse::Failure::kConnect;
        ESP_LOGW(TAG, "HTTP request failed: %s (errno %d)", esp_err_to_name(err), socket_errno);
    } else {
        result.status = esp_http_client_get_status_code(client);
        result.reason = reason_phrase(result.status);
    }
    esp_http_client_cleanup(client);
    return result;
}

void EspPlatform::run_in_background(std::function<void()> task) {
    auto* heap_task = new std::function<void()>(std::move(task));
    // A full queue means the worker is badly behind (for example a dead server on every press); dropping
    // the newest task is better than blocking Bluetooth processing.
    if (work_queue_ == nullptr || xQueueSend(work_queue_, &heap_task, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Background queue full; dropping a task");
        delete heap_task;
    }
}

void EspPlatform::worker_entry(void* arg) { static_cast<EspPlatform*>(arg)->worker_loop(); }

void EspPlatform::worker_loop() {
    while (true) {
        std::function<void()>* task = nullptr;
        if (xQueueReceive(work_queue_, &task, portMAX_DELAY) == pdTRUE && task != nullptr) {
            (*task)();
            delete task;
        }
    }
}
