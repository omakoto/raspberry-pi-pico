// Tests for satellite.cpp (reports, the registry, the forwarder) and for PacketDedupe in triggers.hpp.

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "dispatcher/bthome.hpp"
#include "dispatcher/satellite.hpp"
#include "dispatcher/triggers.hpp"
#include "minitest.hpp"
#include "test_util.hpp"

using namespace dispatcher;

namespace {

const char* const kDesk = "AA:BB:CC:DD:EE:01";
const char* const kOther = "AA:BB:CC:DD:EE:02";
const char* const kPressB2 = "44002101643a003a013a003a00";       // id 33, button 2 press
const char* const kPressB2Again = "44002201643a003a013a003a00";  // id 34, button 2 press
const char* const kBatteryOnly = "4400210164";                    // id 33, battery 100 and nothing else
const char* const kDoubleB2 = "44002301643a003a023a003a00";       // id 35, button 2 double press
const char* const kEncrypted = "45aabbcc";                         // encrypted: only the dispatcher can read it

std::vector<uint8_t> bytes(const std::string& hex) { return test_util::from_hex(hex); }

bool offer(Forwarder& f, const std::string& hex, double now, const char* mac = kDesk,
           std::optional<int> rssi = -40) {
  const std::vector<uint8_t> data = bytes(hex);
  return f.offer(mac, rssi, data.data(), data.size(), now).queued;
}

// The acknowledgement beeps for an advertisement; -1 if it was not queued.
int ack_beeps(Forwarder& f, const std::string& hex, double now, const char* mac = kDesk) {
  const std::vector<uint8_t> data = bytes(hex);
  const Forwarder::Offer offer = f.offer(mac, -40, data.data(), data.size(), now);
  return offer.queued ? offer.ack_beeps : -1;
}

// A forwarder whose dispatcher has answered a heartbeat, listing kDesk (in lowercase, which is accepted).
Forwarder knowing_the_desk(double now = 1000.0) {
  Forwarder f("cafe");
  double wait = -1;
  f.next_report(now, 5, &wait);
  f.report_done(true, "", now, 0, std::vector<std::string>{"aa:bb:cc:dd:ee:01", "not a mac"});
  return f;
}

// "4400%02x01643a003a013a003a00" with the given packet id: a button 2 press.
std::string press_with_id(int id) {
  char hex[64];
  std::snprintf(hex, sizeof(hex), "4400%02x01643a003a013a003a00", id & 0xff);
  return hex;
}

ReportEvent make_event(const std::string& mac = kDesk) {
  ReportEvent event;
  event.mac = mac;
  event.rssi = -52;
  event.data = bytes(kPressB2);
  event.age_ms = 40;
  return event;
}

Report make_report() {
  Report report;
  report.time_ms = 1700000000000LL;
  report.boot = "ab12";
  report.seq = 7;
  report.uptime_s = 99;
  report.dropped = 3;
  report.queued = 2;
  report.events.push_back(make_event());
  return report;
}

// A report document with `events_json` as its events and the usual other members.
std::string doc(const std::string& events_json) {
  return "{\"time\":1700000000000,\"boot\":\"ab12\",\"seq\":1,\"events\":" + events_json + "}";
}

std::string event_doc(const std::string& mac = "AA:BB:CC:DD:EE:01", const std::string& rssi = "-40",
                      const std::string& data = "\"44002101643a003a013a003a00\"",
                      const std::string& age = "10") {
  std::string out = "{\"mac\":\"" + mac + "\"";
  if (!rssi.empty()) out += ",\"rssi\":" + rssi;
  if (!data.empty()) out += ",\"data\":" + data;
  if (!age.empty()) out += ",\"age_ms\":" + age;
  return out + "}";
}

}  // namespace

// ---- reports ----

