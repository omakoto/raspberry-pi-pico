// Satellites: boards placed near buttons that are out of the dispatcher's Bluetooth range. A satellite
// scans like the dispatcher does, and forwards each new press (the raw BTHome payload) to the dispatcher over
// Wi-Fi; the dispatcher handles it exactly like a press it heard itself.
//
// The protocol is one request, sent by the satellite for presses and, with no events, as a heartbeat:
//
//   POST /api/satellite/report
//   X-Satellite: <satellite id>
//   X-Signature: <hex HMAC-SHA256 of the body, keyed with the satellite's secret>
//   {"time": <Unix ms>, "boot": "<random hex>", "seq": <n>, "uptime_s": ..., "dropped": ..., "queued": ...,
//    "events": [{"mac": "AA:BB:CC:DD:EE:FF", "rssi": -52, "data": "<hex payload>", "age_ms": 40}]}
//
// The dispatcher answers {"ok": true, "accepted": <events it took>, "devices": ["AA:BB:CC:DD:EE:FF", ...]}, the
// MACs of its devices, so the satellite can beep for presses of those devices and not for every BTHome
// device in range.
//
// The signature proves the sender has the secret. `time` (both boards set their clocks with SNTP) limits how
// long a recorded request stays usable, and `boot` with `seq` (increasing within one boot) rejects replays
// inside that window without the dispatcher having to remember every request.
//
// Data flows one way, button -> satellite -> dispatcher, and nothing forwards what it receives, so there
// are no loops. Several satellites hearing the same press is fine: the trigger engine's dedupe drops copies.
//
// Run test/run.sh (which builds test_satellite.cpp) every time this file or satellite.cpp is touched.

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "dispatcher/config.hpp"
#include "dispatcher/triggers.hpp"

namespace dispatcher {

constexpr const char* kSatelliteReportPath = "/api/satellite/report";
constexpr const char* kSatelliteIdHeader = "X-Satellite";
constexpr const char* kSatelliteSignatureHeader = "X-Signature";
constexpr const char* kSatellitesDocument = "satellites.json";

// ---- the report --------------------------------------------------------------------------------------

struct ReportEvent {
  std::string mac;            // normalized
  std::optional<int> rssi;
  std::vector<uint8_t> data;  // the BTHome service-data payload
  int age_ms = 0;             // how long ago the satellite heard it
};

struct Report {
  int64_t time_ms = 0;
  std::string boot;
  int64_t seq = 0;
  int64_t uptime_s = 0;
  int64_t dropped = 0;  // events the satellite dropped since it started (queue full, or too old to send)
  int64_t queued = 0;   // events waiting at the satellite besides the ones in this report
  std::vector<ReportEvent> events;
};

constexpr size_t kMaxReportBytes = 8 * 1024;
// The answer to a report lists at most this many devices, so that it fits in kMaxReportAnswerBytes.
constexpr size_t kMaxReportedDevices = 128;
constexpr size_t kMaxReportAnswerBytes = 4 * 1024;
constexpr size_t kMaxReportEvents = 16;
constexpr size_t kMaxEventPayload = 64;

std::string report_to_text(const Report& report);
Parsed<Report> report_from_text(const std::string& text);

// ---- the dispatcher's side ---------------------------------------------------------------------------

struct SatelliteRecord {
  std::string id;
  std::string name;
  std::string secret;  // raw bytes
};

// What the dispatcher knows about a satellite from its reports; kept in memory only.
struct SatelliteStatus {
  bool seen = false;
  double last_seen = 0;     // monotonic
  std::string address;      // where the last report came from
  std::string boot;
  int64_t seq = 0;
  int64_t last_time_ms = 0;  // the newest accepted report time
  int64_t uptime_s = 0;
  int64_t dropped = 0;
  int64_t queued = 0;
  int64_t events = 0;        // events accepted since the dispatcher started
  int64_t stale = 0;         // events refused for being too old
  // The last refused report, for the page; refusals are also logged, at most once a minute.
  std::string last_error;
  double last_error_at = 0;  // monotonic
  double last_error_logged = -1e9;
};

class SatelliteRegistry {
 public:
  static constexpr size_t kMaxSatellites = 16;
  static constexpr size_t kMaxNameLength = 40;
  static constexpr size_t kSecretBytes = 32;
  // A report whose time differs from the dispatcher's clock by more than this is refused.
  static constexpr int64_t kMaxClockSkewMs = 30 * 1000;
  // Events heard longer ago than this are dropped: a late "wake the PC" should not fire at all.
  static constexpr int kMaxEventAgeMs = 5 * 1000;
  // A satellite heard from within this long counts as online (heartbeats come every 30 s).
  static constexpr double kOnlineS = 75.0;

