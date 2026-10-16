#include "mac_tracker.hpp"
#include <cstdio>
#include <cstring>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char* TAG = "MacTracker";
static const char* NVS_NAMESPACE = "wol_macs";

MacTracker::MacTracker() : own_mac_{}, total_rx_packets_(0), purge_on_next_rx_(false), log_rx_packets_(false) {}

MacTracker::~MacTracker() {}

bool MacTracker::parse_mac(const std::string& str, uint8_t mac_out[6]) {
    unsigned int bytes[6];
    if (sscanf(str.c_str(), "%x:%x:%x:%x:%x:%x",
               &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4], &bytes[5]) == 6 ||
        sscanf(str.c_str(), "%x-%x-%x-%x-%x-%x",
               &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4], &bytes[5]) == 6) {
        for (int i = 0; i < 6; ++i) {
            mac_out[i] = static_cast<uint8_t>(bytes[i]);
        }
        return true;
    }
    return false;
}

std::string MacTracker::format_mac(const uint8_t mac[6]) {
    char buf[20];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(buf);
}

bool MacTracker::is_own_mac(const uint8_t mac[6]) const {
    return memcmp(own_mac_, mac, 6) == 0;
}

bool MacTracker::init(const uint8_t own_mac[6], const std::vector<std::string>& static_macs, bool log_rx_packets) {
    memcpy(own_mac_, own_mac, 6);
    log_rx_packets_ = log_rx_packets;

    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();

    // 1. Add static configured MACs
    int64_t now_ms = esp_timer_get_time() / 1000;
    for (const auto& mac_str : static_macs) {
        uint8_t raw[6];
        if (parse_mac(mac_str, raw)) {
            MacEntry entry = {};
            memcpy(entry.mac, raw, 6);
            entry.mac_str = format_mac(raw);
            entry.first_seen_ms = now_ms;
            entry.last_seen_ms = now_ms;
            entry.packet_count = 0;
            entry.is_static = true;
            entries_.push_back(entry);
            ESP_LOGI(TAG, "Configured static target MAC: %s", entry.mac_str.c_str());
        } else if (!mac_str.empty()) {
            ESP_LOGW(TAG, "Failed to parse static MAC: '%s'", mac_str.c_str());
        }
    }

    // 2. Load learned MACs and purge state from NVS
    load_from_nvs();

    return true;
}

void MacTracker::load_from_nvs() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No persisted MAC database in NVS (nvs_open returned %s)", esp_err_to_name(err));
        return;
    }

    uint8_t purge_armed = 0;
    if (nvs_get_u8(handle, "purge_arm", &purge_armed) == ESP_OK) {
        purge_on_next_rx_.store(purge_armed != 0, std::memory_order_relaxed);
        if (purge_armed) {
            ESP_LOGI(TAG, "NVS state: Purge is armed on next PC packet.");
        }
    }

    uint32_t count = 0;
    if (nvs_get_u32(handle, "count", &count) != ESP_OK || count == 0) {
        nvs_close(handle);
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    for (uint32_t i = 0; i < count; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "m%lu", static_cast<unsigned long>(i));
        size_t required_size = 0;
        if (nvs_get_str(handle, key, nullptr, &required_size) == ESP_OK && required_size > 0) {
            std::vector<char> str_buf(required_size);
            if (nvs_get_str(handle, key, str_buf.data(), &required_size) == ESP_OK) {
                std::string mac_str(str_buf.data());
                uint8_t raw[6];
                if (parse_mac(mac_str, raw)) {
                    // Check if already in static list
                    bool exists = false;
                    for (const auto& e : entries_) {
                        if (memcmp(e.mac, raw, 6) == 0) {
                            exists = true;
                            break;
                        }
                    }
                    if (!exists) {
                        MacEntry entry = {};
                        memcpy(entry.mac, raw, 6);
                        entry.mac_str = format_mac(raw);
                        entry.first_seen_ms = now_ms;
                        entry.last_seen_ms = now_ms;
                        entry.packet_count = 0;
                        entry.is_static = false;
                        entries_.push_back(entry);
                        ESP_LOGI(TAG, "Loaded persisted learned MAC [%lu]: %s",
                                 static_cast<unsigned long>(i), entry.mac_str.c_str());
                    }
                }
            }
        }
    }
    nvs_close(handle);
}

void MacTracker::save_learned_to_nvs() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return;
    }

    // Erase existing keys in namespace
    nvs_erase_all(handle);

    uint8_t purge_armed = purge_on_next_rx_.load(std::memory_order_relaxed) ? 1 : 0;
    nvs_set_u8(handle, "purge_arm", purge_armed);

    uint32_t learned_idx = 0;
    for (const auto& e : entries_) {
        if (!e.is_static) {
            char key[16];
            snprintf(key, sizeof(key), "m%lu", static_cast<unsigned long>(learned_idx));
            nvs_set_str(handle, key, e.mac_str.c_str());
            learned_idx++;
        }
    }
    nvs_set_u32(handle, "count", learned_idx);
    nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "Persisted %lu learned MAC(s) to NVS (purge_arm=%d)",
             static_cast<unsigned long>(learned_idx), purge_armed);
}

