// Config validation, serialization and import. See dispatcher/config.hpp.

#include "dispatcher/config.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace dispatcher {

namespace {

constexpr size_t kMaxNameLen = 60;
constexpr size_t kMaxUrlLen = 2000;
constexpr size_t kMaxBodyLen = 10000;
constexpr int kMaxHeaders = 20;
constexpr size_t kMaxParameterJsonLen = 4000;

const char* const kHttpMethods[] = {"GET", "POST", "PUT", "DELETE"};
const char* const kActionTypes[] = {"http", "switchbot_command", "switchbot_scene"};
const char* const kCommandTypes[] = {"command", "customize"};
const char* const kButtonEvents[] = {"press",      "double_press",      "triple_press", "long_press",
                                     "long_double_press", "long_triple_press", "hold_press"};
const char* const kWheelEvent = "turn";

template <typename T>
Parsed<T> ok(T value) {
  return Parsed<T>{std::optional<T>(std::move(value)), std::string()};
}

template <typename T>
Parsed<T> fail(std::string message) {
  return Parsed<T>{std::nullopt, std::move(message)};
}

template <size_t N>
std::string join(const char* const (&items)[N]) {
  std::string out;
  for (size_t i = 0; i < N; i++) {
    if (i > 0) out += ", ";
    out += items[i];
  }
  return out;
}

template <size_t N>
bool one_of(const std::string& value, const char* const (&items)[N]) {
  for (const char* item : items) {
    if (value == item) return true;
  }
  return false;
}

const cJSON* get(const cJSON* object, const char* key) {
  return cJSON_GetObjectItemCaseSensitive(object, key);
}

// Counts characters rather than bytes, so limits mean the same for non-ASCII text.
size_t char_count(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s) {
    if ((c & 0xC0) != 0x80) n++;
  }
  return n;
}

bool is_strip_char(unsigned char c) {
  return c == ' ' || (c >= '\t' && c <= '\r') || (c >= 0x1c && c <= 0x1f);
}

std::string strip(const std::string& s) {
  size_t begin = 0;
  size_t end = s.size();
  while (begin < end && is_strip_char(static_cast<unsigned char>(s[begin]))) begin++;
  while (end > begin && is_strip_char(static_cast<unsigned char>(s[end - 1]))) end--;
  return s.substr(begin, end - begin);
}

bool has_control_char(const std::string& s) {
  for (unsigned char c : s) {
    if (c < 0x20 || c == 0x7f) return true;
  }
  return false;
}

std::string print_unformatted(const cJSON* item) {
  char* text = cJSON_PrintUnformatted(item);
  if (text == nullptr) return std::string();
  std::string out(text);
  cJSON_free(text);
  return out;
}

// ---- Python-style text for messages and numbers ---------------------------------------------

// Shortest text that reads back as the same double.
std::string shortest_double(double value) {
  char buf[40];
  for (int precision = 1; precision <= 17; precision++) {
    std::snprintf(buf, sizeof(buf), "%.*g", precision, value);
    if (std::strtod(buf, nullptr) == value) break;
  }
  return buf;
}

// A float as written in exported documents: whole values keep a ".0" so they stay floats.
std::string float_text(double value) {
  if (std::isfinite(value) && value == std::floor(value) && std::fabs(value) < 1e16) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.1f", value);
    return buf;
  }
  return shortest_double(value);
}

// Quotes a string for an error message: 'text', or "text" when it contains a single quote.
std::string quoted(const std::string& s) {
  const bool use_double = s.find('\'') != std::string::npos && s.find('"') == std::string::npos;
  const char quote = use_double ? '"' : '\'';
  std::string out(1, quote);
  for (unsigned char c : s) {
    if (c == static_cast<unsigned char>(quote) || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c < 0x20 || c == 0x7f) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\x%02x", c);
      out += buf;
    } else {
      out += static_cast<char>(c);
    }
  }
  out += quote;
  return out;
}

