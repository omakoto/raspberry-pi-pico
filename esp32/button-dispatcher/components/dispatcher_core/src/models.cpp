#include "dispatcher/models.hpp"

#include <cctype>

namespace dispatcher {

namespace {

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

std::optional<std::string> normalize_mac(const std::string& value) {
  size_t begin = 0;
  size_t end = value.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;

  // Six pairs of hex digits, with the same separator (':' or '-') between all of them, or none.
  std::string digits;
  char separator = 0;
  size_t i = begin;
  for (int pair = 0; pair < 6; ++pair) {
    if (pair > 0 && i < end && (value[i] == ':' || value[i] == '-')) {
      if (separator != 0 && value[i] != separator) return std::nullopt;
      separator = value[i];
      ++i;
    } else if (pair > 0 && separator != 0) {
      return std::nullopt;
    }
    if (i + 2 > end || hex_value(value[i]) < 0 || hex_value(value[i + 1]) < 0) return std::nullopt;
    digits += static_cast<char>(std::toupper(static_cast<unsigned char>(value[i])));
    digits += static_cast<char>(std::toupper(static_cast<unsigned char>(value[i + 1])));
    i += 2;
  }
  if (i != end) return std::nullopt;

  std::string mac;
  for (size_t d = 0; d < 12; d += 2) {
    if (d > 0) mac += ':';
    mac += digits.substr(d, 2);
  }
  return mac;
}

const char* action_type_name(const Action& action) {
  if (std::holds_alternative<HttpAction>(action)) return "http";
  if (std::holds_alternative<SwitchBotCommandAction>(action)) return "switchbot_command";
  return "switchbot_scene";
}

const Button* Config::find_button(const std::string& id) const {
  for (const Button& b : buttons) {
    if (b.id == id) return &b;
  }
  return nullptr;
}

const Binding* Config::find_binding(const std::string& id) const {
  for (const Binding& b : bindings) {
    if (b.id == id) return &b;
  }
  return nullptr;
}

}  // namespace dispatcher
