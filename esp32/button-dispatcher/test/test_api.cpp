// Tests for api.cpp: the whole web API driven request by request over a fake platform, with a scripted
// SwitchBot server and a fake target for the HTTP actions.

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cJSON.h"
#include "dispatcher/api.hpp"
#include "dispatcher/app.hpp"
#include "dispatcher/config.hpp"
#include "dispatcher/switchbot.hpp"
#include "dispatcher/util.hpp"
#include "fake_platform.hpp"
#include "minitest.hpp"
#include "test_util.hpp"

using namespace dispatcher;

namespace {

const char* const kDesk = "AA:BB:CC:DD:EE:01";
const char* const kPressB2 = "44002101643a003a013a003a00";       // id 33, button 2 press (four-button remote)
const char* const kPressB2Again = "44002201643a003a013a003a00";  // id 34, the same press again
const char* const kPressB1 = "44002301643a013a003a003a00";       // id 35, button 1 press
const char* const kPressB2Id35 = "44002301643a003a013a003a00";   // id 35, button 2 press
const char* const kPassword = "correct horse battery";
const char* const kToken = "test-token";
const char* const kSecret = "test-secret";
const char* const kTargetUrl = "http://target.test/hit";
const char* const kDeadUrl = "http://127.0.0.1:1/x";

constexpr int kFastIterations = 2;

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

bool starts_with(const std::string& text, const std::string& prefix) {
  return text.compare(0, prefix.size(), prefix) == 0;
}

// ---- the response, with easy access to fields of its JSON ----

struct Reply {
  int status = 0;
  std::string body;
  std::string content_type;
  HeaderList headers;
  JsonPtr json = make_json();

  // A member by dotted path; numeric parts index arrays ("buttons.0.name"). Null if absent.
  const cJSON* at(const std::string& path) const {
    const cJSON* node = json.get();
    size_t pos = 0;
    while (node != nullptr && pos <= path.size()) {
      size_t dot = path.find('.', pos);
      if (dot == std::string::npos) dot = path.size();
      const std::string part = path.substr(pos, dot - pos);
      if (cJSON_IsArray(node)) {
        node = cJSON_GetArrayItem(node, std::atoi(part.c_str()));
      } else {
        node = cJSON_GetObjectItemCaseSensitive(node, part.c_str());
      }
      pos = dot + 1;
    }
    return node;
  }
  bool has(const std::string& path) const { return at(path) != nullptr; }
  bool is_null(const std::string& path) const { return cJSON_IsNull(at(path)); }
  std::string str(const std::string& path) const {
    const cJSON* node = at(path);
    return cJSON_IsString(node) ? node->valuestring : "<not a string>";
  }
  double num(const std::string& path) const {
    const cJSON* node = at(path);
    return cJSON_IsNumber(node) ? node->valuedouble : -12345.0;
  }
  bool flag(const std::string& path) const { return cJSON_IsTrue(at(path)); }
  size_t size(const std::string& path) const {
    const cJSON* node = at(path);
    return cJSON_IsArray(node) ? static_cast<size_t>(cJSON_GetArraySize(node)) : 9999;
  }
  std::string error() const { return str("error"); }
  std::string header(const std::string& name) const {
    const std::string* value = test_support::find_header(headers, name);
    return value != nullptr ? *value : "";
  }
};

// Builds requests the way the HTTP layer would, and keeps the session cookie like a browser.
class ApiClient {
 public:
  explicit ApiClient(Api* api) : api_(api) {}

  void use(Api* api) { api_ = api; }

  Reply call(const std::string& method, const std::string& target, const std::string& body = "",
             bool csrf = true) {
    Request request;
    request.method = method;
    const size_t question = target.find('?');
    request.path = target.substr(0, question);
    if (question != std::string::npos) {
      size_t pos = question + 1;
      while (pos <= target.size()) {
        size_t amp = target.find('&', pos);
        if (amp == std::string::npos) amp = target.size();
        const std::string pair = target.substr(pos, amp - pos);
        const size_t eq = pair.find('=');
        if (eq == std::string::npos) {
          request.query[pair] = "";
        } else {
          request.query[pair.substr(0, eq)] = pair.substr(eq + 1);
        }
        pos = amp + 1;
      }
    }
    request.body = body;
    request.cookie = cookie;
    request.csrf = csrf ? "1" : "";
    request.client = address;
    return finish(api_->handle(request));
  }

  Reply get(const std::string& target) { return call("GET", target); }
  Reply post(const std::string& target, const std::string& body = "") { return call("POST", target, body); }
  Reply put(const std::string& target, const std::string& body = "") { return call("PUT", target, body); }
  Reply del(const std::string& target) { return call("DELETE", target); }

  std::string cookie;                // what a browser would send in the Cookie header
  std::string address = "127.0.0.1";

 private:
  Reply finish(Response response) {
    Reply reply;
    reply.status = response.status;
    reply.content_type = response.content_type;
    reply.headers = response.headers;
    reply.body = response.body;
    reply.json = make_json(cJSON_ParseWithOpts(reply.body.c_str(), nullptr, true));
    for (const auto& header : response.headers) {
      if (header.first != "Set-Cookie") continue;
      const std::string pair = header.second.substr(0, header.second.find(';'));
      const size_t eq = pair.find('=');
      if (pair.substr(0, eq) != kSessionCookieName) continue;
      cookie = eq + 1 < pair.size() ? pair : "";
    }
    return reply;
  }

  Api* api_;
};

// ---- a scripted SwitchBot server that verifies the signature like the real service ----

struct FakeSwitchBot {
  std::vector<std::string> commands;  // "<device id> <request body>"
  int devices_calls = 0;

  static bool signature_ok(const test_support::RecordedRequest& request) {
    const std::string* token = test_support::find_header(request.headers, "Authorization");
    const std::string* t = test_support::find_header(request.headers, "t");
    const std::string* nonce = test_support::find_header(request.headers, "nonce");
    const std::string* sign = test_support::find_header(request.headers, "sign");
    if (!token || !t || !nonce || !sign || t->empty() || nonce->empty()) return false;
    const std::string expected = base64_encode(test_crypto::hmac_sha256(kSecret, std::string(kToken) + *t + *nonce));
    return *token == kToken && *sign == expected;
  }

  static HttpResponse answer(const test_support::RecordedRequest& request, const std::string& body_json) {
    HttpResponse response;
    if (!signature_ok(request)) {
      response.status = 401;
      return response;
    }
    response.status = 200;
    response.body = "{\"statusCode\":100,\"message\":\"success\",\"body\":" + body_json + "}";
    return response;
  }

  HttpResponse handle(const test_support::RecordedRequest& request) {
    const std::string path = request.url.substr(std::string(kSwitchBotApiBase).size());
    if (request.method == "GET" && path == "/devices") {
      ++devices_calls;
      return answer(request,
                    "{\"deviceList\":["
                    "{\"deviceId\":\"BOT1\",\"deviceName\":\"Living Bot\",\"deviceType\":\"Bot\","
                    "\"hubDeviceId\":\"HUB1\"},"
                    "{\"deviceId\":\"HUB1\",\"deviceName\":\"Hub Mini\",\"deviceType\":\"Hub Mini\","
                    "\"hubDeviceId\":\"HUB1\"}],"
                    "\"infraredRemoteList\":["
                    "{\"deviceId\":\"TV1\",\"deviceName\":\"TV\",\"remoteType\":\"TV\",\"hubDeviceId\":\"HUB1\"}]}");
    }
    if (request.method == "GET" && path == "/scenes") {
      return answer(request, "[{\"sceneId\":\"S1\",\"sceneName\":\"Good night\"}]");
    }
    if (request.method == "POST" && starts_with(path, "/devices/") && path.size() > 18 &&
        path.compare(path.size() - 9, 9, "/commands") == 0) {
      commands.push_back(path.substr(9, path.size() - 18) + " " + request.body);
      return answer(request, "{}");
    }
    HttpResponse response;
    response.status = 404;
    return response;
  }
};

// ---- the fixture: an app, its API, a client, and the fake network ----

struct Env {
  test_support::FakePlatform platform;
  std::unique_ptr<AppCore> app;
  std::unique_ptr<Api> api;
  ApiClient client{nullptr};
  FakeSwitchBot switchbot;
  std::vector<std::string> hits;  // "<method> <url>" of requests to the target
  HostnameConfig hostname_config;  // from config.toml, applied by open()

  Env() {
    platform.http_handler = [this](const test_support::RecordedRequest& request) { return network(request); };
    open();
  }

  HttpResponse network(const test_support::RecordedRequest& request) {
    if (starts_with(request.url, kSwitchBotApiBase)) return switchbot.handle(request);
    if (starts_with(request.url, kTargetUrl)) {
      hits.push_back(request.method + " " + request.url);
      HttpResponse response;
      response.status = 200;
      response.reason = "OK";
      return response;
    }
    HttpResponse response;
    response.failure = HttpResponse::Failure::kConnect;
    return response;
  }

  // Starts (or restarts) the application over whatever documents the platform holds.
  void open() {
    api.reset();
    app = std::make_unique<AppCore>(platform, kFastIterations);
    app->hostname_config = hostname_config;
    app->init();
    api = std::make_unique<Api>(*app);
    client.use(api.get());
  }

  void sign_in() {
    const Reply reply = client.post("/api/auth/setup", std::string("{\"password\":\"") + kPassword + "\"}");
    CHECK_EQ(reply.status, 200);
  }

  void press(const char* hex, const char* mac = kDesk) {
    const std::vector<uint8_t> bytes = test_util::from_hex(hex);
    app->handle_packet(mac, -40, bytes.data(), bytes.size());
  }

  // Adds kDesk through the real "Find button" flow and returns the save response.
  Reply add_button(const std::string& name = "Desk remote") {
    client.post("/api/learn/start");
    press(kPressB2);
    platform.mono += 1;
    const Reply status = client.get("/api/learn");
    CHECK_EQ(status.str("phase"), std::string("confirm"));
    press(kPressB2Again);
    Reply saved = client.post("/api/learn/save", "{\"name\":\"" + name + "\"}");
    CHECK(saved.status == 200 || saved.status == 201);
    return saved;
  }

  std::string http_binding(const std::string& button_id, const std::string& name = "Hit target",
                           bool enabled = true, int cooldown_ms = 0, const std::string& action = "") {
    return "{\"name\":\"" + name + "\",\"enabled\":" + (enabled ? "true" : "false") +
           ",\"cooldown_ms\":" + std::to_string(cooldown_ms) + ",\"trigger\":{\"button\":\"" + button_id +
           "\",\"channel\":null,\"input\":\"button:2\",\"event\":\"press\"},\"action\":" +
           (action.empty() ? "{\"type\":\"http\",\"method\":\"GET\",\"url\":\"" + std::string(kTargetUrl) + "\"}"
                           : action) +
           "}";
  }

  std::string log_text() {
    const Reply log = client.get("/api/log");
    std::string text;
    for (size_t i = 0; i < log.size("entries"); ++i) {
      text += log.str("entries." + std::to_string(i) + ".kind") + ": " +
              log.str("entries." + std::to_string(i) + ".text") + "\n";
    }
    return text;
  }

  std::string config_document() { return platform.documents["config.json"]; }
};

std::string credentials_body(const std::string& token, const std::string& secret) {
  return "{\"token\":\"" + token + "\",\"secret\":\"" + secret + "\"}";
}

std::string password_body(const std::string& password) { return "{\"password\":\"" + password + "\"}"; }

}  // namespace

// ---- auth ----

TEST(api_auth_status_before_and_after_setup) {
  Env env;
  const Reply before = env.client.get("/api/auth/status");
  CHECK_EQ(before.status, 200);
  CHECK_EQ(before.body, std::string("{\"configured\":false,\"authenticated\":false}"));
  env.sign_in();
  CHECK_EQ(env.client.get("/api/auth/status").body, std::string("{\"configured\":true,\"authenticated\":true}"));
}

TEST(api_auth_setup_rejects_short_passwords_and_runs_only_once) {
  Env env;
  const Reply shortened = env.client.post("/api/auth/setup", password_body("short"));
  CHECK_EQ(shortened.status, 400);
  CHECK(contains(shortened.error(), "password"));
  CHECK(env.client.cookie.empty());
  env.sign_in();
  const Reply again = env.client.post("/api/auth/setup", password_body("another password"));
  CHECK_EQ(again.status, 409);
  CHECK_EQ(again.error(), std::string("a password is already set"));
}

TEST(api_requests_need_a_session) {
  Env env;
  const Reply denied = env.client.get("/api/state");
  CHECK_EQ(denied.status, 401);
  CHECK_EQ(denied.error(), std::string("not signed in"));
  env.sign_in();
  CHECK_EQ(env.client.get("/api/state").status, 200);
  env.client.cookie = "bd_session=garbage";
  CHECK_EQ(env.client.get("/api/state").status, 401);
}

