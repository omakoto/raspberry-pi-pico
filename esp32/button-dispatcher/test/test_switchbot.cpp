// Tests for switchbot.cpp and the generated command table, against an emulated SwitchBot API.

#include <algorithm>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "dispatcher/switchbot.hpp"
#include "dispatcher/util.hpp"
#include "fake_platform.hpp"
#include "minitest.hpp"
#include "switchbot_commands.hpp"

using namespace dispatcher;

namespace {

const char* const kToken = "test-token";
const char* const kSecret = "test-secret";

const char* const kDevicesBody =
    "{\"deviceList\":["
    "{\"deviceId\":\"BOT1\",\"deviceName\":\"Living Bot\",\"deviceType\":\"Bot\",\"hubDeviceId\":\"HUB1\"},"
    "{\"deviceId\":\"HUB1\",\"deviceName\":\"Hub Mini\",\"deviceType\":\"Hub Mini\",\"hubDeviceId\":\"HUB1\"}],"
    "\"infraredRemoteList\":["
    "{\"deviceId\":\"TV1\",\"deviceName\":\"TV\",\"remoteType\":\"TV\",\"hubDeviceId\":\"HUB1\"}]}";
const char* const kScenesBody = "[{\"sceneId\":\"S1\",\"sceneName\":\"Good night\"}]";

bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

bool starts_with(const std::string& text, const std::string& prefix) { return text.compare(0, prefix.size(), prefix) == 0; }

// Emulates the SwitchBot API, verifying the signature the way the real service does.
struct FakeSwitchBot {
  int status_code = 100;
  std::string message = "success";
  int http_status = 200;
  int devices_calls = 0;
  int scenes_calls = 0;
  std::string devices_body = kDevicesBody;
  std::vector<std::string> commands;    // "<device id> <request body>"
  std::vector<std::string> scenes_run;  // scene ids

  static bool signature_ok(const test_support::RecordedRequest& request) {
    const std::string* token = test_support::find_header(request.headers, "Authorization");
    const std::string* t = test_support::find_header(request.headers, "t");
    const std::string* nonce = test_support::find_header(request.headers, "nonce");
    const std::string* sign = test_support::find_header(request.headers, "sign");
    if (!token || !t || !nonce || !sign || t->empty() || nonce->empty()) return false;
    const std::string expected = base64_encode(test_crypto::hmac_sha256(kSecret, std::string(kToken) + *t + *nonce));
    return *token == kToken && *sign == expected;
  }

  HttpResponse respond(const test_support::RecordedRequest& request, const std::string& body_json) {
    HttpResponse response;
    if (!signature_ok(request)) {
      response.status = 401;
      return response;
    }
    response.status = http_status;
    if (http_status != 200) return response;
    response.body = "{\"statusCode\":" + std::to_string(status_code) + ",\"message\":\"" + message +
                    "\",\"body\":" + body_json + "}";
    return response;
  }

  HttpResponse handle(const test_support::RecordedRequest& request) {
    const std::string base = kSwitchBotApiBase;
    if (!starts_with(request.url, base)) return HttpResponse{HttpResponse::Failure::kConnect, 0, "", ""};
    const std::string path = request.url.substr(base.size());
    if (request.method == "GET" && path == "/devices") {
      ++devices_calls;
      return respond(request, devices_body);
    }
    if (request.method == "GET" && path == "/scenes") {
      ++scenes_calls;
      return respond(request, kScenesBody);
    }
    if (request.method == "POST" && starts_with(path, "/devices/") && path.size() > 9 + 9 &&
        path.compare(path.size() - 9, 9, "/commands") == 0) {
      commands.push_back(path.substr(9, path.size() - 18) + " " + request.body);
      return respond(request, "{}");
    }
    if (request.method == "POST" && starts_with(path, "/scenes/") && path.size() > 8 + 8 &&
        path.compare(path.size() - 8, 8, "/execute") == 0) {
      scenes_run.push_back(path.substr(8, path.size() - 16));
      return respond(request, "{}");
    }
    HttpResponse response;
    response.status = 404;
    return response;
  }
};

struct ClientFixture {
  test_support::FakePlatform platform;
  FakeSwitchBot api;
  SwitchBotClient client{platform, kToken, kSecret};
  ClientFixture() {
    platform.http_handler = [this](const test_support::RecordedRequest& r) { return api.handle(r); };
  }
};

struct ServiceFixture {
  test_support::FakePlatform platform;
  FakeSwitchBot api;
  std::pair<std::string, std::string> credentials{kToken, kSecret};
  SwitchBotService service{platform, [this] { return credentials; }};
  ServiceFixture() {
    platform.http_handler = [this](const test_support::RecordedRequest& r) { return api.handle(r); };
  }
};

// The platform's request log is not thread-safe, so the threaded test goes straight to the handler.
struct ThreadSafePlatform : public test_support::FakePlatform {
  HttpResponse http_request(const std::string& method, const std::string& url, const HeaderList& headers,
                            const std::string& body, double timeout_s, size_t) override {
    return http_handler(test_support::RecordedRequest{method, url, headers, body, timeout_s});
  }
};

std::string error_of(const SwitchBotBody& body) {
  CHECK(!body.ok());
  return body.error;
}

std::vector<std::string> names_of(const std::vector<CommandRow>& rows) {
  std::vector<std::string> names;
  for (const CommandRow& row : rows) names.push_back(row.command);
  return names;
}

}  // namespace