TEST(satellite_report_round_trips) {
  Report report = make_report();
  ReportEvent second = make_event(kOther);
  second.rssi = std::nullopt;
  second.age_ms = 1234;
  second.data = bytes(kPressB2Again);
  report.events.push_back(second);

  const Parsed<Report> parsed = report_from_text(report_to_text(report));
  CHECK(parsed.ok());
  CHECK_EQ(parsed.value->time_ms, report.time_ms);
  CHECK_EQ(parsed.value->boot, std::string("ab12"));
  CHECK_EQ(parsed.value->seq, static_cast<int64_t>(7));
  CHECK_EQ(parsed.value->uptime_s, static_cast<int64_t>(99));
  CHECK_EQ(parsed.value->dropped, static_cast<int64_t>(3));
  CHECK_EQ(parsed.value->queued, static_cast<int64_t>(2));
  CHECK_EQ(parsed.value->events.size(), static_cast<size_t>(2));
  CHECK_EQ(parsed.value->events[0].mac, std::string(kDesk));
  CHECK(parsed.value->events[0].rssi == std::optional<int>(-52));
  CHECK(parsed.value->events[0].data == bytes(kPressB2));
  CHECK_EQ(parsed.value->events[0].age_ms, 40);
  CHECK_EQ(parsed.value->events[1].mac, std::string(kOther));
  CHECK(!parsed.value->events[1].rssi.has_value());
  CHECK(parsed.value->events[1].data == bytes(kPressB2Again));
  CHECK_EQ(parsed.value->events[1].age_ms, 1234);
}

TEST(satellite_report_normalizes_macs_and_lowercases_the_boot_id) {
  const Parsed<Report> parsed = report_from_text(
      "{\"time\":5,\"boot\":\"ABCDEF01\",\"seq\":1,\"events\":[" + event_doc("aa-bb-cc-dd-ee-0a") + "]}");
  CHECK(parsed.ok());
  CHECK_EQ(parsed.value->boot, std::string("abcdef01"));
  CHECK_EQ(parsed.value->events[0].mac, std::string("AA:BB:CC:DD:EE:0A"));
}

TEST(satellite_report_without_events_is_a_heartbeat) {
  const Parsed<Report> parsed = report_from_text("{\"time\":5,\"boot\":\"ab\",\"seq\":1}");
  CHECK(parsed.ok());
  CHECK(parsed.value->events.empty());
  const Parsed<Report> empty = report_from_text(doc("[]"));
  CHECK(empty.ok());
  CHECK(empty.value->events.empty());
}

TEST(satellite_report_rejects_bad_documents) {
  CHECK(!report_from_text("").ok());
  CHECK(!report_from_text("[]").ok());
  CHECK(!report_from_text("5").ok());
  CHECK(!report_from_text("{\"boot\":\"ab\",\"seq\":1}").ok());                // no time
  CHECK(!report_from_text("{\"time\":5,\"seq\":1}").ok());                     // no boot
  CHECK(!report_from_text("{\"time\":5,\"boot\":\"ab\"}").ok());               // no seq
  CHECK(!report_from_text("{\"time\":5,\"boot\":\"xyz\",\"seq\":1}").ok());    // non-hex boot
  CHECK(!report_from_text("{\"time\":5,\"boot\":\"\",\"seq\":1}").ok());
  CHECK(!report_from_text("{\"time\":\"5\",\"boot\":\"ab\",\"seq\":1}").ok());
  CHECK(!report_from_text("{\"time\":5,\"boot\":\"ab\",\"seq\":1.5}").ok());
  CHECK(!report_from_text("{\"time\":5,\"boot\":\"ab\",\"seq\":-1}").ok());
  CHECK(!report_from_text("{\"time\":5,\"boot\":\"ab\",\"seq\":1,\"dropped\":\"x\"}").ok());
  CHECK(!report_from_text("{\"time\":5,\"boot\":\"ab\",\"seq\":1,\"events\":{}}").ok());
  CHECK(!report_from_text("{\"time\":5,\"boot\":\"ab\",\"seq\":1,\"events\":[5]}").ok());
}

TEST(satellite_report_rejects_too_many_events) {
  std::string events = "[";
  for (int i = 0; i < 17; ++i) events += (i > 0 ? "," : "") + event_doc();
  CHECK(!report_from_text(doc(events + "]")).ok());
  std::string sixteen = "[";
  for (int i = 0; i < 16; ++i) sixteen += (i > 0 ? "," : "") + event_doc();
  CHECK(report_from_text(doc(sixteen + "]")).ok());
}

