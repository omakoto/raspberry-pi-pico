// SwitchBot Open API v1.1 client, response parsing and the caching service. See switchbot.hpp.

#include "dispatcher/switchbot.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dispatcher/util.hpp"
#include "switchbot_commands.hpp"

namespace dispatcher {

namespace {

constexpr int kStatusOk = 100;

// The explanation of the well-known statusCode values, or null for other codes.
const char* known_status_text(int code) {
  switch (code) {
    case 151: return "device type error";
    case 152: return "device not found";
    case 160: return "command is not supported by this device";
    case 161: return "device offline";
    case 171: return "hub device offline";
    case 190: return "device internal error (often an invalid parameter or command)";
    default: return nullptr;
  }
}

// A string member, or `fallback` if it is absent or not a string.
std::string string_member(const cJSON* object, const char* key, const std::string& fallback = "") {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (cJSON_IsString(item) && item->valuestring != nullptr) return item->valuestring;
  return fallback;
}

// Quotes text the way Python's repr() does for the common cases, so messages read the same on both
// implementations: single quotes unless the text contains a single quote and no double quote.
std::string quote_repr(const std::string& text) {
  const bool use_double = text.find('\'') != std::string::npos && text.find('"') == std::string::npos;
  const char quote = use_double ? '"' : '\'';
  std::string out(1, quote);
  for (char c : text) {
    if (c == '\\' || c == quote) {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else {
      out += c;
    }
  }
  out += quote;
  return out;
}

// Formats like Python's {:g}.
std::string format_g(double value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%g", value);
  return text;
}

// The status code as shown in messages: an integer when it is one, "None" when absent or not a number.
std::string code_text(const cJSON* code) {
  if (!cJSON_IsNumber(code)) return "None";
  char text[40];
  if (code->valuedouble == static_cast<double>(static_cast<long long>(code->valuedouble))) {
    std::snprintf(text, sizeof(text), "%lld", static_cast<long long>(code->valuedouble));
  } else {
    std::snprintf(text, sizeof(text), "%g", code->valuedouble);
  }
  return text;
}

SwitchBotBody failure(const std::string& message) {
  SwitchBotBody result;
  result.error = message;
  return result;
}

SwitchBotBody success(JsonPtr body) {
  SwitchBotBody result;
  result.value.emplace(std::move(body));
  return result;
}

// Parses a complete JSON document; nullptr if the text is not exactly one JSON value (trailing garbage and
// truncated text are both rejected).
cJSON* parse_whole(const std::string& text) {
  const char* end = nullptr;
  cJSON* json = cJSON_ParseWithLengthOpts(text.data(), text.size(), &end, /*require_null_terminated=*/false);
  if (json == nullptr) return nullptr;
  const char* limit = text.data() + text.size();
  while (end != nullptr && end < limit && std::isspace(static_cast<unsigned char>(*end))) ++end;
  if (end != limit) {
    cJSON_Delete(json);
    return nullptr;
  }
  return json;
}

std::string trim(const std::string& text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  return text.substr(begin, end - begin);
}

const generated::CommandTable* find_table(const generated::CommandTable* tables, size_t count,
                                          const std::string& type) {
  for (size_t i = 0; i < count; ++i) {
    if (type == tables[i].type) return &tables[i];
  }
  return nullptr;
}

void append_rows(std::vector<CommandRow>* out, const generated::CommandEntry* rows, size_t count) {
  for (size_t i = 0; i < count; ++i) out->push_back({rows[i].command, rows[i].parameter, rows[i].description});
}

}  // namespace

std::vector<SwitchBotDevice> device_items(const cJSON* body) {
  std::vector<SwitchBotDevice> items;
  if (!cJSON_IsObject(body)) return items;
  const cJSON* list = cJSON_GetObjectItemCaseSensitive(body, "deviceList");
  if (cJSON_IsArray(list)) {
    const cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, list) {
      if (!cJSON_IsObject(entry)) continue;
      SwitchBotDevice device;
      device.id = string_member(entry, "deviceId");
      if (device.id.empty()) continue;
      device.name = string_member(entry, "deviceName");
      device.type = string_member(entry, "deviceType");
      device.hub = string_member(entry, "hubDeviceId");
      items.push_back(std::move(device));
    }
  }
  const cJSON* remotes = cJSON_GetObjectItemCaseSensitive(body, "infraredRemoteList");
  if (cJSON_IsArray(remotes)) {
    const cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, remotes) {
      if (!cJSON_IsObject(entry)) continue;
      SwitchBotDevice device;
      device.id = string_member(entry, "deviceId");
      if (device.id.empty()) continue;
      device.name = string_member(entry, "deviceName");
      const std::string remote_type = string_member(entry, "remoteType");
      device.type = trim("IR " + remote_type);
      device.hub = string_member(entry, "hubDeviceId");
      device.remote_type = remote_type;
      items.push_back(std::move(device));
    }
  }
  return items;
}

