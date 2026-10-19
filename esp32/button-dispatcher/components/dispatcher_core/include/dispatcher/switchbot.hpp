// SwitchBot Open API v1.1 client, response parsing, and a service that caches the device and scene lists.
//
// Mirrors pi/button-dispatcher/button_dispatcher/switchbot.py and switchbot_commands.py. API reference:
// https://github.com/OpenWonderLabs/SwitchBotAPI. Every request carries the token, a timestamp, a nonce and an
// HMAC-SHA256 signature of the three made with the secret. Credentials come from the config (typed in the web
// page), never from code. All network access goes through Platform::http_request.

#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cJSON.h"
#include "dispatcher/config.hpp"
#include "dispatcher/platform.hpp"

namespace dispatcher {

constexpr const char* kSwitchBotApiBase = "https://api.switch-bot.com/v1.1";
constexpr double kSwitchBotTimeoutS = 15.0;
// Device lists of a large account can be big; a response beyond this is cut off and reported as invalid.
constexpr size_t kSwitchBotMaxResponseBytes = 48 * 1024;

// A physical device or an IR remote (a virtual device) from GET /devices.
struct SwitchBotDevice {
  std::string id;
  std::string name;
  std::string type;                       // deviceType, or "IR <remoteType>" for an IR remote
  std::string hub;                        // the hub device id, empty if none
  std::optional<std::string> remote_type; // set only for IR remotes, for example "TV"
};

struct SwitchBotScene {
  std::string id;
  std::string name;
};

// Reduces the "body" of GET /devices to a list: physical devices first, then IR remotes. Tolerates missing
// or malformed parts (they are skipped).
std::vector<SwitchBotDevice> device_items(const cJSON* body);
// Reduces the "body" of GET /scenes (an array) to a list.
std::vector<SwitchBotScene> scene_items(const cJSON* body);

// One documented command: its name, a hint for the parameter, and a description.
struct CommandRow {
  std::string command;
  std::string parameter;
  std::string description;
};

struct CommandList {
  std::vector<CommandRow> commands;
  std::vector<std::string> notes;
};

// The documented commands for a device, from a table built into the firmware (generated from the Python
// project's switchbot_commands.py by tools/gen_switchbot_commands.py). `remote_type` is set for IR remotes,
// whose commands depend on the appliance type. Unknown types give an empty list and a note.
CommandList commands_for(const std::string& device_type, const std::optional<std::string>& remote_type);

// The signed headers the API requires on every request: Authorization (the token), t (milliseconds as text),
// nonce, sign (base64 of HMAC-SHA256 over token + t + nonce, keyed with the secret) and Content-Type.
HeaderList make_auth_headers(Platform& platform, const std::string& token, const std::string& secret,
                             const std::string& t, const std::string& nonce);

// The body of a successful API response, or the message to show the user.
using SwitchBotBody = Parsed<JsonPtr>;

class SwitchBotClient {
 public:
  SwitchBotClient(Platform& platform, std::string token, std::string secret,
                  std::string api_base = kSwitchBotApiBase, double timeout_s = kSwitchBotTimeoutS);

  // Sends a request ("GET" or "POST"; `json_body` may be empty) and returns the response's "body" field.
  // Errors: no wall clock yet, transport failures, HTTP errors, invalid JSON, or a non-100 statusCode (the
  // message then names the code and explains the well-known ones).
  SwitchBotBody request(const std::string& method, const std::string& path, const std::string& json_body = "");

  SwitchBotBody devices();
  SwitchBotBody scenes();
  // `parameter_json` is the command parameter as JSON text (for example "\"default\"" or "{\"a\":1}").
  SwitchBotBody command(const std::string& device_id, const std::string& command, const std::string& parameter_json,
                        const std::string& command_type);
  SwitchBotBody execute_scene(const std::string& scene_id);

 private:
  Platform& platform_;
  std::string token_;
  std::string secret_;
  std::string api_base_;
  double timeout_s_;
};

// Creates clients from the current credentials and caches the device and scene lists. Thread-safe; network
// calls are made without holding its internal lock.
class SwitchBotService {
 public:
  using Credentials = std::function<std::pair<std::string, std::string>()>;  // (token, secret)

  static constexpr double kCacheTtlS = 300.0;

  SwitchBotService(Platform& platform, Credentials credentials, std::string api_base = kSwitchBotApiBase);

  // Drops cached lists, for example after the credentials changed.
  void invalidate();

  Parsed<std::vector<SwitchBotDevice>> devices(bool refresh);
  Parsed<std::vector<SwitchBotScene>> scenes(bool refresh);

  struct DeviceCommands {
    SwitchBotDevice device;
    CommandList list;
  };
  // The documented commands for one of the user's devices, looked up in the (cached) device list.
  Parsed<DeviceCommands> commands_for_device(const std::string& device_id);

  // Run a command or a scene with the current credentials. The error is the message to show the user.
  SwitchBotBody command(const std::string& device_id, const std::string& command, const std::string& parameter_json,
                        const std::string& command_type);
  SwitchBotBody execute_scene(const std::string& scene_id);

 private:
  // A client for the current credentials, or the "not set" message.
  Parsed<SwitchBotClient> client();

  Platform& platform_;
  Credentials credentials_;
  std::string api_base_;
  std::mutex mutex_;
  std::optional<std::pair<double, std::vector<SwitchBotDevice>>> device_cache_;
  std::optional<std::pair<double, std::vector<SwitchBotScene>>> scene_cache_;
};

}  // namespace dispatcher