TEST(api_session_cookie_is_found_among_other_cookies) {
  Env env;
  env.sign_in();
  env.client.cookie = "theme=dark; " + env.client.cookie + "; other=1";
  CHECK_EQ(env.client.get("/api/state").status, 200);
}

TEST(api_changes_need_the_csrf_header) {
  Env env;
  env.sign_in();
  const Reply refused = env.client.call("POST", "/api/learn/start", "", false);
  CHECK_EQ(refused.status, 403);
  CHECK_EQ(refused.error(), std::string("missing X-Button-Dispatcher header"));
  // The sign-in endpoints are public but still need the header.
  CHECK_EQ(env.client.call("POST", "/api/auth/login", password_body(kPassword), false).status, 403);
  // The check comes before the session check.
  env.client.cookie.clear();
  CHECK_EQ(env.client.call("DELETE", "/api/buttons/x", "", false).status, 403);
}

TEST(api_reads_work_without_the_csrf_header) {
  Env env;
  env.sign_in();
  CHECK_EQ(env.client.call("GET", "/api/state", "", false).status, 200);
}

TEST(api_login_and_logout) {
  Env env;
  env.sign_in();
  const Reply out = env.client.post("/api/auth/logout");
  CHECK_EQ(out.status, 200);
  CHECK_EQ(env.client.get("/api/state").status, 401);
  const Reply wrong = env.client.post("/api/auth/login", password_body("wrong wrong"));
  CHECK_EQ(wrong.status, 401);
  CHECK_EQ(wrong.error(), std::string("wrong password"));
  CHECK_EQ(env.client.post("/api/auth/login", password_body(kPassword)).status, 200);
  CHECK_EQ(env.client.get("/api/state").status, 200);
}

TEST(api_repeated_wrong_passwords_are_throttled) {
  Env env;
  env.sign_in();
  env.client.post("/api/auth/logout");
  for (int i = 0; i < LoginLimiter::kMaxFailures; ++i) {
    CHECK_EQ(env.client.post("/api/auth/login", password_body("nope nope")).status, 401);
  }
  const Reply blocked = env.client.post("/api/auth/login", password_body(kPassword));
  CHECK_EQ(blocked.status, 429);
  CHECK(starts_with(blocked.error(), "too many failed attempts; try again in "));
  CHECK(contains(blocked.error(), " seconds"));
  // Another address is not affected.
  ApiClient other(env.api.get());
  other.address = "10.0.0.7";
  CHECK_EQ(other.post("/api/auth/login", password_body(kPassword)).status, 200);
  env.platform.mono += LoginLimiter::kBlockS + 1;
  CHECK_EQ(env.client.post("/api/auth/login", password_body(kPassword)).status, 200);
}

TEST(api_a_successful_login_clears_the_failures) {
  Env env;
  env.sign_in();
  for (int round = 0; round < 3; ++round) {
    for (int i = 0; i < LoginLimiter::kMaxFailures - 1; ++i) {
      CHECK_EQ(env.client.post("/api/auth/login", password_body("nope nope")).status, 401);
    }
    CHECK_EQ(env.client.post("/api/auth/login", password_body(kPassword)).status, 200);
  }
}

TEST(api_failed_sign_ins_are_logged_with_the_client_address) {
  Env env;
  env.sign_in();
  env.client.address = "192.168.1.9";
  env.client.post("/api/auth/login", password_body("nope nope"));
  const std::string log = env.log_text();
  CHECK(contains(log, "error: Failed sign-in from 192.168.1.9"));
  CHECK(contains(log, "info: Password set"));
}

TEST(api_an_unknown_client_address_is_throttled_as_one_client) {
  Env env;
  env.sign_in();
  env.client.address = "";
  for (int i = 0; i < LoginLimiter::kMaxFailures; ++i) env.client.post("/api/auth/login", password_body("nope nope"));
  CHECK_EQ(env.client.post("/api/auth/login", password_body(kPassword)).status, 429);
  CHECK(contains(env.log_text(), "Failed sign-in from ?"));
}

TEST(api_change_password) {
  Env env;
  env.sign_in();
  const Reply wrong = env.client.post("/api/auth/password", "{\"current\":\"wrong wrong\",\"new\":\"brand new pass\"}");
  CHECK_EQ(wrong.status, 403);
  CHECK_EQ(wrong.error(), std::string("the current password is wrong"));
  const Reply weak = env.client.post("/api/auth/password",
                                     std::string("{\"current\":\"") + kPassword + "\",\"new\":\"short\"}");
  CHECK_EQ(weak.status, 400);
  const std::string old_cookie = env.client.cookie;
  const Reply changed = env.client.post("/api/auth/password",
                                        std::string("{\"current\":\"") + kPassword + "\",\"new\":\"brand new pass\"}");
  CHECK_EQ(changed.status, 200);
  CHECK(contains(changed.header("Set-Cookie"), "bd_session="));
  CHECK_EQ(env.client.get("/api/state").status, 200);  // the fresh cookie works
  env.client.cookie = old_cookie;                      // the old session ended with the old key
  CHECK_EQ(env.client.get("/api/state").status, 401);
  CHECK_EQ(env.client.post("/api/auth/login", password_body(kPassword)).status, 401);
  CHECK_EQ(env.client.post("/api/auth/login", password_body("brand new pass")).status, 200);
  CHECK(contains(env.log_text(), "info: Password changed"));
}

TEST(api_session_cookie_attributes) {
  Env env;
  const Reply setup = env.client.post("/api/auth/setup", password_body(kPassword));
  const std::string cookie = setup.header("Set-Cookie");
  CHECK(starts_with(cookie, "bd_session="));
  CHECK(contains(cookie, "HttpOnly"));
  CHECK(contains(cookie, "SameSite=Strict"));
  CHECK(contains(cookie, "Path=/"));
  CHECK(contains(cookie, "Max-Age=2592000"));

  const Reply logout = env.client.post("/api/auth/logout");
  const std::string removal = logout.header("Set-Cookie");
  CHECK(starts_with(removal, "bd_session=;"));
  CHECK(contains(removal, "Max-Age=0"));
  CHECK(contains(removal, "Path=/"));
  CHECK(env.client.cookie.empty());

  const Reply login = env.client.post("/api/auth/login", password_body(kPassword));
  CHECK(contains(login.header("Set-Cookie"), "HttpOnly"));
  CHECK(contains(login.header("Set-Cookie"), "SameSite=Strict"));
}

TEST(api_session_expires_after_the_ttl) {
  Env env;
  env.sign_in();
  env.platform.mono += kSessionTtlS + 1;
  CHECK_EQ(env.client.get("/api/state").status, 401);
  CHECK_EQ(env.client.get("/api/auth/status").body,
           std::string("{\"configured\":true,\"authenticated\":false}"));
}

// ---- learn ----

TEST(api_find_button_flow_adds_the_button) {
  Env env;
  env.sign_in();
  const Reply saved = env.add_button("Desk remote");
  CHECK_EQ(saved.status, 201);
  CHECK(saved.flag("created"));
  CHECK_EQ(saved.str("button.mac"), std::string(kDesk));
  CHECK_EQ(saved.str("button.name"), std::string("Desk remote"));
  CHECK_EQ(saved.str("trigger.button"), saved.str("button.id"));
  CHECK(saved.is_null("trigger.channel"));
  CHECK_EQ(saved.str("trigger.input"), std::string("button:2"));
  CHECK_EQ(saved.str("trigger.event"), std::string("press"));
  CHECK(starts_with(saved.str("button.id"), "btn-"));
  const Reply state = env.client.get("/api/state");
  CHECK_EQ(state.size("buttons"), static_cast<size_t>(1));
  CHECK_EQ(state.str("buttons.0.name"), std::string("Desk remote"));
  CHECK(contains(env.log_text(), "info: Added button \"Desk remote\" (AA:BB:CC:DD:EE:01)"));
  // The session is over once the button is saved.
  CHECK_EQ(env.client.get("/api/learn").str("phase"), std::string("idle"));
}

TEST(api_learning_an_already_known_button_reuses_it) {
  Env env;
  env.sign_in();
  const Reply first = env.add_button("Desk remote");
  env.platform.mono += 60;
  const Reply second = env.add_button("Different name");
  CHECK_EQ(second.status, 200);
  CHECK(!second.flag("created"));
  CHECK_EQ(second.str("button.id"), first.str("button.id"));
  CHECK_EQ(second.str("button.name"), std::string("Desk remote"));
  CHECK_EQ(env.client.get("/api/state").size("buttons"), static_cast<size_t>(1));
}

TEST(api_learn_status_names_the_known_button) {
  Env env;
  env.sign_in();
  const Reply saved = env.add_button();
  env.platform.mono += 60;
  const Reply started = env.client.post("/api/learn/start");
  CHECK_EQ(started.status, 200);
  CHECK_EQ(started.str("phase"), std::string("waiting"));
  CHECK(started.is_null("candidate"));
  env.press(kPressB1);
  env.platform.mono += 1;
  const Reply status = env.client.get("/api/learn");
  CHECK_EQ(status.str("phase"), std::string("confirm"));
  CHECK_EQ(status.str("candidate.known_button"), saved.str("button.id"));
  CHECK_EQ(status.str("candidate.mac"), std::string(kDesk));
  CHECK_EQ(status.str("candidate.input"), std::string("button:1"));
  CHECK_EQ(status.num("candidate.rssi"), -40.0);
  CHECK(status.has("seconds_left"));
}

TEST(api_learn_status_of_an_unknown_button_has_no_known_button) {
  Env env;
  env.sign_in();
  env.client.post("/api/learn/start");
  env.press(kPressB2);
  env.platform.mono += 1;
  const Reply status = env.client.get("/api/learn");
  CHECK(status.has("candidate.known_button"));
  CHECK(status.is_null("candidate.known_button"));
}

TEST(api_save_without_a_confirmed_press_is_refused) {
  Env env;
  env.sign_in();
  env.client.post("/api/learn/start");
  const Reply refused = env.client.post("/api/learn/save", "{\"name\":\"Nope\"}");
  CHECK_EQ(refused.status, 409);
  CHECK_EQ(refused.error(), std::string("no confirmed button to save; use Find button first"));
}

TEST(api_save_with_an_empty_name_is_refused_and_keeps_the_session) {
  Env env;
  env.sign_in();
  env.client.post("/api/learn/start");
  env.press(kPressB2);
  env.platform.mono += 1;
  env.client.get("/api/learn");
  env.press(kPressB2Again);
  CHECK_EQ(env.client.post("/api/learn/save", "{\"name\":\"\"}").status, 400);
  CHECK_EQ(env.client.get("/api/learn").str("phase"), std::string("confirmed"));
  CHECK_EQ(env.client.post("/api/learn/save", "{\"name\":\"Fine\"}").status, 201);
}

TEST(api_learn_cancel) {
  Env env;
  env.sign_in();
  env.client.post("/api/learn/start");
  const Reply cancelled = env.client.post("/api/learn/cancel");
  CHECK_EQ(cancelled.body, std::string("{\"ok\":true}"));
  CHECK_EQ(env.client.get("/api/learn").str("phase"), std::string("idle"));
}

TEST(api_advertised_name_becomes_the_model) {
  Env env;
  env.sign_in();
  {
    std::lock_guard<std::mutex> lock(env.app->mutex);
    env.app->names[kDesk] = "SBBT-104CUS";
  }
  const Reply saved = env.add_button();
  CHECK_EQ(saved.str("button.model"), std::string("SBBT-104CUS"));
}

// ---- buttons and bindings ----

TEST(api_state_lists_buttons_bindings_and_choices) {
  Env env;
  env.sign_in();
  env.add_button();
  const Reply state = env.client.get("/api/state");
  CHECK_EQ(state.size("bindings"), static_cast<size_t>(0));
  CHECK_EQ(state.str("settings.http_timeout_s") == "<not a string>", true);
  CHECK_EQ(state.num("settings.http_timeout_s"), 10.0);
  CHECK(contains(state.body, "\"switchbot\":{\"configured\":false}"));
  CHECK_EQ(state.str("choices.button_events.0.value"), std::string("press"));
  CHECK_EQ(state.str("choices.button_events.0.label"), std::string("single press"));
  CHECK_EQ(state.size("choices.button_events"), static_cast<size_t>(7));
  CHECK_EQ(state.str("choices.wheel_events.0.value"), std::string("turn"));
  CHECK_EQ(state.str("choices.wheel_events.0.label"), std::string("turn"));
  CHECK(contains(state.body, "\"http_methods\":[\"GET\",\"POST\",\"PUT\",\"DELETE\"]"));
}