// ---- make_auth_headers ----

TEST(switchbot_signature_is_base64_hmac_of_token_time_nonce) {
  test_support::FakePlatform platform;
  const HeaderList h = make_auth_headers(platform, "tok", "sec", "1700000000000", "abc");
  CHECK_EQ(h.size(), size_t(5));
  CHECK_EQ(h[0].first, std::string("Authorization"));
  CHECK_EQ(h[0].second, std::string("tok"));
  CHECK_EQ(h[1].first, std::string("t"));
  CHECK_EQ(h[1].second, std::string("1700000000000"));
  CHECK_EQ(h[2].first, std::string("nonce"));
  CHECK_EQ(h[2].second, std::string("abc"));
  CHECK_EQ(h[3].first, std::string("sign"));
  CHECK_EQ(h[3].second, base64_encode(test_crypto::hmac_sha256("sec", "tok1700000000000abc")));
  CHECK_EQ(h[4].first, std::string("Content-Type"));
  CHECK_EQ(h[4].second, std::string("application/json; charset=utf8"));
}

TEST(switchbot_nonce_and_timestamp_are_generated_per_request) {
  ClientFixture f;
  f.client.devices();
  f.client.devices();
  CHECK_EQ(f.platform.requests.size(), size_t(2));
  const std::string* n1 = test_support::find_header(f.platform.requests[0].headers, "nonce");
  const std::string* n2 = test_support::find_header(f.platform.requests[1].headers, "nonce");
  CHECK(n1 && n2 && *n1 != *n2);
  const std::string* t = test_support::find_header(f.platform.requests[0].headers, "t");
  CHECK(t != nullptr && t->size() == 13);
  CHECK_EQ(*t, std::to_string(f.platform.wall_ms));
  CHECK_EQ(n1->size(), size_t(36));
}

// ---- client ----

TEST(switchbot_client_lists_devices_and_scenes) {
  ClientFixture f;
  SwitchBotBody devices = f.client.devices();
  CHECK(devices.ok());
  const cJSON* list = cJSON_GetObjectItemCaseSensitive(devices.value->get(), "deviceList");
  CHECK(cJSON_IsArray(list));
  CHECK_EQ(std::string(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(list, 0), "deviceId")->valuestring),
           std::string("BOT1"));
  SwitchBotBody scenes = f.client.scenes();
  CHECK(scenes.ok());
  CHECK_EQ(std::string(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(scenes.value->get(), 0), "sceneId")
                           ->valuestring),
           std::string("S1"));
  CHECK_EQ(f.platform.requests[0].url, std::string("https://api.switch-bot.com/v1.1/devices"));
  CHECK_EQ(f.platform.requests[0].method, std::string("GET"));
  CHECK_EQ(f.platform.requests[0].timeout_s, 15.0);
}

TEST(switchbot_client_command_body) {
  ClientFixture f;
  CHECK(f.client.command("BOT1", "press", "default", "command").ok());
  CHECK_EQ(f.api.commands.size(), size_t(1));
  CHECK_EQ(f.api.commands[0],
           std::string("BOT1 {\"command\":\"press\",\"parameter\":\"default\",\"commandType\":\"command\"}"));
}

