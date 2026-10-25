// Tests for device.cpp: host names, pairing codes and the device settings document.

#include <string>

#include "dispatcher/device.hpp"
#include "dispatcher/util.hpp"
#include "minitest.hpp"

using namespace dispatcher;

namespace {

const char* const kMac = "24:0A:C4:12:34:56";

std::string secret_bytes() {
  std::string secret;
  for (int i = 0; i < 32; ++i) secret += static_cast<char>(i * 7 + 1);
  return secret;
}

Pairing make_pairing() {
  Pairing pairing;
  pairing.id = "sat-1a2b3c";
  pairing.secret = secret_bytes();
  pairing.host = "dispatcher-24-0a-c4-12-34-56.local";
  pairing.port = 8080;
  return pairing;
}

std::string code_with(const std::string& id, const std::string& secret_hex, const std::string& address) {
  return "bdsat1:" + id + ":" + secret_hex + "@" + address;
}

const std::string kSecretHex = hex_encode(secret_bytes());

}  // namespace

// ---- host names ----

TEST(device_mac_suffix_lowercases_and_uses_hyphens) {
  CHECK_EQ(mac_suffix("24:0A:C4:12:34:56"), std::string("24-0a-c4-12-34-56"));
  CHECK_EQ(mac_suffix("aa:bb:cc:dd:ee:ff"), std::string("aa-bb-cc-dd-ee-ff"));
}

TEST(device_mac_suffix_rejects_junk) {
  CHECK_EQ(mac_suffix(""), std::string());
  CHECK_EQ(mac_suffix("hello"), std::string());
  CHECK_EQ(mac_suffix("24:0A:C4:12:34"), std::string());
  CHECK_EQ(mac_suffix("ZZ:0A:C4:12:34:56"), std::string());
}

TEST(device_hostname_defaults_to_the_base_and_mac) {
  DeviceSettings settings;
  CHECK_EQ(effective_hostname(settings, "buttons", true, kMac), std::string("buttons-24-0a-c4-12-34-56"));
}

TEST(device_hostname_uses_a_custom_base) {
  DeviceSettings settings;
  settings.hostname_base = "desk";
  CHECK_EQ(effective_hostname(settings, "buttons", true, kMac), std::string("desk-24-0a-c4-12-34-56"));
}

TEST(device_hostname_without_mac_suffix) {
  DeviceSettings settings;
  settings.hostname_base = "desk";
  settings.hostname_append_mac = false;
  CHECK_EQ(effective_hostname(settings, "buttons", true, kMac), std::string("desk"));
}

TEST(device_hostname_uses_the_default_mac_suffix_choice) {
  DeviceSettings settings;
  settings.hostname_base = "desk";
  CHECK_EQ(effective_hostname(settings, "buttons", false, kMac), std::string("desk"));
  settings.hostname_append_mac = true;
  CHECK_EQ(effective_hostname(settings, "buttons", false, kMac), std::string("desk-24-0a-c4-12-34-56"));
  settings.hostname_append_mac = false;
  CHECK_EQ(effective_hostname(settings, "buttons", true, kMac), std::string("desk"));
}

TEST(device_hostname_leaves_out_an_unknown_mac) {
  DeviceSettings settings;
  CHECK_EQ(effective_hostname(settings, "buttons", true, ""), std::string("buttons"));
  CHECK_EQ(effective_hostname(settings, "buttons", true, "junk"), std::string("buttons"));
}

TEST(device_hostname_falls_back_to_dispatcher) {
  DeviceSettings settings;
  CHECK_EQ(effective_hostname(settings, "", true, kMac), std::string("dispatcher-24-0a-c4-12-34-56"));
  CHECK_EQ(effective_hostname(settings, "", true, ""), std::string("dispatcher"));
}

TEST(device_check_hostname_accepts_empty_and_valid_names) {
  CHECK_EQ(check_hostname_base("", true), std::string());
  CHECK_EQ(check_hostname_base("desk", true), std::string());
  CHECK_EQ(check_hostname_base("my-desk-2", false), std::string());
  CHECK_EQ(check_hostname_base("A", true), std::string());
}

TEST(device_check_hostname_rejects_bad_characters) {
  CHECK(!check_hostname_base("my desk", true).empty());
  CHECK(!check_hostname_base("desk_1", true).empty());
  CHECK(!check_hostname_base("desk.local", false).empty());
  CHECK(!check_hostname_base("d\xc3\xa9sk", false).empty());
}