TEST(api_binding_crud) {
  Env env;
  env.sign_in();
  const std::string button = env.add_button().str("button.id");
  const Reply created = env.client.post("/api/bindings", env.http_binding(button));
  CHECK_EQ(created.status, 201);
  const std::string id = created.str("id");
  CHECK(starts_with(id, "bind-"));
  const Reply edited = env.client.put("/api/bindings/" + id, env.http_binding(button, "Renamed", false));
  CHECK_EQ(edited.status, 200);
  CHECK_EQ(edited.str("name"), std::string("Renamed"));
  CHECK_EQ(edited.str("id"), id);
  CHECK(!edited.flag("enabled"));
  const Reply state = env.client.get("/api/state");
  CHECK_EQ(state.size("bindings"), static_cast<size_t>(1));
  CHECK_EQ(state.str("bindings.0.name"), std::string("Renamed"));
  CHECK_EQ(env.client.del("/api/bindings/" + id).status, 200);
  const Reply gone = env.client.del("/api/bindings/" + id);
  CHECK_EQ(gone.status, 404);
  CHECK_EQ(gone.error(), std::string("no such binding"));
  CHECK_EQ(env.client.put("/api/bindings/" + id, env.http_binding(button)).status, 404);
}

TEST(api_binding_ids_ignore_an_id_in_the_body) {
  Env env;
  env.sign_in();
  const std::string button = env.add_button().str("button.id");
  std::string body = env.http_binding(button);
  body.insert(1, "\"id\":\"chosen\",");
  const Reply created = env.client.post("/api/bindings", body);
  CHECK_EQ(created.status, 201);
  CHECK(created.str("id") != "chosen");
}

TEST(api_bindings_keep_their_action_shapes) {
  Env env;
  env.sign_in();
  const std::string button = env.add_button().str("button.id");
  const Reply created = env.client.post(
      "/api/bindings",
      env.http_binding(button, "Bot", true, 700,
                       "{\"type\":\"switchbot_command\",\"device_id\":\"BOT1\",\"device_name\":\"Living Bot\","
                       "\"command\":\"press\"}"));
  CHECK_EQ(created.status, 201);
  CHECK_EQ(created.str("action.type"), std::string("switchbot_command"));
  CHECK_EQ(created.str("action.parameter"), std::string("default"));
  CHECK_EQ(created.str("action.command_type"), std::string("command"));
  CHECK_EQ(created.num("cooldown_ms"), 700.0);
}

TEST(api_invalid_bindings_are_explained) {
  Env env;
  env.sign_in();
  const std::string button = env.add_button().str("button.id");
  const Reply bad_url = env.client.post(
      "/api/bindings", env.http_binding(button, "Hit target", true, 0, "{\"type\":\"http\",\"url\":\"ftp://nope\"}"));
  CHECK_EQ(bad_url.status, 400);
  CHECK(contains(bad_url.error(), "URL"));
  const Reply unknown = env.client.post("/api/bindings", env.http_binding("ghost"));
  CHECK_EQ(unknown.status, 400);
  CHECK(contains(unknown.error(), "unknown button"));
  CHECK_EQ(env.client.post("/api/bindings", "{\"nonsense\":true}").status, 400);
  CHECK_EQ(env.client.get("/api/state").size("bindings"), static_cast<size_t>(0));
}

TEST(api_bad_json_bodies) {
  Env env;
  env.sign_in();
  const Reply broken = env.client.post("/api/bindings", "{oops");
  CHECK_EQ(broken.status, 400);
  CHECK_EQ(broken.error(), std::string("the request body is not valid JSON"));
  const Reply empty = env.client.post("/api/bindings", "");
  CHECK_EQ(empty.status, 400);
  CHECK_EQ(empty.error(), std::string("the request body is not valid JSON"));
  const Reply trailing = env.client.post("/api/bindings", "{} junk");
  CHECK_EQ(trailing.status, 400);
  const Reply list = env.client.post("/api/bindings", "[1]");
  CHECK_EQ(list.status, 400);
  CHECK_EQ(list.error(), std::string("the request body must be a JSON object"));
  CHECK_EQ(env.client.put("/api/settings", "3").status, 400);
}

TEST(api_rename_button) {
  Env env;
  env.sign_in();
  const std::string id = env.add_button().str("button.id");
  const Reply renamed = env.client.put("/api/buttons/" + id, "{\"name\":\"Bedside\"}");
  CHECK_EQ(renamed.status, 200);
  CHECK_EQ(renamed.str("name"), std::string("Bedside"));
  CHECK_EQ(renamed.str("mac"), std::string(kDesk));
  CHECK_EQ(env.client.put("/api/buttons/" + id, "{\"name\":\"\"}").status, 400);
  CHECK_EQ(env.client.put("/api/buttons/" + id, "{}").status, 400);
  const Reply ghost = env.client.put("/api/buttons/ghost", "{\"name\":\"x\"}");
  CHECK_EQ(ghost.status, 404);
  CHECK_EQ(ghost.error(), std::string("no such button"));
  CHECK_EQ(env.client.get("/api/state").str("buttons.0.name"), std::string("Bedside"));
}

TEST(api_deleting_a_used_button_needs_cascade) {
  Env env;
  env.sign_in();
  const std::string id = env.add_button().str("button.id");
  env.client.post("/api/bindings", env.http_binding(id));
  const Reply refused = env.client.del("/api/buttons/" + id);
  CHECK_EQ(refused.status, 409);
  CHECK_EQ(refused.error(),
           std::string("1 binding(s) use this button; delete them too, or retry with cascade=1"));
  CHECK_EQ(env.client.del("/api/buttons/" + id + "?cascade=1").status, 200);
  const Reply state = env.client.get("/api/state");
  CHECK_EQ(state.size("buttons"), static_cast<size_t>(0));
  CHECK_EQ(state.size("bindings"), static_cast<size_t>(0));
}

TEST(api_deleting_an_unused_button) {
  Env env;
  env.sign_in();
  const std::string id = env.add_button().str("button.id");
  CHECK_EQ(env.client.del("/api/buttons/" + id).status, 200);
  const Reply again = env.client.del("/api/buttons/" + id);
  CHECK_EQ(again.status, 404);
  CHECK_EQ(again.error(), std::string("no such button"));
}

// ---- firing ----

namespace {

// A signed-in environment with the desk button and the time moved past the presses that added it.
struct DeskFixture {
  Env env;
  std::string button;
  DeskFixture() {
    env.sign_in();
    button = env.add_button().str("button.id");
    env.platform.mono += 60;
  }
  Reply add_binding(const std::string& name = "Hit target", bool enabled = true, int cooldown_ms = 0,
                    const std::string& action = "") {
    return env.client.post("/api/bindings", env.http_binding(button, name, enabled, cooldown_ms, action));
  }
};

}  // namespace

TEST(api_a_press_runs_the_action_and_is_logged) {
  DeskFixture f;
  f.add_binding();
  f.env.press(kPressB2);
  CHECK_EQ(f.env.hits.size(), static_cast<size_t>(1));
  CHECK_EQ(f.env.hits[0], std::string("GET ") + kTargetUrl);
  const std::string log = f.env.log_text();
  CHECK(contains(log, "ble: Desk remote"));
  CHECK(contains(log, "fire: Binding \"Hit target\" fired"));
  CHECK(contains(log, "action: \"Hit target\": GET http://target.test: HTTP 200 OK"));
}

TEST(api_repeated_copies_of_a_press_run_the_action_once) {
  DeskFixture f;
  f.add_binding();
  for (int i = 0; i < 25; ++i) f.env.press(kPressB2);
  CHECK_EQ(f.env.hits.size(), static_cast<size_t>(1));
}

TEST(api_other_buttons_and_unknown_devices_do_not_fire) {
  DeskFixture f;
  f.add_binding();
  f.env.press(kPressB1);
  f.env.press(kPressB2, "11:22:33:44:55:66");
  CHECK(f.env.hits.empty());
  CHECK(contains(f.env.log_text(), "unknown device 11:22:33:44:55:66"));
}

TEST(api_disabled_bindings_do_not_fire) {
  DeskFixture f;
  f.add_binding("Hit target", false);
  f.env.press(kPressB2);
  CHECK(f.env.hits.empty());
}

TEST(api_editing_a_binding_takes_effect_immediately) {
  DeskFixture f;
  const Reply binding = f.add_binding("Hit target", false);
  f.env.client.put("/api/bindings/" + binding.str("id"), f.env.http_binding(f.button));
  f.env.press(kPressB2);
  CHECK_EQ(f.env.hits.size(), static_cast<size_t>(1));
}

TEST(api_deleting_a_binding_stops_it_firing) {
  DeskFixture f;
  const Reply binding = f.add_binding();
  f.env.client.del("/api/bindings/" + binding.str("id"));
  f.env.press(kPressB2);
  CHECK(f.env.hits.empty());
}

TEST(api_a_failing_action_is_logged_as_an_error) {
  DeskFixture f;
  f.add_binding("Hit target", true, 0, std::string("{\"type\":\"http\",\"method\":\"GET\",\"url\":\"") + kDeadUrl + "\"}");
  f.env.press(kPressB2);
  CHECK(contains(f.env.log_text(), "error: \"Hit target\": GET http://127.0.0.1:1: connection failed"));
}

TEST(api_run_now) {
  DeskFixture f;
  const Reply binding = f.add_binding();
  const Reply ran = f.env.client.post("/api/bindings/" + binding.str("id") + "/run");
  CHECK_EQ(ran.status, 200);
  CHECK(ran.flag("ok"));
  CHECK(contains(ran.str("summary"), "HTTP 200"));
  CHECK_EQ(f.env.hits.size(), static_cast<size_t>(1));
  CHECK(contains(f.env.log_text(), "action: \"Hit target\""));
  const Reply ghost = f.env.client.post("/api/bindings/ghost/run");
  CHECK_EQ(ghost.status, 404);
  CHECK_EQ(ghost.error(), std::string("no such binding"));
}

TEST(api_run_now_reports_failure) {
  DeskFixture f;
  const Reply binding = f.add_binding(
      "Hit target", true, 0, std::string("{\"type\":\"http\",\"url\":\"") + kDeadUrl + "\"}");
  const Reply ran = f.env.client.post("/api/bindings/" + binding.str("id") + "/run");
  CHECK_EQ(ran.status, 200);
  CHECK(!ran.flag("ok"));
  CHECK(contains(ran.str("summary"), "connection failed"));
}

TEST(api_simulate_fires_matching_bindings_ignoring_cooldown) {
  DeskFixture f;
  f.add_binding("First", true, 60000);
  f.add_binding("Second");
  std::string trigger = "{\"button\":\"" + f.button + "\",\"input\":\"button:2\",\"event\":\"press\",\"channel\":null}";
  for (int i = 0; i < 2; ++i) {
    const Reply reply = f.env.client.post("/api/simulate", trigger);
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(reply.body, std::string("{\"matched\":[\"First\",\"Second\"]}"));
  }
  CHECK_EQ(f.env.hits.size(), static_cast<size_t>(4));
  CHECK(contains(f.env.log_text(), "info: Simulated: Desk remote, button 2, single press -> 2 binding(s)"));
  const Reply none = f.env.client.post(
      "/api/simulate", "{\"button\":\"" + f.button + "\",\"input\":\"button:3\",\"event\":\"press\"}");
  CHECK_EQ(none.body, std::string("{\"matched\":[]}"));
  const Reply ghost = f.env.client.post("/api/simulate", "{\"button\":\"ghost\",\"input\":\"button:2\",\"event\":\"press\"}");
  CHECK_EQ(ghost.status, 404);
  CHECK_EQ(ghost.error(), std::string("no such button"));
  CHECK_EQ(f.env.client.post("/api/simulate", "{\"button\":\"x\"}").status, 400);
}

TEST(api_simulate_queues_the_work_instead_of_doing_it) {
  DeskFixture f;
  f.add_binding();
  f.env.platform.run_background_immediately = false;
  const Reply reply = f.env.client.post("/api/simulate",
                                        "{\"button\":\"" + f.button + "\",\"input\":\"button:2\",\"event\":\"press\"}");
  CHECK_EQ(reply.status, 200);
  CHECK(f.env.hits.empty());
  CHECK_EQ(f.env.platform.background.size(), static_cast<size_t>(1));
  f.env.platform.run_queued_background();
  CHECK_EQ(f.env.hits.size(), static_cast<size_t>(1));
}

