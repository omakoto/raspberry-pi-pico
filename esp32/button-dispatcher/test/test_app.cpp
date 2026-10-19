// Tests for app.cpp (AppCore): loading and saving the config, the Bluetooth entry point, and the actions.
// The SwitchBot actions are covered by the API tests, which have a scripted SwitchBot server.

#include <string>
#include <vector>

#include "dispatcher/app.hpp"
#include "fake_platform.hpp"
#include "minitest.hpp"
#include "test_util.hpp"

using namespace dispatcher;

namespace {

const char* const kDesk = "AA:BB:CC:DD:EE:01";
const char* const kPressB2 = "44002101643a003a013a003a00";    // id 33, button 2 press (four-button remote)
const char* const kPressB2Again = "44002201643a003a013a003a00";  // id 34
const char* const kPressB1 = "44002301643a013a003a003a00";    // id 35, button 1 press

constexpr int kFastIterations = 2;

Binding http_binding(const std::string& id, const std::string& url, const std::string& input = "button:2",
                     const std::string& event = "press", int cooldown_ms = 0) {
  Binding binding;
  binding.id = id;
  binding.name = "Hit " + id;
  binding.cooldown_ms = cooldown_ms;
  binding.trigger = {"desk", input, event, std::nullopt};
  HttpAction http;
  http.url = url;
  binding.action = http;
  return binding;
}

struct Fixture {
  test_support::FakePlatform platform;
  AppCore app{platform, kFastIterations};

  Fixture() {
    // Answer every request with 200 OK by default.
    platform.http_handler = [](const test_support::RecordedRequest&) {
      HttpResponse response;
      response.status = 200;
      response.reason = "OK";
      return response;
    };
  }

  // Puts a button and some bindings into the config the way the API would.
  void configure(std::vector<Binding> bindings) {
    std::lock_guard<std::mutex> lock(app.mutex);
    app.config.buttons = {{"desk", "Desk remote", kDesk, "SBBT-104CUS"}};
    app.config.bindings = std::move(bindings);
    std::string error;
    CHECK(app.commit_config(&error));
  }

  void press(const char* hex, const char* mac = kDesk) {
    const std::vector<uint8_t> bytes = test_util::from_hex(hex);
    app.handle_packet(mac, -40, bytes.data(), bytes.size());
  }

  std::vector<LogEntry> entries() {
    std::lock_guard<std::mutex> lock(app.mutex);
    return app.log.since(0);
  }

  std::string log_text() {
    std::string text;
    for (const LogEntry& e : entries()) text += e.kind + ": " + e.text + "\n";
    return text;
  }
};

bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

}  // namespace

// ---- loading and saving ----

TEST(app_starts_empty_without_stored_documents) {
  Fixture f;
  f.app.init();
  CHECK(f.app.config.buttons.empty());
  CHECK(f.app.config.bindings.empty());
  CHECK(!f.app.auth.is_configured());
}

TEST(app_commit_saves_a_document_that_a_restart_loads) {
  Fixture f;
  f.app.init();
  f.configure({http_binding("wake", "http://wol.local/wake")});
  f.app.config.switchbot_token = "tok";
  f.app.config.switchbot_secret = "sec";
  {
    std::lock_guard<std::mutex> lock(f.app.mutex);
    CHECK(f.app.commit_config(nullptr));
  }
  CHECK(contains(f.platform.documents["config.json"], "\"wake\""));

  AppCore restarted(f.platform, kFastIterations);
  std::string error;
  restarted.init(&error);
  CHECK_EQ(error, std::string(""));
  CHECK_EQ(restarted.config.bindings.size(), static_cast<size_t>(1));
  CHECK_EQ(restarted.config.buttons[0].mac, std::string(kDesk));
  CHECK_EQ(restarted.config.switchbot_token, std::string("tok"));
}

TEST(app_invalid_stored_config_starts_empty_and_reports_why) {
  Fixture f;
  f.platform.documents["config.json"] = "{\"version\": 99}";
  std::string error;
  f.app.init(&error);
  CHECK(contains(error, "config.json is invalid"));
  CHECK(contains(error, "unsupported config version"));
  CHECK(f.app.config.buttons.empty());
  // The bad document is not overwritten until something is saved.
  CHECK_EQ(f.platform.documents["config.json"], std::string("{\"version\": 99}"));
}

TEST(app_commit_reports_a_failed_save_but_keeps_the_engine_current) {
  Fixture f;
  f.app.init();
  f.platform.fail_saves = true;
  f.app.config.buttons = {{"desk", "Desk remote", kDesk, ""}};
  f.app.config.bindings = {http_binding("wake", "http://x.invalid/")};
  std::string error;
  {
    std::lock_guard<std::mutex> lock(f.app.mutex);
    CHECK(!f.app.commit_config(&error));
  }
  CHECK_EQ(error, std::string("the configuration could not be saved"));
  f.press(kPressB2);
  CHECK_EQ(f.platform.requests.size(), static_cast<size_t>(1));  // the unsaved binding still works
}

