#include "config_manager.hpp"
#include "dispatcher/config_secrets.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <sstream>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "nvs.h"
#include "wear_levelling.h"

static const char* TAG = "ConfigManager";

ConfigManager::ConfigManager() : base_path_(""), wl_handle_(WL_INVALID_HANDLE), initialized_(false) {}

ConfigManager::~ConfigManager() {
    unmount();
}

void ConfigManager::unmount() {
    if (initialized_ && wl_handle_ != WL_INVALID_HANDLE) {
        esp_vfs_fat_spiflash_unmount_rw_wl(base_path_.c_str(), wl_handle_);
        ESP_LOGI(TAG, "FATFS partition unmounted from '%s'", base_path_.c_str());
    }
    wl_handle_ = WL_INVALID_HANDLE;
    initialized_ = false;
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
    static const char* WS = " \t\r\n\f\v";
    auto first = s.find_first_not_of(WS);
    if (first == std::string::npos) {
        return std::string();
    }
    auto last = s.find_last_not_of(WS);
    return s.substr(first, last - first + 1);
}

namespace {

const char* const kSecretsNamespace = "secrets";
const char* const kSecretsBlobKey = "values";

bool read_whole_file(const std::string& path, std::string* text) {
    FILE* file = fopen(path.c_str(), "r");
    if (!file) return false;
    char buf[256];
    size_t n;
    text->clear();
    while ((n = fread(buf, 1, sizeof(buf), file)) > 0) text->append(buf, n);
    fclose(file);
    return true;
}

// Replaces the file through a temporary one, so a power cut leaves either the old or the new content.
bool replace_file(const std::string& path, const std::string& text) {
    const std::string tmp = path + ".tmp";
    FILE* file = fopen(tmp.c_str(), "w");
    if (!file) return false;
    const bool written = fwrite(text.data(), 1, text.size(), file) == text.size();
    const bool closed = fclose(file) == 0;
    if (!written || !closed) {
        remove(tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        // FAT may refuse to rename over an existing file.
        remove(path.c_str());
        if (rename(tmp.c_str(), path.c_str()) != 0) {
            remove(tmp.c_str());
            return false;
        }
    }
    return true;
}

}  // namespace

// Secrets live in a single blob in the default NVS partition, because NVS key names are limited to 15
// characters, which config keys such as "home_assistant_token" exceed.
std::vector<dispatcher::ConfigEntry> ConfigManager::load_stored_secrets() {
    nvs_handle_t handle;
    if (nvs_open(kSecretsNamespace, NVS_READONLY, &handle) != ESP_OK) return {};
    size_t size = 0;
    std::string blob;
    if (nvs_get_blob(handle, kSecretsBlobKey, nullptr, &size) == ESP_OK && size > 0) {
        blob.resize(size);
        if (nvs_get_blob(handle, kSecretsBlobKey, blob.data(), &size) != ESP_OK) blob.clear();
    }
    nvs_close(handle);
    return dispatcher::decode_secrets(blob);
}

bool ConfigManager::store_secrets(const std::vector<dispatcher::ConfigEntry>& secrets) {
    nvs_handle_t handle;
    if (nvs_open(kSecretsNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    const std::string blob = dispatcher::encode_secrets(secrets);
    bool ok = nvs_set_blob(handle, kSecretsBlobKey, blob.data(), blob.size()) == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
}

bool ConfigManager::parse_file(const std::string& path, std::vector<ScrubPlan>* plans) {
    std::string text;
    if (!read_whole_file(path, &text)) {
        ESP_LOGD(TAG, "Cannot open config file '%s'", path.c_str());
        return false;
    }
    for (const auto& entry : dispatcher::parse_config_text(text)) {
        // A masked secret stands for the stored one.
        if (dispatcher::is_secret_key(entry.key) && entry.value == dispatcher::kMaskedSecret) continue;
        config_map_[entry.key] = entry.value;
    }
    if (!dispatcher::find_exposed_secrets(text).empty()) plans->push_back({path, text});
    return true;
}

bool ConfigManager::load(const std::string& config_path, const std::string& override_path) {
    // The override is read last so that it wins.
    std::vector<ScrubPlan> plans;
    bool loaded_base = parse_file(config_path, &plans);
    if (loaded_base) {
        ESP_LOGI(TAG, "Loaded base configuration from '%s'", config_path.c_str());
    } else {
        ESP_LOGW(TAG, "Could not open base config '%s'", config_path.c_str());
    }

    bool loaded_override = parse_file(override_path, &plans);
    if (loaded_override) {
        ESP_LOGI(TAG, "Loaded override configuration from '%s'", override_path.c_str());
    }

    hide_secrets(plans);
    return loaded_base || loaded_override;
}

// The config drive can be read by anyone who plugs the board into a PC, so secrets found in the files are
// moved into private storage and commented out in the files. The files are only rewritten once the
// secrets are safely stored. Secrets that were hidden earlier are filled in for keys the files no longer
// have a value for.
void ConfigManager::hide_secrets(const std::vector<ScrubPlan>& plans) {
    std::vector<dispatcher::ConfigEntry> stored = load_stored_secrets();
    if (!plans.empty()) {
        // Files are in load order, so a later file's value for a key replaces an earlier one's, as it does
        // in config_map_.
        for (const auto& plan : plans) {
            for (auto& secret : dispatcher::find_exposed_secrets(plan.text)) {
                auto old = std::find_if(stored.begin(), stored.end(),
                                        [&](const dispatcher::ConfigEntry& e) { return e.key == secret.key; });
                if (old != stored.end()) {
                    old->value = secret.value;
                } else {
                    stored.push_back(std::move(secret));
                }
            }
        }
        if (store_secrets(stored)) {
            for (const auto& plan : plans) {
                if (replace_file(plan.path, dispatcher::scrub_secrets(plan.text))) {
                    ESP_LOGI(TAG, "Moved the secrets in '%s' to private storage", plan.path.c_str());
                } else {
                    ESP_LOGE(TAG, "Could not rewrite '%s'; its secrets stay visible on the USB drive", plan.path.c_str());
                }
            }
        } else {
            ESP_LOGE(TAG, "Could not store the secrets; they stay visible in the config files on the USB drive");
        }
    }
    for (const auto& secret : stored) {
        if (config_map_.find(secret.key) == config_map_.end()) config_map_[secret.key] = secret.value;
    }
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

std::vector<std::string> ConfigManager::get_string_list(const std::string& key) const {
    std::vector<std::string> result;
    auto it = config_map_.find(key);
    if (it == config_map_.end() || it->second.empty()) {
        return result;
    }

    std::stringstream ss(it->second);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = trim(item);
        if (!item.empty()) {
            result.push_back(item);
        }
    }
    return result;
}