TEST(api_log_since) {
  DeskFixture f;
  const Reply first = f.env.client.get("/api/log");
  CHECK(first.num("last_seq") > 0);
  CHECK(first.size("entries") > 0);
  CHECK(first.has("entries.0.seq"));
  CHECK(first.has("entries.0.kind"));
  f.env.press(kPressB1);
  const Reply newer = f.env.client.get("/api/log?since=" + std::to_string(static_cast<int>(first.num("last_seq"))));
  CHECK_EQ(newer.size("entries"), static_cast<size_t>(1));
  CHECK(newer.num("last_seq") > first.num("last_seq"));
  for (const char* bad : {"abc", "-1", "", "1.5", "99999999999999999999"}) {
    const Reply reply = f.env.client.get(std::string("/api/log?since=") + bad);
    CHECK_EQ(reply.status, 400);
    CHECK_EQ(reply.error(), std::string("since must be a number"));
  }
  CHECK_EQ(f.env.client.get("/api/log?since=4294967296").size("entries"), static_cast<size_t>(0));
}

TEST(api_log_times_are_wall_clock_seconds) {
  Env env;
  env.sign_in();
  const Reply log = env.client.get("/api/log");
  // The platform's wall clock reads 1700000000 s when the monotonic clock reads `mono`, and the entry was
  // logged at `mono`.
  CHECK_EQ(log.num("entries.0.time"), 1700000000.0);
  env.platform.mono += 10;
  env.platform.wall_ms += 10000;
  CHECK_EQ(env.client.get("/api/log").num("entries.0.time"), 1700000000.0);
}

// ---- settings and SwitchBot ----

TEST(api_settings) {
  Env env;
  env.sign_in();
  const Reply set = env.client.put("/api/settings", "{\"http_timeout_s\":5}");
  CHECK_EQ(set.status, 200);
  CHECK_EQ(set.num("http_timeout_s"), 5.0);
  CHECK_EQ(set.body, std::string("{\"http_timeout_s\":5.0}"));
  CHECK_EQ(env.client.put("/api/settings", "{\"http_timeout_s\":0}").status, 400);
  CHECK_EQ(env.client.put("/api/settings", "{\"http_timeout_s\":61}").status, 400);
  CHECK_EQ(env.client.get("/api/state").num("settings.http_timeout_s"), 5.0);
  // A request that leaves the field out keeps the stored value.
  CHECK_EQ(env.client.put("/api/settings", "{}").body, std::string("{\"http_timeout_s\":5.0}"));
}

TEST(api_credentials_are_write_only) {
  Env env;
  env.sign_in();
  const Reply set = env.client.put("/api/switchbot/credentials", credentials_body(kToken, kSecret));
  CHECK_EQ(set.status, 200);
  CHECK_EQ(set.body, std::string("{\"configured\":true}"));
  const Reply state = env.client.get("/api/state");
  CHECK(contains(state.body, "\"switchbot\":{\"configured\":true}"));
  CHECK(!contains(state.body, kToken));
  CHECK(!contains(state.body, kSecret));
  CHECK(!contains(env.client.get("/api/log").body, kToken));
}

TEST(api_a_blank_field_keeps_the_stored_value) {
  Env env;
  env.sign_in();
  env.client.put("/api/switchbot/credentials", credentials_body(kToken, kSecret));
  env.client.put("/api/switchbot/credentials", credentials_body("", ""));
  env.client.put("/api/switchbot/credentials", credentials_body("   ", "  "));
  {
    std::lock_guard<std::mutex> lock(env.app->mutex);
    CHECK_EQ(env.app->config.switchbot_token, std::string(kToken));
    CHECK_EQ(env.app->config.switchbot_secret, std::string(kSecret));
  }
  env.client.put("/api/switchbot/credentials", "{\"token\":\"  other \"}");
  std::lock_guard<std::mutex> lock(env.app->mutex);
  CHECK_EQ(env.app->config.switchbot_token, std::string("other"));
  CHECK_EQ(env.app->config.switchbot_secret, std::string(kSecret));
}

TEST(api_credentials_must_be_text) {
  Env env;
  env.sign_in();
  const Reply bad = env.client.put("/api/switchbot/credentials", "{\"token\":5}");
  CHECK_EQ(bad.status, 400);
  CHECK_EQ(bad.error(), std::string("token and secret must be text"));
  CHECK_EQ(env.client.put("/api/switchbot/credentials", "{\"secret\":null}").status, 400);
}

TEST(api_credentials_can_be_removed) {
  Env env;
  env.sign_in();
  env.client.put("/api/switchbot/credentials", credentials_body(kToken, kSecret));
  const Reply removed = env.client.del("/api/switchbot/credentials");
  CHECK_EQ(removed.body, std::string("{\"configured\":false}"));
  std::lock_guard<std::mutex> lock(env.app->mutex);
  CHECK(!env.app->config.has_switchbot_credentials());
}

TEST(api_device_scene_and_command_lists) {
  Env env;
  env.sign_in();
  env.client.put("/api/switchbot/credentials", credentials_body(kToken, kSecret));
  const Reply devices = env.client.get("/api/switchbot/devices");
  CHECK_EQ(devices.status, 200);
  CHECK_EQ(devices.size("devices"), static_cast<size_t>(3));
  CHECK_EQ(devices.str("devices.0.id"), std::string("BOT1"));
  CHECK_EQ(devices.str("devices.1.id"), std::string("HUB1"));
  CHECK_EQ(devices.str("devices.2.id"), std::string("TV1"));
  CHECK_EQ(devices.str("devices.0.hub"), std::string("HUB1"));
  CHECK(devices.is_null("devices.0.remote_type"));
  CHECK_EQ(devices.str("devices.2.remote_type"), std::string("TV"));
  CHECK_EQ(devices.str("devices.2.type"), std::string("IR TV"));
  const Reply scenes = env.client.get("/api/switchbot/scenes");
  CHECK_EQ(scenes.body, std::string("{\"scenes\":[{\"id\":\"S1\",\"name\":\"Good night\"}]}"));
  const Reply commands = env.client.get("/api/switchbot/commands?device_id=BOT1");
  CHECK_EQ(commands.status, 200);
  CHECK_EQ(commands.str("device.id"), std::string("BOT1"));
  CHECK_EQ(commands.str("commands.0.command"), std::string("turnOff"));
  CHECK_EQ(commands.str("commands.1.command"), std::string("turnOn"));
  CHECK_EQ(commands.str("commands.2.command"), std::string("press"));
  CHECK(commands.has("commands.0.parameter"));
  CHECK(commands.has("commands.0.description"));
  CHECK(commands.has("notes"));
  const Reply missing = env.client.get("/api/switchbot/commands");
  CHECK_EQ(missing.status, 400);
  CHECK_EQ(missing.error(), std::string("device_id is required"));
  CHECK_EQ(env.client.get("/api/switchbot/commands?device_id=NOPE").status, 502);
}

TEST(api_device_lists_are_cached_until_refreshed) {
  Env env;
  env.sign_in();
  env.client.put("/api/switchbot/credentials", credentials_body(kToken, kSecret));
  env.client.get("/api/switchbot/devices");
  env.client.get("/api/switchbot/devices");
  CHECK_EQ(env.switchbot.devices_calls, 1);
  env.client.get("/api/switchbot/devices?refresh=1");
  CHECK_EQ(env.switchbot.devices_calls, 2);
}

TEST(api_lists_fail_clearly_without_credentials_or_with_wrong_ones) {
  Env env;
  env.sign_in();
  const Reply unset = env.client.get("/api/switchbot/devices");
  CHECK_EQ(unset.status, 502);
  CHECK(contains(unset.error(), "not set"));
  env.client.put("/api/switchbot/credentials", credentials_body(kToken, "wrong"));
  const Reply wrong = env.client.get("/api/switchbot/devices");
  CHECK_EQ(wrong.status, 502);
  CHECK(contains(wrong.error(), "authentication failed"));
  CHECK_EQ(env.client.get("/api/switchbot/scenes").status, 502);
}

TEST(api_changing_credentials_drops_the_cached_lists) {
  Env env;
  env.sign_in();
  env.client.put("/api/switchbot/credentials", credentials_body(kToken, kSecret));
  CHECK_EQ(env.client.get("/api/switchbot/devices").status, 200);
  env.client.put("/api/switchbot/credentials", "{\"secret\":\"wrong\"}");
  CHECK_EQ(env.client.get("/api/switchbot/devices").status, 502);
}

TEST(api_a_switchbot_binding_runs_through_the_api) {
  Env env;
  env.sign_in();
  env.client.put("/api/switchbot/credentials", credentials_body(kToken, kSecret));
  const std::string button = env.add_button().str("button.id");
  const Reply binding = env.client.post(
      "/api/bindings",
      "{\"name\":\"Bot\",\"trigger\":{\"button\":\"" + button + "\",\"input\":\"button:2\",\"event\":\"press\"},"
      "\"action\":{\"type\":\"switchbot_command\",\"device_id\":\"BOT1\",\"device_name\":\"Living Bot\","
      "\"command\":\"press\"}}");
  CHECK_EQ(binding.status, 201);
  const Reply ran = env.client.post("/api/bindings/" + binding.str("id") + "/run");
  CHECK_EQ(ran.status, 200);
  CHECK(ran.flag("ok"));
  CHECK_EQ(ran.str("summary"), std::string("SwitchBot: sent press to Living Bot"));
  CHECK_EQ(env.switchbot.commands.size(), static_cast<size_t>(1));
  CHECK(starts_with(env.switchbot.commands[0], "BOT1 "));
  CHECK(contains(env.switchbot.commands[0], "\"command\":\"press\""));
}

TEST(api_a_switchbot_failure_is_reported_by_run_now) {
  Env env;
  env.sign_in();
  const std::string button = env.add_button().str("button.id");
  const Reply binding = env.client.post(
      "/api/bindings",
      "{\"name\":\"Scene\",\"trigger\":{\"button\":\"" + button + "\",\"input\":\"button:2\",\"event\":\"press\"},"
      "\"action\":{\"type\":\"switchbot_scene\",\"scene_id\":\"S1\"}}");
  const Reply ran = env.client.post("/api/bindings/" + binding.str("id") + "/run");
  CHECK_EQ(ran.status, 200);
  CHECK(!ran.flag("ok"));
  CHECK(starts_with(ran.str("summary"), "SwitchBot: "));
}

// ---- export and import ----

namespace {

struct ExportFixture {
  Env env;
  std::string button;
  ExportFixture() {
    env.sign_in();
    button = env.add_button().str("button.id");
    env.client.post("/api/bindings", env.http_binding(button));
    env.client.put("/api/switchbot/credentials", credentials_body(kToken, kSecret));
  }
  Reply export_config(bool credentials = false) {
    Reply reply = env.client.get(std::string("/api/export") + (credentials ? "?credentials=1" : ""));
    CHECK_EQ(reply.status, 200);
    CHECK(contains(reply.header("Content-Disposition"), "attachment"));
    CHECK(contains(reply.header("Content-Disposition"), "button-dispatcher-config.json"));
    CHECK_EQ(reply.content_type, std::string("application/json"));
    return reply;
  }
};

}  // namespace

TEST(api_export_leaves_credentials_out_by_default) {
  ExportFixture f;
  const Reply data = f.export_config();
  CHECK_EQ(data.size("buttons"), static_cast<size_t>(1));
  CHECK_EQ(data.size("bindings"), static_cast<size_t>(1));
  CHECK(!data.has("switchbot"));
  CHECK(!contains(data.body, kToken));
  CHECK(data.body.back() == '\n');
  CHECK(contains(data.body, "\n  \"buttons\""));
}

TEST(api_export_with_credentials_on_request) {
  ExportFixture f;
  const Reply data = f.export_config(true);
  CHECK_EQ(data.str("switchbot.token"), std::string(kToken));
  CHECK_EQ(data.str("switchbot.secret"), std::string(kSecret));
}

TEST(api_export_never_contains_the_password_hash_or_key) {
  ExportFixture f;
  const std::string text = f.export_config(true).body;
  CHECK(!contains(text, "pbkdf2"));
  CHECK(!contains(text, "password"));
  CHECK(!contains(text, "hash"));
}

TEST(api_export_then_import_replace_round_trips) {
  ExportFixture f;
  const std::string data = f.export_config().body;
  f.env.client.del("/api/buttons/" + f.button + "?cascade=1");
  const Reply summary = f.env.client.post("/api/import", "{\"config\":" + data + ",\"mode\":\"replace\"}");
  CHECK_EQ(summary.status, 200);
  CHECK_EQ(summary.body, std::string("{\"buttons\":1,\"bindings\":1}"));
  // The file had no credentials, so the stored ones are untouched.
  {
    std::lock_guard<std::mutex> lock(f.env.app->mutex);
    CHECK(f.env.app->config.has_switchbot_credentials());
  }
  f.env.platform.mono += 60;
  f.env.press(kPressB2Id35);
  CHECK_EQ(f.env.hits.size(), static_cast<size_t>(1));
  CHECK(contains(f.env.log_text(), "info: Imported config (replace): 1 button(s), 1 binding(s)"));
}