TEST(satellite_report_rejects_bad_events) {
  CHECK(!report_from_text(doc("[" + event_doc("not-a-mac") + "]")).ok());
  CHECK(!report_from_text(doc("[" + event_doc("") + "]")).ok());
  // rssi must be in [-127, 20].
  CHECK(report_from_text(doc("[" + event_doc("AA:BB:CC:DD:EE:01", "-127") + "]")).ok());
  CHECK(report_from_text(doc("[" + event_doc("AA:BB:CC:DD:EE:01", "20") + "]")).ok());
  CHECK(!report_from_text(doc("[" + event_doc("AA:BB:CC:DD:EE:01", "-128") + "]")).ok());
  CHECK(!report_from_text(doc("[" + event_doc("AA:BB:CC:DD:EE:01", "21") + "]")).ok());
  CHECK(!report_from_text(doc("[" + event_doc("AA:BB:CC:DD:EE:01", "\"loud\"") + "]")).ok());
  // data: 1 to 64 bytes of hex.
  CHECK(!report_from_text(doc("[" + event_doc("AA:BB:CC:DD:EE:01", "-40", "\"\"") + "]")).ok());
  CHECK(!report_from_text(doc("[" + event_doc("AA:BB:CC:DD:EE:01", "-40", "\"zz\"") + "]")).ok());
  CHECK(!report_from_text(doc("[" + event_doc("AA:BB:CC:DD:EE:01", "-40", "\"abc\"") + "]")).ok());
  CHECK(!report_from_text(doc("[" + event_doc("AA:BB:CC:DD:EE:01", "-40", "5") + "]")).ok());
  CHECK(!report_from_text(doc("[" + event_doc("AA:BB:CC:DD:EE:01", "-40", "") + "]")).ok());
  CHECK(report_from_text(doc("[" + event_doc("AA:BB:CC:DD:EE:01", "-40", "\"" + std::string(128, 'a') + "\"") + "]")).ok());
  CHECK(!report_from_text(doc("[" + event_doc("AA:BB:CC:DD:EE:01", "-40", "\"" + std::string(130, 'a') + "\"") + "]")).ok());
  // age_ms is required and must be a non-negative whole number.
  CHECK(!report_from_text(doc("[" + event_doc("AA:BB:CC:DD:EE:01", "-40", "\"aa\"", "") + "]")).ok());
  CHECK(!report_from_text(doc("[" + event_doc("AA:BB:CC:DD:EE:01", "-40", "\"aa\"", "-1") + "]")).ok());
}

TEST(satellite_report_rejects_oversize_text) {
  const std::string padded = "{\"time\":5,\"boot\":\"ab\",\"seq\":1,\"pad\":\"" + std::string(8 * 1024, 'x') + "\"}";
  CHECK(padded.size() > kMaxReportBytes);
  CHECK(!report_from_text(padded).ok());
  CHECK(report_from_text("{\"time\":5,\"boot\":\"ab\",\"seq\":1,\"pad\":\"" + std::string(1000, 'x') + "\"}").ok());
}

// ---- the registry ----

TEST(satellite_registry_find) {
  SatelliteRegistry registry;
  registry.records.push_back({"sat-1", "Porch", std::string(32, 'a')});
  registry.records.push_back({"sat-2", "Garage", std::string(32, 'b')});
  CHECK(registry.find("sat-2") != nullptr);
  CHECK_EQ(registry.find("sat-2")->name, std::string("Garage"));
  CHECK(registry.find("sat-3") == nullptr);
  CHECK(registry.find("") == nullptr);
}

TEST(satellite_registry_check_name) {
  SatelliteRegistry registry;
  CHECK(registry.check_name("").size() > 0);
  CHECK(registry.check_name(std::string(41, 'a')).size() > 0);
  CHECK_EQ(registry.check_name(std::string(40, 'a')), std::string());
  CHECK(registry.check_name("bad\nname").size() > 0);
  CHECK(registry.check_name("bad\tname").size() > 0);
  CHECK_EQ(registry.check_name("Porch"), std::string());
  registry.records.push_back({"sat-1", "Porch", std::string(32, 'a')});
  CHECK(registry.check_name("Porch").size() > 0);
  CHECK(registry.check_name("PORCH").size() > 0);
  CHECK_EQ(registry.check_name("Garage"), std::string());
}

TEST(satellite_registry_name_length_counts_characters_not_bytes) {
  SatelliteRegistry registry;
  std::string name;
  for (int i = 0; i < 40; ++i) name += "\xc3\xa9";  // 40 characters, 80 bytes
  CHECK_EQ(registry.check_name(name), std::string());
}

