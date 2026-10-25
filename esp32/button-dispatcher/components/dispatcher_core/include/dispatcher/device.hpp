// Settings of the board itself rather than of the buttons: its role (dispatcher or satellite), its network
// name, and, on a satellite, how to reach the dispatcher it reports to.
//
// The network name comes from config.toml rather than device.json, so that one shared config file can name
// every board (see HostnameConfig).
//
// They are kept in their own document, device.json, apart from config.json: config.json is the format the
// Raspberry Pi version exports and imports, and these settings (including the pairing secret) belong to one
// board and must never travel with an exported config.
//
// Run test/run.sh (which builds test_device.cpp) every time this file or device.cpp is touched.

#pragma once

#include <optional>
#include <string>

#include "dispatcher/config.hpp"

namespace dispatcher {

constexpr const char* kDeviceDocument = "device.json";
constexpr const char* kDefaultHostnameBase = "dispatcher";
// A DNS label is at most 63 characters, and "-00-11-22-33-44-55" takes 18 of them.
constexpr size_t kMaxHostnameLabel = 63;
constexpr size_t kMacSuffixLength = 18;

enum class Role { kDispatcher, kSatellite };

const char* role_name(Role role);  // "dispatcher" or "satellite"
std::optional<Role> parse_role(const std::string& name);

// What a satellite needs to report to its dispatcher; comes from the dispatcher's pairing code.
struct Pairing {
  std::string id;      // the satellite's id on the dispatcher, such as "sat-1a2b3c"
  std::string secret;  // raw bytes of the shared HMAC key
  std::string host;    // the dispatcher's host name or address
  int port = 80;
};

struct DeviceSettings {
  Role role = Role::kDispatcher;
  std::optional<Pairing> pairing;
};

// How the board is named on the network, from config.toml. The shared config file is flashed onto every board,
// so the plain keys (`hostname`, `hostname_append_mac`) give each board "<base>-<Wi-Fi MAC>", and
// one board can be given its own name with keys that carry its MAC, such as
// `board.24-0a-c4-12-34-56.hostname = "dispatcher"` and `board.24-0a-c4-12-34-56.hostname_append_mac = false`.
// These are single-line dotted keys rather than a [board.<mac>] table because the other projects that share
// the file read it line by line, and would take a `hostname` line under a table header as their own.
struct HostnameConfig {
  std::string base = kDefaultHostnameBase;
  // Whether "-<Wi-Fi MAC>" is appended, so that boards flashed with the same config.toml get different names.
  bool append_mac = true;
};

// "AA:BB:CC:DD:EE:FF" -> "aa-bb-cc-dd-ee-ff"; empty if `mac` is not a MAC address.
std::string mac_suffix(const std::string& mac);

// "board.aa-bb-cc-dd-ee-ff." for the board with Wi-Fi MAC `mac`: the prefix of the config keys that apply to
// that board only. Empty if `mac` is not a MAC address.
std::string board_config_prefix(const std::string& mac);

// The name the board uses on the network (mDNS "<name>.local" and DHCP), for example
// "dispatcher-aa-bb-cc-dd-ee-ff". The base is trimmed and lowercased, and an empty one means
// kDefaultHostnameBase. An invalid base (see check_hostname_base) is replaced by kDefaultHostnameBase with the
// MAC appended, so the board still gets a name no other board has, and `*problem` (if given) says why;
// otherwise `*problem` is cleared. The MAC is left out if it is unknown.
std::string effective_hostname(const HostnameConfig& config, const std::string& mac, std::string* problem = nullptr);

// Checks a host name base: letters, digits and hyphens, not starting or ending with a hyphen, and short enough
// for one DNS label with the MAC suffix if it is appended. Returns the message to show, or "" if it is valid.
// An empty base is valid (it means the default).
std::string check_hostname_base(const std::string& base, bool append_mac);

// A pairing code, the one string the dispatcher shows for a new satellite and the satellite's page accepts:
// "bdsat1:<id>:<secret hex>@<host>:<port>".
std::string format_pairing_code(const Pairing& pairing);
Parsed<Pairing> parse_pairing_code(const std::string& code);

Parsed<DeviceSettings> device_settings_from_text(const std::string& text);
std::string device_settings_to_text(const DeviceSettings& settings);

}  // namespace dispatcher