TEST(api_import_merge_adds_to_what_exists) {
  ExportFixture f;
  const Reply summary = f.env.client.post(
      "/api/import",
      "{\"config\":{\"buttons\":[{\"id\":\"other\",\"name\":\"Other\",\"mac\":\"11:22:33:44:55:66\"}],"
      "\"bindings\":[]},\"mode\":\"merge\"}");
  CHECK_EQ(summary.status, 200);
  CHECK_EQ(summary.body, std::string("{\"buttons\":2,\"bindings\":1}"));
  // The mode defaults to merge.
  const Reply defaulted = f.env.client.post(
      "/api/import", "{\"config\":{\"buttons\":[{\"id\":\"third\",\"name\":\"T\",\"mac\":\"11:22:33:44:55:77\"}]}}");
  CHECK_EQ(defaulted.body, std::string("{\"buttons\":3,\"bindings\":1}"));
  CHECK(contains(f.env.log_text(), "Imported config (merge)"));
}

TEST(api_import_drops_the_cached_switchbot_lists) {
  ExportFixture f;
  f.env.client.get("/api/switchbot/devices");
  const Reply summary = f.env.client.post(
      "/api/import", "{\"config\":{\"switchbot\":{\"token\":\"x\",\"secret\":\"y\"}},\"mode\":\"merge\"}");
  CHECK_EQ(summary.status, 200);
  CHECK_EQ(f.env.client.get("/api/switchbot/devices").status, 502);
}

TEST(api_invalid_import_is_refused_and_changes_nothing) {
  ExportFixture f;
  const std::string before = f.export_config(true).body;
  const std::string document = f.env.config_document();
  for (const char* body : {"{\"config\":{\"bindings\":[{\"id\":\"x\"}]},\"mode\":\"merge\"}",
                           "{\"config\":{\"version\":99},\"mode\":\"replace\"}",
                           "{\"config\":{},\"mode\":\"overwrite\"}",
                           "{\"config\":[],\"mode\":\"merge\"}",
                           "{\"mode\":\"merge\"}"}) {
    const Reply reply = f.env.client.post("/api/import", body);
    CHECK_EQ(reply.status, 400);
  }
  CHECK_EQ(f.env.client.post("/api/import", "{\"mode\":\"merge\"}").error(),
           std::string("the request needs a 'config' field"));
  CHECK_EQ(f.export_config(true).body, before);
  CHECK_EQ(f.env.config_document(), document);
}

// ---- restart ----

TEST(api_password_and_config_survive_a_restart_but_sessions_do_not) {
  Env env;
  env.sign_in();
  const std::string button = env.add_button().str("button.id");
  env.client.post("/api/bindings", env.http_binding(button));
  const std::string old_cookie = env.client.cookie;
  CHECK(!old_cookie.empty());

  env.open();  // a second AppCore and Api over the same stored documents
  CHECK_EQ(env.client.cookie, old_cookie);
  CHECK_EQ(env.client.get("/api/state").status, 401);
  CHECK_EQ(env.client.get("/api/auth/status").body, std::string("{\"configured\":true,\"authenticated\":false}"));
  CHECK_EQ(env.client.post("/api/auth/login", password_body("wrong wrong")).status, 401);
  CHECK_EQ(env.client.post("/api/auth/login", password_body(kPassword)).status, 200);
  const Reply state = env.client.get("/api/state");
  CHECK_EQ(state.size("buttons"), static_cast<size_t>(1));
  CHECK_EQ(state.size("bindings"), static_cast<size_t>(1));
  // The restarted application fires the stored binding.
  env.platform.mono += 60;
  env.press(kPressB2Id35);
  CHECK_EQ(env.hits.size(), static_cast<size_t>(1));
}

// ---- storage ----

TEST(api_config_json_is_rewritten_after_each_change) {
  Env env;
  env.sign_in();
  CHECK_EQ(env.config_document(), std::string(""));  // setting the password does not touch the config
  const std::string button = env.add_button().str("button.id");
  CHECK(contains(env.config_document(), kDesk));
  const Reply binding = env.client.post("/api/bindings", env.http_binding(button));
  CHECK(contains(env.config_document(), "Hit target"));
  env.client.put("/api/bindings/" + binding.str("id"), env.http_binding(button, "Edited"));
  CHECK(contains(env.config_document(), "Edited"));
  CHECK(!contains(env.config_document(), "Hit target"));
  env.client.put("/api/buttons/" + button, "{\"name\":\"Bedside\"}");
  CHECK(contains(env.config_document(), "Bedside"));
  env.client.put("/api/settings", "{\"http_timeout_s\":5}");
  CHECK(contains(env.config_document(), "\"http_timeout_s\": 5.0"));
  env.client.put("/api/switchbot/credentials", credentials_body(kToken, kSecret));
  CHECK(contains(env.config_document(), kToken));
  env.client.del("/api/switchbot/credentials");
  CHECK(!contains(env.config_document(), kToken));
  env.client.del("/api/bindings/" + binding.str("id"));
  CHECK(!contains(env.config_document(), "Edited"));
  env.client.post("/api/import", "{\"config\":{\"buttons\":[{\"id\":\"o\",\"name\":\"Imported\",\"mac\":\"11:22:33:44:55:66\"}]}}");
  CHECK(contains(env.config_document(), "Imported"));
  env.client.del("/api/buttons/" + button);
  CHECK(!contains(env.config_document(), "Bedside"));
}

TEST(api_a_failed_save_is_reported) {
  Env env;
  env.sign_in();
  const std::string button = env.add_button().str("button.id");
  env.platform.fail_saves = true;
  const Reply failed = env.client.put("/api/buttons/" + button, "{\"name\":\"Bedside\"}");
  CHECK_EQ(failed.status, 500);
  CHECK_EQ(failed.error(), std::string("the configuration could not be saved"));
}

// ---- limits and routing ----

TEST(api_oversized_bodies_are_refused) {
  Env env;
  env.sign_in();
  const std::string padding(64 * 1024, 'a');
  const Reply huge = env.client.post("/api/bindings", "{\"name\":\"" + padding + "\"}");
  CHECK_EQ(huge.status, 413);
  CHECK_EQ(huge.error(), std::string("request too large"));
  CHECK_EQ(env.client.put("/api/settings", "{\"x\":\"" + padding + "\"}").status, 413);
  // A body just under the limit is parsed (and refused for its content).
  const std::string fits(60 * 1024, 'a');
  CHECK_EQ(env.client.post("/api/bindings", "{\"name\":\"" + fits + "\"}").status, 400);
  // The limit applies to the sign-in routes too.
  CHECK_EQ(env.client.post("/api/auth/login", "{\"password\":\"" + padding + "\"}").status, 413);
}

TEST(api_is_slow_only_for_requests_that_use_the_network) {
  Env env;
  auto slow = [&](const std::string& method, const std::string& path) {
    Request request;
    request.method = method;
    request.path = path;
    return env.api->is_slow(request);
  };
  CHECK(slow("POST", "/api/bindings/bind-1/run"));
  CHECK(slow("GET", "/api/switchbot/devices"));
  CHECK(slow("GET", "/api/switchbot/scenes"));
  CHECK(slow("GET", "/api/switchbot/commands"));
  CHECK(!slow("GET", "/api/bindings/bind-1/run"));
  CHECK(!slow("POST", "/api/simulate"));
  CHECK(!slow("GET", "/api/state"));
  CHECK(!slow("POST", "/api/bindings"));
  CHECK(!slow("PUT", "/api/bindings/bind-1"));
  CHECK(!slow("PUT", "/api/switchbot/credentials"));
  CHECK(!slow("GET", "/api/export"));
  CHECK(!slow("POST", "/api/auth/login"));
  CHECK(!slow("GET", "/api/nothing"));
  CHECK(!slow("GET", "/"));
  CHECK(!slow("POST", "/api/bindings//run"));
}

TEST(api_unknown_paths_and_methods) {
  Env env;
  env.sign_in();
  const Reply page = env.client.get("/index.html");
  CHECK_EQ(page.status, 404);
  CHECK_EQ(page.error(), std::string("not found"));
  CHECK_EQ(env.client.get("/").status, 404);
  CHECK_EQ(env.client.get("/api").status, 404);
  const Reply unknown = env.client.get("/api/nothing");
  CHECK_EQ(unknown.status, 404);
  CHECK_EQ(unknown.error(), std::string("not found"));
  CHECK_EQ(env.client.post("/api/nothing").status, 404);
  CHECK_EQ(env.client.get("/api/state/extra").status, 404);
  CHECK_EQ(env.client.get("/api/state/").status, 404);
  const Reply wrong = env.client.post("/api/state");
  CHECK_EQ(wrong.status, 405);
  CHECK_EQ(wrong.error(), std::string("method not allowed"));
  CHECK_EQ(env.client.get("/api/auth/login").status, 405);
  CHECK_EQ(env.client.put("/api/bindings").status, 405);
  CHECK_EQ(env.client.get("/api/bindings/x").status, 405);
  CHECK_EQ(env.client.del("/api/settings").status, 405);
  CHECK_EQ(env.client.call("HEAD", "/api/state").status, 405);
}

TEST(api_unknown_api_paths_need_a_session_like_the_others) {
  Env env;
  CHECK_EQ(env.client.get("/api/nothing").status, 401);
  CHECK_EQ(env.client.post("/api/state").status, 401);
  CHECK_EQ(env.client.get("/nothing").status, 404);
}

TEST(api_every_route_except_sign_in_needs_a_session) {
  Env env;
  const char* const routes[][2] = {
      {"POST", "/api/auth/logout"}, {"POST", "/api/auth/password"}, {"GET", "/api/state"},
      {"PUT", "/api/buttons/a"}, {"DELETE", "/api/buttons/a"}, {"POST", "/api/bindings"},
      {"PUT", "/api/bindings/a"}, {"DELETE", "/api/bindings/a"}, {"POST", "/api/bindings/a/run"},
      {"POST", "/api/simulate"}, {"POST", "/api/learn/start"}, {"GET", "/api/learn"},
      {"POST", "/api/learn/cancel"}, {"POST", "/api/learn/save"}, {"GET", "/api/log"},
      {"PUT", "/api/settings"}, {"PUT", "/api/switchbot/credentials"},
      {"DELETE", "/api/switchbot/credentials"}, {"GET", "/api/switchbot/devices"},
      {"GET", "/api/switchbot/scenes"}, {"GET", "/api/switchbot/commands"}, {"GET", "/api/export"},
      {"POST", "/api/import"}, {"GET", "/api/device"}, {"PUT", "/api/device"},
      {"PUT", "/api/device/pairing"}, {"DELETE", "/api/device/pairing"}, {"GET", "/api/satellites"},
      {"POST", "/api/satellites"}, {"DELETE", "/api/satellites/a"}};
  for (const auto& route : routes) {
    const Reply reply = env.client.call(route[0], route[1], "{}");
    if (reply.status != 401) std::printf("  %s %s answered %d\n", route[0], route[1], reply.status);
    CHECK_EQ(reply.status, 401);
  }
}

TEST(api_responses_are_valid_json_with_the_right_content_type) {
  Env env;
  env.sign_in();
  for (const char* path : {"/api/state", "/api/log", "/api/learn", "/api/nothing"}) {
    const Reply reply = env.client.get(path);
    CHECK(reply.json != nullptr);
    CHECK_EQ(reply.content_type, std::string("application/json"));
  }
}

// ---- concurrency ----

TEST(api_concurrent_requests_and_presses_are_safe) {
  Env env;
  env.sign_in();
  const std::string button = env.add_button().str("button.id");
  env.client.post("/api/bindings", env.http_binding(button));
  env.platform.mono += 60;

  std::atomic<int> failures{0};
  auto reader = [&](const char* path) {
    ApiClient client(env.api.get());
    client.cookie = env.client.cookie;
    for (int i = 0; i < 150; ++i) {
      const Reply reply = client.get(path);
      if (reply.status != 200 || reply.json == nullptr) ++failures;
    }
  };
  std::thread state_reader(reader, "/api/state");
  std::thread log_reader(reader, "/api/log");
  std::thread presser([&] {
    for (int id = 0; id < 100; ++id) {
      char hex[64];
      std::snprintf(hex, sizeof(hex), "4400%02x01643a003a013a003a00", id);
      env.press(hex);
    }
  });
  state_reader.join();
  log_reader.join();
  presser.join();
  CHECK_EQ(failures.load(), 0);
  CHECK_EQ(env.hits.size(), static_cast<size_t>(100));
}

// ---- device settings ----

TEST(api_device_defaults) {
  Env env;
  env.sign_in();
  const Reply device = env.client.get("/api/device");
  CHECK_EQ(device.status, 200);
  CHECK_EQ(device.str("role"), std::string("dispatcher"));
  CHECK_EQ(device.str("mac"), std::string("24:0A:C4:12:34:56"));
  CHECK_EQ(device.str("hostname"), std::string("dispatcher-24-0a-c4-12-34-56"));
  CHECK_EQ(device.str("board_config_prefix"), std::string("board.24-0a-c4-12-34-56."));
  CHECK(device.is_null("pairing"));
}

