// The "Press a button" flow: hear a button press, then ask for a second press to confirm it.
//
// Mirrors pi/button-dispatcher/button_dispatcher/learn.py. The first press picks a candidate; the
// second must come from the same device with the same channel, control and event, which guards against
// picking up a neighbor's button or a stray press. The second press has to be a separate press (a quick
// double press is a different event), and repeated copies of one press never count as a second press
// because the trigger engine drops them before they get here.

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "dispatcher/triggers.hpp"

namespace dispatcher {

// The wording for a button event in messages and the page's menus: "single press" for "press", "hold" for
// "hold_press", and so on; unknown events are returned unchanged.
std::string event_label(const std::string& event);

// A short human description such as "button 2, single press, channel 3".
std::string describe(const Observed& observed);

struct LearnCandidate {
  std::string mac;
  std::string name;  // the advertised name, if one was heard
  std::optional<int> rssi;
  std::optional<int> channel;
  std::optional<int> battery;
  std::string input;
  std::string event;
  std::string description;
};

struct LearnStatus {
  std::string phase;  // idle, waiting, confirm, confirmed, timeout
  std::string message;
  std::optional<LearnCandidate> candidate;
  double seconds_left = 0.0;
};

class LearnSession {
 public:
  // Seconds from a monotonic clock.
  using Clock = std::function<double()>;

  // Presses of other devices that arrive this soon after the first one compete for the candidate, and
  // the strongest signal wins; this is what picks the button next to the receiver over a neighbor's.
  static constexpr double kCollectWindowS = 0.7;
  static constexpr double kFirstPressTimeoutS = 30.0;
  static constexpr double kConfirmTimeoutS = 30.0;

  explicit LearnSession(Clock clock) : clock_(std::move(clock)) {}

  void start();
  void cancel();
  bool active() const { return phase_ == "waiting" || phase_ == "confirm"; }

  // Offers a heard user action; `name` is the advertised name of the device, if known.
  void feed(const Observed& observed, const std::string& name = "");

  LearnStatus status();

  const std::optional<Observed>& candidate() const { return candidate_; }
  const std::string& candidate_name() const { return candidate_name_; }

 private:
  void reset();
  void advance();

  Clock clock_;
  std::string phase_ = "idle";
  double deadline_ = 0.0;
  std::optional<double> collect_until_;
  // First press heard from each device during the collect window, in the order heard.
  std::vector<std::pair<Observed, std::string>> candidates_;
  std::optional<Observed> candidate_;
  std::string candidate_name_;
  std::string mismatch_;
};

}  // namespace dispatcher