TEST(switchbot_client_command_parameter_json_is_embedded) {
  ClientFixture f;
  CHECK(f.client.command("BOT1", "setAll", "{\"a\":1}", "command").ok());
  CHECK(f.client.command("BOT1", "setAll", "\"x\"", "command").ok());
  CHECK(f.client.command("BOT1", "setAll", "26,1,3,on", "customize").ok());
  CHECK_EQ(f.api.commands[0],
           std::string("BOT1 {\"command\":\"setAll\",\"parameter\":{\"a\":1},\"commandType\":\"command\"}"));
  CHECK_EQ(f.api.commands[1],
           std::string("BOT1 {\"command\":\"setAll\",\"parameter\":\"x\",\"commandType\":\"command\"}"));
  CHECK_EQ(f.api.commands[2],
           std::string("BOT1 {\"command\":\"setAll\",\"parameter\":\"26,1,3,on\",\"commandType\":\"customize\"}"));
}

TEST(switchbot_client_execute_scene) {
  ClientFixture f;
  CHECK(f.client.execute_scene("S1").ok());
  CHECK_EQ(f.api.scenes_run, std::vector<std::string>{"S1"});
  CHECK_EQ(f.platform.requests[0].method, std::string("POST"));
  CHECK_EQ(f.platform.requests[0].body, std::string(""));
}

TEST(switchbot_client_wrong_credentials_are_reported) {
  ClientFixture f;
  SwitchBotClient bad(f.platform, kToken, "wrong");
  CHECK_EQ(error_of(bad.devices()), std::string("HTTP 401: authentication failed; check the token and secret"));
}

TEST(switchbot_client_api_error_codes_are_explained) {
  ClientFixture f;
  f.api.status_code = 161;
  f.api.message = "device offline";
  CHECK_EQ(error_of(f.client.command("BOT1", "press", "default", "command")), std::string("API error 161: device offline"));
  f.api.status_code = 190;
  f.api.message = "bad parameter";
  const std::string error = error_of(f.client.command("BOT1", "press", "default", "command"));
  CHECK_EQ(error,
           std::string("API error 190: bad parameter (device internal error (often an invalid parameter or command))"));
  CHECK(contains(error, "invalid parameter"));
  f.api.status_code = 152;
  f.api.message = "";
  CHECK_EQ(error_of(f.client.devices()), std::string("API error 152: device not found"));
  f.api.status_code = 999;
  f.api.message = "";
  CHECK_EQ(error_of(f.client.devices()), std::string("API error 999: unknown error"));
  f.api.message = "weird";
  CHECK_EQ(error_of(f.client.devices()), std::string("API error 999: weird"));
}

TEST(switchbot_client_http_error_status) {
  ClientFixture f;
  f.api.http_status = 500;
  CHECK_EQ(error_of(f.client.devices()), std::string("HTTP 500"));
}

TEST(switchbot_client_401_is_authentication_failure) {
  ClientFixture f;
  f.api.http_status = 401;
  CHECK_EQ(error_of(f.client.devices()), std::string("HTTP 401: authentication failed; check the token and secret"));
}

TEST(switchbot_client_unreachable_server) {
  ClientFixture f;
  f.platform.http_handler = [](const test_support::RecordedRequest&) {
    HttpResponse response;
    response.failure = HttpResponse::Failure::kConnect;
    return response;
  };
  CHECK_EQ(error_of(f.client.devices()), std::string("cannot reach SwitchBot"));
}

TEST(switchbot_client_timeout_text) {
  ClientFixture f;
  f.platform.http_handler = [](const test_support::RecordedRequest&) {
    HttpResponse response;
    response.failure = HttpResponse::Failure::kTimeout;
    return response;
  };
  CHECK_EQ(error_of(f.client.devices()), std::string("timed out after 15s"));
  SwitchBotClient quick(f.platform, kToken, kSecret, kSwitchBotApiBase, 2.5);
  CHECK_EQ(error_of(quick.devices()), std::string("timed out after 2.5s"));
}

TEST(switchbot_client_needs_the_wall_clock) {
  ClientFixture f;
  f.platform.clock_valid = false;
  CHECK_EQ(error_of(f.client.devices()),
           std::string("the clock is not set yet, so SwitchBot requests cannot be signed; check the Wi-Fi connection"));
  CHECK(f.platform.requests.empty());
}