// ---- Bluetooth entry point ----

TEST(app_a_press_runs_the_action_and_is_logged) {
  Fixture f;
  f.app.init();
  f.configure({http_binding("wake", "http://wol.local/wake")});
  f.press(kPressB2);
  CHECK_EQ(f.platform.requests.size(), static_cast<size_t>(1));
  CHECK_EQ(f.platform.requests[0].method, std::string("GET"));
  CHECK_EQ(f.platform.requests[0].url, std::string("http://wol.local/wake"));
  const std::string log = f.log_text();
  CHECK(contains(log, "ble: Desk remote: button 2, single press"));
  CHECK(contains(log, "fire: Binding \"Hit wake\" fired"));
  CHECK(contains(log, "action: \"Hit wake\": GET http://wol.local: HTTP 200 OK"));
}

TEST(app_actions_are_queued_when_the_platform_defers_them) {
  Fixture f;
  f.platform.run_background_immediately = false;
  f.app.init();
  f.configure({http_binding("wake", "http://wol.local/wake")});
  f.press(kPressB2);
  CHECK(f.platform.requests.empty());  // nothing has run yet: the packet handler only queues
  CHECK_EQ(f.platform.background.size(), static_cast<size_t>(1));
  f.platform.run_queued_background();
  CHECK_EQ(f.platform.requests.size(), static_cast<size_t>(1));
}

TEST(app_repeated_copies_of_a_press_run_the_action_once) {
  Fixture f;
  f.app.init();
  f.configure({http_binding("wake", "http://wol.local/wake")});
  for (int i = 0; i < 25; ++i) f.press(kPressB2);
  CHECK_EQ(f.platform.requests.size(), static_cast<size_t>(1));
}

TEST(app_other_buttons_and_unknown_devices_do_not_fire_but_are_logged) {
  Fixture f;
  f.app.init();
  f.configure({http_binding("wake", "http://wol.local/wake")});
  f.press(kPressB1);
  f.press(kPressB2, "11:22:33:44:55:66");
  CHECK(f.platform.requests.empty());
  CHECK(contains(f.log_text(), "unknown device 11:22:33:44:55:66"));
}

TEST(app_unknown_device_log_includes_its_advertised_name) {
  Fixture f;
  f.app.init();
  f.configure({});
  f.app.names["11:22:33:44:55:66"] = "SBBT-104CUS";
  f.press(kPressB2, "11:22:33:44:55:66");
  CHECK(contains(f.log_text(), "unknown device 11:22:33:44:55:66 (SBBT-104CUS)"));
}

TEST(app_decode_problems_of_a_known_device_are_logged_once_a_minute) {
  Fixture f;
  f.app.init();
  f.configure({});
  f.press("45aabbcc");  // encrypted
  f.platform.mono += 5;
  f.press("45aabbcd");
  CHECK_EQ(f.entries().size(), static_cast<size_t>(1));
  CHECK(contains(f.log_text(), "error: Desk remote: encrypted payload (unsupported)"));
  f.platform.mono += AppCore::kErrorLogIntervalS;
  f.press("45aabbce");
  CHECK_EQ(f.entries().size(), static_cast<size_t>(2));
  // Problems of unknown devices are not logged at all.
  f.press("45aabbcc", "11:22:33:44:55:66");
  CHECK_EQ(f.entries().size(), static_cast<size_t>(2));
}

TEST(app_presses_feed_the_learn_flow) {
  Fixture f;
  f.app.init();
  f.configure({});
  {
    std::lock_guard<std::mutex> lock(f.app.mutex);
    f.app.learn.start();
  }
  f.press(kPressB2);
  f.platform.mono += 1;
  f.press(kPressB2Again);
  std::lock_guard<std::mutex> lock(f.app.mutex);
  CHECK_EQ(f.app.learn.status().phase, std::string("confirmed"));
}

TEST(app_a_failing_action_is_logged_as_an_error) {
  Fixture f;
  f.app.init();
  f.platform.http_handler = [](const test_support::RecordedRequest&) {
    HttpResponse response;
    response.failure = HttpResponse::Failure::kConnect;
    return response;
  };
  f.configure({http_binding("wake", "http://127.0.0.1:1/x")});
  f.press(kPressB2);
  CHECK(contains(f.log_text(), "error: \"Hit wake\": GET http://127.0.0.1:1: connection failed"));
}

// ---- actions ----

TEST(app_http_action_sends_method_headers_and_body) {
  Fixture f;
  f.app.init();
  HttpAction http;
  http.method = "POST";
  http.url = "http://h/p?x=1";
  http.headers = {{"X-Token", "abc"}};
  http.body = "{\"on\": true}";
  http.timeout_s = 5.0;
  const ActionResult result = f.app.run_action(http);
  CHECK(result.ok);
  CHECK_EQ(result.summary, std::string("POST http://h: HTTP 200 OK"));
  const auto& request = f.platform.requests[0];
  CHECK_EQ(request.method, std::string("POST"));
  CHECK_EQ(request.body, std::string("{\"on\": true}"));
  CHECK_EQ(request.timeout_s, 5.0);
  CHECK_EQ(*test_support::find_header(request.headers, "X-Token"), std::string("abc"));
  CHECK_EQ(*test_support::find_header(request.headers, "content-type"), std::string("application/json"));
}

