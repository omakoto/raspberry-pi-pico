#pragma once

#include <functional>
#include <string>

#include "dispatcher/platform.hpp"
#include "buzzer.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// The ESP-IDF implementation of dispatcher::Platform: esp_timer and SNTP time, hardware-backed mbedtls
// crypto, the private `state` FAT partition for the stored documents, esp_http_client for the network, and a
// single worker task for background work.
class EspPlatform : public dispatcher::Platform {
public:
    EspPlatform();

    // Mounts the `state` partition (formatting it the first time) and starts the background worker.
    bool init();

    double monotonic_s() const override;
    bool wall_clock_valid() const override;
    int64_t wall_time_ms() const override;

    void random_bytes(uint8_t* out, size_t length) override;
    std::string hmac_sha256(const std::string& key, const std::string& message) override;
    std::string pbkdf2_hmac_sha256(const std::string& password, const std::string& salt, int iterations,
                                   size_t output_length) override;

    bool load_document(const std::string& name, std::string* text) override;
    bool save_document(const std::string& name, const std::string& text) override;

    dispatcher::HttpResponse http_request(const std::string& method, const std::string& url,
                                          const dispatcher::HeaderList& headers, const std::string& body,
                                          double timeout_s, size_t max_response_bytes) override;

    std::string device_mac() const override;
    void apply_hostname(const std::string& hostname) override;

    void run_in_background(std::function<void()> task) override;
    void beep(dispatcher::Beep kind, int count) override;

    // The buzzer to sound beeps on; without one, beeps are ignored. Not owned.
    void set_buzzer(Buzzer* buzzer) { buzzer_ = buzzer; }
    // What apply_hostname does (renaming mDNS and the Wi-Fi interface); set once the network is up.
    void set_hostname_handler(std::function<void(const std::string&)> handler) { hostname_handler_ = std::move(handler); }

private:
    static void worker_entry(void* arg);
    void worker_loop();

    bool state_mounted_;
    Buzzer* buzzer_ = nullptr;
    std::function<void(const std::string&)> hostname_handler_;
    QueueHandle_t work_queue_;
};