  std::vector<SatelliteRecord> records;
  std::map<std::string, SatelliteStatus> status;  // by id

  const SatelliteRecord* find(const std::string& id) const;

  // Checks a new satellite's name: 1-40 characters, no control characters, unique (ignoring case).
  std::string check_name(const std::string& name) const;

  Parsed<std::vector<SatelliteRecord>> parse(const std::string& text) const;
  std::string to_text() const;

  // Whether a report with valid signature and time is new (not a replay or out of order) for `status`.
  static bool is_fresh(const SatelliteStatus& status, const Report& report);
};

// ---- the satellite's side ----------------------------------------------------------------------------

// The forwarding state machine: queues new presses, decides when to send and what, and keeps the status
// shown on the page. The caller (AppCore) does the sending and serializes calls with its mutex.
//
// It also decides what the satellite's buzzer says, for presses of the devices the dispatcher knows: an
// acknowledgement when a press is heard, then whether it reached the dispatcher.
class Forwarder {
 public:
  static constexpr size_t kMaxQueue = 32;
  static constexpr double kMaxEventAgeS = 5.0;
  static constexpr double kRetryS = 1.0;
  static constexpr double kHeartbeatS = 30.0;
  // While the dispatcher cannot be reached and nothing is queued, it is tried this often.
  static constexpr double kIdleRetryS = 5.0;

  explicit Forwarder(std::string boot) : boot_(std::move(boot)) {}

  struct Offer {
    // Whether the advertisement was queued: it reports a press, a turn or is encrypted (the dispatcher logs
    // those for known devices), and it is not a repeat of an event already queued or sent.
    bool queued = false;
    // The short beeps that acknowledge it, as the dispatcher would: 1 for a press, 2 for a double press, 3 for
    // a triple press, and 1 for an encrypted packet (which only the dispatcher can read). 0 for a device the
    // dispatcher doesn't know, since every BTHome device in range is forwarded (learn mode needs them).
    int ack_beeps = 0;
  };

  // Takes one advertisement.
  Offer offer(const std::string& mac, std::optional<int> rssi, const uint8_t* data, size_t length, double now);

  // The report to send now, if one is due; otherwise sets `*wait_s` to the time until the next one is due.
  // Events too old to be worth sending are dropped first.
  std::optional<Report> next_report(double now, int64_t wall_ms, double* wait_s);

  // The outcome of the report next_report returned last. `error` says why it failed; `accepted` is how many
  // of its events the dispatcher took (it refuses events that grew too old on the way), and `devices` the
  // dispatcher's devices from its answer, if it listed them. Returns true if the report delivered presses of
  // known devices, all of which the dispatcher took.
  bool report_done(bool ok, const std::string& error, double now, size_t accepted = SIZE_MAX,
                   const std::optional<std::vector<std::string>>& devices = std::nullopt);

  // True, once, after presses of known devices were lost: dropped for being too old to send or for lack of
  // room, or refused by the dispatcher.
  bool take_known_lost();

  bool is_known(const std::string& mac) const;

  // Drops queued events and makes the next report due at once (pairing or role changes).
  void reset(double now);

  // Ages queued events out while nothing can be sent (no pairing yet, or the clock is not set).
  void expire(double now);

  // ---- status ----
  size_t queued() const { return queue_.size(); }
  int64_t forwarded() const { return forwarded_; }
  int64_t dropped() const { return dropped_; }
  bool has_succeeded() const { return last_ok_ >= 0; }
  double last_ok() const { return last_ok_; }  // monotonic, -1 if never
  bool failing() const { return failures_ > 0; }
  int failures() const { return failures_; }
  const std::string& last_error() const { return last_error_; }
  double last_error_at() const { return last_error_at_; }

 private:
  struct Queued {
    uint64_t serial;
    ReportEvent event;
    double heard_at;
    bool known;  // a device of the dispatcher's when it was heard
  };

  void drop_front();

  std::string boot_;
  PacketDedupe dedupe_;
  std::deque<Queued> queue_;
  uint64_t next_serial_ = 1;
  uint64_t in_flight_up_to_ = 0;  // the newest serial in the report being sent
  size_t in_flight_events_ = 0;
  std::vector<std::string> known_devices_;  // normalized MACs
  bool known_lost_ = false;
  int64_t seq_ = 0;
  double next_heartbeat_ = 0;
  double retry_at_ = 0;
  int64_t forwarded_ = 0;
  int64_t dropped_ = 0;
  double last_ok_ = -1;
  int failures_ = 0;
  std::string last_error_;
  double last_error_at_ = 0;
};

}  // namespace dispatcher
