// Settings of the board itself rather than of the buttons: its role (dispatcher or satellite), its network
// name, and, on a satellite, how to reach the dispatcher it reports to.
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
  // The first part of the host name; empty means the default (config.toml's `hostname`).
  std::string hostname_base;
  // Whether "-<Wi-Fi MAC>" is appended, so that boards flashed with the same config.toml get different names.
  // Unset means the default (config.toml's `hostname_append_mac`), so one board's config-override.toml can give
  // it an exact name from the first boot.
  std::optional<bool> hostname_append_mac;
  std::optional<Pairing> pairing;
};

// "AA:BB:CC:DD:EE:FF" -> "aa-bb-cc-dd-ee-ff"; empty if `mac` is not a MAC address.
std::string mac_suffix(const std::string& mac);

// The name the board uses on the network (mDNS "<name>.local" and DHCP), for example
// "dispatcher-aa-bb-cc-dd-ee-ff". Falls back to `default_base`, then to kDefaultHostnameBase, when the base is
// empty, and to `default_append_mac` when the settings don't say whether to append the MAC; the MAC is left out
// if it is unknown.
std::string effective_hostname(const DeviceSettings& settings, const std::string& default_base,
                               bool default_append_mac, const std::string& mac);

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