TEST(api_state_has_the_device_role_and_hostname) {
  Env env;
  env.sign_in();
  const Reply state = env.client.get("/api/state");
  CHECK_EQ(state.str("device.role"), std::string("dispatcher"));
  CHECK_EQ(state.str("device.hostname"), std::string("dispatcher-24-0a-c4-12-34-56"));
}

TEST(api_device_hostname_comes_from_the_config) {
  Env env;
  env.hostname_config.base = "dispatcher";
  env.hostname_config.append_mac = false;
  env.open();
  env.sign_in();
  CHECK_EQ(env.client.get("/api/device").str("hostname"), std::string("dispatcher"));
  CHECK_EQ(env.client.get("/api/state").str("device.hostname"), std::string("dispatcher"));
}

TEST(api_device_hostname_cannot_be_set_from_the_page) {
  Env env;
  env.sign_in();
  const Reply put = env.client.put("/api/device", "{\"hostname_base\":\"desk\",\"hostname_append_mac\":false}");
  CHECK_EQ(put.status, 200);
  CHECK_EQ(put.str("hostname"), std::string("dispatcher-24-0a-c4-12-34-56"));
  CHECK(!contains(env.platform.documents["device.json"], "desk"));
}

TEST(api_device_rejects_invalid_values) {
  Env env;
  env.sign_in();
  CHECK_EQ(env.client.put("/api/device", "{\"role\":\"boss\"}").status, 400);
  CHECK_EQ(env.client.put("/api/device", "{\"role\":3}").status, 400);
  CHECK_EQ(env.client.put("/api/device", "[]").status, 400);
}

TEST(api_device_settings_are_saved_and_survive_a_restart) {
  Env env;
  env.sign_in();
  env.client.put("/api/device", "{\"role\":\"satellite\"}");
  CHECK(env.platform.documents.count("device.json") == 1);
  CHECK(contains(env.platform.documents["device.json"], "satellite"));
  // The device settings never travel in config.json.
  CHECK(!contains(env.config_document(), "satellite"));

  env.open();
  CHECK_EQ(env.client.post("/api/auth/login", password_body(kPassword)).status, 200);
  CHECK_EQ(env.client.get("/api/device").str("role"), std::string("satellite"));
}

TEST(api_device_becoming_a_satellite_is_announced_to_the_firmware) {
  Env env;
  int calls = 0;
  env.app->on_became_satellite = [&calls] { ++calls; };
  env.sign_in();
  env.client.put("/api/device", "{\"role\":\"satellite\"}");
  env.client.put("/api/device", "{\"role\":\"satellite\"}");  // no change
  CHECK_EQ(calls, 1);
  env.client.put("/api/device", "{\"role\":\"dispatcher\"}");
  CHECK_EQ(calls, 1);
  env.client.put("/api/device", "{\"role\":\"satellite\"}");
  CHECK_EQ(calls, 2);
}

TEST(api_device_role_change_is_logged) {
  Env env;
  env.sign_in();
  const Reply put = env.client.put("/api/device", "{\"role\":\"satellite\"}");
  CHECK_EQ(put.status, 200);
  CHECK_EQ(put.str("role"), std::string("satellite"));
  CHECK_EQ(env.client.get("/api/state").str("device.role"), std::string("satellite"));
  CHECK(contains(env.log_text(), "This board is now a satellite"));
}

TEST(api_device_save_failure_is_reported) {
  Env env;
  env.sign_in();
  env.platform.fail_saves = true;
  CHECK_EQ(env.client.put("/api/device", "{\"role\":\"satellite\"}").status, 500);
}

TEST(api_corrupt_device_json_leaves_the_defaults) {
  Env env;
  env.platform.documents["device.json"] = "{\"role\":\"boss\"}";
  env.platform.documents["satellites.json"] = "garbage";
  env.open();
  env.sign_in();
  CHECK_EQ(env.client.get("/api/device").str("role"), std::string("dispatcher"));
  CHECK_EQ(env.client.get("/api/satellites").size("satellites"), static_cast<size_t>(0));
}

// ---- satellites (dispatcher side) ----

namespace {

struct PairedSatellite {
  std::string id;
  std::string name;
  std::string code;
  std::string secret;
};

PairedSatellite add_satellite(Env& env, const std::string& name = "Porch") {
  const Reply created = env.client.post("/api/satellites", "{\"name\":\"" + name + "\"}");
  CHECK_EQ(created.status, 201);
  PairedSatellite satellite;
  satellite.id = created.str("satellite.id");
  satellite.name = name;
  satellite.code = created.str("pairing_code");
  const Parsed<Pairing> pairing = parse_pairing_code(satellite.code);
  CHECK(pairing.ok());
  if (pairing.ok()) satellite.secret = pairing.value->secret;
  return satellite;
}

std::string sign(const std::string& secret, const std::string& body) {
  return hex_encode(test_crypto::hmac_sha256(secret, body));
}

std::string report_body(Env& env, int64_t seq, const std::string& boot = "ab12", int age_ms = 40,
                        const char* data = kPressB2, const char* mac = kDesk, int64_t time_offset_ms = 0) {
  Report report;
  report.time_ms = env.platform.wall_ms + time_offset_ms;
  report.boot = boot;
  report.seq = seq;
  ReportEvent event;
  event.mac = mac;
  event.rssi = -40;
  event.data = test_util::from_hex(data);
  event.age_ms = age_ms;
  report.events.push_back(event);
  return report_to_text(report);
}

// Sends a satellite's report straight to the Api: no cookie, no CSRF header, just the two satellite headers.
Reply send_report(Env& env, const PairedSatellite& satellite, const std::string& body,
                  const std::string& signature = "", const std::string& id = "") {
  Request request;
  request.method = "POST";
  request.path = "/api/satellite/report";
  request.body = body;
  request.client = "10.0.0.7";
  request.satellite = id.empty() ? satellite.id : id;
  request.signature = signature.empty() ? sign(satellite.secret, body) : signature;
  const Response response = env.api->handle(request);
  Reply reply;
  reply.status = response.status;
  reply.body = response.body;
  reply.content_type = response.content_type;
  reply.json = make_json(cJSON_ParseWithOpts(reply.body.c_str(), nullptr, true));
  return reply;
}

size_t count_of(const std::string& text, const std::string& part) {
  size_t count = 0;
  for (size_t pos = text.find(part); pos != std::string::npos; pos = text.find(part, pos + 1)) ++count;
  return count;
}

}  // namespace

TEST(api_satellite_create_returns_a_pairing_code) {
  Env env;
  env.sign_in();
  const Reply created = env.client.post("/api/satellites", "{\"name\":\"  Porch \"}");
  CHECK_EQ(created.status, 201);
  CHECK(starts_with(created.str("satellite.id"), "sat-"));
  CHECK_EQ(created.str("satellite.name"), std::string("Porch"));
  CHECK(!created.flag("satellite.online"));
  const std::string code = created.str("pairing_code");
  CHECK(starts_with(code, "bdsat1:"));
  CHECK(contains(code, "@dispatcher-24-0a-c4-12-34-56.local:80"));
  CHECK(contains(code, created.str("satellite.id") + ":"));
  // The secret is only in the code, never in the satellite object.
  CHECK(!contains(created.body.substr(0, created.body.find("pairing_code")), "secret"));
}

TEST(api_satellite_create_uses_the_http_port_and_hostname) {
  Env env;
  env.hostname_config.base = "hub";
  env.open();
  env.app->http_port = 8080;
  env.sign_in();
  const Reply created = env.client.post("/api/satellites", "{\"name\":\"Porch\"}");
  CHECK(contains(created.str("pairing_code"), "@hub-24-0a-c4-12-34-56.local:8080"));
}

TEST(api_satellite_names_must_be_valid_and_unique) {
  Env env;
  env.sign_in();
  add_satellite(env, "Porch");
  CHECK_EQ(env.client.post("/api/satellites", "{\"name\":\"porch\"}").status, 400);
  CHECK_EQ(env.client.post("/api/satellites", "{\"name\":\"\"}").status, 400);
  CHECK_EQ(env.client.post("/api/satellites", "{}").status, 400);
  CHECK_EQ(env.client.post("/api/satellites", "{\"name\":\"" + std::string(41, 'x') + "\"}").status, 400);
  CHECK_EQ(env.client.get("/api/satellites").size("satellites"), static_cast<size_t>(1));
}

TEST(api_satellite_list_and_delete) {
  Env env;
  env.sign_in();
  const PairedSatellite satellite = add_satellite(env);
  const Reply list = env.client.get("/api/satellites");
  CHECK_EQ(list.status, 200);
  CHECK_EQ(list.size("satellites"), static_cast<size_t>(1));
  CHECK_EQ(list.str("satellites.0.id"), satellite.id);
  CHECK_EQ(list.str("satellites.0.name"), std::string("Porch"));
  CHECK(!list.flag("satellites.0.online"));
  CHECK(list.is_null("satellites.0.last_seen"));
  CHECK(list.num("max") > 0);

  CHECK_EQ(env.client.del("/api/satellites/" + satellite.id).status, 200);
  CHECK_EQ(env.client.get("/api/satellites").size("satellites"), static_cast<size_t>(0));
  CHECK_EQ(env.client.del("/api/satellites/" + satellite.id).status, 404);
}

TEST(api_satellites_are_saved_and_survive_a_restart) {
  Env env;
  env.sign_in();
  const PairedSatellite satellite = add_satellite(env);
  CHECK(env.platform.documents.count("satellites.json") == 1);
  env.open();
  CHECK_EQ(env.client.post("/api/auth/login", password_body(kPassword)).status, 200);
  const Reply list = env.client.get("/api/satellites");
  CHECK_EQ(list.size("satellites"), static_cast<size_t>(1));
  CHECK_EQ(list.str("satellites.0.id"), satellite.id);
  // The restored secret still verifies reports.
  CHECK_EQ(send_report(env, satellite, report_body(env, 1)).status, 200);
}

TEST(api_satellite_save_failure_leaves_the_list_unchanged) {
  Env env;
  env.sign_in();
  env.platform.fail_saves = true;
  CHECK_EQ(env.client.post("/api/satellites", "{\"name\":\"Porch\"}").status, 500);
  env.platform.fail_saves = false;
  CHECK_EQ(env.client.get("/api/satellites").size("satellites"), static_cast<size_t>(0));
}

TEST(api_satellites_are_limited) {
  Env env;
  env.sign_in();
  for (size_t i = 0; i < SatelliteRegistry::kMaxSatellites; ++i) add_satellite(env, "Sat " + std::to_string(i));
  CHECK_EQ(env.client.post("/api/satellites", "{\"name\":\"One too many\"}").status, 409);
}

TEST(api_export_never_contains_satellite_secrets) {
  Env env;
  env.sign_in();
  const PairedSatellite satellite = add_satellite(env);
  const Reply exported = env.client.get("/api/export?credentials=1");
  CHECK_EQ(exported.status, 200);
  CHECK(!contains(exported.body, hex_encode(satellite.secret)));
  CHECK(!contains(exported.body, "satellites"));
  CHECK(!contains(exported.body, satellite.id));
  CHECK(!contains(exported.body, "bdsat1"));
}

// ---- satellite reports (dispatcher side) ----

TEST(api_satellite_report_fires_the_binding_and_marks_the_satellite_online) {
  DeskFixture f;
  f.add_binding();
  const PairedSatellite satellite = add_satellite(f.env);
  const Reply reply = send_report(f.env, satellite, report_body(f.env, 1));
  CHECK_EQ(reply.status, 200);
  CHECK(reply.flag("ok"));
  CHECK_EQ(reply.num("accepted"), 1.0);
  CHECK_EQ(f.env.hits.size(), static_cast<size_t>(1));
  const std::string log = f.env.log_text();
  CHECK(contains(log, "(via Porch)"));
  CHECK(contains(log, "fire: Binding \"Hit target\" fired"));
  CHECK(contains(log, "Satellite \"Porch\" is online (10.0.0.7)"));

  const Reply list = f.env.client.get("/api/satellites");
  CHECK(list.flag("satellites.0.online"));
  CHECK(!list.is_null("satellites.0.last_seen"));
  CHECK_EQ(list.str("satellites.0.address"), std::string("10.0.0.7"));
  CHECK_EQ(list.num("satellites.0.events"), 1.0);
  CHECK_EQ(list.str("satellites.0.last_error"), std::string());
}