bool MacTracker::on_frame_received(const uint8_t src_mac[6]) {
    // 1. Validity checks
    // Reject multicast (bit 0 of 1st octet is 1) and broadcast (FF:FF:FF:FF:FF:FF)
    if (src_mac[0] & 0x01) {
        return false;
    }

    // Reject all zeros
    static const uint8_t ZERO_MAC[6] = {0, 0, 0, 0, 0, 0};
    if (memcmp(src_mac, ZERO_MAC, 6) == 0) {
        return false;
    }

    // Reject our own W5500 MAC
    if (is_own_mac(src_mac)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now_ms = esp_timer_get_time() / 1000;
    std::string mac_str = format_mac(src_mac);

    // 2. Check if purge on next rx is armed
    if (purge_on_next_rx_.load(std::memory_order_relaxed)) {
        ESP_LOGI(TAG, "PC Wake packet received! Purging previous learned MACs, starting fresh with: %s",
                 mac_str.c_str());

        // Discard all non-static entries
        std::vector<MacEntry> retained_static;
        for (const auto& e : entries_) {
            if (e.is_static) {
                retained_static.push_back(e);
            }
        }
        entries_ = std::move(retained_static);
        purge_on_next_rx_.store(false, std::memory_order_relaxed);

        // Add this fresh MAC
        bool already_static = false;
        for (auto& e : entries_) {
            if (memcmp(e.mac, src_mac, 6) == 0) {
                e.last_seen_ms = now_ms;
                e.packet_count++;
                already_static = true;
                break;
            }
        }
        if (!already_static) {
            MacEntry entry = {};
            memcpy(entry.mac, src_mac, 6);
            entry.mac_str = mac_str;
            entry.first_seen_ms = now_ms;
            entry.last_seen_ms = now_ms;
            entry.packet_count = 1;
            entry.is_static = false;
            entries_.push_back(entry);
        }

        save_learned_to_nvs();
        return true;
    }

    // 3. Normal packet reception: check if MAC already known
    for (auto& e : entries_) {
        if (memcmp(e.mac, src_mac, 6) == 0) {
            e.last_seen_ms = now_ms;
            e.packet_count++;
            if (log_rx_packets_) {
                ESP_LOGI(TAG, "  [Packet from known MAC] %s (packet count: %lu)",
                         mac_str.c_str(), static_cast<unsigned long>(e.packet_count));
            }
            return false;
        }
    }

    // 4. Newly discovered MAC address
    MacEntry entry = {};
    memcpy(entry.mac, src_mac, 6);
    entry.mac_str = mac_str;
    entry.first_seen_ms = now_ms;
    entry.last_seen_ms = now_ms;
    entry.packet_count = 1;
    entry.is_static = false;
    entries_.push_back(entry);

    ESP_LOGI(TAG, ">>> [NEW MAC RECORDED] %s (Total tracked: %u)",
             mac_str.c_str(), static_cast<unsigned>(entries_.size()));

    save_learned_to_nvs();
    return true;
}

void MacTracker::record_packet(const uint8_t src[6], const uint8_t dst[6], uint16_t ethertype, uint32_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    total_rx_packets_++;

    CapturedPacket pkt = {};
    pkt.timestamp_ms = esp_timer_get_time() / 1000;
    pkt.src_mac = format_mac(src);
    pkt.dst_mac = format_mac(dst);
    pkt.ethertype = ethertype;
    pkt.len = len;

    recent_packets_.push_back(pkt);
    if (recent_packets_.size() > 20) {
        recent_packets_.erase(recent_packets_.begin());
    }
}

std::vector<CapturedPacket> MacTracker::get_recent_packets() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return recent_packets_;
}

uint32_t MacTracker::get_total_rx_packets() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_rx_packets_;
}

void MacTracker::arm_purge_on_next_rx() {
    purge_on_next_rx_.store(true, std::memory_order_relaxed);

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, "purge_arm", 1);
        nvs_commit(handle);
        nvs_close(handle);
    }
    ESP_LOGI(TAG, "Armed purge on next PC packet. When PC wakes, old entries will be cleared.");
}

void MacTracker::clear_learned() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MacEntry> retained_static;
    for (const auto& e : entries_) {
        if (e.is_static) {
            retained_static.push_back(e);
        }
    }
    entries_ = std::move(retained_static);
    purge_on_next_rx_.store(false, std::memory_order_relaxed);
    save_learned_to_nvs();
    ESP_LOGI(TAG, "Manually cleared all learned MAC addresses.");
}

std::vector<MacEntry> MacTracker::get_all_entries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
}

std::vector<std::string> MacTracker::get_mac_strings() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    result.reserve(entries_.size());
    for (const auto& e : entries_) {
        result.push_back(e.mac_str);
    }
    return result;
}

size_t MacTracker::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

bool MacTracker::is_purge_armed() const {
    return purge_on_next_rx_.load(std::memory_order_relaxed);
}
