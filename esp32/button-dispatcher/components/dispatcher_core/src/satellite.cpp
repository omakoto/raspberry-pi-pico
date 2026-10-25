// Satellite reports, the dispatcher's satellite list, and the satellite's forwarder. See dispatcher/satellite.hpp.

#include "dispatcher/satellite.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "dispatcher/beeps.hpp"
#include "dispatcher/bthome.hpp"
#include "dispatcher/models.hpp"
#include "dispatcher/util.hpp"

namespace dispatcher {

namespace {

constexpr size_t kMaxBootLength = 32;

template <typename T>
Parsed<T> ok(T value) {
  return Parsed<T>{std::optional<T>(std::move(value)), std::string()};
}

template <typename T>
Parsed<T> fail(std::string message) {
  return Parsed<T>{std::nullopt, std::move(message)};
}

const cJSON* get(const cJSON* object, const char* key) { return cJSON_GetObjectItemCaseSensitive(object, key); }

std::string print(const cJSON* json) {
  char* text = cJSON_PrintUnformatted(json);
  std::string out = text != nullptr ? text : "";
  cJSON_free(text);
  return out;
}

// A whole number in [low, high], or nothing. JSON numbers are doubles, which hold every integer used here.
std::optional<int64_t> integer_in(const cJSON* item, double low, double high) {
  if (!cJSON_IsNumber(item)) return std::nullopt;
  const double value = item->valuedouble;
  if (!std::isfinite(value) || value != std::floor(value) || value < low || value > high) return std::nullopt;
  return static_cast<int64_t>(value);
}

// Reads an optional counter; missing means 0.
bool read_counter(const cJSON* object, const char* key, int64_t* out) {
  const cJSON* item = get(object, key);
  if (item == nullptr) return true;
  const std::optional<int64_t> value = integer_in(item, 0, 9007199254740991.0);
  if (!value) return false;
  *out = *value;
  return true;
}

bool is_hex(const std::string& text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
}

std::string lower(const std::string& text) {
  std::string out = text;
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool has_control_char(const std::string& s) {
  return std::any_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; });
}

size_t char_count(const std::string& s) {
  return static_cast<size_t>(std::count_if(s.begin(), s.end(), [](char c) { return (c & 0xC0) != 0x80; }));
}

}  // namespace

// ---- the report --------------------------------------------------------------------------------------

std::string report_to_text(const Report& report) {
  JsonPtr json = make_json(cJSON_CreateObject());
  cJSON_AddNumberToObject(json.get(), "time", static_cast<double>(report.time_ms));
  cJSON_AddStringToObject(json.get(), "boot", report.boot.c_str());
  cJSON_AddNumberToObject(json.get(), "seq", static_cast<double>(report.seq));
  cJSON_AddNumberToObject(json.get(), "uptime_s", static_cast<double>(report.uptime_s));
  cJSON_AddNumberToObject(json.get(), "dropped", static_cast<double>(report.dropped));
  cJSON_AddNumberToObject(json.get(), "queued", static_cast<double>(report.queued));
  cJSON* events = cJSON_AddArrayToObject(json.get(), "events");
  for (const ReportEvent& event : report.events) {
    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "mac", event.mac.c_str());
    if (event.rssi) {
      cJSON_AddNumberToObject(item, "rssi", *event.rssi);
    } else {
      cJSON_AddNullToObject(item, "rssi");
    }
    cJSON_AddStringToObject(item, "data", hex_encode(std::string(event.data.begin(), event.data.end())).c_str());
    cJSON_AddNumberToObject(item, "age_ms", event.age_ms);
    cJSON_AddItemToArray(events, item);
  }
  return print(json.get());
}