TEST(api_satellite_goes_offline_after_a_while) {
  DeskFixture f;
  const PairedSatellite satellite = add_satellite(f.env);
  CHECK_EQ(send_report(f.env, satellite, report_body(f.env, 1)).status, 200);
  f.env.platform.mono += 60;
  CHECK(f.env.client.get("/api/satellites").flag("satellites.0.online"));
  f.env.platform.mono += 20;
  CHECK(!f.env.client.get("/api/satellites").flag("satellites.0.online"));
}

TEST(api_satellite_report_needs_neither_a_session_nor_the_csrf_header) {
  Env env;
  env.sign_in();
  const PairedSatellite satellite = add_satellite(env);
  env.client.cookie.clear();
  CHECK_EQ(env.client.get("/api/satellites").status, 401);
  CHECK_EQ(send_report(env, satellite, report_body(env, 1)).status, 200);
  // The other new routes still need a session and the header.
  CHECK_EQ(env.client.get("/api/device").status, 401);
  CHECK_EQ(env.client.post("/api/satellites", "{\"name\":\"X\"}").status, 401);
  CHECK_EQ(env.client.call("POST", "/api/satellites", "{\"name\":\"X\"}", false).status, 403);
}

TEST(api_satellite_report_replays_are_refused) {
  DeskFixture f;
  f.add_binding();
  const PairedSatellite satellite = add_satellite(f.env);
  const std::string body = report_body(f.env, 1);
  CHECK_EQ(send_report(f.env, satellite, body).status, 200);
  const Reply replay = send_report(f.env, satellite, body);
  CHECK_EQ(replay.status, 409);
  CHECK(!replay.error().empty());
  CHECK_EQ(send_report(f.env, satellite, report_body(f.env, 2, "ab12", 40, kPressB2Again)).status, 200);
  // An older sequence number of the same boot is refused too.
  CHECK_EQ(send_report(f.env, satellite, report_body(f.env, 1, "ab12", 40, kPressB2Again)).status, 409);
  CHECK_EQ(f.env.hits.size(), static_cast<size_t>(2));
  CHECK(f.env.client.get("/api/satellites").str("satellites.0.last_error").size() > 0);
}

TEST(api_satellite_restart_with_a_new_boot_id_is_accepted_only_if_newer) {
  DeskFixture f;
  const PairedSatellite satellite = add_satellite(f.env);
  CHECK_EQ(send_report(f.env, satellite, report_body(f.env, 5)).status, 200);
  // A recorded report from an older boot cannot be replayed.
  CHECK_EQ(send_report(f.env, satellite, report_body(f.env, 6, "cd34", 40, kPressB2, kDesk, -1000)).status, 409);
  f.env.platform.wall_ms += 1000;
  CHECK_EQ(send_report(f.env, satellite, report_body(f.env, 1, "cd34")).status, 200);
}

TEST(api_satellite_report_with_a_wrong_signature_or_id_is_refused) {
  DeskFixture f;
  f.add_binding();
  const PairedSatellite satellite = add_satellite(f.env);
  const std::string body = report_body(f.env, 1);
  const Reply wrong = send_report(f.env, satellite, body, sign("not the secret, but 32 bytes long!", body));
  CHECK_EQ(wrong.status, 401);
  CHECK_EQ(send_report(f.env, satellite, body, "deadbeef").status, 401);
  CHECK_EQ(send_report(f.env, satellite, body, sign(satellite.secret, body + " ")).status, 401);
  CHECK_EQ(send_report(f.env, satellite, body, "", "sat-unknown").status, 401);
  CHECK_EQ(send_report(f.env, satellite, body, "", "").status, 200);  // the right one still works afterwards
}

TEST(api_satellite_signature_is_case_insensitive_hex) {
  Env env;
  env.sign_in();
  const PairedSatellite satellite = add_satellite(env);
  std::string body = report_body(env, 1);
  std::string signature = sign(satellite.secret, body);
  for (char& c : signature) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  CHECK_EQ(send_report(env, satellite, body, signature).status, 200);
}

TEST(api_deleted_satellite_can_no_longer_report) {
  Env env;
  env.sign_in();
  const PairedSatellite satellite = add_satellite(env);
  CHECK_EQ(send_report(env, satellite, report_body(env, 1)).status, 200);
  env.client.del("/api/satellites/" + satellite.id);
  CHECK_EQ(send_report(env, satellite, report_body(env, 2)).status, 401);
}

TEST(api_satellite_report_with_a_skewed_clock_is_refused) {
  DeskFixture f;
  f.add_binding();
  const PairedSatellite satellite = add_satellite(f.env);
  CHECK_EQ(send_report(f.env, satellite, report_body(f.env, 1, "ab12", 40, kPressB2, kDesk, 31000)).status, 409);
  CHECK_EQ(send_report(f.env, satellite, report_body(f.env, 2, "ab12", 40, kPressB2, kDesk, -31000)).status, 409);
  CHECK(f.env.hits.empty());
  // Within the limit it is fine, and a slow report counts its transit time as extra age.
  CHECK_EQ(send_report(f.env, satellite, report_body(f.env, 3, "ab12", 40, kPressB2, kDesk, -29000)).status, 200);
}

TEST(api_satellite_report_transit_time_counts_towards_the_event_age) {
  DeskFixture f;
  f.add_binding();
  const PairedSatellite satellite = add_satellite(f.env);
  // The report is 4.9 s old when it arrives and the event was already 200 ms old when it was sent.
  const Reply reply = send_report(f.env, satellite, report_body(f.env, 1, "ab12", 200, kPressB2, kDesk, -4900));
  CHECK_EQ(reply.status, 200);
  CHECK_EQ(reply.num("accepted"), 0.0);
  CHECK(f.env.hits.empty());
}

TEST(api_satellite_report_needs_a_valid_clock) {
  DeskFixture f;
  f.add_binding();
  const PairedSatellite satellite = add_satellite(f.env);
  const std::string body = report_body(f.env, 1);
  f.env.platform.clock_valid = false;
  const Reply refused = send_report(f.env, satellite, body);
  CHECK_EQ(refused.status, 503);
  CHECK(f.env.hits.empty());
  f.env.platform.clock_valid = true;
  CHECK_EQ(send_report(f.env, satellite, body).status, 200);  // a refused report does not use up its sequence number
}

TEST(api_satellite_report_drops_events_that_are_too_old) {
  DeskFixture f;
  f.add_binding();
  const PairedSatellite satellite = add_satellite(f.env);
  const Reply reply = send_report(f.env, satellite, report_body(f.env, 1, "ab12", 6000));
  CHECK_EQ(reply.status, 200);
  CHECK_EQ(reply.num("accepted"), 0.0);
  CHECK(f.env.hits.empty());
  const Reply list = f.env.client.get("/api/satellites");
  CHECK_EQ(list.num("satellites.0.stale"), 1.0);
  CHECK_EQ(list.num("satellites.0.events"), 0.0);
  CHECK(list.flag("satellites.0.online"));  // it still counts as a heartbeat
  // An event just under the limit is fine.
  CHECK_EQ(send_report(f.env, satellite, report_body(f.env, 2, "ab12", 4900)).num("accepted"), 1.0);
}

TEST(api_satellite_report_rejects_malformed_bodies) {
  Env env;
  env.sign_in();
  const PairedSatellite satellite = add_satellite(env);
  const std::string bad = "{\"time\":1}";
  CHECK_EQ(send_report(env, satellite, bad).status, 400);
  CHECK_EQ(send_report(env, satellite, "not json").status, 400);
  CHECK_EQ(send_report(env, satellite, std::string(9 * 1024, ' ')).status, 413);
}

TEST(api_satellite_refusals_are_logged_at_most_once_a_minute) {
  Env env;
  env.sign_in();
  const PairedSatellite satellite = add_satellite(env);
  for (int i = 0; i < 5; ++i) send_report(env, satellite, "not json");
  CHECK_EQ(count_of(env.log_text(), "refused"), static_cast<size_t>(1));
  env.platform.mono += 61;
  send_report(env, satellite, "not json");
  CHECK_EQ(count_of(env.log_text(), "refused"), static_cast<size_t>(2));
}

TEST(api_satellite_press_heard_by_two_satellites_and_the_scanner_fires_once) {
  DeskFixture f;
  f.add_binding();
  const PairedSatellite porch = add_satellite(f.env, "Porch");
  const PairedSatellite garage = add_satellite(f.env, "Garage");
  CHECK_EQ(send_report(f.env, porch, report_body(f.env, 1)).status, 200);
  CHECK_EQ(send_report(f.env, garage, report_body(f.env, 1, "ff00")).status, 200);
  f.env.press(kPressB2);
  CHECK_EQ(f.env.hits.size(), static_cast<size_t>(1));
  CHECK_EQ(count_of(f.env.log_text(), "fire:"), static_cast<size_t>(1));
}

TEST(api_satellite_report_of_an_unknown_device_is_logged_but_does_not_fire) {
  DeskFixture f;
  f.add_binding();
  const PairedSatellite satellite = add_satellite(f.env);
  send_report(f.env, satellite, report_body(f.env, 1, "ab12", 40, kPressB2, "11:22:33:44:55:66"));
  CHECK(f.env.hits.empty());
  CHECK(contains(f.env.log_text(), "unknown device 11:22:33:44:55:66"));
  CHECK(contains(f.env.log_text(), "(via Porch)"));
}

// ---- satellite side ----

namespace {

Pairing receiver_pairing() {
  Pairing pairing;
  pairing.id = "sat-abc123";
  for (int i = 0; i < 32; ++i) pairing.secret += static_cast<char>(i + 100);
  pairing.host = "receiver.test";
  pairing.port = 8080;
  return pairing;
}

// A signed-in environment set up as a paired satellite.
struct SatelliteFixture {
  Env env;
  Pairing pairing = receiver_pairing();
  int answer_status = 200;
  bool unreachable = false;
  // The "devices" member of the dispatcher's answer; none by default.
  std::string answer_devices;

  SatelliteFixture() {
    env.sign_in();
    CHECK_EQ(env.client.put("/api/device", "{\"role\":\"satellite\"}").status, 200);
    CHECK_EQ(env.client.put("/api/device/pairing", "{\"code\":\"" + format_pairing_code(pairing) + "\"}").status, 200);
    env.platform.http_handler = [this](const test_support::RecordedRequest&) {
      HttpResponse response;
      if (unreachable) {
        response.failure = HttpResponse::Failure::kConnect;
      } else {
        response.status = answer_status;
        const std::string devices = answer_devices.empty() ? "" : ",\"devices\":" + answer_devices;
        response.body = answer_status == 200 ? "{\"ok\":true,\"accepted\":1" + devices + "}" : "{\"error\":\"nope\"}";
      }
      return response;
    };
    env.platform.requests.clear();
  }

  void hear(const char* hex = kPressB2, const char* mac = kDesk) {
    const std::vector<uint8_t> bytes = test_util::from_hex(hex);
    env.app->handle_advert(mac, -40, bytes.data(), bytes.size());
  }
};

}  // namespace

TEST(api_satellite_pairing_is_stored_and_shown_without_the_secret) {
  SatelliteFixture f;
  const Reply device = f.env.client.get("/api/device");
  CHECK_EQ(device.str("role"), std::string("satellite"));
  CHECK_EQ(device.str("pairing.id"), std::string("sat-abc123"));
  CHECK_EQ(device.str("pairing.host"), std::string("receiver.test"));
  CHECK_EQ(device.num("pairing.port"), 8080.0);
  CHECK(!contains(device.body, "secret"));
  CHECK(!contains(device.body, hex_encode(f.pairing.secret)));
  CHECK(contains(f.env.log_text(), "Paired with the dispatcher at receiver.test:8080 as sat-abc123"));
  CHECK(contains(f.env.platform.documents["device.json"], "bdsat1:"));
}

TEST(api_satellite_pairing_rejects_a_bad_code) {
  Env env;
  env.sign_in();
  CHECK_EQ(env.client.put("/api/device/pairing", "{\"code\":\"nonsense\"}").status, 400);
  CHECK_EQ(env.client.put("/api/device/pairing", "{}").status, 400);
  CHECK(env.client.get("/api/device").is_null("pairing"));
}

TEST(api_satellite_pairing_can_be_removed) {
  SatelliteFixture f;
  CHECK_EQ(f.env.client.del("/api/device/pairing").status, 200);
  CHECK(f.env.client.get("/api/device").is_null("pairing"));
  CHECK(contains(f.env.log_text(), "Unpaired from the dispatcher"));
  CHECK(!contains(f.env.platform.documents["device.json"], "bdsat1:"));
}

TEST(api_satellite_forwards_instead_of_firing_locally) {
  SatelliteFixture f;
  // Even with a matching button and binding on the satellite itself.
  f.env.platform.mono += 0;
  f.hear();
  CHECK(f.env.hits.empty());
  CHECK(!contains(f.env.log_text(), "ble:"));
  CHECK_EQ(f.env.app->forwarder.queued(), static_cast<size_t>(1));
}

