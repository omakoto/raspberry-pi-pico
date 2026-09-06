#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <utility>
#include "wear_levelling.h"

class ConfigManager {
public:
    ConfigManager();
    ~ConfigManager();

    bool init(const char* base_path = "/spiflash", const char* partition_label = "storage");
    bool load(const std::string& config_path = "/spiflash/config.toml",
              const std::string& override_path = "/spiflash/config-override.toml");

    std::string get_string(const std::string& key, const std::string& default_val = "") const;
    int get_int(const std::string& key, int default_val = 0) const;
    bool get_bool(const std::string& key, bool default_val = false) const;
    bool has_key(const std::string& key) const;

    std::vector<std::pair<std::string, std::string>> get_wifi_ap_list() const;
    wl_handle_t get_wl_handle() const { return wl_handle_; }

private:
    bool parse_file(const std::string& path);
    static std::string trim(const std::string& s);

    std::unordered_map<std::string, std::string> config_map_;
    std::string base_path_;
    wl_handle_t wl_handle_;
    bool initialized_;
};