TEST(switchbot_client_invalid_json_response) {
  ClientFixture f;
  for (const char* text : {"", "not json", "{\"statusCode\":100", "[1,2]", "{} trailing"}) {
    const std::string body = text;
    f.platform.http_handler = [body](const test_support::RecordedRequest&) {
      HttpResponse response;
      response.status = 200;
      response.body = body;
      return response;
    };
    CHECK_EQ(error_of(f.client.devices()), std::string("invalid response from SwitchBot"));
  }
}

TEST(switchbot_client_response_beyond_the_size_limit_is_invalid) {
  ClientFixture f;
  f.api.devices_body = "{\"deviceList\":[{\"deviceId\":\"BIG\",\"deviceName\":\"" +
                       std::string(kSwitchBotMaxResponseBytes + 100, 'x') + "\"}]}";
  CHECK_EQ(error_of(f.client.devices()), std::string("invalid response from SwitchBot"));
}

TEST(switchbot_client_missing_body_is_null) {
  ClientFixture f;
  f.platform.http_handler = [](const test_support::RecordedRequest&) {
    HttpResponse response;
    response.status = 200;
    response.body = "{\"statusCode\":100,\"message\":\"success\"}";
    return response;
  };
  SwitchBotBody body = f.client.devices();
  CHECK(body.ok());
  CHECK(cJSON_IsNull(body.value->get()));
}

// ---- parsing helpers ----

TEST(switchbot_device_items_skip_entries_without_an_id) {
  JsonPtr json = make_json(cJSON_Parse(
      "{\"deviceList\":[{\"deviceName\":\"no id\"},{\"deviceId\":\"A\"},5],"
      "\"infraredRemoteList\":[{\"remoteType\":\"TV\"},{\"deviceId\":\"B\"}]}"));
  const std::vector<SwitchBotDevice> items = device_items(json.get());
  CHECK_EQ(items.size(), size_t(2));
  CHECK_EQ(items[0].id, std::string("A"));
  CHECK_EQ(items[0].type, std::string(""));
  CHECK(!items[0].remote_type.has_value());
  CHECK_EQ(items[1].id, std::string("B"));
  CHECK_EQ(items[1].type, std::string("IR"));
  CHECK_EQ(items[1].remote_type, std::optional<std::string>(""));
  CHECK(device_items(nullptr).empty());
  JsonPtr scenes = make_json(cJSON_Parse("[{\"sceneName\":\"x\"},{\"sceneId\":\"S\"}]"));
  CHECK_EQ(scene_items(scenes.get()).size(), size_t(1));
}

// ---- service ----

TEST(switchbot_service_devices_are_simplified_and_include_ir_remotes) {
  ServiceFixture f;
  Parsed<std::vector<SwitchBotDevice>> devices = f.service.devices(false);
  CHECK(devices.ok());
  CHECK_EQ(devices.value->size(), size_t(3));
  const std::vector<SwitchBotDevice>& d = *devices.value;
  CHECK_EQ(d[0].id, std::string("BOT1"));
  CHECK_EQ(d[0].type, std::string("Bot"));
  CHECK_EQ(d[0].name, std::string("Living Bot"));
  CHECK_EQ(d[0].hub, std::string("HUB1"));
  CHECK(!d[0].remote_type.has_value());
  CHECK_EQ(d[1].type, std::string("Hub Mini"));
  CHECK_EQ(d[2].id, std::string("TV1"));
  CHECK_EQ(d[2].type, std::string("IR TV"));
  CHECK_EQ(d[2].remote_type, std::optional<std::string>("TV"));
}

TEST(switchbot_service_scenes) {
  ServiceFixture f;
  Parsed<std::vector<SwitchBotScene>> scenes = f.service.scenes(false);
  CHECK(scenes.ok());
  CHECK_EQ(scenes.value->size(), size_t(1));
  CHECK_EQ((*scenes.value)[0].id, std::string("S1"));
  CHECK_EQ((*scenes.value)[0].name, std::string("Good night"));
  f.service.scenes(false);
  CHECK_EQ(f.api.scenes_calls, 1);
  f.service.scenes(true);
  CHECK_EQ(f.api.scenes_calls, 2);
}