TEST(device_check_hostname_rejects_leading_and_trailing_hyphens) {
  CHECK(!check_hostname_base("-desk", true).empty());
  CHECK(!check_hostname_base("desk-", true).empty());
  CHECK(!check_hostname_base("-", false).empty());
}

TEST(device_check_hostname_enforces_the_length_limit) {
  CHECK_EQ(check_hostname_base(std::string(45, 'a'), true), std::string());
  CHECK(!check_hostname_base(std::string(46, 'a'), true).empty());
  CHECK_EQ(check_hostname_base(std::string(63, 'a'), false), std::string());
  CHECK(!check_hostname_base(std::string(64, 'a'), false).empty());
}

// ---- pairing codes ----

TEST(device_pairing_code_round_trips) {
  const Pairing pairing = make_pairing();
  const std::string code = format_pairing_code(pairing);
  CHECK_EQ(code, "bdsat1:sat-1a2b3c:" + kSecretHex + "@dispatcher-24-0a-c4-12-34-56.local:8080");
  const Parsed<Pairing> parsed = parse_pairing_code(code);
  CHECK(parsed.ok());
  CHECK_EQ(parsed.value->id, pairing.id);
  CHECK_EQ(parsed.value->secret, pairing.secret);
  CHECK_EQ(parsed.value->host, pairing.host);
  CHECK_EQ(parsed.value->port, 8080);
}

TEST(device_pairing_code_without_a_port_means_80) {
  const Parsed<Pairing> parsed = parse_pairing_code(code_with("sat-1", kSecretHex, "192.168.1.5"));
  CHECK(parsed.ok());
  CHECK_EQ(parsed.value->host, std::string("192.168.1.5"));
  CHECK_EQ(parsed.value->port, 80);
}

TEST(device_pairing_code_trims_whitespace) {
  const Parsed<Pairing> parsed = parse_pairing_code("  \n" + code_with("sat-1", kSecretHex, "host.test:81") + " \t\n");
  CHECK(parsed.ok());
  CHECK_EQ(parsed.value->port, 81);
}

TEST(device_pairing_code_rejects_a_wrong_prefix) {
  CHECK(!parse_pairing_code("").ok());
  CHECK(!parse_pairing_code("bdsat2:sat-1:" + kSecretHex + "@host.test:80").ok());
  CHECK(!parse_pairing_code("sat-1:" + kSecretHex + "@host.test:80").ok());
}

TEST(device_pairing_code_rejects_a_missing_at_sign) {
  CHECK(!parse_pairing_code("bdsat1:sat-1:" + kSecretHex).ok());
}

TEST(device_pairing_code_rejects_bad_ids) {
  CHECK(!parse_pairing_code(code_with("", kSecretHex, "host.test:80")).ok());
  CHECK(!parse_pairing_code(code_with("sat 1", kSecretHex, "host.test:80")).ok());
  CHECK(!parse_pairing_code(code_with("sat/1", kSecretHex, "host.test:80")).ok());
  CHECK(!parse_pairing_code(code_with(std::string(41, 'a'), kSecretHex, "host.test:80")).ok());
}

TEST(device_pairing_code_rejects_a_secret_that_is_not_32_bytes) {
  CHECK(!parse_pairing_code(code_with("sat-1", kSecretHex.substr(0, 62), "host.test:80")).ok());
  CHECK(!parse_pairing_code(code_with("sat-1", kSecretHex + "00", "host.test:80")).ok());
  CHECK(!parse_pairing_code(code_with("sat-1", std::string(64, 'z'), "host.test:80")).ok());
  CHECK(!parse_pairing_code(code_with("sat-1", kSecretHex.substr(0, 63), "host.test:80")).ok());
}

TEST(device_pairing_code_rejects_bad_ports) {
  CHECK(!parse_pairing_code(code_with("sat-1", kSecretHex, "host.test:0")).ok());
  CHECK(!parse_pairing_code(code_with("sat-1", kSecretHex, "host.test:70000")).ok());
  CHECK(!parse_pairing_code(code_with("sat-1", kSecretHex, "host.test:http")).ok());
  CHECK(!parse_pairing_code(code_with("sat-1", kSecretHex, "host.test:")).ok());
  CHECK(parse_pairing_code(code_with("sat-1", kSecretHex, "host.test:65535")).ok());
}

