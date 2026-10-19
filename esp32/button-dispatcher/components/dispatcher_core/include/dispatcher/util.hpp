// Small pure helpers: encodings, constant-time comparison, URL display.

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace dispatcher {

std::string base64_encode(const std::string& raw);
// URL-safe alphabet without padding, for tokens that travel in cookies.
std::string base64url_encode(const std::string& raw);

std::string hex_encode(const std::string& raw);
// Returns std::nullopt unless `text` is an even number of hex digits.
std::optional<std::string> hex_decode(const std::string& text);

// Compares without exiting early at the first difference, so timing doesn't reveal how much matched.
bool constant_time_equals(const std::string& a, const std::string& b);

// The scheme and host (and port) of a URL, such as "https://maker.ifttt.com": enough for the log, without a
// path, query or user name that might carry a key or password. Returns "<url>" if there is no host.
std::string host_of(const std::string& url);

// A random version-4 UUID such as "9f1c2a6e-4b7d-4e1a-8c3d-5a6b7c8d9e0f" from 16 random bytes.
std::string format_uuid4(const uint8_t random[16]);

}  // namespace dispatcher
