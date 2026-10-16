#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

struct MacEntry {
    uint8_t mac[6];
    std::string mac_str;
    int64_t first_seen_ms;
    int64_t last_seen_ms;
    uint32_t packet_count;
    bool is_static;
};

struct CapturedPacket {
    int64_t timestamp_ms;
    std::string src_mac;
    std::string dst_mac;
    uint16_t ethertype;
    uint32_t len;
};

class MacTracker {
public:
    MacTracker();
    ~MacTracker();

    // Initializes NVS and loads persisted MACs and purge state
    bool init(const uint8_t own_mac[6], const std::vector<std::string>& static_macs, bool log_rx_packets = false);

    // Called on every Ethernet packet received from PC
    // Returns true if this packet triggered a new MAC learn or purge transition
    bool on_frame_received(const uint8_t src_mac[6]);

    // Records a captured packet into the live ring buffer for debugging
    void record_packet(const uint8_t src[6], const uint8_t dst[6], uint16_t ethertype, uint32_t len);

    // Arms purge so the next packet received from PC wipes old learned MACs
    void arm_purge_on_next_rx();

    // Manually clears all learned MACs (static entries are preserved)
    void clear_learned();

    // Query methods (thread-safe)
    std::vector<MacEntry> get_all_entries() const;
    std::vector<std::string> get_mac_strings() const;
    std::vector<CapturedPacket> get_recent_packets() const;
    uint32_t get_total_rx_packets() const;
    size_t count() const;
    bool is_purge_armed() const;

    // Static conversion utilities
    static bool parse_mac(const std::string& str, uint8_t mac_out[6]);
    static std::string format_mac(const uint8_t mac[6]);

    // Upper bound on learned (non-static) MACs. When full, the least recently seen one is evicted.
    static constexpr size_t MAX_LEARNED_MACS = 16;

private:
    void load_from_nvs();
    bool load_legacy_from_nvs(nvs_handle_t handle);
    MacEntry* add_learned_locked(const uint8_t mac[6], const std::string& mac_str, int64_t now_ms);
    void request_save();
    void save_to_nvs();
    static void persist_task_entry(void* arg);
    bool is_own_mac(const uint8_t mac[6]) const;

    mutable std::mutex mutex_;
    uint8_t own_mac_[6];
    std::vector<MacEntry> entries_;
    std::vector<CapturedPacket> recent_packets_;
    uint32_t total_rx_packets_;
    std::atomic<bool> purge_on_next_rx_;
    bool log_rx_packets_;
    TaskHandle_t persist_task_;
    // Number of learned MACs found in the old one-key-per-MAC NVS layout, which are erased after
    // the first save in the current layout.
    uint32_t legacy_key_count_;
    bool legacy_keys_present_;
};
