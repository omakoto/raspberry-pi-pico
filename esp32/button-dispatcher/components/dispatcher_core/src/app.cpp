#include "dispatcher/app.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <vector>

#include "dispatcher/util.hpp"

namespace dispatcher {

namespace {

constexpr const char* kConfigDocument = "config.json";

// A duration for messages: 10 prints as "10", 2.5 as "2.5".
std::string format_seconds(double seconds) {
  char text[32];
  std::snprintf(text, sizeof(text), "%g", seconds);
  return text;
}

bool has_header(const HeaderList& headers, const char* name) {
  const std::string wanted(name);
  for (const auto& header : headers) {
    if (header.first.size() != wanted.size()) continue;
    bool same = true;
    for (size_t i = 0; i < wanted.size() && same; ++i) {
      same = std::tolower(static_cast<unsigned char>(header.first[i])) ==
             std::tolower(static_cast<unsigned char>(wanted[i]));
    }
    if (same) return true;
  }
  return false;
}

// A body that looks like JSON is sent as JSON; anything else as plain text. The user can override it with
// their own Content-Type header.
std::string default_content_type(const std::string& body) {
  const size_t first = body.find_first_not_of(" \t\r\n");
  const bool json = first != std::string::npos && (body[first] == '{' || body[first] == '[');
  return json ? "application/json" : "text/plain; charset=utf-8";
}

}  // namespace

AppCore::AppCore(Platform& platform, int pbkdf2_iterations, std::string switchbot_api_base)
    : engine([&platform] { return platform.monotonic_s(); }),
      learn([&platform] { return platform.monotonic_s(); }),
      log(kLogCapacity, [&platform] { return platform.monotonic_s(); }),
      auth(platform, pbkdf2_iterations),
      limiter([&platform] { return platform.monotonic_s(); }),
      switchbot(platform,
                [this] {
                  std::lock_guard<std::mutex> lock(credentials_mutex_);
                  return credentials_;
                },
                std::move(switchbot_api_base)),
      platform_(platform) {}

void AppCore::init(std::string* error) {
  std::string text;
  if (platform_.load_document(kConfigDocument, &text)) {
    Parsed<Config> parsed = config_from_text(text);
    if (parsed.ok()) {
      config = std::move(*parsed.value);
    } else if (error != nullptr) {
      *error = std::string(kConfigDocument) + " is invalid: " + parsed.error;
    }
  }
  {
    std::lock_guard<std::mutex> lock(credentials_mutex_);
    credentials_ = {config.switchbot_token, config.switchbot_secret};
  }
  engine.configure(config.buttons, config.bindings);
  auth.load();
}

bool AppCore::commit_config(std::string* error) {
  // The running engine follows the edited config even if storing it fails, so the page keeps working;
  // the caller reports the failure.
  engine.configure(config.buttons, config.bindings);
  {
    std::lock_guard<std::mutex> lock(credentials_mutex_);
    credentials_ = {config.switchbot_token, config.switchbot_secret};
  }
  if (!platform_.save_document(kConfigDocument, config_to_text(config, true, true) + "\n")) {
    if (error != nullptr) *error = "the configuration could not be saved";
    return false;
  }
  return true;
}

std::string AppCore::label_for(const std::string& mac) const {
  for (const Button& button : config.buttons) {
    if (button.mac == mac) return button.name;
  }
  std::string label = "unknown device " + mac;
  auto name = names.find(mac);
  if (name != names.end() && !name->second.empty()) label += " (" + name->second + ")";
  return label;
}

double AppCore::wall_time_of(double mono) const {
  return static_cast<double>(platform_.wall_time_ms()) / 1000.0 - (platform_.monotonic_s() - mono);
}

void AppCore::handle_packet(const std::string& mac, std::optional<int> rssi, const uint8_t* data, size_t length) {
  std::vector<Binding> to_run;
  {
    std::lock_guard<std::mutex> lock(mutex);
    const ProcessResult result = engine.process(mac, rssi, data, length);
    if (!result.error.empty() && result.known_device) {
      const std::string key = normalize_mac(mac).value_or(mac);
      const double now = platform_.monotonic_s();
      auto last = last_error_log_.find(key);
      if (last == last_error_log_.end() || now - last->second >= kErrorLogIntervalS) {
        last_error_log_[key] = now;
        log.add("error", label_for(key) + ": " + result.error);
      }
    }
    for (const Observed& observed : result.observed) {
      auto name = names.find(observed.mac);
      learn.feed(observed, name != names.end() ? name->second : "");
      log.add("ble", label_for(observed.mac) + ": " + describe(observed));
    }
    for (const Firing& firing : result.fired) {
      log.add("fire", "Binding \"" + firing.binding.name + "\" fired");
      to_run.push_back(firing.binding);
    }
  }
  // One acknowledgement per advertisement, however many bindings the press fires.
  int ack_beeps = 0;
  for (const Binding& binding : to_run) ack_beeps = std::max(ack_beeps, click_count(binding.trigger.event));
  if (ack_beeps > 0) platform_.beep(Beep::kShort, ack_beeps);
  // Queued only after the lock is released: a platform may run the task right here, and the task takes the lock.
  for (const Binding& binding : to_run) spawn_binding(binding);
}

void AppCore::spawn_binding(const Binding& binding) {
  platform_.run_in_background([this, binding] {
    const ActionResult result = run_binding(binding);
    // A short beep for success; a failure sounds longer, since an active buzzer has only one pitch.
    if (result.ok) platform_.beep(Beep::kShort, 1);
    else platform_.beep(Beep::kLong, 3);
  });
}

ActionResult AppCore::run_binding(const Binding& binding) {
  const ActionResult result = run_action(binding.action);
  std::lock_guard<std::mutex> lock(mutex);
  log.add(result.ok ? "action" : "error", "\"" + binding.name + "\": " + result.summary);
  return result;
}

ActionResult AppCore::run_action(const Action& action) {
  if (const auto* command = std::get_if<SwitchBotCommandAction>(&action)) {
    const SwitchBotBody sent =
        switchbot.command(command->device_id, command->command, command->parameter_json, command->command_type);
    if (!sent.ok()) return {false, "SwitchBot: " + sent.error};
    return {true, "SwitchBot: sent " + command->command + " to " +
                      (command->device_name.empty() ? command->device_id : command->device_name)};
  }
  if (const auto* scene = std::get_if<SwitchBotSceneAction>(&action)) {
    const SwitchBotBody ran = switchbot.execute_scene(scene->scene_id);
    if (!ran.ok()) return {false, "SwitchBot: " + ran.error};
    return {true, "SwitchBot: ran scene " + (scene->scene_name.empty() ? scene->scene_id : scene->scene_name)};
  }

  const auto& http = std::get<HttpAction>(action);
  double timeout_s = 0.0;
  if (http.timeout_s) {
    timeout_s = *http.timeout_s;
  } else {
    std::lock_guard<std::mutex> lock(mutex);
    timeout_s = config.settings.http_timeout_s;
  }
  HeaderList headers = http.headers;
  if (!http.body.empty() && !has_header(headers, "Content-Type")) {
    headers.emplace_back("Content-Type", default_content_type(http.body));
  }
  // Only the scheme and host go into messages: the rest of the URL may carry a key (IFTTT webhook URLs do).
  const std::string where = host_of(http.url);
  const HttpResponse response =
      platform_.http_request(http.method, http.url, headers, http.body, timeout_s, kMaxActionResponseBytes);
  const std::string prefix = http.method + " " + where + ": ";
  const std::string why = response.detail.empty() ? "" : " (" + response.detail + ")";
  switch (response.failure) {
    case HttpResponse::Failure::kTimeout:
      return {false, prefix + "timed out after " + format_seconds(timeout_s) + "s" + why};
    case HttpResponse::Failure::kConnect:
      return {false, prefix + "connection failed" + why};
    case HttpResponse::Failure::kNone:
      break;
  }
  std::string summary = prefix + "HTTP " + std::to_string(response.status);
  if (!response.reason.empty()) summary += " " + response.reason;
  return {response.status < 400, summary};
}

}  // namespace dispatcher