TEST(satellite_registry_round_trips) {
  SatelliteRegistry registry;
  registry.records.push_back({"sat-1", "Porch", std::string(32, '\x01')});
  std::string secret;
  for (int i = 0; i < 32; ++i) secret += static_cast<char>(255 - i);
  registry.records.push_back({"sat-2", "Garage \"east\"", secret});
  SatelliteRegistry loaded;
  const auto parsed = loaded.parse(registry.to_text());
  CHECK(parsed.ok());
  CHECK_EQ(parsed.value->size(), static_cast<size_t>(2));
  CHECK_EQ((*parsed.value)[0].id, std::string("sat-1"));
  CHECK_EQ((*parsed.value)[0].name, std::string("Porch"));
  CHECK_EQ((*parsed.value)[0].secret, std::string(32, '\x01'));
  CHECK_EQ((*parsed.value)[1].name, std::string("Garage \"east\""));
  CHECK_EQ((*parsed.value)[1].secret, secret);
}

TEST(satellite_registry_parse_rejects_damage) {
  SatelliteRegistry registry;
  CHECK(!registry.parse("").ok());
  CHECK(!registry.parse("[]").ok());
  CHECK(!registry.parse("{}").ok());
  const std::string good(64, 'a');
  CHECK(registry.parse("{\"satellites\":[{\"id\":\"s\",\"name\":\"n\",\"secret\":\"" + good + "\"}]}").ok());
  CHECK(!registry.parse("{\"satellites\":[{\"id\":\"s\",\"name\":\"n\",\"secret\":\"" + good.substr(2) + "\"}]}").ok());
  CHECK(!registry.parse("{\"satellites\":[{\"id\":\"s\",\"name\":\"n\",\"secret\":\"" + std::string(64, 'z') + "\"}]}").ok());
  CHECK(!registry.parse("{\"satellites\":[{\"id\":\"s\",\"name\":\"n\"}]}").ok());
  CHECK(!registry.parse("{\"satellites\":[{\"name\":\"n\",\"secret\":\"" + good + "\"}]}").ok());
}

TEST(satellite_is_fresh) {
  SatelliteStatus status;
  Report report;
  report.boot = "aa";
  report.seq = 1;
  report.time_ms = 1000;
  CHECK(SatelliteRegistry::is_fresh(status, report));  // never seen

  status.seen = true;
  status.boot = "aa";
  status.seq = 5;
  status.last_time_ms = 1000;
  // Same boot: the sequence number must grow.
  report.seq = 5;
  CHECK(!SatelliteRegistry::is_fresh(status, report));
  report.seq = 4;
  CHECK(!SatelliteRegistry::is_fresh(status, report));
  report.seq = 6;
  CHECK(SatelliteRegistry::is_fresh(status, report));
  // A new boot: the time must be newer, whatever the sequence number.
  report.boot = "bb";
  report.seq = 1;
  report.time_ms = 1000;
  CHECK(!SatelliteRegistry::is_fresh(status, report));
  report.time_ms = 999;
  CHECK(!SatelliteRegistry::is_fresh(status, report));
  report.time_ms = 1001;
  CHECK(SatelliteRegistry::is_fresh(status, report));
}

// ---- PacketDedupe ----

TEST(satellite_dedupe_recognizes_repeats_within_the_window) {
  PacketDedupe dedupe;
  const std::vector<uint8_t> raw = bytes(kPressB2);
  const auto packet = bthome::decode(raw.data(), raw.size());
  CHECK(packet.has_value());
  CHECK(!dedupe.is_duplicate(kDesk, *packet, 100.0));
  CHECK(dedupe.is_duplicate(kDesk, *packet, 100.5));
  CHECK(dedupe.is_duplicate(kDesk, *packet, 109.9));
  // The window slides with every copy, so a long burst of repeats stays one event.
  CHECK(dedupe.is_duplicate(kDesk, *packet, 119.0));
  // After a quiet window the same id counts as a new event (counter wrap or restarted device).
  CHECK(!dedupe.is_duplicate(kDesk, *packet, 130.5));
}

TEST(satellite_dedupe_new_ids_are_new_events) {
  PacketDedupe dedupe;
  const std::vector<uint8_t> a = bytes(kPressB2);
  const std::vector<uint8_t> b = bytes(kPressB2Again);
  const auto pa = bthome::decode(a.data(), a.size());
  const auto pb = bthome::decode(b.data(), b.size());
  CHECK(!dedupe.is_duplicate(kDesk, *pa, 100.0));
  CHECK(!dedupe.is_duplicate(kDesk, *pb, 100.1));
  // A late copy of the earlier event is still recognized.
  CHECK(dedupe.is_duplicate(kDesk, *pa, 100.2));
}