std::vector<SwitchBotScene> scene_items(const cJSON* body) {
  std::vector<SwitchBotScene> items;
  if (!cJSON_IsArray(body)) return items;
  const cJSON* entry = nullptr;
  cJSON_ArrayForEach(entry, body) {
    if (!cJSON_IsObject(entry)) continue;
    SwitchBotScene scene;
    scene.id = string_member(entry, "sceneId");
    if (scene.id.empty()) continue;
    scene.name = string_member(entry, "sceneName");
    items.push_back(std::move(scene));
  }
  return items;
}

CommandList commands_for(const std::string& device_type, const std::optional<std::string>& remote_type) {
  CommandList result;
  if (!remote_type) {
    const generated::CommandTable* table =
        find_table(generated::kDeviceCommandTables, generated::kDeviceCommandTableCount, device_type);
    if (table == nullptr) {
      result.notes.push_back("No commands are listed for device type " + quote_repr(device_type) +
                             "; it may only support status, or be newer than this table. You can still type a "
                             "command by hand.");
      return result;
    }
    append_rows(&result.commands, table->rows, table->count);
    return result;
  }

  const std::string custom_note = "Buttons added in the app can be sent by name with the command type \"customize\".";
  if (*remote_type == "Others") {
    result.notes.push_back("\"Others\" remotes only have custom buttons. " + custom_note);
    return result;
  }
  result.notes.push_back(custom_note);
  // "DIY" remotes are learned button by button; their commands follow the base appliance type.
  std::string base = *remote_type;
  if (base.compare(0, 4, "DIY ") == 0) {
    base = base.substr(4);
    result.notes.push_back(
        "This is a learned (DIY) remote; buttons it hasn't learned won't work, and learned buttons may need the "
        "command type \"customize\".");
  }
  append_rows(&result.commands, generated::kIrCommonCommands, generated::kIrCommonCommandCount);
  const generated::CommandTable* table =
      find_table(generated::kIrCommandTables, generated::kIrCommandTableCount, base);
  if (table != nullptr) append_rows(&result.commands, table->rows, table->count);
  return result;
}

HeaderList make_auth_headers(Platform& platform, const std::string& token, const std::string& secret,
                             const std::string& t, const std::string& nonce) {
  const std::string digest = platform.hmac_sha256(secret, token + t + nonce);
  return {
      {"Authorization", token},
      {"t", t},
      {"nonce", nonce},
      {"sign", base64_encode(digest)},
      {"Content-Type", "application/json; charset=utf8"},
  };
}

// ---- SwitchBotClient ---------------------------------------------------------------------------

SwitchBotClient::SwitchBotClient(Platform& platform, std::string token, std::string secret, std::string api_base,
                                 double timeout_s)
    : platform_(platform),
      token_(std::move(token)),
      secret_(std::move(secret)),
      api_base_(std::move(api_base)),
      timeout_s_(timeout_s) {}

SwitchBotBody SwitchBotClient::request(const std::string& method, const std::string& path,
                                       const std::string& json_body) {
  // The signature covers a timestamp, which is meaningless until the clock has been set.
  if (!platform_.wall_clock_valid()) {
    return failure(
        "the clock is not set yet, so SwitchBot requests cannot be signed; check the Wi-Fi connection");
  }
  uint8_t random[16];
  platform_.random_bytes(random, sizeof(random));
  const HeaderList headers = make_auth_headers(platform_, token_, secret_, std::to_string(platform_.wall_time_ms()),
                                               format_uuid4(random));

  const HttpResponse response = platform_.http_request(method, api_base_ + path, headers, json_body, timeout_s_,
                                                       kSwitchBotMaxResponseBytes);
  if (response.failure == HttpResponse::Failure::kTimeout) return failure("timed out after " + format_g(timeout_s_) + "s");
  if (response.failure != HttpResponse::Failure::kNone) return failure("cannot reach SwitchBot");
  if (response.status == 401) return failure("HTTP 401: authentication failed; check the token and secret");
  if (response.status >= 400) return failure("HTTP " + std::to_string(response.status));

  JsonPtr payload = make_json(parse_whole(response.body));
  if (!payload || !cJSON_IsObject(payload.get())) return failure("invalid response from SwitchBot");

  const cJSON* code = cJSON_GetObjectItemCaseSensitive(payload.get(), "statusCode");
  if (!cJSON_IsNumber(code) || code->valuedouble != kStatusOk) {
    const char* known = cJSON_IsNumber(code) ? known_status_text(static_cast<int>(code->valuedouble)) : nullptr;
    std::string message = string_member(payload.get(), "message");
    if (message.empty()) message = known != nullptr ? known : "unknown error";
    if (known != nullptr && message != known) message += std::string(" (") + known + ")";
    return failure("API error " + code_text(code) + ": " + message);
  }

  cJSON* body = cJSON_DetachItemFromObjectCaseSensitive(payload.get(), "body");
  return success(make_json(body != nullptr ? body : cJSON_CreateNull()));
}

SwitchBotBody SwitchBotClient::devices() { return request("GET", "/devices"); }

SwitchBotBody SwitchBotClient::scenes() { return request("GET", "/scenes"); }

