// The web API: request routing, the CSRF and session checks, and one small handler per route.
//
// The handlers answer with the same JSON shapes and messages the web page was written against. Locking
// follows app.hpp: a handler holds app.mutex while it reads or changes state and releases it before anything
// that may block on the network or that locks internally (running actions, the SwitchBot service).
//
// Run test/run.sh (which builds test_api.cpp) every time this file is touched.

#include "dispatcher/api.hpp"

#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "cJSON.h"
#include "dispatcher/bthome.hpp"
#include "dispatcher/config.hpp"
#include "dispatcher/learn.hpp"

namespace dispatcher {

namespace {

// Larger request bodies are refused unread: every body this API takes is a small JSON document, and a
// device with a few hundred KiB of RAM cannot afford to parse an arbitrary one.
constexpr size_t kMaxBodyBytes = 64 * 1024;

struct Context {
  AppCore& app;
  const Request& request;
  std::string id;  // the {id} part of the path, if the route has one
};

using Handler = Response (*)(Context&);

struct Route {
  const char* method;
  const char* pattern;  // "{id}" matches any one non-empty path segment
  Handler handler;
  bool slow;  // calls out to the network
};

// ---- response helpers ----------------------------------------------------------------------------

std::string print(const cJSON* json) {
  char* text = cJSON_PrintUnformatted(json);
  std::string out = text != nullptr ? text : "";
  cJSON_free(text);
  return out;
}

Response reply(int status, const JsonPtr& json) {
  Response response;
  response.status = status;
  response.body = print(json.get());
  return response;
}

Response fail(int status, const std::string& message) {
  JsonPtr json = make_json(cJSON_CreateObject());
  cJSON_AddStringToObject(json.get(), "error", message.c_str());
  return reply(status, json);
}

Response ok_reply() {
  JsonPtr json = make_json(cJSON_CreateObject());
  cJSON_AddTrueToObject(json.get(), "ok");
  return reply(200, json);
}

Response configured_reply(bool configured) {
  JsonPtr json = make_json(cJSON_CreateObject());
  cJSON_AddBoolToObject(json.get(), "configured", configured);
  return reply(200, json);
}

Response save_failed(const std::string& error) { return fail(500, error); }

std::string session_cookie(const std::string& token) {
  return std::string(kSessionCookieName) + "=" + token + "; Max-Age=" +
         std::to_string(static_cast<long long>(kSessionTtlS)) + "; Path=/; HttpOnly; SameSite=Strict";
}

// Tells the browser to forget the session cookie.
std::string expired_session_cookie() { return std::string(kSessionCookieName) + "=; Max-Age=0; Path=/"; }

Response with_session(Response response, const std::string& token) {
  response.headers.emplace_back("Set-Cookie", session_cookie(token));
  return response;
}

// ---- request helpers -----------------------------------------------------------------------------

std::string trim(const std::string& text) {
  const char* space = " \t\r\n\f\v";
  const size_t first = text.find_first_not_of(space);
  if (first == std::string::npos) return "";
  return text.substr(first, text.find_last_not_of(space) - first + 1);
}

// The value of cookie `name` in a Cookie header such as "a=1; bd_session=xyz".
std::string cookie_value(const std::string& header, const std::string& name) {
  size_t pos = 0;
  while (pos <= header.size()) {
    size_t end = header.find(';', pos);
    if (end == std::string::npos) end = header.size();
    const std::string pair = trim(header.substr(pos, end - pos));
    const size_t eq = pair.find('=');
    if (eq != std::string::npos && pair.compare(0, eq, name) == 0) return pair.substr(eq + 1);
    pos = end + 1;
  }
  return "";
}

std::string query_param(const Request& request, const std::string& name) {
  auto it = request.query.find(name);
  return it == request.query.end() ? "" : it->second;
}

const cJSON* field(const cJSON* object, const char* key) { return cJSON_GetObjectItemCaseSensitive(object, key); }

// A body field as text the way a lenient reader would take it: missing or null is empty, text is itself and
// anything else is its JSON form.
std::string text_of(const cJSON* item) {
  if (item == nullptr || cJSON_IsNull(item)) return "";
  if (cJSON_IsString(item)) return item->valuestring;
  return print(item);
}

// Parses the request body, which must be a JSON object. On failure `*error` is the response to send.
JsonPtr read_object(const Request& request, Response* error) {
  if (request.body.size() > kMaxBodyBytes) {
    *error = fail(413, "request too large");
    return make_json();
  }
  JsonPtr body = make_json(cJSON_ParseWithOpts(request.body.c_str(), nullptr, true));
  if (!body) {
    *error = fail(400, "the request body is not valid JSON");
  } else if (!cJSON_IsObject(body.get())) {
    *error = fail(400, "the request body must be a JSON object");
    body.reset();
  }
  return body;
}

// Replaces (or adds) a string member.
void set_string(cJSON* object, const char* key, const std::string& value) {
  cJSON_DeleteItemFromObject(object, key);
  cJSON_AddStringToObject(object, key, value.c_str());
}

uint32_t random_u32(AppCore& app) {
  uint8_t bytes[4] = {0, 0, 0, 0};
  app.platform().random_bytes(bytes, sizeof(bytes));
  return static_cast<uint32_t>(bytes[0]) | static_cast<uint32_t>(bytes[1]) << 8 |
         static_cast<uint32_t>(bytes[2]) << 16 | static_cast<uint32_t>(bytes[3]) << 24;
}

// An id nobody uses yet; the random part is only 24 bits, so a clash is rare but possible.
template <typename Exists>
std::string fresh_id(AppCore& app, const std::string& prefix, Exists exists) {
  std::string id = new_id(prefix, random_u32(app));
  while (exists(id)) id = new_id(prefix, random_u32(app));
  return id;
}

std::vector<std::string> button_ids(const Config& config) {
  std::vector<std::string> ids;
  for (const Button& b : config.buttons) ids.push_back(b.id);
  return ids;
}

const Button* button_for_mac(const Config& config, const std::string& mac) {
  for (const Button& b : config.buttons) {
    if (b.mac == mac) return &b;
  }
  return nullptr;
}

int find_binding_index(const Config& config, const std::string& id) {
  for (size_t i = 0; i < config.bindings.size(); ++i) {
    if (config.bindings[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

// Moves one member of `from` into `to`.
void move_member(cJSON* from, cJSON* to, const char* key) {
  cJSON* item = cJSON_DetachItemFromObject(from, key);
  if (item != nullptr) cJSON_AddItemToObject(to, key, item);
}

// ---- auth routes ---------------------------------------------------------------------------------

bool session_valid(AppCore& app, const Request& request) {
  const std::string token = cookie_value(request.cookie, kSessionCookieName);
  std::lock_guard<std::mutex> lock(app.mutex);
  return app.auth.verify_token(token);
}

Response auth_status(Context& c) {
  const bool authenticated = session_valid(c.app, c.request);
  std::lock_guard<std::mutex> lock(c.app.mutex);
  JsonPtr json = make_json(cJSON_CreateObject());
  cJSON_AddBoolToObject(json.get(), "configured", c.app.auth.is_configured());
  cJSON_AddBoolToObject(json.get(), "authenticated", authenticated);
  return reply(200, json);
}

Response auth_setup(Context& c) {
  std::lock_guard<std::mutex> lock(c.app.mutex);
  if (c.app.auth.is_configured()) return fail(409, "a password is already set");
  Response error;
  JsonPtr body = read_object(c.request, &error);
  if (!body) return error;
  const std::string problem = c.app.auth.set_password(text_of(field(body.get(), "password")));
  if (!problem.empty()) return fail(400, problem);
  c.app.log.add("info", "Password set");
  return with_session(ok_reply(), c.app.auth.make_token());
}

Response auth_login(Context& c) {
  const std::string client = c.request.client.empty() ? "?" : c.request.client;
  std::lock_guard<std::mutex> lock(c.app.mutex);
  const double wait = c.app.limiter.retry_after(client);
  if (wait > 0) {
    return fail(429, "too many failed attempts; try again in " + std::to_string(static_cast<int>(wait) + 1) +
                         " seconds");
  }
  Response error;
  JsonPtr body = read_object(c.request, &error);
  if (!body) return error;
  if (!c.app.auth.verify_password(text_of(field(body.get(), "password")))) {
    c.app.limiter.record_failure(client);
    c.app.log.add("error", "Failed sign-in from " + client);
    return fail(401, "wrong password");
  }
  c.app.limiter.record_success(client);
  return with_session(ok_reply(), c.app.auth.make_token());
}

Response auth_logout(Context&) {
  Response response = ok_reply();
  response.headers.emplace_back("Set-Cookie", expired_session_cookie());
  return response;
}

Response auth_password(Context& c) {
  std::lock_guard<std::mutex> lock(c.app.mutex);
  Response error;
  JsonPtr body = read_object(c.request, &error);
  if (!body) return error;
  if (!c.app.auth.verify_password(text_of(field(body.get(), "current")))) {
    return fail(403, "the current password is wrong");
  }
  const std::string problem = c.app.auth.set_password(text_of(field(body.get(), "new")));
  if (!problem.empty()) return fail(400, problem);
  c.app.log.add("info", "Password changed");
  // Changing the password ended every session, including this one, so the page gets a fresh cookie.
  return with_session(ok_reply(), c.app.auth.make_token());
}

// ---- state, buttons, bindings ----------------------------------------------------------------------

JsonPtr choices_json() {
  JsonPtr json = make_json(cJSON_CreateObject());
  cJSON* events = cJSON_AddArrayToObject(json.get(), "button_events");
  for (const std::string& event : bthome::all_button_events()) {
    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "value", event.c_str());
    cJSON_AddStringToObject(item, "label", event_label(event).c_str());
    cJSON_AddItemToArray(events, item);
  }
  cJSON* wheel = cJSON_AddArrayToObject(json.get(), "wheel_events");
  cJSON* turn = cJSON_CreateObject();
  cJSON_AddStringToObject(turn, "value", bthome::kWheelEvent);
  cJSON_AddStringToObject(turn, "label", "turn");
  cJSON_AddItemToArray(wheel, turn);
  cJSON* methods = cJSON_AddArrayToObject(json.get(), "http_methods");
  for (const char* method : {"GET", "POST", "PUT", "DELETE"}) cJSON_AddItemToArray(methods, cJSON_CreateString(method));
  return json;
}

Response get_state(Context& c) {
  JsonPtr json = make_json(cJSON_CreateObject());
  {
    std::lock_guard<std::mutex> lock(c.app.mutex);
    // The config tree already has the buttons, bindings and settings in the shapes the page uses.
    JsonPtr config = config_to_json(c.app.config, false);
    move_member(config.get(), json.get(), "buttons");
    move_member(config.get(), json.get(), "bindings");
    move_member(config.get(), json.get(), "settings");
    cJSON* switchbot = cJSON_AddObjectToObject(json.get(), "switchbot");
    cJSON_AddBoolToObject(switchbot, "configured", c.app.config.has_switchbot_credentials());
  }
  cJSON_AddItemToObject(json.get(), "choices", choices_json().release());
  return reply(200, json);
}

Response put_button(Context& c) {
  std::lock_guard<std::mutex> lock(c.app.mutex);
  Config& config = c.app.config;
  Button* button = nullptr;
  for (Button& b : config.buttons) {
    if (b.id == c.id) button = &b;
  }
  if (button == nullptr) return fail(404, "no such button");
  Response error;
  JsonPtr body = read_object(c.request, &error);
  if (!body) return error;
  // Only the name can be edited; the other fields are re-validated together with it.
  JsonPtr candidate = make_json(cJSON_CreateObject());
  cJSON_AddStringToObject(candidate.get(), "id", button->id.c_str());
  const cJSON* name = field(body.get(), "name");
  if (name != nullptr) cJSON_AddItemToObject(candidate.get(), "name", cJSON_Duplicate(name, true));
  cJSON_AddStringToObject(candidate.get(), "mac", button->mac.c_str());
  cJSON_AddStringToObject(candidate.get(), "model", button->model.c_str());
  Parsed<Button> updated = parse_button(candidate.get());
  if (!updated.ok()) return fail(400, updated.error);
  button->name = updated.value->name;
  std::string save_error;
  if (!c.app.commit_config(&save_error)) return save_failed(save_error);
  return reply(200, button_to_json(*button));
}

Response delete_button(Context& c) {
  std::lock_guard<std::mutex> lock(c.app.mutex);
  Config& config = c.app.config;
  if (config.find_button(c.id) == nullptr) return fail(404, "no such button");
  size_t users = 0;
  for (const Binding& b : config.bindings) {
    if (b.trigger.button == c.id) ++users;
  }
  if (users > 0 && query_param(c.request, "cascade") != "1") {
    return fail(409, std::to_string(users) + " binding(s) use this button; delete them too, or retry with cascade=1");
  }
  std::vector<Binding> kept_bindings;
  for (Binding& b : config.bindings) {
    if (b.trigger.button != c.id) kept_bindings.push_back(std::move(b));
  }
  config.bindings = std::move(kept_bindings);
  std::vector<Button> kept_buttons;
  for (Button& b : config.buttons) {
    if (b.id != c.id) kept_buttons.push_back(std::move(b));
  }
  config.buttons = std::move(kept_buttons);
  std::string save_error;
  if (!c.app.commit_config(&save_error)) return save_failed(save_error);
  return ok_reply();
}

Response post_binding(Context& c) {
  std::lock_guard<std::mutex> lock(c.app.mutex);
  Response error;
  JsonPtr body = read_object(c.request, &error);
  if (!body) return error;
  Config& config = c.app.config;
  set_string(body.get(), "id", fresh_id(c.app, "bind", [&](const std::string& id) {
    return config.find_binding(id) != nullptr;
  }));
  Parsed<Binding> binding = parse_binding(body.get(), button_ids(config));
  if (!binding.ok()) return fail(400, binding.error);
  config.bindings.push_back(*binding.value);
  std::string save_error;
  if (!c.app.commit_config(&save_error)) return save_failed(save_error);
  return reply(201, binding_to_json(config.bindings.back()));
}

Response put_binding(Context& c) {
  std::lock_guard<std::mutex> lock(c.app.mutex);
  Config& config = c.app.config;
  const int index = find_binding_index(config, c.id);
  if (index < 0) return fail(404, "no such binding");
  Response error;
  JsonPtr body = read_object(c.request, &error);
  if (!body) return error;
  set_string(body.get(), "id", c.id);
  Parsed<Binding> binding = parse_binding(body.get(), button_ids(config));
  if (!binding.ok()) return fail(400, binding.error);
  config.bindings[index] = *binding.value;
  std::string save_error;
  if (!c.app.commit_config(&save_error)) return save_failed(save_error);
  return reply(200, binding_to_json(config.bindings[index]));
}

Response delete_binding(Context& c) {
  std::lock_guard<std::mutex> lock(c.app.mutex);
  Config& config = c.app.config;
  const int index = find_binding_index(config, c.id);
  if (index < 0) return fail(404, "no such binding");
  config.bindings.erase(config.bindings.begin() + index);
  std::string save_error;
  if (!c.app.commit_config(&save_error)) return save_failed(save_error);
  return ok_reply();
}

Response run_binding_now(Context& c) {
  Binding binding;
  {
    std::lock_guard<std::mutex> lock(c.app.mutex);
    const Binding* found = c.app.config.find_binding(c.id);
    if (found == nullptr) return fail(404, "no such binding");
    binding = *found;
  }
  // Without the lock: the action talks to the network and run_binding locks for itself.
  const ActionResult result = c.app.run_binding(binding);
  JsonPtr json = make_json(cJSON_CreateObject());
  cJSON_AddBoolToObject(json.get(), "ok", result.ok);
  cJSON_AddStringToObject(json.get(), "summary", result.summary.c_str());
  return reply(200, json);
}

// Fires the bindings of a trigger as if the button had been pressed; the cooldown does not apply.
Response simulate(Context& c) {
  Response error;
  JsonPtr body = read_object(c.request, &error);
  if (!body) return error;
  Parsed<Trigger> trigger = parse_trigger(body.get());
  if (!trigger.ok()) return fail(400, trigger.error);

  std::vector<Binding> matched;
  {
    std::lock_guard<std::mutex> lock(c.app.mutex);
    const Button* button = c.app.config.find_button(trigger.value->button);
    if (button == nullptr) return fail(404, "no such button");
    Observed observed;
    observed.mac = button->mac;
    observed.channel = trigger.value->channel;
    observed.input = trigger.value->input;
    observed.event = trigger.value->event;
    matched = c.app.engine.matching_bindings(observed);
    c.app.log.add("info", "Simulated: " + button->name + ", " + describe(observed) + " -> " +
                              std::to_string(matched.size()) + " binding(s)");
  }
  // Queued after the lock is released: a platform may run the task right here, and the task takes the lock.
  JsonPtr json = make_json(cJSON_CreateObject());
  cJSON* names = cJSON_AddArrayToObject(json.get(), "matched");
  for (const Binding& binding : matched) {
    cJSON_AddItemToArray(names, cJSON_CreateString(binding.name.c_str()));
    c.app.spawn_binding(binding);
  }
  return reply(200, json);
}

// ---- learn, log, settings --------------------------------------------------------------------------

void add_optional_int(cJSON* object, const char* key, const std::optional<int>& value) {
  if (value) {
    cJSON_AddNumberToObject(object, key, *value);
  } else {
    cJSON_AddNullToObject(object, key);
  }
}

// With a `config`, the candidate also says which configured button (if any) has its MAC address.
JsonPtr learn_status_json(const LearnStatus& status, const Config* config) {
  JsonPtr json = make_json(cJSON_CreateObject());
  cJSON_AddStringToObject(json.get(), "phase", status.phase.c_str());
  cJSON_AddStringToObject(json.get(), "message", status.message.c_str());
  if (status.candidate) {
    const LearnCandidate& candidate = *status.candidate;
    cJSON* object = cJSON_AddObjectToObject(json.get(), "candidate");
    cJSON_AddStringToObject(object, "mac", candidate.mac.c_str());
    cJSON_AddStringToObject(object, "name", candidate.name.c_str());
    add_optional_int(object, "rssi", candidate.rssi);
    add_optional_int(object, "channel", candidate.channel);
    cJSON_AddStringToObject(object, "input", candidate.input.c_str());
    cJSON_AddStringToObject(object, "event", candidate.event.c_str());
    add_optional_int(object, "battery", candidate.battery);
    cJSON_AddStringToObject(object, "description", candidate.description.c_str());
    if (config != nullptr) {
      const Button* existing = button_for_mac(*config, candidate.mac);
      if (existing != nullptr) {
        cJSON_AddStringToObject(object, "known_button", existing->id.c_str());
      } else {
        cJSON_AddNullToObject(object, "known_button");
      }
    }
  } else {
    cJSON_AddNullToObject(json.get(), "candidate");
  }
  cJSON_AddNumberToObject(json.get(), "seconds_left", status.seconds_left);
  return json;
}

Response learn_start(Context& c) {
  std::lock_guard<std::mutex> lock(c.app.mutex);
  c.app.learn.start();
  return reply(200, learn_status_json(c.app.learn.status(), nullptr));
}

Response learn_status(Context& c) {
  std::lock_guard<std::mutex> lock(c.app.mutex);
  return reply(200, learn_status_json(c.app.learn.status(), &c.app.config));
}

Response learn_cancel(Context& c) {
  std::lock_guard<std::mutex> lock(c.app.mutex);
  c.app.learn.cancel();
  return ok_reply();
}

Response learn_save(Context& c) {
  Response error;
  JsonPtr body = read_object(c.request, &error);
  if (!body) return error;
  std::lock_guard<std::mutex> lock(c.app.mutex);
  const LearnStatus status = c.app.learn.status();
  if (status.phase != "confirmed" || !c.app.learn.candidate()) {
    return fail(409, "no confirmed button to save; use Find button first");
  }
  const Observed candidate = *c.app.learn.candidate();
  Config& config = c.app.config;
  const Button* existing = button_for_mac(config, candidate.mac);
  const bool created = existing == nullptr;
  Button saved;
  if (created) {
    JsonPtr fields = make_json(cJSON_CreateObject());
    cJSON_AddStringToObject(fields.get(), "id", fresh_id(c.app, "btn", [&](const std::string& id) {
                                                  return config.find_button(id) != nullptr;
                                                }).c_str());
    const cJSON* name = field(body.get(), "name");
    if (name != nullptr) cJSON_AddItemToObject(fields.get(), "name", cJSON_Duplicate(name, true));
    cJSON_AddStringToObject(fields.get(), "mac", candidate.mac.c_str());
    cJSON_AddStringToObject(fields.get(), "model", c.app.learn.candidate_name().c_str());
    Parsed<Button> parsed = parse_button(fields.get());
    if (!parsed.ok()) return fail(400, parsed.error);
    saved = *parsed.value;
    config.buttons.push_back(saved);
    std::string save_error;
    if (!c.app.commit_config(&save_error)) return save_failed(save_error);
    c.app.log.add("info", "Added button \"" + saved.name + "\" (" + saved.mac + ")");
  } else {
    saved = *existing;
  }
  c.app.learn.cancel();

  JsonPtr json = make_json(cJSON_CreateObject());
  cJSON_AddItemToObject(json.get(), "button", button_to_json(saved).release());
  cJSON_AddBoolToObject(json.get(), "created", created);
  cJSON* trigger = cJSON_AddObjectToObject(json.get(), "trigger");
  cJSON_AddStringToObject(trigger, "button", saved.id.c_str());
  add_optional_int(trigger, "channel", candidate.channel);
  cJSON_AddStringToObject(trigger, "input", candidate.input.c_str());
  cJSON_AddStringToObject(trigger, "event", candidate.event.c_str());
  return reply(created ? 201 : 200, json);
}

Response get_log(Context& c) {
  uint32_t since = 0;
  if (c.request.query.count("since") != 0) {
    const std::string text = query_param(c.request, "since");
    if (text.empty() || text.size() > 18 || text.find_first_not_of("0123456789") != std::string::npos) {
      return fail(400, "since must be a number");
    }
    const unsigned long long value = std::strtoull(text.c_str(), nullptr, 10);
    since = value > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(value);
  }
  std::lock_guard<std::mutex> lock(c.app.mutex);
  JsonPtr json = make_json(cJSON_CreateObject());
  cJSON* entries = cJSON_AddArrayToObject(json.get(), "entries");
  for (const LogEntry& entry : c.app.log.since(since)) {
    cJSON* item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "seq", entry.seq);
    cJSON_AddNumberToObject(item, "time", c.app.wall_time_of(entry.time));
    cJSON_AddStringToObject(item, "kind", entry.kind.c_str());
    cJSON_AddStringToObject(item, "text", entry.text.c_str());
    cJSON_AddItemToArray(entries, item);
  }
  cJSON_AddNumberToObject(json.get(), "last_seq", c.app.log.last_seq());
  return reply(200, json);
}

JsonPtr settings_json(const Config& config) {
  JsonPtr full = config_to_json(config, false);
  JsonPtr json = make_json();
  json.reset(cJSON_DetachItemFromObject(full.get(), "settings"));
  return json;
}

Response put_settings(Context& c) {
  Response error;
  JsonPtr body = read_object(c.request, &error);
  if (!body) return error;
  std::lock_guard<std::mutex> lock(c.app.mutex);
  // The stored settings overlaid with the fields sent, so a request may carry just the ones it changes. The
  // stored value goes in as a plain number because the parser reads numbers back.
  JsonPtr merged = make_json(cJSON_CreateObject());
  cJSON_AddNumberToObject(merged.get(), "http_timeout_s", c.app.config.settings.http_timeout_s);
  for (const cJSON* item = body->child; item != nullptr; item = item->next) {
    if (item->string == nullptr) continue;
    cJSON_DeleteItemFromObject(merged.get(), item->string);
    cJSON_AddItemToObject(merged.get(), item->string, cJSON_Duplicate(item, true));
  }
  Parsed<Settings> parsed = parse_settings(merged.get());
  if (!parsed.ok()) return fail(400, parsed.error);
  c.app.config.settings = *parsed.value;
  std::string save_error;
  if (!c.app.commit_config(&save_error)) return save_failed(save_error);
  return reply(200, settings_json(c.app.config));
}

// ---- SwitchBot ---------------------------------------------------------------------------------------

Response put_switchbot_credentials(Context& c) {
  Response error;
  JsonPtr body = read_object(c.request, &error);
  if (!body) return error;
  const cJSON* token = field(body.get(), "token");
  const cJSON* secret = field(body.get(), "secret");
  if ((token != nullptr && !cJSON_IsString(token)) || (secret != nullptr && !cJSON_IsString(secret))) {
    return fail(400, "token and secret must be text");
  }
  // A field that is left out or blank keeps its stored value.
  const std::string new_token = token != nullptr ? trim(token->valuestring) : "";
  const std::string new_secret = secret != nullptr ? trim(secret->valuestring) : "";
  bool configured = false;
  std::string save_error;
  bool saved = false;
  {
    std::lock_guard<std::mutex> lock(c.app.mutex);
    if (!new_token.empty()) c.app.config.switchbot_token = new_token;
    if (!new_secret.empty()) c.app.config.switchbot_secret = new_secret;
    saved = c.app.commit_config(&save_error);
    configured = c.app.config.has_switchbot_credentials();
  }
  // The service locks internally, and the cached lists belong to the old credentials.
  c.app.switchbot.invalidate();
  if (!saved) return save_failed(save_error);
  return configured_reply(configured);
}

Response delete_switchbot_credentials(Context& c) {
  std::string save_error;
  bool saved = false;
  {
    std::lock_guard<std::mutex> lock(c.app.mutex);
    c.app.config.switchbot_token.clear();
    c.app.config.switchbot_secret.clear();
    saved = c.app.commit_config(&save_error);
  }
  c.app.switchbot.invalidate();
  if (!saved) return save_failed(save_error);
  return configured_reply(false);
}

Response switchbot_devices(Context& c) {
  const Parsed<std::vector<SwitchBotDevice>> devices = c.app.switchbot.devices(query_param(c.request, "refresh") == "1");
  if (!devices.ok()) return fail(502, devices.error);
  JsonPtr json = make_json(cJSON_CreateObject());
  cJSON* list = cJSON_AddArrayToObject(json.get(), "devices");
  for (const SwitchBotDevice& device : *devices.value) {
    cJSON_AddItemToArray(list, [&device] {
      cJSON* item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "id", device.id.c_str());
      cJSON_AddStringToObject(item, "name", device.name.c_str());
      cJSON_AddStringToObject(item, "type", device.type.c_str());
      cJSON_AddStringToObject(item, "hub", device.hub.c_str());
      if (device.remote_type) {
        cJSON_AddStringToObject(item, "remote_type", device.remote_type->c_str());
      } else {
        cJSON_AddNullToObject(item, "remote_type");
      }
      return item;
    }());
  }
  return reply(200, json);
}

Response switchbot_scenes(Context& c) {
  const Parsed<std::vector<SwitchBotScene>> scenes = c.app.switchbot.scenes(query_param(c.request, "refresh") == "1");
  if (!scenes.ok()) return fail(502, scenes.error);
  JsonPtr json = make_json(cJSON_CreateObject());
  cJSON* list = cJSON_AddArrayToObject(json.get(), "scenes");
  for (const SwitchBotScene& scene : *scenes.value) {
    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "id", scene.id.c_str());
    cJSON_AddStringToObject(item, "name", scene.name.c_str());
    cJSON_AddItemToArray(list, item);
  }
  return reply(200, json);
}

Response switchbot_commands(Context& c) {
  const std::string device_id = query_param(c.request, "device_id");
  if (device_id.empty()) return fail(400, "device_id is required");
  const Parsed<SwitchBotService::DeviceCommands> found = c.app.switchbot.commands_for_device(device_id);
  if (!found.ok()) return fail(502, found.error);
  const SwitchBotDevice& device = found.value->device;
  JsonPtr json = make_json(cJSON_CreateObject());
  cJSON* object = cJSON_AddObjectToObject(json.get(), "device");
  cJSON_AddStringToObject(object, "id", device.id.c_str());
  cJSON_AddStringToObject(object, "name", device.name.c_str());
  cJSON_AddStringToObject(object, "type", device.type.c_str());
  cJSON_AddStringToObject(object, "hub", device.hub.c_str());
  if (device.remote_type) {
    cJSON_AddStringToObject(object, "remote_type", device.remote_type->c_str());
  } else {
    cJSON_AddNullToObject(object, "remote_type");
  }
  cJSON* notes = cJSON_AddArrayToObject(json.get(), "notes");
  for (const std::string& note : found.value->list.notes) cJSON_AddItemToArray(notes, cJSON_CreateString(note.c_str()));
  cJSON* commands = cJSON_AddArrayToObject(json.get(), "commands");
  for (const CommandRow& row : found.value->list.commands) {
    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "command", row.command.c_str());
    cJSON_AddStringToObject(item, "parameter", row.parameter.c_str());
    cJSON_AddStringToObject(item, "description", row.description.c_str());
    cJSON_AddItemToArray(commands, item);
  }
  return reply(200, json);
}

// ---- export / import ---------------------------------------------------------------------------------

Response export_config(Context& c) {
  const bool include_credentials = query_param(c.request, "credentials") == "1";
  Response response;
  {
    std::lock_guard<std::mutex> lock(c.app.mutex);
    response.body = config_to_text(c.app.config, include_credentials, true) + "\n";
  }
  response.headers.emplace_back("Content-Disposition", "attachment; filename=\"button-dispatcher-config.json\"");
  return response;
}

Response import_config_route(Context& c) {
  Response error;
  JsonPtr body = read_object(c.request, &error);
  if (!body) return error;
  const cJSON* document = field(body.get(), "config");
  if (document == nullptr) return fail(400, "the request needs a 'config' field");
  const cJSON* mode_field = field(body.get(), "mode");
  const std::string mode = mode_field == nullptr ? "merge" : text_of(mode_field);

  std::string save_error;
  bool saved = false;
  JsonPtr json = make_json(cJSON_CreateObject());
  {
    std::lock_guard<std::mutex> lock(c.app.mutex);
    Parsed<Config> imported = import_config(c.app.config, document, mode);
    if (!imported.ok()) return fail(400, imported.error);
    c.app.config = std::move(*imported.value);
    saved = c.app.commit_config(&save_error);
    if (saved) {
      const size_t buttons = c.app.config.buttons.size();
      const size_t bindings = c.app.config.bindings.size();
      c.app.log.add("info", "Imported config (" + mode + "): " + std::to_string(buttons) + " button(s), " +
                                std::to_string(bindings) + " binding(s)");
      cJSON_AddNumberToObject(json.get(), "buttons", static_cast<double>(buttons));
      cJSON_AddNumberToObject(json.get(), "bindings", static_cast<double>(bindings));
    }
  }
  c.app.switchbot.invalidate();
  if (!saved) return save_failed(save_error);
  return reply(200, json);
}

// ---- routing -----------------------------------------------------------------------------------------

const Route kRoutes[] = {
    {"GET", "/api/auth/status", auth_status, false},
    {"POST", "/api/auth/setup", auth_setup, false},
    {"POST", "/api/auth/login", auth_login, false},
    {"POST", "/api/auth/logout", auth_logout, false},
    {"POST", "/api/auth/password", auth_password, false},
    {"GET", "/api/state", get_state, false},
    {"PUT", "/api/buttons/{id}", put_button, false},
    {"DELETE", "/api/buttons/{id}", delete_button, false},
    {"POST", "/api/bindings", post_binding, false},
    {"PUT", "/api/bindings/{id}", put_binding, false},
    {"DELETE", "/api/bindings/{id}", delete_binding, false},
    {"POST", "/api/bindings/{id}/run", run_binding_now, true},
    {"POST", "/api/simulate", simulate, false},
    {"POST", "/api/learn/start", learn_start, false},
    {"GET", "/api/learn", learn_status, false},
    {"POST", "/api/learn/cancel", learn_cancel, false},
    {"POST", "/api/learn/save", learn_save, false},
    {"GET", "/api/log", get_log, false},
    {"PUT", "/api/settings", put_settings, false},
    {"PUT", "/api/switchbot/credentials", put_switchbot_credentials, false},
    {"DELETE", "/api/switchbot/credentials", delete_switchbot_credentials, false},
    {"GET", "/api/switchbot/devices", switchbot_devices, true},
    {"GET", "/api/switchbot/scenes", switchbot_scenes, true},
    {"GET", "/api/switchbot/commands", switchbot_commands, true},
    {"GET", "/api/export", export_config, false},
    {"POST", "/api/import", import_config_route, false},
};

// The sign-in routes work without a session.
bool is_public_path(const std::string& path) {
  return path == "/api/auth/status" || path == "/api/auth/setup" || path == "/api/auth/login";
}

bool is_safe_method(const std::string& method) { return method == "GET" || method == "HEAD" || method == "OPTIONS"; }

std::vector<std::string> split_path(const std::string& path) {
  std::vector<std::string> parts;
  size_t pos = 0;
  while (true) {
    const size_t slash = path.find('/', pos);
    if (slash == std::string::npos) {
      parts.push_back(path.substr(pos));
      return parts;
    }
    parts.push_back(path.substr(pos, slash - pos));
    pos = slash + 1;
  }
}

bool match_pattern(const std::string& pattern, const std::string& path, std::string* id) {
  const std::vector<std::string> want = split_path(pattern);
  const std::vector<std::string> have = split_path(path);
  if (want.size() != have.size()) return false;
  for (size_t i = 0; i < want.size(); ++i) {
    if (want[i] == "{id}") {
      if (have[i].empty()) return false;
      *id = have[i];
    } else if (want[i] != have[i]) {
      return false;
    }
  }
  return true;
}

struct Match {
  const Route* route = nullptr;  // null if no route has this path and method
  bool path_known = false;       // some route has this path, with another method
  std::string id;
};

Match find_route(const std::string& method, const std::string& path) {
  Match match;
  for (const Route& route : kRoutes) {
    std::string id;
    if (!match_pattern(route.pattern, path, &id)) continue;
    match.path_known = true;
    if (method == route.method) {
      match.route = &route;
      match.id = id;
      return match;
    }
  }
  return match;
}

bool is_api_path(const std::string& path) { return path.compare(0, 5, "/api/") == 0; }

}  // namespace

Response Api::handle(const Request& request) {
  if (!is_api_path(request.path)) return fail(404, "not found");
  if (!is_safe_method(request.method) && request.csrf != "1") {
    return fail(403, std::string("missing ") + kCsrfHeaderName + " header");
  }
  if (!is_public_path(request.path) && !session_valid(app_, request)) return fail(401, "not signed in");

  Match match = find_route(request.method, request.path);
  if (match.route == nullptr) {
    return match.path_known ? fail(405, "method not allowed") : fail(404, "not found");
  }
  Context context{app_, request, match.id};
  return match.route->handler(context);
}

bool Api::is_slow(const Request& request) const {
  if (!is_api_path(request.path)) return false;
  const Match match = find_route(request.method, request.path);
  return match.route != nullptr && match.route->slow;
}

}  // namespace dispatcher
