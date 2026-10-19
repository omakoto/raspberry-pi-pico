// Tests for config.cpp: validation, serialization, round trips and import.

#include <string>
#include <vector>

#include "dispatcher/config.hpp"
#include "minitest.hpp"

using namespace dispatcher;

namespace {

JsonPtr J(const std::string& text) { return make_json(cJSON_Parse(text.c_str())); }

std::string print(const cJSON* json) {
  char* text = cJSON_PrintUnformatted(json);
  std::string out = text != nullptr ? text : "";
  cJSON_free(text);
  return out;
}

const char* const kButton =
    R"({"id": "desk", "name": "Desk remote", "mac": "aa:bb:cc:dd:ee:01", "model": "SBBT-104CUS"})";
const char* const kBinding = R"({
  "id": "wake", "name": "Wake PC", "enabled": true, "cooldown_ms": 700,
  "trigger": {"button": "desk", "channel": null, "input": "button:2", "event": "press"},
  "action": {"type": "http", "method": "GET", "url": "http://wol.local/wake"}})";

// A valid document with one button and one binding; `extra` is spliced in as more top-level members.
std::string doc(const std::string& extra = "") {
  return std::string(R"({"version": 1, "buttons": [)") + kButton + R"(], "bindings": [)" + kBinding + "]" +
         extra + "}";
}

// Parses a document that must be valid.
Config load(const std::string& text) {
  Parsed<Config> parsed = config_from_text(text);
  CHECK(parsed.ok());
  if (!parsed.ok()) std::printf("  load error: %s\n", parsed.error.c_str());
  return parsed.ok() ? *parsed.value : Config();
}

std::string config_error(const std::string& text) { return config_from_text(text).error; }

std::string action_error(const std::string& text) {
  JsonPtr j = J(text);
  Parsed<Action> parsed = parse_action(j.get());
  CHECK_EQ(parsed.ok(), parsed.error.empty());
  return parsed.error;
}

// Parses a valid action and returns its normalized JSON text.
std::string action_json(const std::string& text) {
  JsonPtr j = J(text);
  Parsed<Action> parsed = parse_action(j.get());
  CHECK(parsed.ok());
  if (!parsed.ok()) return "";
  JsonPtr out = action_to_json(*parsed.value);
  return print(out.get());
}

std::string trigger_error(const std::string& text) {
  JsonPtr j = J(text);
  return parse_trigger(j.get()).error;
}

Config current_config() {
  Config config = load(doc());
  config.switchbot_token = "old-token";
  config.switchbot_secret = "old-secret";
  return config;
}

// A button object that differs from kButton by the given members.
std::string button_json(const std::string& id, const std::string& mac) {
  return R"({"id": ")" + id + R"(", "name": "Desk remote", "mac": ")" + mac + R"(", "model": "SBBT-104CUS"})";
}

std::string binding_json(const std::string& id, const std::string& name) {
  return R"({"id": ")" + id + R"(", "name": ")" + name + R"(",
  "trigger": {"button": "desk", "input": "button:2", "event": "press"},
  "action": {"type": "http", "url": "http://wol.local/wake"}})";
}

std::vector<std::string> ids_of(const Config& config, bool buttons) {
  std::vector<std::string> ids;
  if (buttons) {
    for (const Button& b : config.buttons) ids.push_back(b.id);
  } else {
    for (const Binding& b : config.bindings) ids.push_back(b.id);
  }
  return ids;
}

using Strings = std::vector<std::string>;

// minitest can only print what has operator<<, so lists are compared as one string.
std::string joined(const Strings& items) {
  std::string out;
  for (const std::string& s : items) out += (out.empty() ? "" : ",") + s;
  return out;
}

}  // namespace

// ---- parse_action ----------------------------------------------------------------------------