Parsed<Report> report_from_text(const std::string& text) {
  if (text.size() > kMaxReportBytes) return fail<Report>("report too large");
  JsonPtr json = make_json(cJSON_ParseWithOpts(text.c_str(), nullptr, true));
  if (!json || !cJSON_IsObject(json.get())) return fail<Report>("the report is not a JSON object");
  Report report;

  const std::optional<int64_t> time = integer_in(get(json.get(), "time"), 0, 9007199254740991.0);
  if (!time) return fail<Report>("the report needs a time");
  report.time_ms = *time;
  const cJSON* boot = get(json.get(), "boot");
  if (!cJSON_IsString(boot) || !is_hex(boot->valuestring) || std::char_traits<char>::length(boot->valuestring) > kMaxBootLength) {
    return fail<Report>("the report needs a boot id");
  }
  report.boot = lower(boot->valuestring);
  const std::optional<int64_t> seq = integer_in(get(json.get(), "seq"), 0, 9007199254740991.0);
  if (!seq) return fail<Report>("the report needs a sequence number");
  report.seq = *seq;
  if (!read_counter(json.get(), "uptime_s", &report.uptime_s) || !read_counter(json.get(), "dropped", &report.dropped) ||
      !read_counter(json.get(), "queued", &report.queued)) {
    return fail<Report>("uptime_s, dropped and queued must be whole numbers");
  }

  const cJSON* events = get(json.get(), "events");
  if (events != nullptr) {
    if (!cJSON_IsArray(events)) return fail<Report>("events must be a list");
    if (static_cast<size_t>(cJSON_GetArraySize(events)) > kMaxReportEvents) {
      return fail<Report>("a report can carry at most " + std::to_string(kMaxReportEvents) + " events");
    }
    for (const cJSON* item = events->child; item != nullptr; item = item->next) {
      if (!cJSON_IsObject(item)) return fail<Report>("each event must be an object");
      ReportEvent event;
      const cJSON* mac = get(item, "mac");
      const std::optional<std::string> normalized =
          cJSON_IsString(mac) ? normalize_mac(mac->valuestring) : std::nullopt;
      if (!normalized) return fail<Report>("an event has an invalid MAC address");
      event.mac = *normalized;
      const cJSON* rssi = get(item, "rssi");
      if (rssi != nullptr && !cJSON_IsNull(rssi)) {
        const std::optional<int64_t> value = integer_in(rssi, -127, 20);
        if (!value) return fail<Report>("an event has an invalid RSSI");
        event.rssi = static_cast<int>(*value);
      }
      const cJSON* data = get(item, "data");
      const std::optional<std::string> bytes = cJSON_IsString(data) ? hex_decode(data->valuestring) : std::nullopt;
      if (!bytes || bytes->empty() || bytes->size() > kMaxEventPayload) {
        return fail<Report>("an event's data must be 1 to " + std::to_string(kMaxEventPayload) + " bytes of hex");
      }
      event.data.assign(bytes->begin(), bytes->end());
      const std::optional<int64_t> age = integer_in(get(item, "age_ms"), 0, 24 * 3600 * 1000.0);
      if (!age) return fail<Report>("an event needs its age_ms");
      event.age_ms = static_cast<int>(*age);
      report.events.push_back(std::move(event));
    }
  }
  return ok(std::move(report));
}

// ---- the dispatcher's side ---------------------------------------------------------------------------

const SatelliteRecord* SatelliteRegistry::find(const std::string& id) const {
  for (const SatelliteRecord& record : records) {
    if (record.id == id) return &record;
  }
  return nullptr;
}

std::string SatelliteRegistry::check_name(const std::string& name) const {
  if (name.empty()) return "the satellite needs a name";
  if (char_count(name) > kMaxNameLength) {
    return "the satellite's name can have at most " + std::to_string(kMaxNameLength) + " characters";
  }
  if (has_control_char(name)) return "the satellite's name cannot contain control characters";
  for (const SatelliteRecord& record : records) {
    if (lower(record.name) == lower(name)) return "there is already a satellite called \"" + record.name + "\"";
  }
  return "";
}

Parsed<std::vector<SatelliteRecord>> SatelliteRegistry::parse(const std::string& text) const {
  using Result = std::vector<SatelliteRecord>;
  JsonPtr json = make_json(cJSON_Parse(text.c_str()));
  if (!json || !cJSON_IsObject(json.get())) return fail<Result>("invalid JSON");
  const cJSON* list = get(json.get(), "satellites");
  if (!cJSON_IsArray(list)) return fail<Result>("satellites must be a list");
  Result result;
  for (const cJSON* item = list->child; item != nullptr; item = item->next) {
    const cJSON* id = get(item, "id");
    const cJSON* name = get(item, "name");
    const cJSON* secret = get(item, "secret");
    if (!cJSON_IsString(id) || !cJSON_IsString(name) || !cJSON_IsString(secret)) {
      return fail<Result>("each satellite needs an id, a name and a secret");
    }
    const std::optional<std::string> raw = hex_decode(secret->valuestring);
    if (!raw || raw->size() != kSecretBytes) return fail<Result>("a satellite's secret is damaged");
    result.push_back({id->valuestring, name->valuestring, *raw});
  }
  if (result.size() > kMaxSatellites) result.resize(kMaxSatellites);
  return ok(std::move(result));
}

std::string SatelliteRegistry::to_text() const {
  JsonPtr json = make_json(cJSON_CreateObject());
  cJSON_AddNumberToObject(json.get(), "version", 1);
  cJSON* list = cJSON_AddArrayToObject(json.get(), "satellites");
  for (const SatelliteRecord& record : records) {
    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "id", record.id.c_str());
    cJSON_AddStringToObject(item, "name", record.name.c_str());
    cJSON_AddStringToObject(item, "secret", hex_encode(record.secret).c_str());
    cJSON_AddItemToArray(list, item);
  }
  return print(json.get());
}

bool SatelliteRegistry::is_fresh(const SatelliteStatus& status, const Report& report) {
  if (!status.seen) return true;
  // Within one boot the sequence number only grows. A new boot must also be newer than anything accepted,
  // which rules out replaying a request recorded during an earlier boot.
  if (report.boot == status.boot) return report.seq > status.seq;
  return report.time_ms > status.last_time_ms;
}

