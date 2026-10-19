#include "dispatcher/auth.hpp"

#include <algorithm>
#include <cstdlib>

#include "cJSON.h"
#include "dispatcher/util.hpp"

namespace dispatcher {

namespace {

constexpr const char* kAuthDocument = "auth.json";
constexpr const char* kHashScheme = "pbkdf2-sha256";
constexpr size_t kSaltBytes = 16;
constexpr size_t kHashBytes = 32;
constexpr size_t kSessionKeyBytes = 32;
constexpr size_t kNonceBytes = 8;

std::string random_string(Platform& platform, size_t length) {
  std::string out(length, '\0');
  platform.random_bytes(reinterpret_cast<uint8_t*>(out.data()), length);
  return out;
}

// Splits "a$b$c$d" at '$'.
std::vector<std::string> split(const std::string& text, char separator) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (true) {
    const size_t at = text.find(separator, start);
    if (at == std::string::npos) {
      parts.push_back(text.substr(start));
      return parts;
    }
    parts.push_back(text.substr(start, at - start));
    start = at + 1;
  }
}

bool parse_int(const std::string& text, long* value) {
  if (text.empty()) return false;
  char* end = nullptr;
  const long parsed = std::strtol(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') return false;
  *value = parsed;
  return true;
}

}  // namespace

AuthService::AuthService(Platform& platform, int iterations) : platform_(platform), iterations_(iterations) {}

void AuthService::new_session_key() { session_key_ = random_string(platform_, kSessionKeyBytes); }

void AuthService::load() {
  password_hash_.clear();
  std::string text;
  if (platform_.load_document(kAuthDocument, &text)) {
    cJSON* root = cJSON_Parse(text.c_str());
    if (root != nullptr) {
      const cJSON* password = cJSON_GetObjectItemCaseSensitive(root, "password");
      if (cJSON_IsString(password) && password->valuestring != nullptr) password_hash_ = password->valuestring;
      cJSON_Delete(root);
    }
  }
  new_session_key();
}

std::string AuthService::set_password(const std::string& password) {
  if (password.size() < kMinPasswordLength || password.size() > kMaxPasswordLength) {
    return "password must be " + std::to_string(kMinPasswordLength) + "-" + std::to_string(kMaxPasswordLength) +
           " characters";
  }
  const std::string salt = random_string(platform_, kSaltBytes);
  const std::string hash = platform_.pbkdf2_hmac_sha256(password, salt, iterations_, kHashBytes);
  const std::string stored = std::string(kHashScheme) + "$" + std::to_string(iterations_) + "$" + hex_encode(salt) +
                             "$" + hex_encode(hash);

  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "password", stored.c_str());
  char* printed = cJSON_PrintUnformatted(root);
  const std::string document = printed != nullptr ? std::string(printed) + "\n" : "{}\n";
  cJSON_free(printed);
  cJSON_Delete(root);
  if (!platform_.save_document(kAuthDocument, document)) return "the password could not be saved";

  password_hash_ = stored;
  // A new key invalidates every session issued under the old password.
  new_session_key();
  return "";
}

bool AuthService::verify_password(const std::string& password) const {
  if (!is_configured()) return false;
  const std::vector<std::string> parts = split(password_hash_, '$');
  long iterations = 0;
  if (parts.size() != 4 || parts[0] != kHashScheme || !parse_int(parts[1], &iterations) || iterations <= 0) {
    return false;
  }
  const std::optional<std::string> salt = hex_decode(parts[2]);
  const std::optional<std::string> expected = hex_decode(parts[3]);
  if (!salt || !expected || expected->empty()) return false;
  const std::string actual =
      platform_.pbkdf2_hmac_sha256(password, *salt, static_cast<int>(iterations), expected->size());
  return constant_time_equals(actual, *expected);
}

std::string AuthService::make_token() {
  const long expiry = static_cast<long>(platform_.monotonic_s() + kSessionTtlS);
  const std::string payload = std::to_string(expiry) + "." + hex_encode(random_string(platform_, kNonceBytes));
  return payload + "." + base64url_encode(platform_.hmac_sha256(session_key_, payload));
}

bool AuthService::verify_token(const std::string& token) const {
  if (token.empty() || !is_configured()) return false;
  const std::vector<std::string> parts = split(token, '.');
  long expiry = 0;
  if (parts.size() != 3 || !parse_int(parts[0], &expiry)) return false;
  const std::string payload = parts[0] + "." + parts[1];
  const std::string expected = base64url_encode(platform_.hmac_sha256(session_key_, payload));
  if (!constant_time_equals(parts[2], expected)) return false;
  return static_cast<double>(expiry) > platform_.monotonic_s();
}

void LoginLimiter::prune(std::vector<double>& times, double now) const {
  times.erase(std::remove_if(times.begin(), times.end(), [now](double t) { return now - t >= kWindowS; }), times.end());
}

double LoginLimiter::retry_after(const std::string& client) {
  auto it = failures_.find(client);
  if (it == failures_.end()) return 0.0;
  const double now = clock_();
  prune(it->second, now);
  // Every stored entry has at least one failure (record_failure relies on that), so drop emptied ones.
  if (it->second.empty()) {
    failures_.erase(it);
    return 0.0;
  }
  if (it->second.size() < static_cast<size_t>(kMaxFailures)) return 0.0;
  return std::max(0.0, it->second.back() + kBlockS - now);
}

void LoginLimiter::record_failure(const std::string& client) {
  const double now = clock_();
  std::vector<double>& times = failures_[client];
  prune(times, now);
  times.push_back(now);
  if (failures_.size() > kMaxClients) {
    // Forget the client whose latest failure is the oldest.
    auto oldest = failures_.begin();
    for (auto it = failures_.begin(); it != failures_.end(); ++it) {
      if (it->second.back() < oldest->second.back()) oldest = it;
    }
    failures_.erase(oldest);
  }
}

void LoginLimiter::record_success(const std::string& client) { failures_.erase(client); }

}  // namespace dispatcher