TEST(api_satellite_forward_once_sends_one_signed_report) {
  SatelliteFixture f;
  f.hear();
  f.hear();  // repeated copies are queued once
  f.hear();
  const double wait = f.env.app->forward_once();
  CHECK_EQ(wait, 0.0);
  CHECK_EQ(f.env.platform.requests.size(), static_cast<size_t>(1));
  const test_support::RecordedRequest& request = f.env.platform.requests[0];
  CHECK_EQ(request.method, std::string("POST"));
  CHECK_EQ(request.url, std::string("http://receiver.test:8080/api/satellite/report"));
  const std::string* id = test_support::find_header(request.headers, "X-Satellite");
  const std::string* signature = test_support::find_header(request.headers, "X-Signature");
  CHECK(id != nullptr);
  CHECK(signature != nullptr);
  if (id != nullptr) CHECK_EQ(*id, std::string("sat-abc123"));
  if (signature != nullptr) CHECK_EQ(*signature, sign(f.pairing.secret, request.body));
  const Parsed<Report> report = report_from_text(request.body);
  CHECK(report.ok());
  CHECK_EQ(report.value->events.size(), static_cast<size_t>(1));
  CHECK_EQ(report.value->events[0].mac, std::string(kDesk));
  CHECK(report.value->events[0].data == test_util::from_hex(kPressB2));
  CHECK_EQ(report.value->time_ms, f.env.platform.wall_ms);
  CHECK(contains(f.env.log_text(), "Reporting to the dispatcher at receiver.test:8080"));
  CHECK_EQ(f.env.app->forwarder.forwarded(), static_cast<int64_t>(1));
  CHECK_EQ(f.env.client.get("/api/device").num("forwarding.forwarded"), 1.0);
  CHECK(f.env.client.get("/api/device").flag("forwarding.connected"));
}

TEST(api_satellite_forward_once_waits_for_the_heartbeat_after_success) {
  SatelliteFixture f;
  CHECK_EQ(f.env.app->forward_once(), 0.0);  // the first report is a heartbeat
  CHECK_EQ(f.env.platform.requests.size(), static_cast<size_t>(1));
  const double wait = f.env.app->forward_once();
  CHECK(wait > 29.0 && wait <= 30.0);
  CHECK_EQ(f.env.platform.requests.size(), static_cast<size_t>(1));
}

TEST(api_satellite_cannot_report_is_logged_once_not_on_every_retry) {
  SatelliteFixture f;
  f.unreachable = true;
  f.hear();
  f.env.app->forward_once();
  for (int i = 0; i < 4; ++i) {
    f.env.platform.mono += 1.1;
    f.env.app->forward_once();
  }
  CHECK_EQ(f.env.platform.requests.size(), static_cast<size_t>(5));
  CHECK_EQ(count_of(f.env.log_text(), "Cannot report to the dispatcher"), static_cast<size_t>(1));
  CHECK(contains(f.env.log_text(), "cannot reach receiver.test:8080"));
  const Reply device = f.env.client.get("/api/device");
  CHECK(!device.flag("forwarding.connected"));
  CHECK(contains(device.str("forwarding.last_error"), "receiver.test"));

  // Recovery is announced, and a later failure is reported again.
  f.unreachable = false;
  f.env.platform.mono += 5.1;
  f.env.app->forward_once();
  CHECK(contains(f.env.log_text(), "Reporting to the dispatcher at receiver.test:8080"));
  f.unreachable = true;
  f.env.platform.mono += 31;
  f.env.app->forward_once();
  CHECK_EQ(count_of(f.env.log_text(), "Cannot report to the dispatcher"), static_cast<size_t>(2));
}

TEST(api_satellite_http_errors_from_the_dispatcher_are_reported) {
  SatelliteFixture f;
  f.answer_status = 401;
  f.env.app->forward_once();
  CHECK(contains(f.env.log_text(), "answered HTTP 401: nope"));
}

TEST(api_satellite_report_answer_lists_the_dispatchers_devices) {
  DeskFixture f;
  const PairedSatellite satellite = add_satellite(f.env);
  const Reply reply = send_report(f.env, satellite, report_body(f.env, 1));
  CHECK_EQ(reply.status, 200);
  CHECK_EQ(reply.size("devices"), static_cast<size_t>(1));
  CHECK_EQ(reply.str("devices.0"), std::string(kDesk));
}

// The satellite's buzzer, for a press of a device the dispatcher listed.
struct BeepingSatellite : SatelliteFixture {
  BeepingSatellite() {
    answer_devices = std::string("[\"") + kDesk + "\"]";
    env.app->forward_once();  // the first report, a heartbeat, brings the list
    env.platform.beeps.clear();
  }
};

TEST(api_satellite_beeps_when_it_hears_a_known_press_and_when_it_is_delivered) {
  BeepingSatellite f;
  f.hear();
  CHECK_EQ(f.env.platform.beeps.size(), static_cast<size_t>(1));
  CHECK(f.env.platform.beeps[0] == std::make_pair(Beep::kShort, 1));
  f.env.app->forward_once();
  CHECK_EQ(f.env.platform.beeps.size(), static_cast<size_t>(2));
  CHECK(f.env.platform.beeps[1] == std::make_pair(Beep::kShort, 1));
}

TEST(api_satellite_is_silent_for_devices_the_dispatcher_does_not_know) {
  BeepingSatellite f;
  f.hear(kPressB2, "AA:BB:CC:DD:EE:99");
  f.env.app->forward_once();
  CHECK(f.env.platform.beeps.empty());
  CHECK_EQ(f.env.app->forwarder.forwarded(), static_cast<int64_t>(1));  // still forwarded
}

TEST(api_satellite_sounds_the_failure_beeps_when_a_known_press_cannot_be_delivered) {
  BeepingSatellite f;
  f.unreachable = true;
  f.hear();
  f.env.app->forward_once();
  CHECK_EQ(f.env.platform.beeps.size(), static_cast<size_t>(1));  // only the acknowledgement so far
  for (int i = 0; i < 6; ++i) {
    f.env.platform.mono += 1.1;
    f.env.app->forward_once();
  }
  CHECK_EQ(f.env.app->forwarder.queued(), static_cast<size_t>(0));
  CHECK_EQ(f.env.platform.beeps.size(), static_cast<size_t>(2));
  CHECK(f.env.platform.beeps[1] == std::make_pair(Beep::kLong, 3));
}

TEST(api_satellite_a_retry_that_gets_through_beeps_success) {
  BeepingSatellite f;
  f.unreachable = true;
  f.hear();
  f.env.app->forward_once();
  f.unreachable = false;
  f.env.platform.mono += 1.1;
  f.env.app->forward_once();
  CHECK_EQ(f.env.platform.beeps.size(), static_cast<size_t>(2));
  CHECK(f.env.platform.beeps[1] == std::make_pair(Beep::kShort, 1));
}

TEST(api_satellite_forward_once_idles_when_it_is_not_a_paired_satellite) {
  Env env;
  env.sign_in();
  CHECK_EQ(env.app->forward_once(), AppCore::kForwardIdleWaitS);  // a dispatcher
  env.client.put("/api/device", "{\"role\":\"satellite\"}");
  CHECK_EQ(env.app->forward_once(), AppCore::kForwardIdleWaitS);  // not paired yet
  CHECK(env.platform.requests.empty());
}

TEST(api_satellite_sends_nothing_while_the_clock_is_not_set) {
  SatelliteFixture f;
  f.env.platform.clock_valid = false;
  f.hear();
  CHECK_EQ(f.env.app->forward_once(), Forwarder::kRetryS);
  CHECK(f.env.platform.requests.empty());
  // A press that waited too long for the clock is dropped rather than sent late.
  f.env.platform.mono += 10;
  f.env.app->forward_once();
  CHECK_EQ(f.env.app->forwarder.queued(), static_cast<size_t>(0));
  f.env.platform.clock_valid = true;
  f.env.app->forward_once();
  const Parsed<Report> report = report_from_text(f.env.platform.requests.at(0).body);
  CHECK(report.ok());
  CHECK(report.value->events.empty());
}

TEST(api_satellite_changing_the_role_back_stops_forwarding) {
  SatelliteFixture f;
  f.hear();
  f.env.client.put("/api/device", "{\"role\":\"dispatcher\"}");
  CHECK_EQ(f.env.app->forwarder.queued(), static_cast<size_t>(0));
  CHECK_EQ(f.env.app->forward_once(), AppCore::kForwardIdleWaitS);
  // Presses are handled locally again.
  f.hear(kPressB1);
  CHECK(contains(f.env.log_text(), "ble:"));
}

TEST(api_satellite_on_forward_queued_is_called_for_a_queued_press) {
  SatelliteFixture f;
  int woken = 0;
  f.env.app->on_forward_queued = [&woken] { ++woken; };
  f.hear();
  CHECK_EQ(woken, 1);
  f.hear();  // a repeat is not queued, so nothing to wake for
  CHECK_EQ(woken, 1);
}

// A satellite board and a dispatcher board wired together: the satellite's HTTP requests are answered by
// the dispatcher's Api.
TEST(api_end_to_end_a_press_heard_by_a_satellite_fires_the_dispatchers_binding) {
  DeskFixture dispatcher;
  dispatcher.add_binding();
  const PairedSatellite paired = add_satellite(dispatcher.env, "Porch");

  Env satellite;
  satellite.sign_in();
  satellite.client.put("/api/device", "{\"role\":\"satellite\"}");
  CHECK_EQ(satellite.client.put("/api/device/pairing", "{\"code\":\"" + paired.code + "\"}").status, 200);
  std::string requested_url;
  satellite.platform.http_handler = [&](const test_support::RecordedRequest& recorded) {
    requested_url = recorded.url;
    Request request;
    request.method = recorded.method;
    request.path = "/api/satellite/report";
    request.body = recorded.body;
    request.client = "10.0.0.9";
    if (const std::string* id = test_support::find_header(recorded.headers, "X-Satellite")) request.satellite = *id;
    if (const std::string* sig = test_support::find_header(recorded.headers, "X-Signature")) request.signature = *sig;
    const Response response = dispatcher.env.api->handle(request);
    HttpResponse answer;
    answer.status = response.status;
    answer.body = response.body;
    return answer;
  };

  const std::vector<uint8_t> press = test_util::from_hex(kPressB2);
  satellite.app->handle_advert(kDesk, -40, press.data(), press.size());
  CHECK(dispatcher.env.hits.empty());
  satellite.platform.mono += 0.05;
  CHECK_EQ(satellite.app->forward_once(), 0.0);

  CHECK_EQ(requested_url, std::string("http://dispatcher-24-0a-c4-12-34-56.local:80/api/satellite/report"));
  CHECK_EQ(dispatcher.env.hits.size(), static_cast<size_t>(1));
  CHECK(contains(dispatcher.env.log_text(), "(via Porch)"));
  CHECK(contains(satellite.log_text(), "Reporting to the dispatcher"));
  CHECK_EQ(satellite.app->forwarder.forwarded(), static_cast<int64_t>(1));
  CHECK(dispatcher.env.client.get("/api/satellites").flag("satellites.0.online"));

  // A second press goes the same way; a heartbeat in between does not fire anything.
  satellite.platform.mono += 40;
  dispatcher.env.platform.mono += 40;
  satellite.app->forward_once();
  CHECK_EQ(dispatcher.env.hits.size(), static_cast<size_t>(1));
  const std::vector<uint8_t> again = test_util::from_hex(kPressB2Again);
  satellite.app->handle_advert(kDesk, -40, again.data(), again.size());
  satellite.app->forward_once();
  CHECK_EQ(dispatcher.env.hits.size(), static_cast<size_t>(2));
}

TEST(api_satellite_report_is_refused_by_a_board_that_is_a_satellite) {
  DeskFixture f;
  f.add_binding();
  const PairedSatellite paired = add_satellite(f.env);
  CHECK_EQ(f.env.client.put("/api/device", "{\"role\":\"satellite\"}").status, 200);
  const Reply refused = send_report(f.env, paired, report_body(f.env, 1));
  CHECK_EQ(refused.status, 409);
  CHECK(contains(refused.error(), "satellite, not a dispatcher"));
  // Refused before the id lookup, so an unknown id gets the same answer.
  CHECK_EQ(send_report(f.env, paired, report_body(f.env, 1), "", "sat-unknown").status, 409);
  CHECK(f.env.hits.empty());
  // Back to a dispatcher, the same report is accepted.
  f.env.client.put("/api/device", "{\"role\":\"dispatcher\"}");
  CHECK_EQ(send_report(f.env, paired, report_body(f.env, 1)).status, 200);
  CHECK_EQ(f.env.hits.size(), static_cast<size_t>(1));
}
