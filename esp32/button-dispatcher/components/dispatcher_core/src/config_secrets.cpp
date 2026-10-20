#include "dispatcher/config_secrets.hpp"

#include <cctype>
#include <cstring>

namespace dispatcher {

namespace {

const char kWhitespace[] = " \t\r\n\f\v";

std::string trim(const std::string& s) {
  const size_t first = s.find_first_not_of(kWhitespace);
  if (first == std::string::npos) return std::string();
  const size_t last = s.find_last_not_of(kWhitespace);
  return s.substr(first, last - first + 1);
}

bool ends_with(const std::string& s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Parses one line (without its line ending) into an entry; false for blank lines, comments and non-entries.
bool parse_line(const std::string& raw, ConfigEntry* entry) {
  const std::string line = trim(raw);
  if (line.empty() || line[0] == '#') return false;
  const size_t eq = line.find('=');
  if (eq == std::string::npos) return false;

  std::string key = trim(line.substr(0, eq));
  if (key.empty()) return false;
  std::string value = trim(line.substr(eq + 1));

  size_t close = std::string::npos;
  if (!value.empty() && (value.front() == '"' || value.front() == '\'')) close = value.find(value.front(), 1);
  if (close != std::string::npos) {
    value = value.substr(1, close - 1);
  } else {
    const size_t hash = value.find('#');
    if (hash != std::string::npos) value = trim(value.substr(0, hash));
  }
  entry->key = std::move(key);
  entry->value = std::move(value);
  return true;
}

bool is_exposed_secret(const ConfigEntry& entry) {
  return is_secret_key(entry.key) && !entry.value.empty() && entry.value != kMaskedSecret;
}

// Calls fn(line_without_ending, ending) for each line of text; the last line may have no ending.
template <typename Fn>
void for_each_line(const std::string& text, Fn fn) {
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    const bool has_newline = end != std::string::npos;
    if (!has_newline) end = text.size();
    size_t content_end = end;
    if (has_newline && content_end > pos && text[content_end - 1] == '\r') --content_end;
    fn(text.substr(pos, content_end - pos), text.substr(content_end, (has_newline ? end + 1 : end) - content_end));
    pos = has_newline ? end + 1 : end;
  }
}

}  // namespace

bool is_secret_key(const std::string& key) {
  std::string stem = key;
  while (!stem.empty() && std::isdigit(static_cast<unsigned char>(stem.back()))) stem.pop_back();
  // "_password" may carry an index ("wifi_password1"); "_key" and "_token" must be the very end, so that
  // "key1" style names that are not secrets are left alone.
  return ends_with(stem, "_password") || ends_with(key, "_key") || ends_with(key, "_token");
}

std::vector<ConfigEntry> parse_config_text(const std::string& text) {
  std::vector<ConfigEntry> entries;
  for_each_line(text, [&](const std::string& line, const std::string&) {
    ConfigEntry entry;
    if (parse_line(line, &entry)) entries.push_back(std::move(entry));
  });
  return entries;
}

std::vector<ConfigEntry> find_exposed_secrets(const std::string& text) {
  std::vector<ConfigEntry> secrets;
  for (auto& entry : parse_config_text(text)) {
    if (is_exposed_secret(entry)) secrets.push_back(std::move(entry));
  }
  return secrets;
}

std::string scrub_secrets(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for_each_line(text, [&](const std::string& line, const std::string& ending) {
    ConfigEntry entry;
    if (parse_line(line, &entry) && is_exposed_secret(entry)) {
      out += "# " + entry.key + " = \"" + kMaskedSecret + "\"";
    } else {
      out += line;
    }
    out += ending;
  });
  return out;
}

std::string encode_secrets(const std::vector<ConfigEntry>& secrets) {
  std::string blob;
  for (const auto& secret : secrets) {
    blob += secret.key;
    blob.push_back('\0');
    blob += secret.value;
    blob.push_back('\0');
  }
  return blob;
}

std::vector<ConfigEntry> decode_secrets(const std::string& blob) {
  std::vector<ConfigEntry> secrets;
  size_t pos = 0;
  while (pos < blob.size()) {
    const size_t key_end = blob.find('\0', pos);
    if (key_end == std::string::npos) break;
    const size_t value_end = blob.find('\0', key_end + 1);
    if (value_end == std::string::npos) break;
    secrets.push_back({blob.substr(pos, key_end - pos), blob.substr(key_end + 1, value_end - key_end - 1)});
    pos = value_end + 1;
  }
  return secrets;
}

}  // namespace dispatcher