// Renders any JSON value for an error message, the way a scripting-language user would expect.
void value_text(const cJSON* v, std::string& out) {
  if (cJSON_IsNull(v)) {
    out += "None";
  } else if (cJSON_IsTrue(v)) {
    out += "True";
  } else if (cJSON_IsFalse(v)) {
    out += "False";
  } else if (cJSON_IsNumber(v)) {
    const double d = v->valuedouble;
    if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 1e15) {
      char buf[40];
      std::snprintf(buf, sizeof(buf), "%.0f", d);
      out += buf;
    } else {
      out += shortest_double(d);
    }
  } else if (cJSON_IsString(v)) {
    out += quoted(v->valuestring);
  } else if (cJSON_IsArray(v)) {
    out += '[';
    for (const cJSON* e = v->child; e != nullptr; e = e->next) {
      if (e != v->child) out += ", ";
      value_text(e, out);
    }
    out += ']';
  } else if (cJSON_IsObject(v)) {
    out += '{';
    for (const cJSON* e = v->child; e != nullptr; e = e->next) {
      if (e != v->child) out += ", ";
      out += quoted(e->string != nullptr ? e->string : "");
      out += ": ";
      value_text(e, out);
    }
    out += '}';
  }
}

// ---- field validators; each returns an error message, or "" when the value is fine -----------

// Trims, then enforces the length and character rules. Stores the trimmed text in `out`.
std::string require_str(const cJSON* v, const std::string& what, size_t max_len, bool allow_empty,
                        std::string* out) {
  if (!cJSON_IsString(v)) return what + " must be a string";
  std::string value = strip(v->valuestring);
  if (value.empty() && !allow_empty) return what + " must not be empty";
  if (char_count(value) > max_len) {
    return what + " is too long (max " + std::to_string(max_len) + " characters)";
  }
  if (has_control_char(value)) return what + " must not contain control characters";
  *out = std::move(value);
  return "";
}

bool is_id_char(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '_';
}

std::string parse_id(const cJSON* v, const std::string& what, std::string* out) {
  bool valid = cJSON_IsString(v);
  if (valid) {
    const std::string s = v->valuestring;
    valid = !s.empty() && s.size() <= 64;
    for (unsigned char c : s) valid = valid && is_id_char(c);
  }
  if (!valid) return what + " must be 1-64 letters, digits, '-' or '_'";
  *out = v->valuestring;
  return "";
}

bool is_header_name(const std::string& s) {
  if (s.empty() || s.size() > 64) return false;
  for (unsigned char c : s) {
    if (!(is_id_char(c) && c != '_')) return false;
  }
  return true;
}

// "button:1" .. "button:99", "wheel:left" or "wheel:right".
bool is_trigger_input(const std::string& s) {
  if (s == "wheel:left" || s == "wheel:right") return true;
  if (s.compare(0, 7, "button:") != 0) return false;
  const std::string n = s.substr(7);
  if (n.empty() || n.size() > 2 || n[0] < '1' || n[0] > '9') return false;
  return n.size() == 1 || (n[1] >= '0' && n[1] <= '9');
}

// True for an integral JSON number in [lo, hi]; JSON booleans are not numbers here.
bool whole_in_range(const cJSON* v, int lo, int hi, int* out) {
  if (!cJSON_IsNumber(v)) return false;
  const double d = v->valuedouble;
  if (!(d >= lo && d <= hi) || d != std::floor(d)) return false;  // also rejects NaN
  *out = static_cast<int>(d);
  return true;
}

bool seconds_in_range(const cJSON* v, double* out) {
  if (!cJSON_IsNumber(v)) return false;
  const double d = v->valuedouble;
  if (!(d >= 1 && d <= 60)) return false;
  *out = d;
  return true;
}

// Whether an optional field counts as "not given": null, false, 0, "" or an empty container.
bool is_empty_value(const cJSON* v) {
  if (v == nullptr || cJSON_IsNull(v) || cJSON_IsFalse(v)) return true;
  if (cJSON_IsNumber(v)) return v->valuedouble == 0;
  if (cJSON_IsString(v)) return v->valuestring[0] == '\0';
  if (cJSON_IsArray(v) || cJSON_IsObject(v)) return v->child == nullptr;
  return false;
}