TEST(satellite_dedupe_remembers_ids_per_device) {
  PacketDedupe dedupe;
  const std::vector<uint8_t> a = bytes(kPressB2);
  const auto pa = bthome::decode(a.data(), a.size());
  CHECK(!dedupe.is_duplicate(kDesk, *pa, 100.0));
  CHECK(!dedupe.is_duplicate(kOther, *pa, 100.0));
  CHECK(dedupe.is_duplicate(kDesk, *pa, 100.1));
  CHECK(dedupe.is_duplicate(kOther, *pa, 100.1));
  CHECK_EQ(dedupe.tracked_devices(), static_cast<size_t>(2));
}

TEST(satellite_dedupe_bounds_the_tracked_devices) {
  PacketDedupe dedupe;
  const std::vector<uint8_t> a = bytes(kPressB2);
  const auto pa = bthome::decode(a.data(), a.size());
  for (int i = 0; i < 400; ++i) {
    char mac[32];
    std::snprintf(mac, sizeof(mac), "AA:BB:CC:DD:%02X:%02X", i / 256, i % 256);
    dedupe.is_duplicate(mac, *pa, 100.0);
  }
  CHECK_EQ(dedupe.tracked_devices(), PacketDedupe::kMaxTrackedDevices);
}

// ---- Forwarder ----

TEST(satellite_forwarder_queues_a_press_once) {
  Forwarder f("cafe");
  CHECK(offer(f, kPressB2, 1000.0));
  for (int i = 0; i < 20; ++i) CHECK(!offer(f, kPressB2, 1000.0 + i * 0.05));
  CHECK_EQ(f.queued(), static_cast<size_t>(1));
  CHECK(offer(f, kPressB2Again, 1001.0));
  CHECK_EQ(f.queued(), static_cast<size_t>(2));
}

TEST(satellite_forwarder_ignores_what_is_not_a_press) {
  Forwarder f("cafe");
  CHECK(!offer(f, kBatteryOnly, 1000.0));
  CHECK(!offer(f, "", 1000.0));
  CHECK(!offer(f, kPressB2, 1000.0, "not a mac"));
  CHECK(!offer(f, "00", 1000.0));  // no BTHome objects at all
  CHECK_EQ(f.queued(), static_cast<size_t>(0));
}

TEST(satellite_forwarder_first_report_is_a_heartbeat_with_the_queued_events) {
  Forwarder f("cafe");
  CHECK(offer(f, kPressB2, 1000.0, kDesk, -41));
  double wait = -1;
  const auto report = f.next_report(1000.04, 1700000000000LL, &wait);
  CHECK(report.has_value());
  CHECK_EQ(wait, 0.0);
  CHECK_EQ(report->boot, std::string("cafe"));
  CHECK_EQ(report->seq, static_cast<int64_t>(1));
  CHECK_EQ(report->time_ms, 1700000000000LL);
  CHECK_EQ(report->events.size(), static_cast<size_t>(1));
  CHECK_EQ(report->events[0].mac, std::string(kDesk));
  CHECK(report->events[0].rssi == std::optional<int>(-41));
  CHECK(report->events[0].data == bytes(kPressB2));
  CHECK_EQ(report->events[0].age_ms, 40);
  CHECK_EQ(report->queued, static_cast<int64_t>(0));
}

TEST(satellite_forwarder_first_report_is_due_even_with_nothing_queued) {
  Forwarder f("cafe");
  double wait = -1;
  const auto report = f.next_report(1000.0, 5, &wait);
  CHECK(report.has_value());
  CHECK(report->events.empty());
}

TEST(satellite_forwarder_success_removes_events_and_waits_for_the_heartbeat) {
  Forwarder f("cafe");
  offer(f, kPressB2, 1000.0);
  double wait = -1;
  CHECK(f.next_report(1000.1, 5, &wait).has_value());
  f.report_done(true, "", 1000.2);
  CHECK_EQ(f.queued(), static_cast<size_t>(0));
  CHECK_EQ(f.forwarded(), static_cast<int64_t>(1));
  CHECK(f.has_succeeded());
  CHECK(!f.failing());
  CHECK_EQ(f.last_ok(), 1000.2);
  CHECK(!f.next_report(1001.0, 5, &wait).has_value());
  CHECK(wait > 29.0 && wait <= 30.0);
  CHECK(!f.next_report(1030.1, 5, &wait).has_value());
  CHECK(f.next_report(1030.3, 5, &wait).has_value());
}

