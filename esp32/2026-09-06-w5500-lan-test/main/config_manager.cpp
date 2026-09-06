#include "config_manager.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

static const char* TAG = "ConfigManager";

ConfigManager::ConfigManager() : base_path_(""), wl_handle_(WL_INVALID_HANDLE), initialized_(false) {}

ConfigManager::~ConfigManager() {
    if (initialized_ && wl_handle_ != WL_INVALID_HANDLE) {
        esp_vfs_fat_spiflash_unmount_rw_wl(base_path_.c_str(), wl_handle_);
    }
}

bool ConfigManager::init(const char* base_path, const char* partition_label) {
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
        .disk_status_check_enable = false,
        .use_one_fat = false
    };

    esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl(base_path, partition_label, &mount_config, &wl_handle_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FATFS partition '%s' at '%s' (%s)",
                 partition_label, base_path, esp_err_to_name(ret));
        return false;
    }

    base_path_ = base_path;
    initialized_ = true;
    ESP_LOGI(TAG, "FATFS partition '%s' mounted successfully at '%s'", partition_label, base_path);
    return true;
}

std::string ConfigManager::trim(const std::string& s) {
    auto start = s.begin();
    while (start != s.end() && std::isspace(static_cast<unsigned char>(*start))) {
        ++start;
    }
    auto end = s.end();
    do {
        --end;
    } while (std::distance(start, end) > 0 && std::isspace(static_cast<unsigned char>(*end)));
    return (start < end + 1) ? std::string(start, end + 1) : std::string();
}

bool ConfigManager::parse_file(const std::string& path) {
    FILE* file = fopen(path.c_str(), "r");
    if (!file) {
        ESP_LOGD(TAG, "Cannot open config file '%s'", path.c_str());
        return false;
    }

    char buf[256];
    while (fgets(buf, sizeof(buf), file) != nullptr) {
        std::string line(buf);
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        auto eq_pos = line.find('=');
        if (eq_pos == std::string::npos) {
            continue;
        }

        std::string key = trim(line.substr(0, eq_pos));
        std::string val = trim(line.substr(eq_pos + 1));

        // Strip trailing comment if value is not in quotes
        bool in_quotes = (val.length() >= 2 && ((val.front() == '"' && val.back() == '"') ||
                                                (val.front() == '\'' && val.back() == '\'')));
        if (!in_quotes) {
            auto hash_pos = val.find('#');
            if (hash_pos != std::string::npos) {
                val = trim(val.substr(0, hash_pos));
            }
        } else {
            val = val.substr(1, val.length() - 2);
        }

        if (!key.empty()) {
            config_map_[key] = val;
        }
    }
    fclose(file);
    return true;
}

bool ConfigManager::load(const std::string& config_path, const std::string& override_path) {
    bool loaded_base = parse_file(config_path);
    if (loaded_base) {
        ESP_LOGI(TAG, "Loaded base configuration from '%s'", config_path.c_str());
    } else {
        ESP_LOGW(TAG, "Could not open base config '%s'", config_path.c_str());
    }

    bool loaded_override = parse_file(override_path);
    if (loaded_override) {
        ESP_LOGI(TAG, "Loaded override configuration from '%s'", override_path.c_str());
    }

    return loaded_base || loaded_override;
}

std::string ConfigManager::get_string(const std::string& key, const std::string& default_val) const {
    auto it = config_map_.find(key);
    if (it != config_map_.end()) {
        return it->second;
    }
    return default_val;
}

int ConfigManager::get_int(const std::string& key, int default_val) const {
    auto it = config_map_.find(key);
    if (it != config_map_.end()) {
        char* end = nullptr;
        long val = strtol(it->second.c_str(), &end, 0);
        if (end != it->second.c_str()) {
            return static_cast<int>(val);
        }
        ESP_LOGW(TAG, "Invalid integer for key '%s': '%s'", key.c_str(), it->second.c_str());
    }
    return default_val;
}

bool ConfigManager::get_bool(const std::string& key, bool default_val) const {
    auto it = config_map_.find(key);
    if (it != config_map_.end()) {
        std::string s = it->second;
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        if (s == "true" || s == "1" || s == "yes") return true;
        if (s == "false" || s == "0" || s == "no") return false;
    }
    return default_val;
}

bool ConfigManager::has_key(const std::string& key) const {
    return config_map_.find(key) != config_map_.end();
}