// Splits out the host of an http(s) URL without a full URL parser: scheme, "//", then the
// authority up to the first '/', '?' or '#', minus any user info and port.
bool is_http_url(const std::string& url) {
  const size_t colon = url.find(':');
  if (colon == std::string::npos || colon == 0) return false;
  const unsigned char first = url[0];
  if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z'))) return false;
  std::string scheme;
  for (size_t i = 0; i < colon; i++) {
    const unsigned char c = url[i];
    const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                         c == '+' || c == '-' || c == '.';
    if (!allowed) return false;
    scheme += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  }
  if (scheme != "http" && scheme != "https") return false;
  const std::string rest = url.substr(colon + 1);
  if (rest.compare(0, 2, "//") != 0) return false;
  const size_t end = rest.find_first_of("/?#", 2);
  std::string authority = rest.substr(2, end == std::string::npos ? std::string::npos : end - 2);
  const size_t at = authority.rfind('@');
  if (at != std::string::npos) authority = authority.substr(at + 1);
  std::string host;
  if (authority.find('[') != std::string::npos) {
    const size_t close = authority.find(']');
    if (authority[0] != '[' || close == std::string::npos) return false;
    host = authority.substr(1, close - 1);
  } else {
    host = authority.substr(0, authority.find(':'));
  }
  return !host.empty();
}

std::string to_upper(std::string s) {
  for (char& c : s) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  return s;
}

// Rejects duplicate ids and two buttons that share a MAC address.
std::string check_unique(const std::vector<Button>& buttons, const std::vector<Binding>& bindings) {
  for (size_t i = 0; i < buttons.size(); i++) {
    for (size_t j = 0; j < i; j++) {
      if (buttons[i].id == buttons[j].id) return "duplicate button id: " + quoted(buttons[i].id);
    }
  }
  for (size_t i = 0; i < bindings.size(); i++) {
    for (size_t j = 0; j < i; j++) {
      if (bindings[i].id == bindings[j].id) return "duplicate binding id: " + quoted(bindings[i].id);
    }
  }
  for (size_t i = 0; i < buttons.size(); i++) {
    for (size_t j = 0; j < i; j++) {
      if (buttons[i].mac == buttons[j].mac) {
        return "buttons " + quoted(buttons[j].id) + " and " + quoted(buttons[i].id) +
               " have the same MAC address";
      }
    }
  }
  return "";
}

bool contains(const std::vector<std::string>& ids, const std::string& id) {
  for (const std::string& s : ids) {
    if (s == id) return true;
  }
  return false;
}

// ---- serialization helpers -------------------------------------------------------------------

// Formats `value` for output; indentation is two spaces per level, as in hand-written configs.
void write_pretty(const cJSON* v, int depth, std::string& out) {
  const bool is_object = cJSON_IsObject(v);
  if (!is_object && !cJSON_IsArray(v)) {
    out += print_unformatted(v);
    return;
  }
  if (v->child == nullptr) {
    out += is_object ? "{}" : "[]";
    return;
  }
  out += is_object ? "{\n" : "[\n";
  for (const cJSON* e = v->child; e != nullptr; e = e->next) {
    out.append(static_cast<size_t>(depth + 1) * 2, ' ');
    if (is_object) {
      JsonPtr key = make_json(cJSON_CreateString(e->string != nullptr ? e->string : ""));
      out += print_unformatted(key.get());
      out += ": ";
    }
    write_pretty(e, depth + 1, out);
    out += e->next != nullptr ? ",\n" : "\n";
  }
  out.append(static_cast<size_t>(depth) * 2, ' ');
  out += is_object ? '}' : ']';
}

}  // namespace

JsonPtr make_json(cJSON* raw) { return JsonPtr(raw, cJSON_Delete); }

// ---- parsing ---------------------------------------------------------------------------------

