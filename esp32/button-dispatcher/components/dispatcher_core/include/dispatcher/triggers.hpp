// Turns BTHome advertisements into fired bindings: dedupe, event extraction, matching, cooldown.
//
// Mirrors pi/button-dispatcher/button_dispatcher/triggers.py. The behaviors come from measuring real
// Shelly BLU remotes (see ~/history/reports/electronics/shelly-remote-switches.md):
//
// - A sender repeats every event's advertisement 15-30 times over about a second. The packet id
//   changes per event, so only the first copy of an id counts.
// - A wheel turn sends a "start" packet (0 steps) at once and a "final" packet with the step count
//   after the wheel stops, and reversing direction cancels the turn in progress. Firing on the start
//   packet is the fastest signal; the final packet is ignored so a turn does not fire twice.
// - Fast wiggles produce bursts of start packets, which the per-binding cooldown absorbs.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "dispatcher/bthome.hpp"
#include "dispatcher/models.hpp"

namespace dispatcher {

// One user action heard from a device (after dedupe).
struct Observed {
  std::string mac;
  std::optional<int> rssi;
  std::optional<int> channel;
  std::string input;
  std::string event;
  int steps = 0;
  std::optional<int> packet_id;
  std::optional<int> battery;
};

struct Firing {
  Binding binding;
  Observed observed;
};

// What happened to one advertisement.
struct ProcessResult {
  // An identical copy of an event that was already handled.
  bool duplicate = false;
  // Why the packet could not be used at all (empty, encrypted, unsupported version); empty if usable.
  std::string error;
  std::optional<bthome::Packet> packet;
  // User actions in the packet that can trigger bindings (wheel "final" packets are left out).
  std::vector<Observed> observed;
  std::vector<Firing> fired;
  // Bindings that matched but did not fire: (binding id, reason such as "cooldown").
  std::vector<std::pair<std::string, std::string>> suppressed;
  // Human-readable remarks for the event log.
  std::vector<std::string> notes;
  // Whether the sending device is one of the configured buttons.
  bool known_device = false;
};

// True if `binding`'s trigger matches an observed action from `button`.
bool trigger_matches(const Binding& binding, const Button& button, const Observed& observed);

class TriggerEngine {
 public:
  // Seconds from a monotonic clock.
  using Clock = std::function<double()>;

  // Repeats of one event arrive within a few seconds; an identical packet id seen again after this
  // long is a counter wrap or a restarted device, so it counts as a new event.
  static constexpr double kDedupeWindowS = 10.0;
  // A device's recent packet ids are remembered so that a late copy of an earlier event, for example
  // one heard by a second receiver, is still recognized after newer events have arrived.
  static constexpr size_t kRecentIdsPerDevice = 8;
  // Bound on remembered devices, so a flood of random addresses cannot grow memory without limit.
  static constexpr size_t kMaxTrackedDevices = 256;

  explicit TriggerEngine(Clock clock) : clock_(std::move(clock)) {}

  void configure(std::vector<Button> buttons, std::vector<Binding> bindings);
  bool is_known(const std::string& mac) const;

  // Handles one BTHome service-data payload heard from `mac`.
  ProcessResult process(const std::string& mac, std::optional<int> rssi, const uint8_t* data, size_t length);
  ProcessResult process(const std::string& mac, std::optional<int> rssi, const std::vector<uint8_t>& data) {
    return process(mac, rssi, data.data(), data.size());
  }

  // The enabled bindings an observed action matches, ignoring dedupe and cooldown.
  std::vector<Binding> matching_bindings(const Observed& observed) const;

  size_t tracked_devices() const { return devices_.size(); }

 private:
  struct Seen {
    std::string key;
    double time;
  };
  struct Device {
    std::string mac;
    std::vector<Seen> recent;
  };

  bool is_duplicate(const std::string& mac, const bthome::Packet& packet, double now);
  void fire(const Observed& observed, double now, ProcessResult& result);

  Clock clock_;
  std::vector<Button> buttons_;
  std::vector<Binding> bindings_;
  // Least recently used device last.
  std::list<Device> devices_;
  std::map<std::string, double> last_fired_;
};

}  // namespace dispatcher
