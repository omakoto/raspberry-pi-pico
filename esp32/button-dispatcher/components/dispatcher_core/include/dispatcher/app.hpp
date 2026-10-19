// The application: all the state, the Bluetooth entry point, and the actions.
//
// Mirrors AppState in pi/button-dispatcher/button_dispatcher/app.py. The web API (api.hpp) works on an AppCore.
//
// Locking: `mutex` guards the state members below. Hold it while reading or changing them, and never while
// calling anything that may block on the network (run_action, the SwitchBot service): those calls take it
// themselves for as long as they need it. std::mutex is not recursive, so code that holds `mutex` must not call
// a method documented as "locks internally".

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "dispatcher/auth.hpp"
#include "dispatcher/config.hpp"
#include "dispatcher/eventlog.hpp"
#include "dispatcher/learn.hpp"
#include "dispatcher/platform.hpp"
#include "dispatcher/switchbot.hpp"
#include "dispatcher/triggers.hpp"

namespace dispatcher {

struct ActionResult {
  bool ok = false;
  std::string summary;
};

class AppCore {
 public:
  static constexpr size_t kLogCapacity = 200;
  // Decode problems of a known device are logged at most this often per device.
  static constexpr double kErrorLogIntervalS = 60.0;
  // Only the start of an action's HTTP response is read; the status is all that is reported.
  static constexpr size_t kMaxActionResponseBytes = 4096;

  // `switchbot_api_base` is only changed by tests and the host harness, to talk to a local fake server.
  explicit AppCore(Platform& platform, int pbkdf2_iterations = AuthService::kDefaultIterations,
                   std::string switchbot_api_base = kSwitchBotApiBase);

  // Loads the stored config and password. If config.json exists but is invalid the app starts with an empty
  // config and `*error` (if given) says why; the invalid document is left alone until the next save.
  void init(std::string* error = nullptr);

  Platform& platform() { return platform_; }

  // ---- state: guard with `mutex` ----
  std::mutex mutex;
  Config config;
  TriggerEngine engine;
  LearnSession learn;
  EventLog log;
  AuthService auth;
  LoginLimiter limiter;
  SwitchBotService switchbot;
  // Advertised device names by MAC, when a scan has heard them (passive scans usually do not).
  std::map<std::string, std::string> names;

  // ---- operations ----

  // Saves the config document and applies the config to the trigger engine and the credential copy.
  // Call with `mutex` held. Returns false, setting `*error`, if the document could not be stored.
  bool commit_config(std::string* error);

  // The Bluetooth entry point: one BTHome service-data payload heard from `mac`. Locks internally. Actions of
  // bindings that fire are queued on the platform's background worker after the lock is released.
  void handle_packet(const std::string& mac, std::optional<int> rssi, const uint8_t* data, size_t length);

  // Runs an action and returns its result; blocks while talking to the network. Locks internally (briefly), so
  // the caller must not hold `mutex`. Does not log.
  ActionResult run_action(const Action& action);
  // Runs a binding's action and logs the outcome as "\"<binding name>\": <summary>". Locks internally.
  ActionResult run_binding(const Binding& binding);
  // Queues run_binding on the platform's background worker. Does not need `mutex`.
  void spawn_binding(const Binding& binding);

  // How a device is named in the log: the button's name, or "unknown device <mac>" (plus the advertised
  // name, if heard). Call with `mutex` held.
  std::string label_for(const std::string& mac) const;

  // The Unix time, in seconds, of an event logged at monotonic time `mono`. Computed when the entry is read,
  // so entries logged before the clock was set show the right time once it is.
  double wall_time_of(double mono) const;

 private:
  Platform& platform_;
  // A copy of the SwitchBot credentials for the service, which may be asked for them while `mutex` is held
  // elsewhere. Updated by init() and commit_config().
  std::mutex credentials_mutex_;
  std::pair<std::string, std::string> credentials_;
  std::map<std::string, double> last_error_log_;
};

}  // namespace dispatcher