Parsed<Button> parse_button(const cJSON* json) {
  if (!cJSON_IsObject(json)) return fail<Button>("button must be an object");
  Button button;
  std::string mac;
  if (auto e = parse_id(get(json, "id"), "button id", &button.id); !e.empty()) return fail<Button>(e);
  if (auto e = require_str(get(json, "name"), "button name", kMaxNameLen, false, &button.name); !e.empty()) {
    return fail<Button>(e);
  }
  if (auto e = require_str(get(json, "mac"), "button MAC address", 17, false, &mac); !e.empty()) {
    return fail<Button>(e);
  }
  const cJSON* model = get(json, "model");
  if (model != nullptr) {
    if (auto e = require_str(model, "button model", kMaxNameLen, true, &button.model); !e.empty()) {
      return fail<Button>(e);
    }
  }
  std::optional<std::string> normalized = normalize_mac(mac);
  if (!normalized) return fail<Button>("invalid MAC address: " + quoted(mac));
  button.mac = std::move(*normalized);
  return ok(std::move(button));
}

Parsed<Trigger> parse_trigger(const cJSON* json) {
  if (!cJSON_IsObject(json)) return fail<Trigger>("trigger must be an object");
  Trigger trigger;
  if (auto e = parse_id(get(json, "button"), "trigger button", &trigger.button); !e.empty()) {
    return fail<Trigger>(e);
  }
  const cJSON* input = get(json, "input");
  if (!cJSON_IsString(input) || !is_trigger_input(input->valuestring)) {
    return fail<Trigger>("trigger input must be button:1 .. button:99, wheel:left or wheel:right");
  }
  trigger.input = input->valuestring;
  const cJSON* event = get(json, "event");
  const bool is_wheel = trigger.input.compare(0, 6, "wheel:") == 0;
  bool event_ok = false;
  if (cJSON_IsString(event)) {
    event_ok = is_wheel ? std::strcmp(event->valuestring, kWheelEvent) == 0
                        : one_of(event->valuestring, kButtonEvents);
  }
  if (!event_ok) {
    return fail<Trigger>("trigger event for " + trigger.input + " must be one of: " +
                         (is_wheel ? std::string(kWheelEvent) : join(kButtonEvents)));
  }
  trigger.event = event->valuestring;
  const cJSON* channel = get(json, "channel");
  if (channel != nullptr && !cJSON_IsNull(channel)) {
    int value = 0;
    if (!whole_in_range(channel, 0, 255, &value)) {
      return fail<Trigger>("trigger channel must be a number from 0 to 255, or empty for any channel");
    }
    trigger.channel = value;
  }
  return ok(std::move(trigger));
}

