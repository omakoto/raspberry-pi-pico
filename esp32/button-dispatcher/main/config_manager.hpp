#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include "dispatcher/config_secrets.hpp"
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
    // A file that holds secrets in the clear, to be rewritten once they are stored.
    struct ScrubPlan {
        std::string path;
        std::string text;
    };

    bool parse_file(const std::string& path, std::vector<ScrubPlan>* plans);
    void hide_secrets(const std::vector<ScrubPlan>& plans);
    static std::vector<dispatcher::ConfigEntry> load_stored_secrets();
    static bool store_secrets(const std::vector<dispatcher::ConfigEntry>& secrets);
    static std::string trim(const std::string& s);

    std::unordered_map<std::string, std::string> config_map_;
    std::string base_path_;
    wl_handle_t wl_handle_;
    bool initialized_;
};
