#include "config_manager.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
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
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
        .disk_status_check_enable = false
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
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
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

        // Strip trailing comment if not in quotes
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
    return true;
}

bool ConfigManager::load(const std::string& config_path, const std::string& override_path) {
    bool loaded_base = parse_file(config_path);
    if (loaded_base) {
        ESP_LOGI(TAG, "Loaded configuration from '%s'", config_path.c_str());
    } else {
        ESP_LOGW(TAG, "Base config file '%s' not found or empty", config_path.c_str());
    }

    bool loaded_override = parse_file(override_path);
    if (loaded_override) {
        ESP_LOGI(TAG, "Loaded override configuration from '%s'", override_path.c_str());
    }

    return loaded_base || loaded_override;
}

bool ConfigManager::has_key(const std::string& key) const {
    return config_map_.find(key) != config_map_.end();
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
    if (it != config_map_.end() && !it->second.empty()) {
        char* end_ptr = nullptr;
        long val = std::strtol(it->second.c_str(), &end_ptr, 10);
        if (end_ptr != it->second.c_str()) {
            return static_cast<int>(val);
        }
    }
    return default_val;
}

bool ConfigManager::get_bool(const std::string& key, bool default_val) const {
    auto it = config_map_.find(key);
    if (it != config_map_.end()) {
        std::string lower = it->second;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        if (lower == "true" || lower == "1" || lower == "yes") {
            return true;
        }
        if (lower == "false" || lower == "0" || lower == "no") {
            return false;
        }
    }
    return default_val;
}

std::vector<std::pair<std::string, std::string>> ConfigManager::get_wifi_ap_list() const {
    std::vector<std::pair<std::string, std::string>> ap_list;

    // Index 0: wifi_ssid / wifi_ssid0
    std::string ssid0 = get_string("wifi_ssid");
    if (ssid0.empty()) {
        ssid0 = get_string("wifi_ssid0");
    }
    std::string pass0 = get_string("wifi_password");
    if (pass0.empty()) {
        pass0 = get_string("wifi_password0");
    }
    if (!ssid0.empty()) {
        ap_list.emplace_back(ssid0, pass0);
    }

    // Indices 1 to 9
    for (int i = 1; i <= 9; ++i) {
        std::string ssid_key = "wifi_ssid" + std::to_string(i);
        std::string pass_key = "wifi_password" + std::to_string(i);
        std::string ssid = get_string(ssid_key);
        std::string pass = get_string(pass_key);
        if (!ssid.empty()) {
            ap_list.emplace_back(ssid, pass);
        }
    }

    return ap_list;
}