Parsed<Action> parse_action(const cJSON* json) {
  if (!cJSON_IsObject(json)) return fail<Action>("action must be an object");
  const cJSON* type = get(json, "type");
  if (!cJSON_IsString(type) || !one_of(type->valuestring, kActionTypes)) {
    return fail<Action>("action type must be one of: " + join(kActionTypes));
  }
  const std::string kind = type->valuestring;

  if (kind == "http") {
    HttpAction action;
    const cJSON* method = get(json, "method");
    if (method != nullptr) {
      // A method that is not text can never be one of the allowed names.
      action.method = cJSON_IsString(method) ? to_upper(method->valuestring) : std::string();
    }
    if (!one_of(action.method, kHttpMethods)) {
      return fail<Action>("HTTP method must be one of: " + join(kHttpMethods));
    }
    if (auto e = require_str(get(json, "url"), "URL", kMaxUrlLen, false, &action.url); !e.empty()) {
      return fail<Action>(e);
    }
    if (!is_http_url(action.url) || action.url.find(' ') != std::string::npos) {
      return fail<Action>("URL must be an http:// or https:// address");
    }
    const cJSON* headers = get(json, "headers");
    if (!is_empty_value(headers)) {
      if (!cJSON_IsObject(headers) || cJSON_GetArraySize(headers) > kMaxHeaders) {
        return fail<Action>("headers must be an object with at most " + std::to_string(kMaxHeaders) +
                            " entries");
      }
      for (const cJSON* h = headers->child; h != nullptr; h = h->next) {
        const std::string name = h->string != nullptr ? h->string : "";
        if (!is_header_name(name)) return fail<Action>("invalid header name: " + quoted(name));
        std::string value;
        if (auto e = require_str(h, "header " + name, 1000, true, &value); !e.empty()) {
          return fail<Action>(e);
        }
        action.headers.emplace_back(name, std::move(value));
      }
    }
    const cJSON* body = get(json, "body");
    if (!is_empty_value(body)) {
      if (!cJSON_IsString(body) || char_count(body->valuestring) > kMaxBodyLen) {
        return fail<Action>("body must be text of at most " + std::to_string(kMaxBodyLen) +
                            " characters");
      }
      action.body = body->valuestring;
    }
    const cJSON* timeout = get(json, "timeout_s");
    const bool unset = timeout == nullptr || cJSON_IsNull(timeout) ||
                       (cJSON_IsString(timeout) && timeout->valuestring[0] == '\0');
    if (!unset) {
      double seconds = 0;
      if (!seconds_in_range(timeout, &seconds)) return fail<Action>("timeout must be between 1 and 60 seconds");
      action.timeout_s = seconds;
    }
    return ok(Action(std::move(action)));
  }

  if (kind == "switchbot_command") {
    SwitchBotCommandAction action;
    if (auto e = require_str(get(json, "device_id"), "SwitchBot device", 200, false, &action.device_id);
        !e.empty()) {
      return fail<Action>(e);
    }
    if (auto e = require_str(get(json, "command"), "SwitchBot command", 100, false, &action.command);
        !e.empty()) {
      return fail<Action>(e);
    }
    const cJSON* device_name = get(json, "device_name");
    if (device_name != nullptr) {
      if (auto e = require_str(device_name, "device name", 200, true, &action.device_name); !e.empty()) {
        return fail<Action>(e);
      }
    }
    const cJSON* parameter = get(json, "parameter");
    if (parameter != nullptr) {
      const std::string encoded = print_unformatted(parameter);
      if (char_count(encoded) > kMaxParameterJsonLen) return fail<Action>("SwitchBot parameter is too long");
      const bool replace_with_default =
          cJSON_IsNull(parameter) || (cJSON_IsString(parameter) && parameter->valuestring[0] == '\0');
      if (!replace_with_default) action.parameter_json = encoded;
    }
    const cJSON* command_type = get(json, "command_type");
    if (command_type != nullptr) {
      if (!cJSON_IsString(command_type) || !one_of(command_type->valuestring, kCommandTypes)) {
        return fail<Action>("command type must be one of: " + join(kCommandTypes));
      }
      action.command_type = command_type->valuestring;
    }
    return ok(Action(std::move(action)));
  }

  SwitchBotSceneAction action;
  if (auto e = require_str(get(json, "scene_id"), "SwitchBot scene", 200, false, &action.scene_id); !e.empty()) {
    return fail<Action>(e);
  }
  const cJSON* scene_name = get(json, "scene_name");
  if (scene_name != nullptr) {
    if (auto e = require_str(scene_name, "scene name", 200, true, &action.scene_name); !e.empty()) {
      return fail<Action>(e);
    }
  }
  return ok(Action(std::move(action)));
}