TEST(switchbot_service_lists_are_cached_until_refreshed_or_expired) {
  ServiceFixture f;
  f.service.devices(false);
  f.service.devices(false);
  CHECK_EQ(f.api.devices_calls, 1);
  f.service.devices(true);
  CHECK_EQ(f.api.devices_calls, 2);
  f.platform.mono += SwitchBotService::kCacheTtlS + 1;
  f.service.devices(false);
  CHECK_EQ(f.api.devices_calls, 3);
}

TEST(switchbot_service_cache_expiry_boundary) {
  ServiceFixture f;
  f.service.devices(false);
  f.platform.mono += SwitchBotService::kCacheTtlS - 0.001;
  f.service.devices(false);
  CHECK_EQ(f.api.devices_calls, 1);
  f.platform.mono += 0.001;  // exactly the TTL old: expired
  f.service.devices(false);
  CHECK_EQ(f.api.devices_calls, 2);
}

TEST(switchbot_service_refresh_does_not_use_the_cache) {
  ServiceFixture f;
  f.service.devices(false);
  f.api.devices_body = "{\"deviceList\":[{\"deviceId\":\"NEW\",\"deviceType\":\"Bot\"}]}";
  CHECK_EQ(f.service.devices(false).value->size(), size_t(3));
  Parsed<std::vector<SwitchBotDevice>> fresh = f.service.devices(true);
  CHECK_EQ(fresh.value->size(), size_t(1));
  CHECK_EQ((*fresh.value)[0].id, std::string("NEW"));
  CHECK_EQ(f.service.devices(false).value->size(), size_t(1));  // the refreshed list is cached
}

TEST(switchbot_service_invalidate_drops_the_cache) {
  ServiceFixture f;
  f.service.devices(false);
  f.service.scenes(false);
  f.service.invalidate();
  f.service.devices(false);
  f.service.scenes(false);
  CHECK_EQ(f.api.devices_calls, 2);
  CHECK_EQ(f.api.scenes_calls, 2);
}

TEST(switchbot_service_errors_are_not_cached) {
  ServiceFixture f;
  f.api.http_status = 500;
  CHECK_EQ(f.service.devices(false).error, std::string("HTTP 500"));
  f.api.http_status = 200;
  CHECK(f.service.devices(false).ok());
  CHECK_EQ(f.api.devices_calls, 2);
}

TEST(switchbot_service_missing_credentials) {
  ServiceFixture f;
  const std::string message = "SwitchBot token and secret are not set (see Settings)";
  for (const auto& credentials : {std::make_pair(std::string(), std::string()),
                                  std::make_pair(std::string("tok"), std::string()),
                                  std::make_pair(std::string(), std::string("sec"))}) {
    f.credentials = credentials;
    CHECK_EQ(f.service.devices(false).error, message);
    CHECK_EQ(f.service.scenes(false).error, message);
    CHECK_EQ(error_of(f.service.command("BOT1", "press", "default", "command")), message);
    CHECK_EQ(error_of(f.service.execute_scene("S1")), message);
    CHECK_EQ(f.service.commands_for_device("BOT1").error, message);
  }
  CHECK(f.platform.requests.empty());
}

TEST(switchbot_service_runs_commands_and_scenes) {
  ServiceFixture f;
  CHECK(f.service.command("BOT1", "press", "default", "command").ok());
  CHECK(f.service.execute_scene("S1").ok());
  CHECK_EQ(f.api.commands.size(), size_t(1));
  CHECK_EQ(f.api.scenes_run.size(), size_t(1));
}

TEST(switchbot_service_picks_up_changed_credentials) {
  ServiceFixture f;
  CHECK(f.service.devices(true).ok());
  f.credentials.second = "wrong";
  CHECK_EQ(f.service.devices(true).error, std::string("HTTP 401: authentication failed; check the token and secret"));
}

