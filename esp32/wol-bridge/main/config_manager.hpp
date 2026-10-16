#pragma once

#include <string>
#include <vector>
#include <unordered_map>
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
    std::vector<std::string> get_string_list(const std::string& key) const;

    // Unmounts the FATFS partition and releases its wear-levelling handle, so the partition can be
    // handed over to the USB MSC driver. Parsed values remain available.
    void unmount();

private:
    bool parse_file(const std::string& path);
    static std::string trim(const std::string& s);

    std::unordered_map<std::string, std::string> config_map_;
    std::string base_path_;
    wl_handle_t wl_handle_;
    bool initialized_;
};