TEST(satellite_forwarder_a_new_press_is_sent_without_waiting_for_the_heartbeat) {
  Forwarder f("cafe");
  double wait = -1;
  f.next_report(1000.0, 5, &wait);
  f.report_done(true, "", 1000.0);
  offer(f, kPressB2, 1005.0);
  const auto report = f.next_report(1005.0, 5, &wait);
  CHECK(report.has_value());
  CHECK_EQ(report->events.size(), static_cast<size_t>(1));
}

TEST(satellite_forwarder_sequence_numbers_increase) {
  Forwarder f("cafe");
  double wait = -1;
  int64_t last = 0;
  for (int i = 0; i < 4; ++i) {
    const auto report = f.next_report(1000.0 + i * 31, 5, &wait);
    CHECK(report.has_value());
    CHECK(report->seq > last);
    last = report->seq;
    f.report_done(true, "", 1000.0 + i * 31);
  }
}

TEST(satellite_forwarder_failure_retries_after_a_second_when_events_are_queued) {
  Forwarder f("cafe");
  offer(f, kPressB2, 1000.0);
  double wait = -1;
  f.next_report(1000.0, 5, &wait);
  f.report_done(false, "cannot reach", 1000.1);
  CHECK_EQ(f.queued(), static_cast<size_t>(1));
  CHECK(f.failing());
  CHECK_EQ(f.failures(), 1);
  CHECK_EQ(f.last_error(), std::string("cannot reach"));
  CHECK_EQ(f.last_error_at(), 1000.1);
  CHECK(!f.next_report(1000.6, 5, &wait).has_value());
  CHECK(wait > 0.4 && wait <= 0.5 + 1e-9);
  const auto retry = f.next_report(1001.2, 5, &wait);
  CHECK(retry.has_value());
  CHECK_EQ(retry->events.size(), static_cast<size_t>(1));
  CHECK_EQ(retry->events[0].age_ms, 1200);
}

TEST(satellite_forwarder_failure_retries_after_five_seconds_when_idle) {
  Forwarder f("cafe");
  double wait = -1;
  f.next_report(1000.0, 5, &wait);
  f.report_done(false, "down", 1000.0);
  CHECK(!f.next_report(1002.0, 5, &wait).has_value());
  CHECK(wait > 2.9 && wait <= 3.0 + 1e-9);
  CHECK(!f.next_report(1004.9, 5, &wait).has_value());
  CHECK(f.next_report(1005.1, 5, &wait).has_value());
}

TEST(satellite_forwarder_success_after_failure_clears_the_failure) {
  Forwarder f("cafe");
  double wait = -1;
  f.next_report(1000.0, 5, &wait);
  f.report_done(false, "down", 1000.0);
  f.report_done(false, "down", 1001.0);
  CHECK_EQ(f.failures(), 2);
  f.next_report(1006.0, 5, &wait);
  f.report_done(true, "", 1006.0);
  CHECK(!f.failing());
  CHECK_EQ(f.failures(), 0);
}

TEST(satellite_forwarder_drops_events_older_than_five_seconds) {
  Forwarder f("cafe");
  offer(f, kPressB2, 1000.0);
  offer(f, kPressB2Again, 1004.0);
  double wait = -1;
  const auto report = f.next_report(1006.0, 5, &wait);
  CHECK(report.has_value());
  CHECK_EQ(f.dropped(), static_cast<int64_t>(1));
  CHECK_EQ(report->dropped, static_cast<int64_t>(1));
  CHECK_EQ(report->events.size(), static_cast<size_t>(1));
  CHECK_EQ(report->events[0].age_ms, 2000);
}

TEST(satellite_forwarder_expire_ages_events_out_while_nothing_can_be_sent) {
  Forwarder f("cafe");
  offer(f, kPressB2, 1000.0);
  f.expire(1004.0);
  CHECK_EQ(f.queued(), static_cast<size_t>(1));
  f.expire(1005.5);
  CHECK_EQ(f.queued(), static_cast<size_t>(0));
  CHECK_EQ(f.dropped(), static_cast<int64_t>(1));
}