Parsed<Binding> parse_binding(const cJSON* json, const std::vector<std::string>& button_ids) {
  if (!cJSON_IsObject(json)) return fail<Binding>("binding must be an object");
  Parsed<Trigger> trigger = parse_trigger(get(json, "trigger"));
  if (!trigger.ok()) return fail<Binding>(trigger.error);
  if (!contains(button_ids, trigger.value->button)) {
    return fail<Binding>("binding refers to an unknown button: " + quoted(trigger.value->button));
  }
  Binding binding;
  binding.trigger = std::move(*trigger.value);
  const cJSON* cooldown = get(json, "cooldown_ms");
  if (cooldown != nullptr && !whole_in_range(cooldown, 0, 60000, &binding.cooldown_ms)) {
    return fail<Binding>("cooldown must be a whole number of milliseconds from 0 to 60000");
  }
  const cJSON* enabled = get(json, "enabled");
  if (enabled != nullptr) {
    if (!cJSON_IsBool(enabled)) return fail<Binding>("enabled must be true or false");
    binding.enabled = cJSON_IsTrue(enabled);
  }
  if (auto e = parse_id(get(json, "id"), "binding id", &binding.id); !e.empty()) return fail<Binding>(e);
  if (auto e = require_str(get(json, "name"), "binding name", kMaxNameLen, false, &binding.name);
      !e.empty()) {
    return fail<Binding>(e);
  }
  Parsed<Action> action = parse_action(get(json, "action"));
  if (!action.ok()) return fail<Binding>(action.error);
  binding.action = std::move(*action.value);
  return ok(std::move(binding));
}

Parsed<Settings> parse_settings(const cJSON* json) {
  if (!cJSON_IsObject(json)) return fail<Settings>("settings must be an object");
  Settings settings;
  const cJSON* timeout = get(json, "http_timeout_s");
  if (timeout != nullptr && !seconds_in_range(timeout, &settings.http_timeout_s)) {
    return fail<Settings>("HTTP timeout must be between 1 and 60 seconds");
  }
  return ok(std::move(settings));
}

Parsed<Config> config_from_json(const cJSON* json, const std::vector<std::string>& extra_button_ids) {
  if (!cJSON_IsObject(json)) return fail<Config>("config must be an object");
  const cJSON* version = get(json, "version");
  if (version != nullptr && !(cJSON_IsNumber(version) && version->valuedouble == kConfigVersion)) {
    std::string text;
    value_text(version, text);
    return fail<Config>("unsupported config version: " + text);
  }

  Config config;
  const cJSON* buttons = get(json, "buttons");
  if (buttons != nullptr) {
    if (!cJSON_IsArray(buttons)) return fail<Config>("buttons must be a list");
    for (const cJSON* b = buttons->child; b != nullptr; b = b->next) {
      Parsed<Button> button = parse_button(b);
      if (!button.ok()) return fail<Config>(button.error);
      config.buttons.push_back(std::move(*button.value));
    }
  }
  std::vector<std::string> button_ids;
  for (const Button& b : config.buttons) button_ids.push_back(b.id);
  button_ids.insert(button_ids.end(), extra_button_ids.begin(), extra_button_ids.end());

  const cJSON* bindings = get(json, "bindings");
  if (bindings != nullptr) {
    if (!cJSON_IsArray(bindings)) return fail<Config>("bindings must be a list");
    for (const cJSON* b = bindings->child; b != nullptr; b = b->next) {
      Parsed<Binding> binding = parse_binding(b, button_ids);
      if (!binding.ok()) return fail<Config>(binding.error);
      config.bindings.push_back(std::move(*binding.value));
    }
  }
  if (auto e = check_unique(config.buttons, config.bindings); !e.empty()) return fail<Config>(e);

  const cJSON* settings = get(json, "settings");
  if (settings != nullptr) {
    Parsed<Settings> parsed = parse_settings(settings);
    if (!parsed.ok()) return fail<Config>(parsed.error);
    config.settings = *parsed.value;
  }

  const cJSON* switchbot = get(json, "switchbot");
  if (switchbot != nullptr && !cJSON_IsNull(switchbot)) {
    if (!cJSON_IsObject(switchbot)) return fail<Config>("switchbot must be an object");
    const cJSON* token = get(switchbot, "token");
    if (token != nullptr) {
      if (auto e = require_str(token, "SwitchBot token", 500, true, &config.switchbot_token); !e.empty()) {
        return fail<Config>(e);
      }
    }
    const cJSON* secret = get(switchbot, "secret");
    if (secret != nullptr) {
      if (auto e = require_str(secret, "SwitchBot secret", 500, true, &config.switchbot_secret); !e.empty()) {
        return fail<Config>(e);
      }
    }
  }
  return ok(std::move(config));
}

