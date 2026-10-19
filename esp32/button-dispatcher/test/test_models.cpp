// Tests for models.cpp.

#include "dispatcher/models.hpp"
#include "minitest.hpp"

using dispatcher::normalize_mac;

TEST(normalize_mac_accepts_common_forms) {
  CHECK_EQ(normalize_mac("aa:bb:cc:dd:ee:ff"), std::optional<std::string>("AA:BB:CC:DD:EE:FF"));
  CHECK_EQ(normalize_mac("AA-BB-CC-DD-EE-FF"), std::optional<std::string>("AA:BB:CC:DD:EE:FF"));
  CHECK_EQ(normalize_mac(" aabbccddeeff "), std::optional<std::string>("AA:BB:CC:DD:EE:FF"));
  CHECK_EQ(normalize_mac("C0:2c:ED:40:7a:8E"), std::optional<std::string>("C0:2C:ED:40:7A:8E"));
}

TEST(normalize_mac_rejects_garbage) {
  for (const char* bad : {"", "AA:BB", "AA:BB:CC:DD:EE:GG", "AA:BB:CC:DD:EE:FF:00", "AA:BB-CC:DD:EE:FF",
                          "AABBCCDDEEF", "AABBCCDDEEFFAA", "AA:BB:CC:DD:EEFF", "not-a-mac", "A:B:C:D:E:F"}) {
    CHECK_EQ(normalize_mac(bad), std::optional<std::string>());
  }
}

TEST(action_type_names_match_the_json) {
  CHECK_EQ(std::string(dispatcher::action_type_name(dispatcher::Action(dispatcher::HttpAction()))), std::string("http"));
  CHECK_EQ(std::string(dispatcher::action_type_name(dispatcher::Action(dispatcher::SwitchBotCommandAction()))),
           std::string("switchbot_command"));
  CHECK_EQ(std::string(dispatcher::action_type_name(dispatcher::Action(dispatcher::SwitchBotSceneAction()))),
           std::string("switchbot_scene"));
}

TEST(config_lookups) {
  dispatcher::Config config;
  config.buttons.push_back({"desk", "Desk", "AA:BB:CC:DD:EE:01", ""});
  dispatcher::Binding binding;
  binding.id = "wake";
  config.bindings.push_back(binding);
  CHECK(config.find_button("desk") != nullptr);
  CHECK(config.find_button("nope") == nullptr);
  CHECK(config.find_binding("wake") != nullptr);
  CHECK(config.find_binding("nope") == nullptr);
  CHECK(!config.has_switchbot_credentials());
  config.switchbot_token = "t";
  CHECK(!config.has_switchbot_credentials());
  config.switchbot_secret = "s";
  CHECK(config.has_switchbot_credentials());
}