TEST(satellite_forwarder_queue_overflow_drops_the_oldest) {
  Forwarder f("cafe");
  for (int i = 0; i < 40; ++i) CHECK(offer(f, press_with_id(i), 1000.0 + i * 0.01));
  CHECK_EQ(f.queued(), Forwarder::kMaxQueue);
  CHECK_EQ(f.dropped(), static_cast<int64_t>(8));
  double wait = -1;
  const auto report = f.next_report(1001.0, 5, &wait);
  CHECK(report.has_value());
  // The oldest eight (ids 0-7) are gone; the first report event is id 8.
  CHECK_EQ(report->events[0].data[2], static_cast<uint8_t>(8));
}

TEST(satellite_forwarder_events_offered_while_a_report_is_in_flight_survive_its_success) {
  Forwarder f("cafe");
  offer(f, kPressB2, 1000.0);
  double wait = -1;
  CHECK(f.next_report(1000.1, 5, &wait).has_value());
  offer(f, kPressB2Again, 1000.2);  // arrives while the report is on its way
  f.report_done(true, "", 1000.3);
  CHECK_EQ(f.forwarded(), static_cast<int64_t>(1));
  CHECK_EQ(f.queued(), static_cast<size_t>(1));
  const auto next = f.next_report(1000.4, 5, &wait);
  CHECK(next.has_value());
  CHECK_EQ(next->events.size(), static_cast<size_t>(1));
  CHECK(next->events[0].data == bytes(kPressB2Again));
}

TEST(satellite_forwarder_sends_at_most_16_events_per_report) {
  Forwarder f("cafe");
  for (int i = 0; i < 20; ++i) offer(f, press_with_id(i), 1000.0);
  double wait = -1;
  const auto first = f.next_report(1000.1, 5, &wait);
  CHECK(first.has_value());
  CHECK_EQ(first->events.size(), kMaxReportEvents);
  CHECK_EQ(first->queued, static_cast<int64_t>(4));
  f.report_done(true, "", 1000.2);
  CHECK_EQ(f.queued(), static_cast<size_t>(4));
  // The rest follow at once rather than waiting for the heartbeat.
  const auto second = f.next_report(1000.2, 5, &wait);
  CHECK(second.has_value());
  CHECK_EQ(second->events.size(), static_cast<size_t>(4));
  CHECK_EQ(second->queued, static_cast<int64_t>(0));
  CHECK(second->seq > first->seq);
}

TEST(satellite_forwarder_reset_clears_the_queue_and_makes_a_report_due) {
  Forwarder f("cafe");
  double wait = -1;
  f.next_report(1000.0, 5, &wait);
  f.report_done(false, "down", 1000.0);
  offer(f, kPressB2, 1001.0);
  f.reset(1002.0);
  CHECK_EQ(f.queued(), static_cast<size_t>(0));
  CHECK(!f.failing());
  CHECK(!f.has_succeeded());
  CHECK_EQ(f.last_error(), std::string());
  const auto report = f.next_report(1002.0, 5, &wait);
  CHECK(report.has_value());
  CHECK(report->events.empty());
}

// ---- Forwarder: what the buzzer says ----

TEST(satellite_forwarder_does_not_acknowledge_devices_the_dispatcher_has_not_listed) {
  Forwarder f("cafe");
  CHECK_EQ(ack_beeps(f, kPressB2, 1000.0), 0);  // no answer from the dispatcher yet
  Forwarder g = knowing_the_desk();
  CHECK_EQ(ack_beeps(g, kPressB2, 1001.0, kOther), 0);
  CHECK(g.is_known(kDesk));
  CHECK(!g.is_known(kOther));
}

TEST(satellite_forwarder_acknowledges_known_presses_like_the_dispatcher) {
  Forwarder f = knowing_the_desk();
  CHECK_EQ(ack_beeps(f, kPressB2, 1001.0), 1);
  CHECK_EQ(ack_beeps(f, kPressB2, 1001.1), -1);  // a repeat is neither queued nor acknowledged
  CHECK_EQ(ack_beeps(f, kDoubleB2, 1002.0), 2);
  CHECK_EQ(ack_beeps(f, kEncrypted, 1003.0), 1);
  CHECK_EQ(ack_beeps(f, kBatteryOnly, 1004.0), -1);
}