TEST(app_content_type_defaults_and_overrides) {
  Fixture f;
  f.app.init();
  HttpAction text;
  text.method = "PUT";
  text.url = "http://h/";
  text.body = "hello";
  f.app.run_action(text);
  CHECK(contains(*test_support::find_header(f.platform.requests[0].headers, "Content-Type"), "text/plain"));

  HttpAction custom;
  custom.method = "POST";
  custom.url = "http://h/";
  custom.headers = {{"content-type", "text/x-custom"}};
  custom.body = "{}";
  f.app.run_action(custom);
  CHECK_EQ(*test_support::find_header(f.platform.requests[1].headers, "Content-Type"), std::string("text/x-custom"));
  CHECK_EQ(f.platform.requests[1].headers.size(), static_cast<size_t>(1));  // no second Content-Type added

  HttpAction no_body;
  no_body.url = "http://h/";
  f.app.run_action(no_body);
  CHECK(test_support::find_header(f.platform.requests[2].headers, "Content-Type") == nullptr);
}

TEST(app_error_statuses_are_failures) {
  Fixture f;
  f.app.init();
  f.platform.http_handler = [](const test_support::RecordedRequest&) {
    HttpResponse response;
    response.status = 404;
    response.reason = "Not Found";
    return response;
  };
  HttpAction http;
  http.url = "http://h/missing";
  const ActionResult result = f.app.run_action(http);
  CHECK(!result.ok);
  CHECK_EQ(result.summary, std::string("GET http://h: HTTP 404 Not Found"));
}

TEST(app_timeouts_use_the_action_setting_then_the_config_default) {
  Fixture f;
  f.app.init();
  f.platform.http_handler = [](const test_support::RecordedRequest&) {
    HttpResponse response;
    response.failure = HttpResponse::Failure::kTimeout;
    return response;
  };
  f.app.config.settings.http_timeout_s = 2.5;
  HttpAction http;
  http.url = "http://h/slow";
  ActionResult result = f.app.run_action(http);
  CHECK(!result.ok);
  CHECK_EQ(result.summary, std::string("GET http://h: timed out after 2.5s"));
  CHECK_EQ(f.platform.requests[0].timeout_s, 2.5);

  http.timeout_s = 10.0;
  result = f.app.run_action(http);
  CHECK_EQ(result.summary, std::string("GET http://h: timed out after 10s"));
  CHECK_EQ(f.platform.requests[1].timeout_s, 10.0);
}

TEST(app_secret_url_parts_never_appear_in_results_or_the_log) {
  Fixture f;
  f.app.init();
  f.platform.http_handler = [](const test_support::RecordedRequest&) {
    HttpResponse response;
    response.failure = HttpResponse::Failure::kConnect;
    return response;
  };
  HttpAction http;
  http.url = "http://user:pw@127.0.0.1:1/trigger/event/with/key/SUPERSECRETKEY?token=SUPERSECRETKEY";
  const ActionResult result = f.app.run_action(http);
  CHECK(!contains(result.summary, "SUPERSECRETKEY"));
  CHECK(!contains(result.summary, "pw"));
  f.configure({http_binding("secret", http.url)});
  f.press(kPressB2);
  CHECK(!contains(f.log_text(), "SUPERSECRETKEY"));
}

TEST(app_run_binding_logs_and_returns_the_result) {
  Fixture f;
  f.app.init();
  const Binding binding = http_binding("wake", "http://wol.local/wake");
  const ActionResult result = f.app.run_binding(binding);
  CHECK(result.ok);
  CHECK(contains(f.log_text(), "action: \"Hit wake\": GET http://wol.local: HTTP 200 OK"));
}

TEST(app_switchbot_actions_without_credentials_fail_clearly) {
  Fixture f;
  f.app.init();
  SwitchBotSceneAction scene;
  scene.scene_id = "S1";
  const ActionResult result = f.app.run_action(scene);
  CHECK(!result.ok);
  CHECK(contains(result.summary, "SwitchBot: SwitchBot token and secret are not set"));
}

// ---- time ----

TEST(app_wall_time_of_an_event_follows_the_clock_when_it_is_set_later) {
  Fixture f;
  f.app.init();
  f.platform.mono = 100.0;
  f.platform.wall_ms = 5000;  // the clock had not been set: it counts from 1970
  const double logged_at = f.platform.mono;
  f.platform.mono = 160.0;
  f.platform.wall_ms = 1700000060000LL;  // set by NTP: 60 s after the event
  CHECK_EQ(f.app.wall_time_of(logged_at), 1700000000.0);
}
