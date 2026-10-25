// Device settings, host names and pairing codes. See dispatcher/device.hpp.

#include "dispatcher/device.hpp"

#include <cctype>
#include <cstdlib>

#include "dispatcher/models.hpp"
#include "dispatcher/util.hpp"

namespace dispatcher {

namespace {

constexpr const char* kPairingPrefix = "bdsat1:";
constexpr size_t kSecretBytes = 32;
constexpr size_t kMaxIdLength = 40;
constexpr size_t kMaxHostLength = 253;

template <typename T>
Parsed<T> ok(T value) {
  return Parsed<T>{std::optional<T>(std::move(value)), std::string()};
}

template <typename T>
Parsed<T> fail(std::string message) {
  return Parsed<T>{std::nullopt, std::move(message)};
}

bool is_id_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'; }

bool valid_id(const std::string& id) {
  if (id.empty() || id.size() > kMaxIdLength) return false;
  for (char c : id) {
    if (!is_id_char(c)) return false;
  }
  return true;
}

// A host name or an IPv4 address: letters, digits, '-' and '.'.
bool valid_host(const std::string& host) {
  if (host.empty() || host.size() > kMaxHostLength || host.front() == '.' || host.front() == '-') return false;
  for (char c : host) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '.') return false;
  }
  return true;
}

// A port number in decimal, 1-65535.
std::optional<int> parse_port(const std::string& text) {
  if (text.empty() || text.size() > 5 || text.find_first_not_of("0123456789") != std::string::npos) {
    return std::nullopt;
  }
  const int port = std::atoi(text.c_str());
  if (port < 1 || port > 65535) return std::nullopt;
  return port;
}

const cJSON* get(const cJSON* object, const char* key) { return cJSON_GetObjectItemCaseSensitive(object, key); }

std::string trim(const std::string& text) {
  const char* space = " \t\r\n\f\v";
  const size_t first = text.find_first_not_of(space);
  if (first == std::string::npos) return "";
  return text.substr(first, text.find_last_not_of(space) - first + 1);
}

}  // namespace

const char* role_name(Role role) { return role == Role::kSatellite ? "satellite" : "dispatcher"; }

std::optional<Role> parse_role(const std::string& name) {
  if (name == "dispatcher") return Role::kDispatcher;
  if (name == "satellite") return Role::kSatellite;
  return std::nullopt;
}

std::string mac_suffix(const std::string& mac) {
  const std::optional<std::string> normalized = normalize_mac(mac);
  if (!normalized) return "";
  std::string out;
  for (char c : *normalized) out += c == ':' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string effective_hostname(const DeviceSettings& settings, const std::string& default_base,
                               bool default_append_mac, const std::string& mac) {
  std::string base = settings.hostname_base;
  if (base.empty()) base = default_base;
  if (base.empty()) base = kDefaultHostnameBase;
  const std::string suffix = mac_suffix(mac);
  if (settings.hostname_append_mac.value_or(default_append_mac) && !suffix.empty()) base += "-" + suffix;
  return base;
}

std::string check_hostname_base(const std::string& base, bool append_mac) {
  if (base.empty()) return "";
  const size_t limit = append_mac ? kMaxHostnameLabel - kMacSuffixLength : kMaxHostnameLabel;
  if (base.size() > limit) {
    return "the host name can have at most " + std::to_string(limit) + " characters" +
           (append_mac ? " when the MAC address is appended" : "");
  }
  for (char c : base) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') {
      return "the host name can only have letters, digits and hyphens";
    }
  }
  if (base.front() == '-' || base.back() == '-') return "the host name cannot start or end with a hyphen";
  return "";
}

std::string format_pairing_code(const Pairing& pairing) {
  return std::string(kPairingPrefix) + pairing.id + ":" + hex_encode(pairing.secret) + "@" + pairing.host + ":" +
         std::to_string(pairing.port);
}

