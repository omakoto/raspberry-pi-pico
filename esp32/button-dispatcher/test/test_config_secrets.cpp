// Tests for config_secrets.cpp: finding secrets in config.toml / config-override.toml and hiding them.

#include <string>
#include <vector>

#include "dispatcher/config_secrets.hpp"
#include "minitest.hpp"

using namespace dispatcher;

namespace {

std::string keys_of(const std::vector<ConfigEntry>& entries) {
  std::string out;
  for (const auto& entry : entries) out += entry.key + ";";
  return out;
}

}  // namespace

TEST(secret_keys_by_suffix) {
  CHECK(is_secret_key("wifi_password"));
  CHECK(is_secret_key("wifi_password1"));
  CHECK(is_secret_key("wifi_password12"));
  CHECK(is_secret_key("pushover_token"));
  CHECK(is_secret_key("switchbot_key"));
  CHECK(is_secret_key("some_project_api_key"));
  CHECK(!is_secret_key("wifi_ssid"));
  CHECK(!is_secret_key("hostname"));
  CHECK(!is_secret_key("buzzer_pin"));
  CHECK(!is_secret_key("password"));
  CHECK(!is_secret_key("key"));
  CHECK(!is_secret_key("token_count"));
  CHECK(!is_secret_key("keyboard"));
  // A digit suffix is only allowed on passwords.
  CHECK(!is_secret_key("api_key1"));
  CHECK(!is_secret_key("pushover_token2"));
}

TEST(parse_config_text_handles_quotes_and_comments) {
  const auto entries = parse_config_text(
      "# a comment\n"
      "\n"
      "hostname = \"disp\"  # trailing\n"
      "pin = 21 # also trailing\n"
      "pw = 'a#b'\n"
      "not an entry\n"
      "  spaced   =   \"x\"  \n");
  CHECK_EQ(entries.size(), size_t(4));
  CHECK_EQ(entries[0].key, std::string("hostname"));
  CHECK_EQ(entries[0].value, std::string("disp"));
  CHECK_EQ(entries[1].value, std::string("21"));
  CHECK_EQ(entries[2].value, std::string("a#b"));
  CHECK_EQ(entries[3].key, std::string("spaced"));
  CHECK_EQ(entries[3].value, std::string("x"));
}

TEST(find_exposed_secrets_skips_hidden_and_empty_ones) {
  const std::string text =
      "wifi_ssid = \"net\"\n"
      "wifi_password = \"hunter2\"\n"
      "# wifi_password1 = \"commented\"\n"
      "wifi_password2 = \"\"\n"
      "wifi_password3 = \"***\"\n"
      "pushover_token = abc\n"
      "other_key = \"k\"\n";
  CHECK_EQ(keys_of(find_exposed_secrets(text)), std::string("wifi_password;pushover_token;other_key;"));
}

TEST(scrub_comments_out_exactly_the_secrets) {
  const std::string text =
      "# header\n"
      "wifi_ssid = \"net\"\n"
      "wifi_password = \"hunter2\"  # my home\n"
      "pushover_token = abc\n"
      "led_pin = 21\n";
  CHECK_EQ(scrub_secrets(text),
           std::string("# header\n"
                       "wifi_ssid = \"net\"\n"
                       "# wifi_password = \"***\"\n"
                       "# pushover_token = \"***\"\n"
                       "led_pin = 21\n"));
}

TEST(scrub_is_idempotent_and_leaves_clean_files_alone) {
  const std::string clean = "wifi_ssid = \"net\"\n# wifi_password = \"***\"\nled_pin = 21\n";
  CHECK_EQ(scrub_secrets(clean), clean);
  const std::string once = scrub_secrets("wifi_password = \"x\"\n");
  CHECK_EQ(scrub_secrets(once), once);
  CHECK(find_exposed_secrets(once).empty());
}

TEST(scrub_keeps_line_endings_and_a_missing_final_newline) {
  CHECK_EQ(scrub_secrets("a = 1\r\nwifi_password = \"x\"\r\nb = 2"),
           std::string("a = 1\r\n# wifi_password = \"***\"\r\nb = 2"));
  CHECK_EQ(scrub_secrets("wifi_password = \"x\""), std::string("# wifi_password = \"***\""));
  CHECK_EQ(scrub_secrets(""), std::string(""));
}

TEST(encode_decode_round_trip) {
  const std::vector<ConfigEntry> secrets = {{"wifi_password", "p w#d"}, {"pushover_token", "t"}, {"x_key", ""}};
  const auto decoded = decode_secrets(encode_secrets(secrets));
  CHECK_EQ(decoded.size(), size_t(3));
  CHECK_EQ(decoded[0].key, std::string("wifi_password"));
  CHECK_EQ(decoded[0].value, std::string("p w#d"));
  CHECK_EQ(decoded[2].key, std::string("x_key"));
  CHECK_EQ(decoded[2].value, std::string(""));
  CHECK(decode_secrets("").empty());
  // A truncated blob yields the complete pairs only.
  CHECK_EQ(decode_secrets(std::string("a\0b\0c\0d", 7)).size(), size_t(1));
}
