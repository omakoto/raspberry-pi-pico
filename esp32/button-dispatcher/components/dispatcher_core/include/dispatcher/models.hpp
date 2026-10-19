// Plain data types shared by the config, the trigger engine and the web API.
//
// Mirrors pi/button-dispatcher/button_dispatcher/models.py. This library has no ESP-IDF
// dependencies so it can be unit-tested on the PC (see test/run.sh), and it never throws: ESP-IDF
// builds without C++ exceptions, so failures are returned as values.

#pragma once

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace dispatcher {

// Returns a MAC address as upper-case "AA:BB:CC:DD:EE:FF". Accepts ':' or '-' separators or none,
// any case, and surrounding spaces; returns std::nullopt for anything else.
std::optional<std::string> normalize_mac(const std::string& value);

// A BLE button or remote the user has taught the dispatcher (called "button" in the config and API).
struct Button {
  std::string id;
  std::string name;
  std::string mac;    // normalized
  std::string model;  // advertised model code, shown to the user and never matched on
};

// Which user action fires a binding.
struct Trigger {
  std::string button;                // a Button id
  std::string input;                 // "button:N" (N from 1) or "wheel:left" / "wheel:right"
  std::string event;                 // "press", "double_press", ..., or "turn" for the wheel
  std::optional<int> channel;        // match only this channel; any channel when empty
};

struct HttpAction {
  std::string method = "GET";                              // GET, POST, PUT or DELETE
  std::string url;                                         // http:// or https://
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::optional<double> timeout_s;                         // empty: use the settings default
};

struct SwitchBotCommandAction {
  std::string device_id;
  std::string device_name;                                 // for display only
  std::string command;
  // The command parameter as JSON text: "\"default\"" for a string, "{\"a\":1}" for an object.
  std::string parameter_json = "\"default\"";
  std::string command_type = "command";                    // "command" or "customize"
};

struct SwitchBotSceneAction {
  std::string scene_id;
  std::string scene_name;                                  // for display only
};

using Action = std::variant<HttpAction, SwitchBotCommandAction, SwitchBotSceneAction>;

// "http", "switchbot_command" or "switchbot_scene": the `type` used in the JSON.
const char* action_type_name(const Action& action);

// A trigger plus the action to run when it fires.
struct Binding {
  std::string id;
  std::string name;
  bool enabled = true;
  int cooldown_ms = 700;  // minimum time between two firings; absorbs wheel wiggles
  Trigger trigger;
  Action action;
};

struct Settings {
  double http_timeout_s = 10.0;
};

struct Config {
  std::vector<Button> buttons;
  std::vector<Binding> bindings;
  Settings settings;
  std::string switchbot_token;
  std::string switchbot_secret;

  const Button* find_button(const std::string& id) const;
  const Binding* find_binding(const std::string& id) const;
  bool has_switchbot_credentials() const { return !switchbot_token.empty() && !switchbot_secret.empty(); }
};

}  // namespace dispatcher