Parsed<Config> config_from_text(const std::string& text) {
  JsonPtr json = make_json(cJSON_ParseWithOpts(text.c_str(), nullptr, true));
  if (!json) return fail<Config>("invalid JSON");
  return config_from_json(json.get());
}

Parsed<Config> import_config(const Config& current, const cJSON* data, const std::string& mode) {
  if (mode != "merge" && mode != "replace") return fail<Config>("import mode must be 'merge' or 'replace'");
  Config result;
  Parsed<Config> imported;
  if (mode == "replace") {
    imported = config_from_json(data);
    if (!imported.ok()) return imported;
    result = *imported.value;
  } else {
    std::vector<std::string> existing_ids;
    for (const Button& b : current.buttons) existing_ids.push_back(b.id);
    imported = config_from_json(data, existing_ids);
    if (!imported.ok()) return imported;
    // Same-id entries are replaced in place so the existing order is kept; new ones are appended.
    result.buttons = current.buttons;
    for (const Button& b : imported.value->buttons) {
      bool replaced = false;
      for (Button& existing : result.buttons) {
        if (existing.id == b.id) {
          existing = b;
          replaced = true;
        }
      }
      if (!replaced) result.buttons.push_back(b);
    }
    result.bindings = current.bindings;
    for (const Binding& b : imported.value->bindings) {
      bool replaced = false;
      for (Binding& existing : result.bindings) {
        if (existing.id == b.id) {
          existing = b;
          replaced = true;
        }
      }
      if (!replaced) result.bindings.push_back(b);
    }
    result.settings = get(data, "settings") != nullptr ? imported.value->settings : current.settings;
    if (auto e = check_unique(result.buttons, result.bindings); !e.empty()) return fail<Config>(e);
  }
  result.switchbot_token =
      !imported.value->switchbot_token.empty() ? imported.value->switchbot_token : current.switchbot_token;
  result.switchbot_secret =
      !imported.value->switchbot_secret.empty() ? imported.value->switchbot_secret : current.switchbot_secret;
  // A binding whose button was dropped by a replace would never fire; reject instead of keeping it.
  for (const Binding& binding : result.bindings) {
    if (result.find_button(binding.trigger.button) == nullptr) {
      return fail<Config>("binding " + quoted(binding.id) + " refers to an unknown button");
    }
  }
  return ok(std::move(result));
}

// ---- serialization ---------------------------------------------------------------------------

JsonPtr button_to_json(const Button& button) {
  JsonPtr obj = make_json(cJSON_CreateObject());
  cJSON_AddStringToObject(obj.get(), "id", button.id.c_str());
  cJSON_AddStringToObject(obj.get(), "name", button.name.c_str());
  cJSON_AddStringToObject(obj.get(), "mac", button.mac.c_str());
  cJSON_AddStringToObject(obj.get(), "model", button.model.c_str());
  return obj;
}

