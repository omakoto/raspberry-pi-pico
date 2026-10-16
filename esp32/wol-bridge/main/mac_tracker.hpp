#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

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

private:
    void load_from_nvs();
    void save_learned_to_nvs();
    bool is_own_mac(const uint8_t mac[6]) const;

    mutable std::mutex mutex_;
    uint8_t own_mac_[6];
    std::vector<MacEntry> entries_;
    std::vector<CapturedPacket> recent_packets_;
    uint32_t total_rx_packets_;
    std::atomic<bool> purge_on_next_rx_;
    bool log_rx_packets_;
};