TEST(config_http_get_defaults) {
  CHECK_EQ(action_json(R"({"type": "http", "url": "https://example.com/x?y=1"})"),
           std::string(R"({"type":"http","method":"GET","url":"https://example.com/x?y=1"})"));
}

TEST(config_http_full) {
  JsonPtr j = J(R"({"type": "http", "method": "post", "url": "http://h/p",
                    "headers": {"Content-Type": "application/json"}, "body": "{}",
                    "timeout_s": 5, "ignored": "x"})");
  Parsed<Action> parsed = parse_action(j.get());
  CHECK(parsed.ok());
  const HttpAction* http = parsed.ok() ? std::get_if<HttpAction>(&*parsed.value) : nullptr;
  CHECK(http != nullptr);
  if (http == nullptr) return;
  CHECK_EQ(http->method, std::string("POST"));
  CHECK_EQ(http->headers.size(), static_cast<size_t>(1));
  CHECK_EQ(http->headers[0].first, std::string("Content-Type"));
  CHECK_EQ(http->headers[0].second, std::string("application/json"));
  CHECK_EQ(http->body, std::string("{}"));
  CHECK(http->timeout_s.has_value());
  CHECK_EQ(http->timeout_s.value_or(0.0), 5.0);
  JsonPtr out = action_to_json(*parsed.value);
  CHECK_EQ(print(out.get()),
           std::string(R"({"type":"http","method":"POST","url":"http://h/p",)"
                       R"("headers":{"Content-Type":"application/json"},"body":"{}","timeout_s":5.0})"));
}

TEST(config_http_rejects_bad_input) {
  const Strings bad = {
      R"({"type": "http", "url": "ftp://example.com"})",
      R"({"type": "http", "url": "not a url"})",
      R"({"type": "http", "url": "http://"})",
      R"({"type": "http", "url": ""})",
      R"({"type": "http", "url": "http://h/", "method": "TRACE"})",
      R"({"type": "http", "url": "http://h/", "headers": {"Bad Name": "x"}})",
      R"({"type": "http", "url": "http://h/", "headers": {"X": "a\r\nb"}})",
      R"({"type": "http", "url": "http://h/", "timeout_s": 0})",
      R"({"type": "http", "url": "http://h/", "timeout_s": 61})",
      R"({"type": "http", "url": "http://h/", "timeout_s": true})",
      R"({"type": "http", "url": "http://h/", "body": ")" + std::string(10001, 'x') + R"("})",
      R"({"type": "nope"})",
      R"("string")",
  };
  for (const std::string& action : bad) {
    CHECK(!action_error(action).empty());
  }
}

TEST(config_http_url_edge_cases) {
  CHECK(action_error(R"({"type": "http", "url": "HTTP://Example.com"})").empty());
  CHECK(action_error(R"({"type": "http", "url": "http://user:pw@host:8080/p"})").empty());
  CHECK(action_error(R"({"type": "http", "url": "http://[::1]:8080/p"})").empty());
  CHECK(!action_error(R"({"type": "http", "url": "http://:80/"})").empty());
  CHECK(!action_error(R"({"type": "http", "url": "http:/host/"})").empty());
  CHECK(!action_error(R"({"type": "http", "url": "http://host/a b"})").empty());
  CHECK(!action_error(R"({"type": "http", "url": "//host/"})").empty());
}

TEST(config_http_limits_and_defaults) {
  // Empty, null or false optional members mean "not given".
  CHECK_EQ(action_json(R"({"type": "http", "url": "http://h/", "headers": null, "body": "", "timeout_s": ""})"),
           std::string(R"({"type":"http","method":"GET","url":"http://h/"})"));
  CHECK_EQ(action_json(R"({"type": "http", "url": "http://h/", "headers": [], "body": false, "timeout_s": null})"),
           std::string(R"({"type":"http","method":"GET","url":"http://h/"})"));
  // The URL is trimmed; the body is kept as given.
  CHECK_EQ(action_json(R"({"type": "http", "url": "  http://h/  ", "body": " b "})"),
           std::string(R"({"type":"http","method":"GET","url":"http://h/","body":" b "})"));
  CHECK(action_error(R"({"type": "http", "url": "http://h/", "body": ")" + std::string(10000, 'x') + R"("})").empty());
  CHECK(action_error(R"({"type": "http", "url": "http://h/", "timeout_s": 1})").empty());
  CHECK(action_error(R"({"type": "http", "url": "http://h/", "timeout_s": 60.0})").empty());
  CHECK(action_error(R"({"type": "http", "url": "http://h/", "headers": {"X": ""}})").empty());
  CHECK(!action_error(R"({"type": "http", "url": "http://h/", "headers": {"X": ")" + std::string(1001, 'v') + R"("}})")
             .empty());
  CHECK(!action_error(R"({"type": "http", "url": "http://h/", "headers": {"X": 1}})").empty());
  CHECK(!action_error(R"({"type": "http", "url": "http://h/", "headers": [1]})").empty());
  CHECK(!action_error(R"({"type": "http", "url": "http://h/", "body": 5})").empty());
  CHECK(!action_error(R"({"type": "http", "url": "http://h/", "method": 5})").empty());
  std::string many = R"({"type": "http", "url": "http://h/", "headers": {)";
  for (int i = 0; i < 21; i++) many += (i ? "," : "") + std::string("\"H") + std::to_string(i) + "\": \"v\"";
  CHECK_EQ(action_error(many + "}}"), std::string("headers must be an object with at most 20 entries"));
}

TEST(config_http_error_messages) {
  CHECK_EQ(action_error(R"({"type": "http", "url": "ftp://example.com"})"),
           std::string("URL must be an http:// or https:// address"));
  CHECK_EQ(action_error(R"({"type": "http", "url": ""})"), std::string("URL must not be empty"));
  CHECK_EQ(action_error(R"({"type": "http"})"), std::string("URL must be a string"));
  CHECK_EQ(action_error(R"({"type": "http", "url": "http://h/", "method": "TRACE"})"),
           std::string("HTTP method must be one of: GET, POST, PUT, DELETE"));
  CHECK_EQ(action_error(R"({"type": "http", "url": "http://h/", "headers": {"Bad Name": "x"}})"),
           std::string("invalid header name: 'Bad Name'"));
  CHECK_EQ(action_error(R"({"type": "http", "url": "http://h/", "headers": {"X": "a\r\nb"}})"),
           std::string("header X must not contain control characters"));
  CHECK_EQ(action_error(R"({"type": "http", "url": "http://h/", "timeout_s": 61})"),
           std::string("timeout must be between 1 and 60 seconds"));
  CHECK_EQ(action_error(R"({"type": "http", "url": "http://h/", "body": ")" + std::string(10001, 'x') + R"("})"),
           std::string("body must be text of at most 10000 characters"));
  CHECK_EQ(action_error(R"({"type": "http", "url": ")" + std::string(2001, 'x') + R"("})"),
           std::string("URL is too long (max 2000 characters)"));
  CHECK_EQ(action_error(R"({"type": "nope"})"),
           std::string("action type must be one of: http, switchbot_command, switchbot_scene"));
  CHECK_EQ(action_error(R"("string")"), std::string("action must be an object"));
}

TEST(config_switchbot_command) {
  CHECK_EQ(action_json(R"({"type": "switchbot_command", "device_id": "ABC123", "command": "press",
                           "device_name": "Living Bot"})"),
           std::string(R"({"type":"switchbot_command","device_id":"ABC123","command":"press",)"
                       R"("device_name":"Living Bot","parameter":"default","command_type":"command"})"));
}

TEST(config_switchbot_command_json_parameter_and_customize) {
  CHECK_EQ(action_json(R"({"type": "switchbot_command", "device_id": "X", "command": "setColor",
                           "parameter": {"r": 1}, "command_type": "customize"})"),
           std::string(R"({"type":"switchbot_command","device_id":"X","command":"setColor",)"
                       R"("parameter":{"r":1},"command_type":"customize"})"));
  // Null and empty parameters fall back to "default"; other scalars are kept.
  const std::string tail = R"("command":"c","parameter":)";
  CHECK_EQ(action_json(R"({"type": "switchbot_command", "device_id": "X", "command": "c", "parameter": null})"),
           std::string(R"({"type":"switchbot_command","device_id":"X",)" + tail +
                       R"("default","command_type":"command"})"));
  CHECK_EQ(action_json(R"({"type": "switchbot_command", "device_id": "X", "command": "c", "parameter": ""})"),
           std::string(R"({"type":"switchbot_command","device_id":"X",)" + tail +
                       R"("default","command_type":"command"})"));
  CHECK_EQ(action_json(R"({"type": "switchbot_command", "device_id": "X", "command": "c", "parameter": 50})"),
           std::string(R"({"type":"switchbot_command","device_id":"X",)" + tail +
                       R"(50,"command_type":"command"})"));
  CHECK_EQ(action_json(R"({"type": "switchbot_command", "device_id": "X", "command": "c", "parameter": "255:0:0"})"),
           std::string(R"({"type":"switchbot_command","device_id":"X",)" + tail +
                       R"("255:0:0","command_type":"command"})"));
}

TEST(config_switchbot_command_rejects_bad_input) {
  const Strings bad = {
      R"({"type": "switchbot_command", "command": "press"})",
      R"({"type": "switchbot_command", "device_id": "X"})",
      R"({"type": "switchbot_command", "device_id": "X", "command": "p", "command_type": "other"})",
      R"({"type": "switchbot_command", "device_id": "X", "command": "p", "command_type": null})",
      R"({"type": "switchbot_command", "device_id": "X", "command": "p", "parameter": ")" +
          std::string(4000, 'x') + R"("})",
  };
  for (const std::string& action : bad) CHECK(!action_error(action).empty());
  CHECK_EQ(action_error(bad[2]), std::string("command type must be one of: command, customize"));
  CHECK_EQ(action_error(bad[4]), std::string("SwitchBot parameter is too long"));
  CHECK_EQ(action_error(bad[0]), std::string("SwitchBot device must be a string"));
}

TEST(config_switchbot_scene) {
  CHECK_EQ(action_json(R"({"type": "switchbot_scene", "scene_id": "S1", "scene_name": "Night"})"),
           std::string(R"({"type":"switchbot_scene","scene_id":"S1","scene_name":"Night"})"));
  CHECK_EQ(action_json(R"({"type": "switchbot_scene", "scene_id": "S1"})"),
           std::string(R"({"type":"switchbot_scene","scene_id":"S1"})"));
  CHECK(!action_error(R"({"type": "switchbot_scene"})").empty());
  CHECK_EQ(action_error(R"({"type": "switchbot_scene"})"), std::string("SwitchBot scene must be a string"));
}

// ---- parse_trigger ---------------------------------------------------------------------------

TEST(config_valid_triggers) {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"button:1", "press"}, {"button:4", "hold_press"}, {"button:99", "long_triple_press"},
      {"wheel:left", "turn"}, {"wheel:right", "turn"}};
  for (const auto& [input, event] : cases) {
    JsonPtr j = J(R"({"button": "desk", "input": ")" + input + R"(", "event": ")" + event + R"("})");
    Parsed<Trigger> t = parse_trigger(j.get());
    CHECK(t.ok());
    if (t.ok()) CHECK(!t.value->channel.has_value());
  }
}

TEST(config_trigger_channel) {
  JsonPtr j = J(R"({"button": "d", "input": "button:1", "event": "press", "channel": 3})");
  Parsed<Trigger> t = parse_trigger(j.get());
  CHECK(t.ok());
  CHECK_EQ(t.ok() ? t.value->channel : std::optional<int>(), std::optional<int>(3));
  j = J(R"({"button": "d", "input": "button:1", "event": "press", "channel": 2.0})");
  t = parse_trigger(j.get());
  CHECK_EQ(t.ok() ? t.value->channel : std::optional<int>(), std::optional<int>(2));
  j = J(R"({"button": "d", "input": "button:1", "event": "press", "channel": 255})");
  CHECK(parse_trigger(j.get()).ok());
}

TEST(config_invalid_triggers) {
  const Strings bad = {
      R"({"button": "d", "input": "button:0", "event": "press"})",
      R"({"button": "d", "input": "button:100", "event": "press"})",
      R"({"button": "d", "input": "wheel:up", "event": "turn"})",
      R"({"button": "d", "input": "button:1", "event": "turn"})",
      R"({"button": "d", "input": "wheel:left", "event": "press"})",
      R"({"button": "d", "input": "button:1", "event": "kick"})",
      R"({"button": "d", "input": "button:1", "event": "press", "channel": -1})",
      R"({"button": "d", "input": "button:1", "event": "press", "channel": 256})",
      R"({"button": "d", "input": "button:1", "event": "press", "channel": 1.5})",
      R"({"button": "d", "input": "button:1", "event": "press", "channel": "1"})",
      R"({"button": "d", "input": "button:1", "event": "press", "channel": true})",
      R"({"button": "bad id!", "input": "button:1", "event": "press"})",
      R"([])",
  };
  for (const std::string& trigger : bad) CHECK(!trigger_error(trigger).empty());
  CHECK_EQ(trigger_error(bad[0]),
           std::string("trigger input must be button:1 .. button:99, wheel:left or wheel:right"));
  CHECK_EQ(trigger_error(bad[3]),
           std::string("trigger event for button:1 must be one of: press, double_press, triple_press, "
                       "long_press, long_double_press, long_triple_press, hold_press"));
  CHECK_EQ(trigger_error(bad[4]), std::string("trigger event for wheel:left must be one of: turn"));
  CHECK_EQ(trigger_error(bad[6]),
           std::string("trigger channel must be a number from 0 to 255, or empty for any channel"));
  CHECK_EQ(trigger_error(bad[11]), std::string("trigger button must be 1-64 letters, digits, '-' or '_'"));
}

// ---- config_from_json / text ------------------------------------------------------------------

TEST(config_valid_document_round_trips) {
  Config config = load(doc());
  CHECK_EQ(config.buttons[0].mac, std::string("AA:BB:CC:DD:EE:01"));
  CHECK_EQ(config.bindings[0].trigger.input, std::string("button:2"));
  JsonPtr out = config_to_json(config, false);
  Config again = load(print(out.get()));
  CHECK_EQ(again.bindings[0].id, std::string("wake"));
  // Exporting the re-imported config gives the same text, compact and pretty.
  CHECK_EQ(config_to_text(again, false, false), config_to_text(config, false, false));
  CHECK_EQ(config_to_text(again, true, true), config_to_text(config, true, true));
}

TEST(config_round_trip_keeps_every_action_kind) {
  const std::string text = R"({"version": 1, "settings": {"http_timeout_s": 7.5},
    "buttons": [)" + std::string(kButton) + R"(],
    "bindings": [
      {"id": "a", "name": "A", "enabled": false, "cooldown_ms": 0,
       "trigger": {"button": "desk", "channel": 2, "input": "button:1", "event": "hold_press"},
       "action": {"type": "http", "method": "put", "url": "https://h/x", "headers": {"A": "1", "B": "2"},
                  "body": "hi", "timeout_s": 2.5}},
      {"id": "b", "name": "B",
       "trigger": {"button": "desk", "input": "wheel:left", "event": "turn"},
       "action": {"type": "switchbot_command", "device_id": "D", "command": "setAll",
                  "parameter": "26,1,3,on", "command_type": "command", "device_name": "Aircon"}},
      {"id": "c", "name": "C",
       "trigger": {"button": "desk", "input": "wheel:right", "event": "turn"},
       "action": {"type": "switchbot_scene", "scene_id": "S", "scene_name": "Night"}}],
    "switchbot": {"token": "t", "secret": "s"}})";
  Config first = load(text);
  CHECK_EQ(first.bindings.size(), static_cast<size_t>(3));
  CHECK_EQ(first.settings.http_timeout_s, 7.5);
  CHECK(!first.bindings[0].enabled);
  CHECK_EQ(first.bindings[0].cooldown_ms, 0);
  CHECK_EQ(first.bindings[0].trigger.channel, std::optional<int>(2));
  const std::string exported = config_to_text(first, true, true);
  Config second = load(exported);
  CHECK_EQ(config_to_text(second, true, true), exported);
  CHECK_EQ(config_to_text(second, true, false), config_to_text(first, true, false));
  CHECK_EQ(second.switchbot_token, std::string("t"));
  const HttpAction* http = std::get_if<HttpAction>(&second.bindings[0].action);
  CHECK(http != nullptr);
  if (http != nullptr) {
    CHECK_EQ(http->method, std::string("PUT"));
    CHECK_EQ(http->headers.size(), static_cast<size_t>(2));
    CHECK_EQ(http->timeout_s.value_or(0.0), 2.5);
  }
}

TEST(config_empty_document_is_valid) {
  Config config = load("{}");
  CHECK(config.buttons.empty());
  CHECK(config.bindings.empty());
  CHECK_EQ(config.settings.http_timeout_s, 10.0);
  CHECK(!config.has_switchbot_credentials());
}

TEST(config_binding_must_refer_to_a_known_button) {
  std::string text = doc();
  text.replace(text.find("\"button\": \"desk\""), 16, "\"button\": \"ghost\"");
  CHECK_EQ(config_error(text), std::string("binding refers to an unknown button: 'ghost'"));
}

TEST(config_duplicate_ids_and_macs_are_rejected) {
  const std::string head = R"({"buttons": [)";
  CHECK_EQ(config_error(head + kButton + "," + button_json("desk", "aa:bb:cc:dd:ee:02") + "]}"),
           std::string("duplicate button id: 'desk'"));
  CHECK_EQ(config_error(head + kButton + "," + button_json("desk2", "AA-BB-CC-DD-EE-01") + "]}"),
           std::string("buttons 'desk' and 'desk2' have the same MAC address"));
  CHECK_EQ(config_error(doc().substr(0, doc().size() - 2) + "," + kBinding + "]}"),
           std::string("duplicate binding id: 'wake'"));
}

TEST(config_unsupported_version) {
  CHECK_EQ(config_error(R"({"version": 2})"), std::string("unsupported config version: 2"));
  CHECK_EQ(config_error(R"({"version": "1"})"), std::string("unsupported config version: '1'"));
  CHECK_EQ(config_error(R"({"version": null})"), std::string("unsupported config version: None"));
  CHECK_EQ(config_error(R"({"version": true})"), std::string("unsupported config version: True"));
  CHECK_EQ(config_error(R"({"version": 1.5})"), std::string("unsupported config version: 1.5"));
  CHECK(config_error(R"({"version": 1})").empty());
}

TEST(config_structure_errors) {
  CHECK_EQ(config_error("{not json"), std::string("invalid JSON"));
  CHECK_EQ(config_error("{} trailing"), std::string("invalid JSON"));
  CHECK_EQ(config_error("[]"), std::string("config must be an object"));
  CHECK_EQ(config_error(R"({"buttons": {}})"), std::string("buttons must be a list"));
  CHECK_EQ(config_error(R"({"bindings": null})"), std::string("bindings must be a list"));
  CHECK_EQ(config_error(R"({"buttons": [1]})"), std::string("button must be an object"));
  CHECK_EQ(config_error(R"({"bindings": [{"id": "x"}]})"), std::string("trigger must be an object"));
  CHECK_EQ(config_error(R"({"settings": null})"), std::string("settings must be an object"));
  CHECK_EQ(config_error(R"({"switchbot": 5})"), std::string("switchbot must be an object"));
  CHECK(config_error(R"({"switchbot": null})").empty());
}

TEST(config_button_validation) {
  CHECK_EQ(config_error(R"({"buttons": [{"id": "a b", "name": "n", "mac": "aa:bb:cc:dd:ee:ff"}]})"),
           std::string("button id must be 1-64 letters, digits, '-' or '_'"));
  CHECK_EQ(config_error(R"({"buttons": [{"id": "a", "name": "  ", "mac": "aa:bb:cc:dd:ee:ff"}]})"),
           std::string("button name must not be empty"));
  CHECK_EQ(config_error(R"({"buttons": [{"id": "a", "name": "n", "mac": "zz"}]})"),
           std::string("invalid MAC address: 'zz'"));
  CHECK_EQ(config_error(R"({"buttons": [{"id": "a", "name": "n", "mac": "aa:bb:cc:dd:ee:ff", "model": null}]})"),
           std::string("button model must be a string"));
  CHECK_EQ(config_error(R"({"buttons": [{"id": "a", "name": "n", "mac": "aa:bb:cc:dd:ee:ff:00:11"}]})"),
           std::string("button MAC address is too long (max 17 characters)"));
  CHECK_EQ(config_error(R"({"buttons": [{"id": "a", "name": ")" + std::string(61, 'n') +
                        R"(", "mac": "aa:bb:cc:dd:ee:ff"}]})"),
           std::string("button name is too long (max 60 characters)"));
  Config config = load(R"({"buttons": [{"id": "a", "name": "  padded  ", "mac": " aabbccddeeff "}]})");
  CHECK_EQ(config.buttons[0].name, std::string("padded"));
  CHECK_EQ(config.buttons[0].mac, std::string("AA:BB:CC:DD:EE:FF"));
  CHECK_EQ(config.buttons[0].model, std::string(""));
  // Limits count characters, not bytes: 60 two-byte characters are fine.
  std::string name;
  for (int i = 0; i < 60; i++) name += "\xc3\xa9";
  CHECK(config_error(R"({"buttons": [{"id": "a", "name": ")" + name + R"(", "mac": "aa:bb:cc:dd:ee:ff"}]})").empty());
}

TEST(config_cooldown_validation) {
  const Strings bad = {"-1", "60001", "\"5\"", "1.5", "true", "null"};
  for (const std::string& value : bad) {
    std::string text = doc();
    text.replace(text.find("\"cooldown_ms\": 700"), 18, "\"cooldown_ms\": " + value);
    CHECK_EQ(config_error(text),
             std::string("cooldown must be a whole number of milliseconds from 0 to 60000"));
  }
  for (const std::string& value : Strings{"0", "60000", "2.0"}) {
    std::string text = doc();
    text.replace(text.find("\"cooldown_ms\": 700"), 18, "\"cooldown_ms\": " + value);
    CHECK(config_error(text).empty());
  }
  std::string text = doc();
  text.replace(text.find("\"enabled\": true"), 15, "\"enabled\": 1");
  CHECK_EQ(config_error(text), std::string("enabled must be true or false"));
}

TEST(config_settings_validation) {
  CHECK_EQ(load(R"({"settings": {"http_timeout_s": 5}})").settings.http_timeout_s, 5.0);
  CHECK_EQ(load(R"({"settings": {}})").settings.http_timeout_s, 10.0);
  for (const std::string& value : Strings{"0", "100", "\"x\"", "true", "null"}) {
    CHECK_EQ(config_error(R"({"settings": {"http_timeout_s": )" + value + "}}"),
             std::string("HTTP timeout must be between 1 and 60 seconds"));
  }
}

TEST(config_credentials_are_exported_only_when_asked) {
  Config config = load(doc(R"(, "switchbot": {"token": "tok", "secret": "sec"})"));
  CHECK_EQ(config.switchbot_token, std::string("tok"));
  CHECK_EQ(config.switchbot_secret, std::string("sec"));
  const std::string without = config_to_text(config, false, true);
  CHECK(without.find("switchbot") == std::string::npos);
  CHECK(without.find("tok") == std::string::npos);
  CHECK(without.find("sec") == std::string::npos);
  JsonPtr with = config_to_json(config, true);
  const cJSON* sb = cJSON_GetObjectItemCaseSensitive(with.get(), "switchbot");
  CHECK(sb != nullptr);
  CHECK_EQ(print(sb), std::string(R"({"token":"tok","secret":"sec"})"));
  // Without any credentials nothing is added even when asked.
  CHECK(config_to_text(load(doc()), true, false).find("switchbot") == std::string::npos);
  CHECK_EQ(config_error(doc(R"(, "switchbot": {"token": 5})")), std::string("SwitchBot token must be a string"));
}

// ---- serialization layout ---------------------------------------------------------------------

TEST(config_pretty_text_layout_and_key_order) {
  const std::string expected =
      "{\n"
      "  \"version\": 1,\n"
      "  \"settings\": {\n"
      "    \"http_timeout_s\": 10.0\n"
      "  },\n"
      "  \"buttons\": [\n"
      "    {\n"
      "      \"id\": \"desk\",\n"
      "      \"name\": \"Desk remote\",\n"
      "      \"mac\": \"AA:BB:CC:DD:EE:01\",\n"
      "      \"model\": \"SBBT-104CUS\"\n"
      "    }\n"
      "  ],\n"
      "  \"bindings\": [\n"
      "    {\n"
      "      \"id\": \"wake\",\n"
      "      \"name\": \"Wake PC\",\n"
      "      \"enabled\": true,\n"
      "      \"cooldown_ms\": 700,\n"
      "      \"trigger\": {\n"
      "        \"button\": \"desk\",\n"
      "        \"channel\": null,\n"
      "        \"input\": \"button:2\",\n"
      "        \"event\": \"press\"\n"
      "      },\n"
      "      \"action\": {\n"
      "        \"type\": \"http\",\n"
      "        \"method\": \"GET\",\n"
      "        \"url\": \"http://wol.local/wake\"\n"
      "      }\n"
      "    }\n"
      "  ]\n"
      "}";
  CHECK_EQ(config_to_text(load(doc()), false, true), expected);
}

TEST(config_compact_text_and_credentials_order) {
  Config config = load(doc(R"(, "switchbot": {"token": "tok", "secret": "sec"})"));
  const std::string compact = config_to_text(config, true, false);
  const char* const keys[] = {"\"version\"", "\"settings\"", "\"buttons\"", "\"bindings\"", "\"switchbot\""};
  size_t last = 0;
  for (const char* key : keys) {
    const size_t at = compact.find(key);
    CHECK(at != std::string::npos);
    CHECK(at >= last);
    last = at;
  }
  CHECK(compact.find('\n') == std::string::npos);
  CHECK_EQ(compact.find(R"({"version":1,"settings":{"http_timeout_s":10.0},"buttons":[{)"), static_cast<size_t>(0));
}

TEST(config_empty_config_text) {
  CHECK_EQ(config_to_text(Config(), false, true),
           std::string("{\n  \"version\": 1,\n  \"settings\": {\n    \"http_timeout_s\": 10.0\n  },\n"
                       "  \"buttons\": [],\n  \"bindings\": []\n}"));
}

TEST(config_binding_and_button_json) {
  Config config = load(doc());
  JsonPtr b = button_to_json(config.buttons[0]);
  CHECK_EQ(print(b.get()),
           std::string(R"({"id":"desk","name":"Desk remote","mac":"AA:BB:CC:DD:EE:01","model":"SBBT-104CUS"})"));
  JsonPtr binding = binding_to_json(config.bindings[0]);
  CHECK_EQ(print(binding.get()),
           std::string(R"({"id":"wake","name":"Wake PC","enabled":true,"cooldown_ms":700,)"
                       R"("trigger":{"button":"desk","channel":null,"input":"button:2","event":"press"},)"
                       R"("action":{"type":"http","method":"GET","url":"http://wol.local/wake"}})"));
}

TEST(config_new_id_format) {
  CHECK_EQ(new_id("btn", 0x00abcdef), std::string("btn-abcdef"));
  CHECK_EQ(new_id("bind", 0xff000012), std::string("bind-000012"));
  CHECK_EQ(new_id("x", 0), std::string("x-000000"));
}

// ---- import ----------------------------------------------------------------------------------

TEST(config_import_replace_swaps_everything_but_keeps_credentials) {
  JsonPtr other = J(R"({"buttons": [)" + button_json("b2", "11:22:33:44:55:66") + R"(], "bindings": []})");
  Parsed<Config> result = import_config(current_config(), other.get(), "replace");
  CHECK(result.ok());
  if (!result.ok()) return;
  CHECK_EQ(joined(ids_of(*result.value, true)), std::string("b2"));
  CHECK(result.value->bindings.empty());
  CHECK_EQ(result.value->switchbot_token, std::string("old-token"));
  CHECK_EQ(result.value->switchbot_secret, std::string("old-secret"));
}

TEST(config_import_replace_uses_imported_credentials_when_present) {
  JsonPtr other = J(R"({"switchbot": {"token": "new", "secret": "newer"}})");
  Parsed<Config> result = import_config(current_config(), other.get(), "replace");
  CHECK(result.ok());
  if (!result.ok()) return;
  CHECK_EQ(result.value->switchbot_token, std::string("new"));
  CHECK_EQ(result.value->switchbot_secret, std::string("newer"));
  // Each half is kept or replaced on its own.
  other = J(R"({"switchbot": {"token": "only-token"}})");
  result = import_config(current_config(), other.get(), "replace");
  CHECK(result.ok());
  if (!result.ok()) return;
  CHECK_EQ(result.value->switchbot_token, std::string("only-token"));
  CHECK_EQ(result.value->switchbot_secret, std::string("old-secret"));
}

TEST(config_import_replace_rejects_bindings_without_buttons) {
  // The current buttons are not available to a replace.
  JsonPtr other = J(R"({"bindings": [)" + binding_json("x", "X") + "]}");
  Parsed<Config> result = import_config(current_config(), other.get(), "replace");
  CHECK_EQ(result.error, std::string("binding refers to an unknown button: 'desk'"));
}

TEST(config_import_merge_adds_and_overwrites_by_id) {
  JsonPtr other = J(R"({"buttons": [], "bindings": [)" + binding_json("wake", "Renamed") + "," +
                    binding_json("two", "Second") + "]}");
  Parsed<Config> result = import_config(current_config(), other.get(), "merge");
  CHECK(result.ok());
  if (!result.ok()) return;
  CHECK_EQ(joined(ids_of(*result.value, true)), std::string("desk"));
  CHECK_EQ(joined(ids_of(*result.value, false)), std::string("wake,two"));
  CHECK_EQ(result.value->bindings[0].name, std::string("Renamed"));
  CHECK_EQ(result.value->bindings[1].name, std::string("Second"));
  CHECK_EQ(result.value->switchbot_token, std::string("old-token"));
}

TEST(config_import_merge_keeps_settings_unless_given) {
  Config current = current_config();
  current.settings.http_timeout_s = 20.0;
  JsonPtr without = J("{}");
  Parsed<Config> result = import_config(current, without.get(), "merge");
  CHECK(result.ok());
  CHECK_EQ(result.ok() ? result.value->settings.http_timeout_s : 0.0, 20.0);
  JsonPtr with = J(R"({"settings": {"http_timeout_s": 3}})");
  result = import_config(current, with.get(), "merge");
  CHECK(result.ok());
  CHECK_EQ(result.ok() ? result.value->settings.http_timeout_s : 0.0, 3.0);
  // A replace always takes the imported settings, defaults included.
  result = import_config(current, without.get(), "replace");
  CHECK_EQ(result.ok() ? result.value->settings.http_timeout_s : 0.0, 10.0);
}

TEST(config_import_merge_allows_binding_to_an_existing_button) {
  JsonPtr other = J(R"({"bindings": [)" + binding_json("three", "Third") + "]}");
  Parsed<Config> result = import_config(current_config(), other.get(), "merge");
  CHECK(result.ok());
  CHECK_EQ(result.ok() ? result.value->bindings.size() : 0, static_cast<size_t>(2));
}

TEST(config_import_merge_rejects_a_mac_clash_with_another_button) {
  JsonPtr other = J(R"({"buttons": [)" + button_json("clone", "aa:bb:cc:dd:ee:01") + "]}");
  Parsed<Config> result = import_config(current_config(), other.get(), "merge");
  CHECK_EQ(result.error, std::string("buttons 'desk' and 'clone' have the same MAC address"));
}

TEST(config_import_merge_overwrites_a_button_with_the_same_id) {
  JsonPtr other = J(R"({"buttons": [)" + button_json("desk", "11:22:33:44:55:66") + "]}");
  Parsed<Config> result = import_config(current_config(), other.get(), "merge");
  CHECK(result.ok());
  if (!result.ok()) return;
  CHECK_EQ(result.value->buttons.size(), static_cast<size_t>(1));
  CHECK_EQ(result.value->buttons[0].mac, std::string("11:22:33:44:55:66"));
}

TEST(config_invalid_import_changes_nothing) {
  const Config current = current_config();
  const std::string before = config_to_text(current, true, false);
  JsonPtr bad = J(R"({"bindings": [{"id": "x"}]})");
  CHECK(!import_config(current, bad.get(), "merge").ok());
  JsonPtr good = J(doc());
  CHECK(!import_config(current, good.get(), "overwrite").ok());
  CHECK_EQ(config_to_text(current, true, false), before);
}

TEST(config_import_error_messages) {
  JsonPtr good = J(doc());
  CHECK_EQ(import_config(current_config(), good.get(), "overwrite").error,
           std::string("import mode must be 'merge' or 'replace'"));
  // The mode is checked before the document.
  CHECK_EQ(import_config(current_config(), nullptr, "").error,
           std::string("import mode must be 'merge' or 'replace'"));
  CHECK_EQ(import_config(current_config(), nullptr, "merge").error, std::string("config must be an object"));
  JsonPtr v2 = J(R"({"version": 2})");
  CHECK_EQ(import_config(current_config(), v2.get(), "replace").error,
           std::string("unsupported config version: 2"));
}

TEST(config_json_owner_releases_memory) {
  // Parse failures and early returns must not leak; this exercises the owner on an empty value.
  JsonPtr none = make_json();
  CHECK(none.get() == nullptr);
  JsonPtr obj = make_json(cJSON_CreateObject());
  CHECK(obj.get() != nullptr);
}