JsonPtr action_to_json(const Action& action) {
  JsonPtr obj = make_json(cJSON_CreateObject());
  cJSON* o = obj.get();
  cJSON_AddStringToObject(o, "type", action_type_name(action));
  if (const HttpAction* http = std::get_if<HttpAction>(&action)) {
    cJSON_AddStringToObject(o, "method", http->method.c_str());
    cJSON_AddStringToObject(o, "url", http->url.c_str());
    if (!http->headers.empty()) {
      cJSON* headers = cJSON_AddObjectToObject(o, "headers");
      for (const auto& [name, value] : http->headers) cJSON_AddStringToObject(headers, name.c_str(), value.c_str());
    }
    if (!http->body.empty()) cJSON_AddStringToObject(o, "body", http->body.c_str());
    if (http->timeout_s) cJSON_AddRawToObject(o, "timeout_s", float_text(*http->timeout_s).c_str());
  } else if (const SwitchBotCommandAction* cmd = std::get_if<SwitchBotCommandAction>(&action)) {
    cJSON_AddStringToObject(o, "device_id", cmd->device_id.c_str());
    cJSON_AddStringToObject(o, "command", cmd->command.c_str());
    if (!cmd->device_name.empty()) cJSON_AddStringToObject(o, "device_name", cmd->device_name.c_str());
    cJSON* parameter = cJSON_Parse(cmd->parameter_json.c_str());
    if (parameter == nullptr) parameter = cJSON_CreateString("default");
    cJSON_AddItemToObject(o, "parameter", parameter);
    cJSON_AddStringToObject(o, "command_type", cmd->command_type.c_str());
  } else if (const SwitchBotSceneAction* scene = std::get_if<SwitchBotSceneAction>(&action)) {
    cJSON_AddStringToObject(o, "scene_id", scene->scene_id.c_str());
    if (!scene->scene_name.empty()) cJSON_AddStringToObject(o, "scene_name", scene->scene_name.c_str());
  }
  return obj;
}

JsonPtr binding_to_json(const Binding& binding) {
  JsonPtr obj = make_json(cJSON_CreateObject());
  cJSON* o = obj.get();
  cJSON_AddStringToObject(o, "id", binding.id.c_str());
  cJSON_AddStringToObject(o, "name", binding.name.c_str());
  cJSON_AddBoolToObject(o, "enabled", binding.enabled);
  cJSON_AddNumberToObject(o, "cooldown_ms", binding.cooldown_ms);
  cJSON* trigger = cJSON_AddObjectToObject(o, "trigger");
  cJSON_AddStringToObject(trigger, "button", binding.trigger.button.c_str());
  if (binding.trigger.channel) {
    cJSON_AddNumberToObject(trigger, "channel", *binding.trigger.channel);
  } else {
    cJSON_AddNullToObject(trigger, "channel");
  }
  cJSON_AddStringToObject(trigger, "input", binding.trigger.input.c_str());
  cJSON_AddStringToObject(trigger, "event", binding.trigger.event.c_str());
  cJSON_AddItemToObject(o, "action", action_to_json(binding.action).release());
  return obj;
}

JsonPtr config_to_json(const Config& config, bool include_credentials) {
  JsonPtr obj = make_json(cJSON_CreateObject());
  cJSON* o = obj.get();
  cJSON_AddNumberToObject(o, "version", kConfigVersion);
  cJSON* settings = cJSON_AddObjectToObject(o, "settings");
  cJSON_AddRawToObject(settings, "http_timeout_s", float_text(config.settings.http_timeout_s).c_str());
  cJSON* buttons = cJSON_AddArrayToObject(o, "buttons");
  for (const Button& b : config.buttons) cJSON_AddItemToArray(buttons, button_to_json(b).release());
  cJSON* bindings = cJSON_AddArrayToObject(o, "bindings");
  for (const Binding& b : config.bindings) cJSON_AddItemToArray(bindings, binding_to_json(b).release());
  if (include_credentials && (!config.switchbot_token.empty() || !config.switchbot_secret.empty())) {
    cJSON* switchbot = cJSON_AddObjectToObject(o, "switchbot");
    cJSON_AddStringToObject(switchbot, "token", config.switchbot_token.c_str());
    cJSON_AddStringToObject(switchbot, "secret", config.switchbot_secret.c_str());
  }
  return obj;
}

std::string config_to_text(const Config& config, bool include_credentials, bool pretty) {
  JsonPtr json = config_to_json(config, include_credentials);
  if (!pretty) return print_unformatted(json.get());
  std::string out;
  write_pretty(json.get(), 0, out);
  return out;
}

std::string new_id(const std::string& prefix, uint32_t random) {
  char suffix[8];
  std::snprintf(suffix, sizeof(suffix), "%06x", static_cast<unsigned>(random & 0xffffff));
  return prefix + "-" + suffix;
}

}  // namespace dispatcher
