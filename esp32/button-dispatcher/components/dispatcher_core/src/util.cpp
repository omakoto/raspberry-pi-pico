#include "dispatcher/util.hpp"

#include <cctype>
#include <cstdio>

namespace dispatcher {

namespace {

std::string base64_with_alphabet(const std::string& raw, const char* alphabet, bool pad) {
  std::string out;
  size_t i = 0;
  while (i + 2 < raw.size()) {
    const uint32_t v = (static_cast<uint8_t>(raw[i]) << 16) | (static_cast<uint8_t>(raw[i + 1]) << 8) |
                       static_cast<uint8_t>(raw[i + 2]);
    out += alphabet[(v >> 18) & 63];
    out += alphabet[(v >> 12) & 63];
    out += alphabet[(v >> 6) & 63];
    out += alphabet[v & 63];
    i += 3;
  }
  const size_t rest = raw.size() - i;
  if (rest == 1) {
    const uint32_t v = static_cast<uint8_t>(raw[i]) << 16;
    out += alphabet[(v >> 18) & 63];
    out += alphabet[(v >> 12) & 63];
    if (pad) out += "==";
  } else if (rest == 2) {
    const uint32_t v = (static_cast<uint8_t>(raw[i]) << 16) | (static_cast<uint8_t>(raw[i + 1]) << 8);
    out += alphabet[(v >> 18) & 63];
    out += alphabet[(v >> 12) & 63];
    out += alphabet[(v >> 6) & 63];
    if (pad) out += '=';
  }
  return out;
}

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

std::string base64_encode(const std::string& raw) {
  return base64_with_alphabet(raw, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", true);
}

std::string base64url_encode(const std::string& raw) {
  return base64_with_alphabet(raw, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_", false);
}

std::string hex_encode(const std::string& raw) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(raw.size() * 2);
  for (unsigned char c : raw) {
    out += digits[c >> 4];
    out += digits[c & 15];
  }
  return out;
}

std::optional<std::string> hex_decode(const std::string& text) {
  if (text.size() % 2 != 0) return std::nullopt;
  std::string out;
  for (size_t i = 0; i < text.size(); i += 2) {
    const int hi = hex_digit(text[i]);
    const int lo = hex_digit(text[i + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    out += static_cast<char>(hi * 16 + lo);
  }
  return out;
}

bool constant_time_equals(const std::string& a, const std::string& b) {
  // The length is not secret (tokens and digests have fixed sizes); the content is.
  if (a.size() != b.size()) return false;
  unsigned char difference = 0;
  for (size_t i = 0; i < a.size(); ++i) difference |= static_cast<unsigned char>(a[i] ^ b[i]);
  return difference == 0;
}

std::string host_of(const std::string& url) {
  const size_t scheme_end = url.find("://");
  if (scheme_end == std::string::npos || scheme_end == 0) return "<url>";
  std::string scheme = url.substr(0, scheme_end);
  for (char& c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const size_t authority_start = scheme_end + 3;
  size_t authority_end = url.find_first_of("/?#", authority_start);
  if (authority_end == std::string::npos) authority_end = url.size();
  std::string authority = url.substr(authority_start, authority_end - authority_start);
  // A user name and password in front of the host must not reach the log.
  const size_t at = authority.rfind('@');
  if (at != std::string::npos) authority = authority.substr(at + 1);
  if (authority.empty()) return "<url>";
  return scheme + "://" + authority;
}

std::string format_uuid4(const uint8_t random[16]) {
  uint8_t b[16];
  for (int i = 0; i < 16; ++i) b[i] = random[i];
  b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40);  // version 4
  b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);  // RFC 4122 variant
  char text[40];
  std::snprintf(text, sizeof(text), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
                b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  return text;
}

}  // namespace dispatcher