Parsed<Pairing> parse_pairing_code(const std::string& code_text) {
  const std::string code = trim(code_text);
  const std::string invalid = "this is not a pairing code; copy it from the dispatcher's Satellites tab";
  if (code.compare(0, std::char_traits<char>::length(kPairingPrefix), kPairingPrefix) != 0) {
    return fail<Pairing>(invalid);
  }
  const std::string rest = code.substr(std::char_traits<char>::length(kPairingPrefix));
  const size_t at = rest.rfind('@');
  if (at == std::string::npos) return fail<Pairing>(invalid);
  const std::string credentials = rest.substr(0, at);
  const std::string address = rest.substr(at + 1);
  const size_t colon = credentials.find(':');
  if (colon == std::string::npos) return fail<Pairing>(invalid);

  Pairing pairing;
  pairing.id = credentials.substr(0, colon);
  if (!valid_id(pairing.id)) return fail<Pairing>(invalid);
  const std::optional<std::string> secret = hex_decode(credentials.substr(colon + 1));
  if (!secret || secret->size() != kSecretBytes) return fail<Pairing>("the secret in the pairing code is damaged");
  pairing.secret = *secret;

  const size_t port_colon = address.rfind(':');
  pairing.host = port_colon == std::string::npos ? address : address.substr(0, port_colon);
  if (port_colon != std::string::npos) {
    const std::optional<int> port = parse_port(address.substr(port_colon + 1));
    if (!port) return fail<Pairing>("the port in the pairing code must be a number from 1 to 65535");
    pairing.port = *port;
  }
  if (!valid_host(pairing.host)) {
    return fail<Pairing>("the dispatcher's address in the pairing code must be a host name or an IPv4 address");
  }
  return ok(std::move(pairing));
}

Parsed<DeviceSettings> device_settings_from_text(const std::string& text) {
  JsonPtr json = make_json(cJSON_Parse(text.c_str()));
  if (!json || !cJSON_IsObject(json.get())) return fail<DeviceSettings>("invalid JSON");
  DeviceSettings settings;

  const cJSON* role = get(json.get(), "role");
  if (role != nullptr) {
    const std::optional<Role> parsed = cJSON_IsString(role) ? parse_role(role->valuestring) : std::nullopt;
    if (!parsed) return fail<DeviceSettings>("unknown role");
    settings.role = *parsed;
  }
  const cJSON* base = get(json.get(), "hostname_base");
  if (base != nullptr) {
    if (!cJSON_IsString(base)) return fail<DeviceSettings>("hostname_base must be text");
    settings.hostname_base = base->valuestring;
  }
  const cJSON* append = get(json.get(), "hostname_append_mac");
  if (append != nullptr) {
    if (!cJSON_IsBool(append)) return fail<DeviceSettings>("hostname_append_mac must be true or false");
    settings.hostname_append_mac = cJSON_IsTrue(append);
  }
  // Without a stored choice the length is checked as if no MAC were appended: the default isn't known here, and
  // the web page stores the choice whenever it sets a base, so a long base never relies on the default.
  if (const std::string problem =
          check_hostname_base(settings.hostname_base, settings.hostname_append_mac.value_or(false));
      !problem.empty()) {
    return fail<DeviceSettings>(problem);
  }
  const cJSON* pairing = get(json.get(), "pairing");
  if (pairing != nullptr && !cJSON_IsNull(pairing)) {
    if (!cJSON_IsString(pairing)) return fail<DeviceSettings>("pairing must be a pairing code");
    Parsed<Pairing> parsed = parse_pairing_code(pairing->valuestring);
    if (!parsed.ok()) return fail<DeviceSettings>(parsed.error);
    settings.pairing = std::move(*parsed.value);
  }
  return ok(std::move(settings));
}

std::string device_settings_to_text(const DeviceSettings& settings) {
  JsonPtr json = make_json(cJSON_CreateObject());
  cJSON_AddNumberToObject(json.get(), "version", 1);
  cJSON_AddStringToObject(json.get(), "role", role_name(settings.role));
  cJSON_AddStringToObject(json.get(), "hostname_base", settings.hostname_base.c_str());
  if (settings.hostname_append_mac) {
    cJSON_AddBoolToObject(json.get(), "hostname_append_mac", *settings.hostname_append_mac);
  }
  // The pairing is stored as its code, so there is one format to parse and validate.
  if (settings.pairing) {
    cJSON_AddStringToObject(json.get(), "pairing", format_pairing_code(*settings.pairing).c_str());
  } else {
    cJSON_AddNullToObject(json.get(), "pairing");
  }
  char* text = cJSON_PrintUnformatted(json.get());
  std::string out = text != nullptr ? text : "";
  cJSON_free(text);
  return out;
}

}  // namespace dispatcher
