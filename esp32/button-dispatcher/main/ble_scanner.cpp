#include "ble_scanner.hpp"

#include <cstring>
#include <string>

#include "dispatcher/bthome.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char* TAG = "BleScanner";

namespace {

constexpr int kQueueLength = 32;
constexpr size_t kMaxPayload = 48;
constexpr int kWorkerStackBytes = 6144;  // the worker decodes, logs and builds strings
constexpr uint8_t kAdTypeServiceData16 = 0x16;
constexpr uint8_t kScanDurationForever = 0;  // BLE_HS_FOREVER

BleScanner* g_scanner = nullptr;  // NimBLE's callbacks carry no context pointer we could use for this

// "AA:BB:CC:DD:EE:FF" from NimBLE's little-endian address bytes.
std::string format_mac(const uint8_t val[6]) {
    char text[18];
    std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", val[5], val[4], val[3], val[2], val[1], val[0]);
    return text;
}

}  // namespace

struct BleScanner::RawAdvert {
    uint8_t mac[6];       // little-endian, as NimBLE reports it
    int8_t rssi;
    uint8_t length;
    uint8_t data[kMaxPayload];
};

BleScanner::BleScanner(dispatcher::AppCore& app, std::function<void()> on_press_heard)
    : app_(app), on_press_heard_(std::move(on_press_heard)), queue_(nullptr), dropped_(0) {}

bool BleScanner::start(const Settings& settings) {
    settings_ = settings;
    g_scanner = this;

    queue_ = xQueueCreate(kQueueLength, sizeof(RawAdvert));
    if (queue_ == nullptr || xTaskCreate(worker_entry, "ble_worker", kWorkerStackBytes, this, 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create the scanner worker");
        return false;
    }

    const esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return false;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    return true;
}

void BleScanner::host_task(void*) {
    nimble_port_run();  // returns only when nimble_port_stop() is called
    nimble_port_freertos_deinit();
}

void BleScanner::on_sync() {
    if (g_scanner != nullptr) g_scanner->start_scan();
}

void BleScanner::on_reset(int reason) { ESP_LOGW(TAG, "Bluetooth host reset (reason %d)", reason); }

void BleScanner::start_scan() {
    struct ble_gap_disc_params params = {};
    params.itvl = static_cast<uint16_t>(settings_.interval_ms * 1000 / 625);
    params.window = static_cast<uint16_t>(settings_.window_ms * 1000 / 625);
    params.passive = settings_.active ? 0 : 1;
    // The controller's duplicate filter would hide the new packet ids of a device it has already seen.
    params.filter_duplicates = 0;
    params.limited = 0;
    params.filter_policy = BLE_HCI_SCAN_FILT_NO_WL;

    const int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, kScanDurationForever, &params, on_gap_event, this);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "Scanning for BTHome advertisements (%s, %d ms every %d ms)", settings_.active ? "active" : "passive",
                 settings_.window_ms, settings_.interval_ms);
    }
}

int BleScanner::on_gap_event(struct ble_gap_event* event, void* arg) {
    auto* self = static_cast<BleScanner*>(arg);
    switch (event->type) {
        case BLE_GAP_EVENT_DISC:
            self->handle_advert(event->disc);
            break;
        case BLE_GAP_EVENT_DISC_COMPLETE:
            // A scan should run forever; start it again if the host ended it.
            self->start_scan();
            break;
        default:
            break;
    }
    return 0;
}

// Runs in the NimBLE host task for every advertisement in range, so it only does the cheapest possible
// test: is there a 16-bit service-data block for BTHome (0xFCD2)? Everything else is dropped here.
void BleScanner::handle_advert(const struct ble_gap_disc_desc& desc) {
    size_t i = 0;
    while (i + 1 < desc.length_data) {
        const uint8_t block_length = desc.data[i];
        if (block_length == 0 || i + 1 + block_length > desc.length_data) return;
        const uint8_t type = desc.data[i + 1];
        const uint8_t* value = desc.data + i + 2;
        const size_t value_length = block_length - 1;
        if (type == kAdTypeServiceData16 && value_length >= 3 &&
            (value[0] | (value[1] << 8)) == dispatcher::bthome::kServiceUuid16) {
            RawAdvert advert;
            const size_t payload = value_length - 2;
            if (payload > kMaxPayload) return;
            std::memcpy(advert.mac, desc.addr.val, 6);
            advert.rssi = desc.rssi;
            advert.length = static_cast<uint8_t>(payload);
            std::memcpy(advert.data, value + 2, payload);
            if (xQueueSend(static_cast<QueueHandle_t>(queue_), &advert, 0) != pdTRUE) ++dropped_;
            return;
        }
        i += 1 + block_length;
    }
}

void BleScanner::worker_entry(void* arg) { static_cast<BleScanner*>(arg)->worker_loop(); }

void BleScanner::worker_loop() {
    RawAdvert advert;
    uint32_t reported_drops = 0;
    while (true) {
        if (xQueueReceive(static_cast<QueueHandle_t>(queue_), &advert, pdMS_TO_TICKS(5000)) != pdTRUE) {
            if (dropped_ != reported_drops) {
                ESP_LOGW(TAG, "%u advertisements were dropped because the queue was full",
                         static_cast<unsigned>(dropped_ - reported_drops));
                reported_drops = dropped_;
            }
            continue;
        }
        // Sensors broadcast constantly; only packets that report a press or a turn (or that a known device
        // sent encrypted, which the core logs as a problem) are worth the application's attention.
        const std::optional<dispatcher::bthome::Packet> packet = dispatcher::bthome::decode(advert.data, advert.length);
        if (!packet) continue;
        const bool has_input = !dispatcher::bthome::input_events(*packet).empty();
        if (!has_input && !packet->encrypted) continue;
        if (has_input && on_press_heard_) on_press_heard_();
        app_.handle_packet(format_mac(advert.mac), advert.rssi, advert.data, advert.length);
    }
}