SwitchBotBody SwitchBotClient::command(const std::string& device_id, const std::string& command,
                                       const std::string& parameter_json, const std::string& command_type) {
  // A parameter that is not JSON (for example the bare word "default") is sent as a JSON string.
  cJSON* parameter = parse_whole(parameter_json);
  if (parameter == nullptr) parameter = cJSON_CreateString(parameter_json.c_str());
  JsonPtr body = make_json(cJSON_CreateObject());
  if (!body || parameter == nullptr) {
    cJSON_Delete(parameter);
    return failure("out of memory");
  }
  cJSON_AddItemToObject(body.get(), "command", cJSON_CreateString(command.c_str()));
  cJSON_AddItemToObject(body.get(), "parameter", parameter);
  cJSON_AddItemToObject(body.get(), "commandType", cJSON_CreateString(command_type.c_str()));
  char* text = cJSON_PrintUnformatted(body.get());
  if (text == nullptr) return failure("out of memory");
  const std::string text_copy = text;
  cJSON_free(text);
  return request("POST", "/devices/" + device_id + "/commands", text_copy);
}

SwitchBotBody SwitchBotClient::execute_scene(const std::string& scene_id) {
  return request("POST", "/scenes/" + scene_id + "/execute");
}

// ---- SwitchBotService --------------------------------------------------------------------------

SwitchBotService::SwitchBotService(Platform& platform, Credentials credentials, std::string api_base)
    : platform_(platform), credentials_(std::move(credentials)), api_base_(std::move(api_base)) {}

void SwitchBotService::invalidate() {
  std::lock_guard<std::mutex> lock(mutex_);
  device_cache_.reset();
  scene_cache_.reset();
}

Parsed<SwitchBotClient> SwitchBotService::client() {
  Parsed<SwitchBotClient> result;
  const std::pair<std::string, std::string> credentials = credentials_();
  if (credentials.first.empty() || credentials.second.empty()) {
    result.error = "SwitchBot token and secret are not set (see Settings)";
    return result;
  }
  result.value.emplace(platform_, credentials.first, credentials.second, api_base_);
  return result;
}

Parsed<std::vector<SwitchBotDevice>> SwitchBotService::devices(bool refresh) {
  Parsed<std::vector<SwitchBotDevice>> result;
  if (!refresh) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (device_cache_ && platform_.monotonic_s() - device_cache_->first < kCacheTtlS) {
      result.value = device_cache_->second;
      return result;
    }
  }
  // The lock is released while talking to the network so other threads are never blocked behind a slow request.
  Parsed<SwitchBotClient> client_or_error = client();
  if (!client_or_error.ok()) {
    result.error = client_or_error.error;
    return result;
  }
  SwitchBotBody body = client_or_error.value->devices();
  if (!body.ok()) {
    result.error = body.error;
    return result;
  }
  result.value = device_items(body.value->get());
  std::lock_guard<std::mutex> lock(mutex_);
  device_cache_.emplace(platform_.monotonic_s(), *result.value);
  return result;
}

Parsed<std::vector<SwitchBotScene>> SwitchBotService::scenes(bool refresh) {
  Parsed<std::vector<SwitchBotScene>> result;
  if (!refresh) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (scene_cache_ && platform_.monotonic_s() - scene_cache_->first < kCacheTtlS) {
      result.value = scene_cache_->second;
      return result;
    }
  }
  Parsed<SwitchBotClient> client_or_error = client();
  if (!client_or_error.ok()) {
    result.error = client_or_error.error;
    return result;
  }
  SwitchBotBody body = client_or_error.value->scenes();
  if (!body.ok()) {
    result.error = body.error;
    return result;
  }
  result.value = scene_items(body.value->get());
  std::lock_guard<std::mutex> lock(mutex_);
  scene_cache_.emplace(platform_.monotonic_s(), *result.value);
  return result;
}

Parsed<SwitchBotService::DeviceCommands> SwitchBotService::commands_for_device(const std::string& device_id) {
  Parsed<DeviceCommands> result;
  Parsed<std::vector<SwitchBotDevice>> devices_or_error = devices(false);
  if (!devices_or_error.ok()) {
    result.error = devices_or_error.error;
    return result;
  }
  for (const SwitchBotDevice& device : *devices_or_error.value) {
    if (device.id == device_id) {
      result.value.emplace(DeviceCommands{device, dispatcher::commands_for(device.type, device.remote_type)});
      return result;
    }
  }
  result.error = "no device " + quote_repr(device_id) + " in your SwitchBot account; refresh the list";
  return result;
}

SwitchBotBody SwitchBotService::command(const std::string& device_id, const std::string& command,
                                        const std::string& parameter_json, const std::string& command_type) {
  Parsed<SwitchBotClient> client_or_error = client();
  if (!client_or_error.ok()) return failure(client_or_error.error);
  return client_or_error.value->command(device_id, command, parameter_json, command_type);
}

SwitchBotBody SwitchBotService::execute_scene(const std::string& scene_id) {
  Parsed<SwitchBotClient> client_or_error = client();
  if (!client_or_error.ok()) return failure(client_or_error.error);
  return client_or_error.value->execute_scene(scene_id);
}

}  // namespace dispatcher
