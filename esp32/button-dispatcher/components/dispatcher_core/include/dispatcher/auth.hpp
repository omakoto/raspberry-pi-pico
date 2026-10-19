// Password storage, session tokens and login throttling for the web page.
//
// Mirrors pi/button-dispatcher/button_dispatcher/auth.py with two deliberate differences: the password is
// hashed with PBKDF2-HMAC-SHA256 (mbedtls has no scrypt), and the session-signing key and the session
// clock live only in memory, so a restart signs everyone out.

#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "dispatcher/platform.hpp"

namespace dispatcher {

constexpr size_t kMinPasswordLength = 8;
constexpr size_t kMaxPasswordLength = 256;
constexpr double kSessionTtlS = 30.0 * 24 * 3600;

class AuthService {
 public:
  // Iterations are stored with the hash, so the default can be raised later without locking anyone out.
  static constexpr int kDefaultIterations = 20000;

  explicit AuthService(Platform& platform, int iterations = kDefaultIterations);

  // Reads the stored password hash, if any, and creates the session-signing key.
  void load();

  bool is_configured() const { return !password_hash_.empty(); }

  // Sets and stores a new password and ends every existing session. Returns an empty string on success,
  // or the message to show the user.
  std::string set_password(const std::string& password);

  bool verify_password(const std::string& password) const;

  // A signed token naming an expiry on the monotonic clock; stateless on the server side.
  std::string make_token();
  bool verify_token(const std::string& token) const;

 private:
  void new_session_key();

  Platform& platform_;
  int iterations_;
  std::string password_hash_;
  std::string session_key_;
};

// Blocks a client after repeated failed logins (guessing protection for a LAN service).
class LoginLimiter {
 public:
  using Clock = std::function<double()>;

  static constexpr int kMaxFailures = 5;
  static constexpr double kWindowS = 300.0;
  static constexpr double kBlockS = 60.0;
  static constexpr size_t kMaxClients = 256;

  explicit LoginLimiter(Clock clock) : clock_(std::move(clock)) {}

  // Seconds the client must wait before trying again (0 if it may try now).
  double retry_after(const std::string& client);
  void record_failure(const std::string& client);
  void record_success(const std::string& client);

  size_t tracked_clients() const { return failures_.size(); }

 private:
  void prune(std::vector<double>& times, double now) const;

  Clock clock_;
  std::map<std::string, std::vector<double>> failures_;
};

}  // namespace dispatcher