TEST(satellite_forwarder_reports_delivery_of_known_presses) {
  Forwarder f = knowing_the_desk();
  double wait = -1;
  offer(f, kPressB2, 1001.0);
  CHECK(f.next_report(1001.0, 5, &wait).has_value());
  CHECK(f.report_done(true, "", 1001.1, 1));
  CHECK(!f.take_known_lost());
}

TEST(satellite_forwarder_delivery_of_unknown_presses_or_heartbeats_is_silent) {
  Forwarder f = knowing_the_desk();
  double wait = -1;
  offer(f, kPressB2, 1001.0, kOther);
  CHECK(f.next_report(1001.0, 5, &wait).has_value());
  CHECK(!f.report_done(true, "", 1001.1, 1));
  CHECK(f.next_report(1040.0, 5, &wait).has_value());  // a heartbeat
  CHECK(!f.report_done(true, "", 1040.1, 0));
  CHECK(!f.take_known_lost());
}

TEST(satellite_forwarder_a_failed_attempt_is_not_yet_a_loss) {
  Forwarder f = knowing_the_desk();
  double wait = -1;
  offer(f, kPressB2, 1001.0);
  CHECK(f.next_report(1001.0, 5, &wait).has_value());
  CHECK(!f.report_done(false, "down", 1001.1));
  CHECK(!f.take_known_lost());
  // The retry gets through, so the press is delivered after all.
  CHECK(f.next_report(1002.2, 5, &wait).has_value());
  CHECK(f.report_done(true, "", 1002.3, 1));
  CHECK(!f.take_known_lost());
}

TEST(satellite_forwarder_known_presses_that_grow_too_old_are_lost_once) {
  Forwarder f = knowing_the_desk();
  offer(f, kPressB2, 1001.0);
  offer(f, kPressB2Again, 1001.5);
  f.expire(1007.0);
  CHECK_EQ(f.queued(), static_cast<size_t>(0));
  CHECK(f.take_known_lost());
  CHECK(!f.take_known_lost());
}

TEST(satellite_forwarder_unknown_presses_that_grow_too_old_are_not_a_loss) {
  Forwarder f = knowing_the_desk();
  offer(f, kPressB2, 1001.0, kOther);
  f.expire(1007.0);
  CHECK_EQ(f.dropped(), static_cast<int64_t>(1));
  CHECK(!f.take_known_lost());
}

TEST(satellite_forwarder_known_presses_refused_by_the_dispatcher_are_lost) {
  Forwarder f = knowing_the_desk();
  double wait = -1;
  offer(f, kPressB2, 1001.0);
  offer(f, kPressB2Again, 1001.1, kOther);
  CHECK(f.next_report(1001.2, 5, &wait).has_value());
  CHECK(!f.report_done(true, "", 1001.3, 1));  // one of the two refused, perhaps the known one
  CHECK(f.take_known_lost());
}

TEST(satellite_forwarder_known_presses_pushed_out_of_a_full_queue_are_lost) {
  Forwarder f = knowing_the_desk();
  offer(f, kPressB2, 1001.0);
  for (int i = 0; i < static_cast<int>(Forwarder::kMaxQueue); ++i) {
    char mac[32];
    std::snprintf(mac, sizeof(mac), "AA:BB:CC:00:00:%02X", i);
    offer(f, kPressB2, 1001.0, mac);
  }
  CHECK(f.take_known_lost());
}

TEST(satellite_forwarder_an_answer_without_devices_keeps_the_list) {
  Forwarder f = knowing_the_desk();
  double wait = -1;
  CHECK(f.next_report(1040.0, 5, &wait).has_value());
  f.report_done(true, "", 1040.1, 0);
  CHECK(f.is_known(kDesk));
  CHECK(f.next_report(1080.0, 5, &wait).has_value());
  f.report_done(true, "", 1080.1, 0, std::vector<std::string>{});
  CHECK(!f.is_known(kDesk));
}

TEST(satellite_forwarder_reset_forgets_the_devices_and_losses) {
  Forwarder f = knowing_the_desk();
  offer(f, kPressB2, 1001.0);
  f.expire(1007.0);
  f.reset(1008.0);
  CHECK(!f.is_known(kDesk));
  CHECK(!f.take_known_lost());
}
