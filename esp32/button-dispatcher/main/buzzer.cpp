#include "buzzer.hpp"

#include <vector>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

static const char* TAG = "Buzzer";

namespace {

// Room for a few sequences of the longest length.
constexpr int kQueueLength = 48;

double monotonic_s() { return static_cast<double>(esp_timer_get_time()) / 1e6; }

}  // namespace

Buzzer::Buzzer(int pin, bool active_low) : pin_(pin), active_low_(active_low), enabled_(false), queue_(nullptr) {}

bool Buzzer::init() {
    if (pin_ < 0) {
        ESP_LOGI(TAG, "Buzzer disabled (buzzer_pin < 0)");
        return true;
    }
    // The silent level is latched before the pin becomes an output, so that a buzzer which sounds on a low
    // level does not chirp during start-up.
    set_raw(false);
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << pin_;
    io.mode = GPIO_MODE_OUTPUT;
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot use GPIO%d for the buzzer", pin_);
        return false;
    }
    set_raw(false);

    queue_ = xQueueCreate(kQueueLength, sizeof(dispatcher::Pulse));
    if (queue_ == nullptr || xTaskCreate(task_entry, "buzzer", 2048, this, 3, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start the buzzer task");
        return false;
    }
    enabled_ = true;
    ESP_LOGI(TAG, "Buzzer on GPIO%d (%s)", pin_, active_low_ ? "active low" : "active high");
    return true;
}

void Buzzer::beep(dispatcher::Beep kind, int count) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const dispatcher::Pulse& pulse : scheduler_.schedule(monotonic_s(), kind, count)) {
        // A full queue only loses beeps; it never blocks the caller.
        if (xQueueSend(queue_, &pulse, 0) != pdTRUE) break;
    }
}

void Buzzer::task_entry(void* arg) { static_cast<Buzzer*>(arg)->run_task(); }

void Buzzer::run_task() {
    while (true) {
        dispatcher::Pulse pulse;
        if (xQueueReceive(queue_, &pulse, portMAX_DELAY) != pdTRUE) continue;
        const double wait_s = pulse.start_s - monotonic_s();
        if (wait_s > 0) vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>(wait_s * 1000.0 + 0.5)));
        set_raw(true);
        vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>(pulse.duration_s * 1000.0 + 0.5)));
        set_raw(false);
    }
}

void Buzzer::set_raw(bool on) { gpio_set_level(static_cast<gpio_num_t>(pin_), (on != active_low_) ? 1 : 0); }