// ---- the satellite's side ----------------------------------------------------------------------------

Forwarder::Offer Forwarder::offer(const std::string& mac_text, std::optional<int> rssi, const uint8_t* data,
                                  size_t length, double now) {
  Offer offer;
  if (length == 0 || length > kMaxEventPayload) return offer;
  const std::optional<std::string> mac = normalize_mac(mac_text);
  if (!mac) return offer;
  const std::optional<bthome::Packet> packet = bthome::decode(data, length);
  if (!packet) return offer;
  const std::vector<bthome::InputEvent> events = bthome::input_events(*packet);
  if (!packet->encrypted && events.empty()) return offer;
  if (dedupe_.is_duplicate(*mac, *packet, now)) return offer;

  Queued queued;
  queued.serial = next_serial_++;
  queued.event.mac = *mac;
  queued.event.rssi = rssi;
  queued.event.data.assign(data, data + length);
  queued.heard_at = now;
  queued.known = is_known(*mac);
  queue_.push_back(std::move(queued));
  while (queue_.size() > kMaxQueue) drop_front();

  offer.queued = true;
  if (is_known(*mac)) {
    offer.ack_beeps = 1;
    for (const bthome::InputEvent& event : events) {
      offer.ack_beeps = std::max(offer.ack_beeps, click_count(event.event));
    }
  }
  return offer;
}

bool Forwarder::is_known(const std::string& mac) const {
  return std::find(known_devices_.begin(), known_devices_.end(), mac) != known_devices_.end();
}

void Forwarder::drop_front() {
  if (queue_.front().known) known_lost_ = true;
  queue_.pop_front();
  ++dropped_;
}

bool Forwarder::take_known_lost() {
  const bool lost = known_lost_;
  known_lost_ = false;
  return lost;
}

void Forwarder::expire(double now) {
  while (!queue_.empty() && now - queue_.front().heard_at > kMaxEventAgeS) drop_front();
}

std::optional<Report> Forwarder::next_report(double now, int64_t wall_ms, double* wait_s) {
  expire(now);
  const bool events_due = !queue_.empty() && now >= retry_at_;
  const bool heartbeat_due = now >= next_heartbeat_;
  if (!events_due && !heartbeat_due) {
    double wait = next_heartbeat_ - now;
    if (!queue_.empty()) wait = std::min(wait, retry_at_ - now);
    *wait_s = std::max(0.0, wait);
    return std::nullopt;
  }

  Report report;
  report.time_ms = wall_ms;
  report.boot = boot_;
  report.seq = ++seq_;
  report.uptime_s = static_cast<int64_t>(now);
  report.dropped = dropped_;
  in_flight_up_to_ = 0;
  in_flight_events_ = 0;
  for (const Queued& queued : queue_) {
    if (report.events.size() >= kMaxReportEvents) break;
    ReportEvent event = queued.event;
    event.age_ms = static_cast<int>(std::lround((now - queued.heard_at) * 1000.0));
    report.events.push_back(std::move(event));
    in_flight_up_to_ = queued.serial;
    ++in_flight_events_;
  }
  report.queued = static_cast<int64_t>(queue_.size() - report.events.size());
  *wait_s = 0;
  return report;
}

bool Forwarder::report_done(bool ok, const std::string& error, double now, size_t accepted,
                            const std::optional<std::vector<std::string>>& devices) {
  bool delivered = false;
  if (ok) {
    // New events may have been queued, and old ones dropped, while the report was on its way.
    bool had_known = false;
    while (!queue_.empty() && queue_.front().serial <= in_flight_up_to_) {
      had_known = had_known || queue_.front().known;
      queue_.pop_front();
      ++forwarded_;
    }
    // The dispatcher doesn't say which events it refused, so a known press may be among them.
    if (accepted < in_flight_events_ && had_known) known_lost_ = true;
    delivered = had_known && accepted >= in_flight_events_;
    if (devices) {
      known_devices_.clear();
      for (const std::string& text : *devices) {
        if (const std::optional<std::string> mac = normalize_mac(text)) known_devices_.push_back(*mac);
      }
    }
    last_ok_ = now;
    failures_ = 0;
    next_heartbeat_ = now + kHeartbeatS;
    retry_at_ = now;
  } else {
    ++failures_;
    last_error_ = error;
    last_error_at_ = now;
    retry_at_ = now + kRetryS;
    next_heartbeat_ = now + kIdleRetryS;
  }
  in_flight_up_to_ = 0;
  in_flight_events_ = 0;
  return delivered;
}

void Forwarder::reset(double now) {
  queue_.clear();
  in_flight_up_to_ = 0;
  in_flight_events_ = 0;
  // The devices belong to the dispatcher the satellite was paired with; a new one sends its own.
  known_devices_.clear();
  known_lost_ = false;
  next_heartbeat_ = now;
  retry_at_ = now;
  failures_ = 0;
  last_ok_ = -1;
  last_error_.clear();
  last_error_at_ = 0;
}

}  // namespace dispatcher
