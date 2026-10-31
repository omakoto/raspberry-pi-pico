#include "buzzer.hpp"

#include <vector>

#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

static const char* TAG = "Buzzer";

namespace {

// Room for a few sequences of the longest length.
constexpr int kQueueLength = 48;

// The buzzer has the LEDC peripheral to itself (nothing else in the firmware uses it).
constexpr ledc_mode_t kSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kChannel = LEDC_CHANNEL_0;
// 10 bits is plenty for a 50% square wave and allows frequencies up to about 78 kHz.
constexpr ledc_timer_bit_t kResolution = LEDC_TIMER_10_BIT;
// A 50% duty cycle gives a passive buzzer its loudest sound; 0 holds the pin low, so no DC flows through
// the buzzer while it is silent.
constexpr uint32_t kDutyOn = 1u << (kResolution - 1);
constexpr uint32_t kDutyOff = 0;

double monotonic_s() { return static_cast<double>(esp_timer_get_time()) / 1e6; }

}  // namespace

Buzzer::Buzzer(int pin, int frequency_hz)
    : pin_(pin), frequency_hz_(frequency_hz), enabled_(false), queue_(nullptr) {}

bool Buzzer::init() {
    if (pin_ < 0) {
        ESP_LOGI(TAG, "Buzzer disabled (buzzer_pin < 0)");
        return true;
    }
    if (frequency_hz_ <= 0) {
        ESP_LOGW(TAG, "Bad buzzer_frequency_hz %d; using %d", frequency_hz_, kDefaultFrequencyHz);
        frequency_hz_ = kDefaultFrequencyHz;
    }
    ledc_timer_config_t timer = {};
    timer.speed_mode = kSpeedMode;
    timer.duty_resolution = kResolution;
    timer.timer_num = kTimer;
    timer.freq_hz = static_cast<uint32_t>(frequency_hz_);
    timer.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&timer) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot set up the buzzer's PWM timer at %d Hz", frequency_hz_);
        return false;
    }
    // The channel starts at duty 0, so the pin goes straight to a silent low level.
    ledc_channel_config_t channel = {};
    channel.gpio_num = pin_;
    channel.speed_mode = kSpeedMode;
    channel.channel = kChannel;
    channel.timer_sel = kTimer;
    channel.duty = kDutyOff;
    channel.hpoint = 0;
    if (ledc_channel_config(&channel) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot use GPIO%d for the buzzer", pin_);
        return false;
    }

    queue_ = xQueueCreate(kQueueLength, sizeof(dispatcher::Pulse));
    if (queue_ == nullptr || xTaskCreate(task_entry, "buzzer", 2048, this, 3, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start the buzzer task");
        return false;
    }
    enabled_ = true;
    ESP_LOGI(TAG, "Buzzer on GPIO%d (%d Hz)", pin_, frequency_hz_);
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
        set_tone(true);
        vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>(pulse.duration_s * 1000.0 + 0.5)));
        set_tone(false);
    }
}

void Buzzer::set_tone(bool on) {
    ledc_set_duty(kSpeedMode, kChannel, on ? kDutyOn : kDutyOff);
    ledc_update_duty(kSpeedMode, kChannel);
}
