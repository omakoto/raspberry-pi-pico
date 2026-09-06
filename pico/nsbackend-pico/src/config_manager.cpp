/*
 * Configuration Manager Implementation for nsbackend-pico.
 * Loads and parses config.toml and config-override.toml using FatFs f_read.
 */

#include "config_manager.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include "ff.h"
#include "flash_fatfs.hpp"
#include "dual_logger.hpp"

static const char* TAG = "Config";
static FATFS s_fs;

ConfigManager::ConfigManager() : initialized_(false) {}

ConfigManager::~ConfigManager() {}

bool ConfigManager::init() {
    FRESULT res = f_mount(&s_fs, "0:", 1);
    if (res != FR_OK) {
        LOG_W(TAG, "Failed to mount FATFS volume (error %d)", res);
        return false;
    }
    initialized_ = true;
    LOG_I(TAG, "FATFS volume mounted successfully on 0:");
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
    FIL fil;
    FRESULT res = f_open(&fil, path.c_str(), FA_READ);
    if (res != FR_OK) {
        LOG_D(TAG, "Cannot open file '%s' (error %d)", path.c_str(), res);
        return false;
    }

    std::string content;
    char buf[256];
    UINT bytes_read = 0;
    while (f_read(&fil, buf, sizeof(buf), &bytes_read) == FR_OK && bytes_read > 0) {
        content.append(buf, bytes_read);
    }
    f_close(&fil);

    std::stringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
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
        LOG_I(TAG, "Loaded configuration from '%s'", config_path.c_str());
    } else {
        LOG_W(TAG, "Base config file '%s' not found or empty", config_path.c_str());
    }

    bool loaded_override = parse_file(override_path);
    if (loaded_override) {
        LOG_I(TAG, "Loaded override configuration from '%s'", override_path.c_str());
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