TEST(switchbot_service_commands_for_a_device) {
  ServiceFixture f;
  Parsed<SwitchBotService::DeviceCommands> info = f.service.commands_for_device("BOT1");
  CHECK(info.ok());
  CHECK_EQ(names_of(info.value->list.commands), (std::vector<std::string>{"turnOff", "turnOn", "press"}));
  CHECK_EQ(info.value->device.name, std::string("Living Bot"));
  Parsed<SwitchBotService::DeviceCommands> ir = f.service.commands_for_device("TV1");
  CHECK(ir.ok());
  const std::vector<std::string> names = names_of(ir.value->list.commands);
  CHECK(std::find(names.begin(), names.end(), "SetChannel") != names.end());
  CHECK(!ir.value->list.notes.empty());
  CHECK_EQ(f.api.devices_calls, 1);  // both lookups used the cached list
}

TEST(switchbot_service_commands_for_an_unknown_device) {
  ServiceFixture f;
  CHECK_EQ(f.service.commands_for_device("NOPE").error,
           std::string("no device 'NOPE' in your SwitchBot account; refresh the list"));
  CHECK_EQ(f.service.commands_for_device("it's").error,
           std::string("no device \"it's\" in your SwitchBot account; refresh the list"));
  CHECK_EQ(f.service.commands_for_device("it's \"x\"").error,
           std::string("no device 'it\\'s \"x\"' in your SwitchBot account; refresh the list"));
}

TEST(switchbot_service_passes_errors_through_unchanged) {
  ServiceFixture f;
  f.platform.clock_valid = false;
  const std::string message =
      "the clock is not set yet, so SwitchBot requests cannot be signed; check the Wi-Fi connection";
  CHECK_EQ(f.service.devices(false).error, message);
  CHECK_EQ(f.service.commands_for_device("BOT1").error, message);
  CHECK_EQ(error_of(f.service.execute_scene("S1")), message);
}

TEST(switchbot_service_thread_safety_smoke) {
  ThreadSafePlatform platform;
  std::mutex mutex;
  int calls = 0;
  platform.http_handler = [&](const test_support::RecordedRequest& request) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      ++calls;
    }
    FakeSwitchBot api;  // stateless for GET /devices
    return api.handle(request);
  };
  SwitchBotService service(platform, [] { return std::make_pair(std::string(kToken), std::string(kSecret)); });
  bool ok1 = false;
  bool ok2 = false;
  std::thread a([&] {
    for (int i = 0; i < 20; ++i) ok1 = service.devices(false).ok() && service.scenes(false).ok();
  });
  std::thread b([&] {
    for (int i = 0; i < 20; ++i) ok2 = service.devices(false).ok() && service.scenes(false).ok();
  });
  a.join();
  b.join();
  CHECK(ok1);
  CHECK(ok2);
  // Racing threads may both fetch the first time, but the cache serves the rest.
  CHECK(calls >= 2 && calls <= 4);
}

// ---- command table ----

TEST(switchbot_table_has_61_device_types) {
  CHECK_EQ(generated::kDeviceCommandTableCount, size_t(61));
}

TEST(switchbot_table_has_no_passcode_field_names_as_device_types) {
  for (const char* bogus : {"endTime", "id", "Key Name", "name", "password", "startTime", "type"}) {
    for (size_t i = 0; i < generated::kDeviceCommandTableCount; ++i) {
      CHECK(std::string(bogus) != generated::kDeviceCommandTables[i].type);
    }
  }
}

TEST(switchbot_table_types_are_sorted_and_unique) {
  for (size_t i = 1; i < generated::kDeviceCommandTableCount; ++i) {
    CHECK(std::string(generated::kDeviceCommandTables[i - 1].type) < generated::kDeviceCommandTables[i].type);
  }
  for (size_t i = 1; i < generated::kIrCommandTableCount; ++i) {
    CHECK(std::string(generated::kIrCommandTables[i - 1].type) < generated::kIrCommandTables[i].type);
  }
}

namespace {
void check_rows(const generated::CommandEntry* rows, size_t count) {
  std::set<std::string> seen;
  for (size_t i = 0; i < count; ++i) {
    const std::string command = rows[i].command;
    CHECK(!command.empty());
    CHECK(command.find_first_not_of(" \t") != std::string::npos);
    CHECK(seen.insert(command + "\n" + rows[i].parameter + "\n" + rows[i].description).second);
    for (const std::string& text : {command, std::string(rows[i].parameter), std::string(rows[i].description)}) {
      // The curly quotes are U+2018, U+2019, U+201C and U+201D, which start with the bytes E2 80.
      CHECK(text.find("\xe2\x80\x98") == std::string::npos);
      CHECK(text.find("\xe2\x80\x99") == std::string::npos);
      CHECK(text.find("\xe2\x80\x9c") == std::string::npos);
      CHECK(text.find("\xe2\x80\x9d") == std::string::npos);
    }
  }
}
}  // namespace

