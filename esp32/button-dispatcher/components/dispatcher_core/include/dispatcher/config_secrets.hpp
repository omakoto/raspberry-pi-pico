#pragma once

// Parsing of the device's config.toml / config-override.toml, and hiding of the secrets in them.
//
// The config files sit on a USB drive that anyone can read by plugging the board in, so secrets in them
// (Wi-Fi passwords, API keys, tokens) are moved into private storage at boot and the files are rewritten
// with the secret commented out. The same override file is shared by several projects, so the rule is by
// key name, not by the keys this firmware happens to use.

#include <string>
#include <utility>
#include <vector>

namespace dispatcher {

struct ConfigEntry {
  std::string key;
  std::string value;
};

// The value that replaces a secret in the file. A secret whose value is this is treated as not set, so the
// stored value is kept.
inline constexpr const char kMaskedSecret[] = "***";

// Whether `key` names a secret: it ends in "_password" (optionally followed by digits, as in
// "wifi_password1"), "_key" or "_token".
bool is_secret_key(const std::string& key);

// The `key = value` lines of a config file in file order. Blank lines, comments and lines without '=' are
// skipped. A quoted value ends at its closing quote (so it may contain '#' and be followed by a comment);
// an unquoted value ends at the first '#'.
std::vector<ConfigEntry> parse_config_text(const std::string& text);

// The entries of the file that hold a secret that is not yet hidden: a secret key with a value that is
// neither empty nor kMaskedSecret.
std::vector<ConfigEntry> find_exposed_secrets(const std::string& text);

// `text` with every exposed secret replaced by the comment line `# key = "***"`, keeping the line endings.
// Unrelated lines are left as they are.
std::string scrub_secrets(const std::string& text);

// Serialization of secrets for a single blob in private storage (flash key names are too short to use the
// config keys directly): "key\0value\0" pairs.
std::string encode_secrets(const std::vector<ConfigEntry>& secrets);
std::vector<ConfigEntry> decode_secrets(const std::string& blob);

}  // namespace dispatcher