TEST(device_pairing_code_rejects_bad_hosts) {
  CHECK(!parse_pairing_code(code_with("sat-1", kSecretHex, ":80")).ok());
  CHECK(!parse_pairing_code(code_with("sat-1", kSecretHex, "bad host:80")).ok());
  CHECK(!parse_pairing_code(code_with("sat-1", kSecretHex, "-host:80")).ok());
  CHECK(!parse_pairing_code(code_with("sat-1", kSecretHex, "ho/st:80")).ok());
}

// ---- the settings document ----

TEST(device_settings_default_to_a_dispatcher) {
  const Parsed<DeviceSettings> parsed = device_settings_from_text("{}");
  CHECK(parsed.ok());
  CHECK(parsed.value->role == Role::kDispatcher);
  CHECK_EQ(parsed.value->hostname_base, std::string());
  CHECK(!parsed.value->hostname_append_mac.has_value());
  CHECK(!parsed.value->pairing.has_value());
}

TEST(device_settings_round_trip) {
  DeviceSettings settings;
  settings.role = Role::kSatellite;
  settings.hostname_base = "porch";
  settings.hostname_append_mac = false;
  settings.pairing = make_pairing();
  const Parsed<DeviceSettings> parsed = device_settings_from_text(device_settings_to_text(settings));
  CHECK(parsed.ok());
  CHECK(parsed.value->role == Role::kSatellite);
  CHECK_EQ(parsed.value->hostname_base, std::string("porch"));
  CHECK(parsed.value->hostname_append_mac == std::optional<bool>(false));
  CHECK(parsed.value->pairing.has_value());
  CHECK_EQ(parsed.value->pairing->id, std::string("sat-1a2b3c"));
  CHECK_EQ(parsed.value->pairing->secret, secret_bytes());
  CHECK_EQ(parsed.value->pairing->host, make_pairing().host);
  CHECK_EQ(parsed.value->pairing->port, 8080);
}

TEST(device_settings_without_pairing_round_trip) {
  const DeviceSettings settings;
  const Parsed<DeviceSettings> parsed = device_settings_from_text(device_settings_to_text(settings));
  CHECK(parsed.ok());
  CHECK(!parsed.value->pairing.has_value());
  CHECK(parsed.value->role == Role::kDispatcher);
}

TEST(device_settings_unset_mac_suffix_choice_round_trips) {
  const DeviceSettings settings;
  const std::string text = device_settings_to_text(settings);
  CHECK(text.find("hostname_append_mac") == std::string::npos);
  const Parsed<DeviceSettings> parsed = device_settings_from_text(text);
  CHECK(parsed.ok());
  CHECK(!parsed.value->hostname_append_mac.has_value());
}

TEST(device_settings_reject_invalid_documents) {
  CHECK(!device_settings_from_text("").ok());
  CHECK(!device_settings_from_text("[]").ok());
  CHECK(!device_settings_from_text("{\"role\":\"boss\"}").ok());
  CHECK(!device_settings_from_text("{\"role\":3}").ok());
  CHECK(!device_settings_from_text("{\"hostname_base\":\"bad name\"}").ok());
  CHECK(!device_settings_from_text("{\"hostname_base\":5}").ok());
  CHECK(!device_settings_from_text("{\"hostname_append_mac\":\"yes\"}").ok());
  CHECK(!device_settings_from_text("{\"pairing\":\"nonsense\"}").ok());
  CHECK(!device_settings_from_text("{\"pairing\":5}").ok());
  CHECK(!device_settings_from_text("{\"hostname_base\":\"" + std::string(50, 'a') + "\",\"hostname_append_mac\":true}").ok());
  CHECK(!device_settings_from_text("{\"hostname_base\":\"" + std::string(64, 'a') + "\"}").ok());
  CHECK(device_settings_from_text("{\"hostname_base\":\"" + std::string(50, 'a') + "\",\"hostname_append_mac\":false}").ok());
}

TEST(device_role_names) {
  CHECK_EQ(std::string(role_name(Role::kDispatcher)), std::string("dispatcher"));
  CHECK_EQ(std::string(role_name(Role::kSatellite)), std::string("satellite"));
  CHECK(parse_role("satellite") == Role::kSatellite);
  CHECK(!parse_role("Satellite").has_value());
}