TEST(switchbot_table_rows_are_unique_and_well_formed) {
  for (size_t i = 0; i < generated::kDeviceCommandTableCount; ++i) {
    check_rows(generated::kDeviceCommandTables[i].rows, generated::kDeviceCommandTables[i].count);
  }
  for (size_t i = 0; i < generated::kIrCommandTableCount; ++i) {
    check_rows(generated::kIrCommandTables[i].rows, generated::kIrCommandTables[i].count);
  }
  check_rows(generated::kIrCommonCommands, generated::kIrCommonCommandCount);
}

TEST(switchbot_table_bot_commands_are_exact) {
  CommandList bot = commands_for("Bot", std::nullopt);
  CHECK_EQ(names_of(bot.commands), (std::vector<std::string>{"turnOff", "turnOn", "press"}));
  CHECK(bot.notes.empty());
  CHECK_EQ(bot.commands[0].parameter, std::string("default"));
  CHECK_EQ(bot.commands[2].parameter, std::string("default"));
  const generated::CommandTable* found = nullptr;
  for (size_t i = 0; i < generated::kDeviceCommandTableCount; ++i) {
    if (std::string(generated::kDeviceCommandTables[i].type) == "Bot") found = &generated::kDeviceCommandTables[i];
  }
  CHECK(found != nullptr);
  CHECK_EQ(found->count, size_t(3));
  CHECK_EQ(std::string(found->rows[0].command), std::string("turnOff"));
  CHECK_EQ(std::string(found->rows[1].command), std::string("turnOn"));
  CHECK_EQ(std::string(found->rows[2].command), std::string("press"));
}

TEST(switchbot_table_unknown_device_type_explains_itself) {
  CommandList list = commands_for("Gizmo 9000", std::nullopt);
  CHECK(list.commands.empty());
  CHECK_EQ(list.notes.size(), size_t(1));
  CHECK(contains(list.notes[0], "type a command by hand"));
  CHECK(contains(list.notes[0], "'Gizmo 9000'"));
}

TEST(switchbot_table_ir_remote_types) {
  CommandList tv = commands_for("IR TV", std::string("TV"));
  const std::vector<std::string> names = names_of(tv.commands);
  CHECK_EQ(names[0], std::string("turnOn"));
  CHECK_EQ(names[1], std::string("turnOff"));
  CHECK(std::find(names.begin(), names.end(), "SetChannel") != names.end());
  CHECK_EQ(tv.notes.size(), size_t(1));
  CHECK(contains(tv.notes[0], "customize"));

  CommandList others = commands_for("IR Others", std::string("Others"));
  CHECK(others.commands.empty());
  CHECK_EQ(others.notes.size(), size_t(1));
  CHECK(starts_with(others.notes[0], "\"Others\" remotes only have custom buttons. "));

  // An appliance type without its own commands still has the common ones.
  CommandList unknown = commands_for("IR Teapot", std::string("Teapot"));
  CHECK_EQ(names_of(unknown.commands), (std::vector<std::string>{"turnOn", "turnOff"}));
}

TEST(switchbot_table_diy_remote_notes) {
  CommandList diy = commands_for("IR DIY Fan", std::string("DIY Fan"));
  const std::vector<std::string> names = names_of(diy.commands);
  CHECK(std::find(names.begin(), names.end(), "swing") != names.end());
  CHECK_EQ(names[0], std::string("turnOn"));
  CHECK_EQ(diy.notes.size(), size_t(2));
  CHECK(contains(diy.notes[1], "learned (DIY) remote"));
}

TEST(switchbot_table_non_ascii_text_survives_the_generated_escapes) {
  CommandList list = commands_for("Smart Radiator Thermostat", std::nullopt);
  bool found = false;
  for (const CommandRow& row : list.commands) {
    if (row.command == "setManualModeTemperature") {
      found = true;
      CHECK(contains(row.description, "4\xe2\x80\x93" "35 \xc2\xb0" "C"));
    }
  }
  CHECK(found);
}
