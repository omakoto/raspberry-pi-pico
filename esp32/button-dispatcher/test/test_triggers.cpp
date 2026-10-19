// Tests for triggers.cpp (dedupe, wheel handling, matching, cooldown), using real captured packets.

#include <cstdio>
#include <string>
#include <vector>

#include "dispatcher/triggers.hpp"
#include "minitest.hpp"
#include "test_util.hpp"

using namespace dispatcher;

namespace {

const char* const kDesk = "AA:BB:CC:DD:EE:01";
const char* const kOther = "AA:BB:CC:DD:EE:02";

// Real packets from a BLU RC Button 4 (four button slots)...
const char* const kPressB1 = "44001f01643a013a003a003a00";    // id 31, button 1 press
const char* const kDoubleB1 = "44002001643a023a003a003a00";   // id 32, button 1 double press
const char* const kPressB2 = "44002101643a003a013a003a00";    // id 33, button 2 press
const char* const kHoldB4 = "44002701643a003a003a003a80";     // id 39, button 4 hold
const char* const kPressB1Id47 = "44002f01643a013a003a003a00";  // id 47, button 1 press
// ...and from a BLU Remote Control (two button slots, wheel, channel).
const char* const kRcB1Ch0 = "44005901643a013a006000";        // id 89, button 1, channel 0
const char* const kRcB1Ch1 = "44005f01643a013a006001";        // id 95, button 1, channel 1
const char* const kWheelLeftStart = "44005b01643c01006000";   // id 91, left, 0 steps, channel 0
const char* const kWheelLeftFinal = "44005c01643c01026000";   // id 92, left, 2 steps
const char* const kWheelRightStart = "44005d01643c02006000";  // id 93, right, 0 steps

struct FakeClock {
  double now = 1000.0;
};

Binding make_binding(const std::string& id, const std::string& input, const std::string& event,
                     const std::string& button = "desk", std::optional<int> channel = std::nullopt,
                     int cooldown_ms = 700, bool enabled = true) {
  Binding binding;
  binding.id = id;
  binding.name = id;
  binding.enabled = enabled;
  binding.cooldown_ms = cooldown_ms;
  binding.trigger = {button, input, event, channel};
  HttpAction http;
  http.url = "http://example.invalid/" + id;
  binding.action = http;
  return binding;
}

std::string fired_ids(const ProcessResult& result) {
  std::string out;
  for (const Firing& f : result.fired) {
    if (!out.empty()) out += ",";
    out += f.binding.id;
  }
  return out;
}

// An engine over a fake clock with two buttons configured.
struct Fixture {
  FakeClock clock;
  TriggerEngine engine{[this] { return clock.now; }};

  Fixture() {}

  void configure(std::vector<Binding> bindings) {
    engine.configure({{"desk", "Desk remote", kDesk, "SBBT-104CUS"}, {"other", "Other remote", kOther, ""}},
                     std::move(bindings));
  }

  ProcessResult press(const char* hex, const char* mac = kDesk, std::optional<int> rssi = -40) {
    return engine.process(mac, rssi, test_util::from_hex(hex));
  }
};

// A press packet with the given id on button 1 of a four-button remote.
std::string press_with_id(int id) {
  char text[40];
  std::snprintf(text, sizeof(text), "4400%02x01643a013a003a003a00", id);
  return text;
}

}  // namespace

// ---- dedupe ----

TEST(triggers_repeats_of_one_event_fire_once) {
  Fixture f;
  f.configure({make_binding("wake", "button:1", "press")});
  CHECK_EQ(fired_ids(f.press(kPressB1)), std::string("wake"));
  for (int i = 0; i < 20; ++i) {
    f.clock.now += 0.05;
    const ProcessResult again = f.press(kPressB1);
    CHECK(again.duplicate);
    CHECK(again.fired.empty());
  }
}

TEST(triggers_new_packet_id_is_a_new_event) {
  Fixture f;
  f.configure({make_binding("wake", "button:1", "press", "desk", std::nullopt, 0)});
  CHECK_EQ(fired_ids(f.press(kPressB1)), std::string("wake"));
  f.clock.now += 2;
  CHECK_EQ(fired_ids(f.press(kPressB1Id47)), std::string("wake"));
}

TEST(triggers_same_packet_id_after_the_window_is_new) {
  Fixture f;
  f.configure({make_binding("wake", "button:1", "press", "desk", std::nullopt, 0)});
  CHECK_EQ(fired_ids(f.press(kPressB1)), std::string("wake"));
  f.clock.now += TriggerEngine::kDedupeWindowS + 1;
  CHECK_EQ(fired_ids(f.press(kPressB1)), std::string("wake"));
}

TEST(triggers_burst_longer_than_the_window_stays_one_event) {
  Fixture f;
  f.configure({make_binding("wake", "button:1", "press", "desk", std::nullopt, 0)});
  CHECK_EQ(fired_ids(f.press(kPressB1)), std::string("wake"));
  for (int i = 0; i < 8; ++i) {
    f.clock.now += 2;
    CHECK(f.press(kPressB1).duplicate);
  }
}

TEST(triggers_dedupe_is_per_device) {
  Fixture f;
  f.configure({make_binding("a", "button:1", "press", "desk"), make_binding("b", "button:1", "press", "other")});
  CHECK_EQ(fired_ids(f.press(kPressB1, kDesk)), std::string("a"));
  // The same packet id from another device is a different event.
  CHECK_EQ(fired_ids(f.press(kPressB1, kOther)), std::string("b"));
}

TEST(triggers_tracked_devices_are_bounded) {
  Fixture f;
  f.configure({});
  for (size_t i = 0; i < TriggerEngine::kMaxTrackedDevices + 50; ++i) {
    char mac[24];
    std::snprintf(mac, sizeof(mac), "AA:BB:CC:00:%02X:%02X", static_cast<unsigned>(i / 256), static_cast<unsigned>(i % 256));
    f.press(kPressB1, mac);
  }
  CHECK(f.engine.tracked_devices() <= TriggerEngine::kMaxTrackedDevices);
}

// Several receivers (for example satellites) can deliver the same events late and out of order.
TEST(triggers_a_late_copy_of_an_earlier_event_is_still_a_duplicate) {
  Fixture f;
  f.configure({make_binding("wake", "button:1", "press", "desk", std::nullopt, 0)});
  // Receiver A hears events 128 and 129; receiver B's copy of 128 arrives afterwards.
  CHECK_EQ(fired_ids(f.press(press_with_id(128).c_str())), std::string("wake"));
  f.clock.now += 0.3;
  CHECK_EQ(fired_ids(f.press(press_with_id(129).c_str())), std::string("wake"));
  f.clock.now += 0.05;
  CHECK(f.press(press_with_id(128).c_str()).duplicate);
  CHECK(f.press(press_with_id(129).c_str()).duplicate);
  // A genuinely new event still gets through.
  f.clock.now += 0.3;
  CHECK_EQ(fired_ids(f.press(press_with_id(130).c_str())), std::string("wake"));
}

TEST(triggers_only_the_most_recent_ids_are_remembered) {
  Fixture f;
  f.configure({make_binding("wake", "button:1", "press", "desk", std::nullopt, 0)});
  for (size_t id = 1; id <= TriggerEngine::kRecentIdsPerDevice + 1; ++id) {
    f.press(press_with_id(static_cast<int>(id)).c_str());
    f.clock.now += 0.1;
  }
  // Event 1 has been pushed out by newer ones, so a copy of it now looks new (and a recent one doesn't).
  CHECK(!f.press(press_with_id(1).c_str()).duplicate);
  CHECK(f.press(press_with_id(static_cast<int>(TriggerEngine::kRecentIdsPerDevice + 1)).c_str()).duplicate);
}

TEST(triggers_ids_expire_individually) {
  Fixture f;
  f.configure({make_binding("wake", "button:1", "press", "desk", std::nullopt, 0)});
  f.press(press_with_id(10).c_str());
  f.clock.now += TriggerEngine::kDedupeWindowS - 1;
  f.press(press_with_id(11).c_str());
  f.clock.now += 2;  // id 10 is now older than the window; id 11 is not
  CHECK(!f.press(press_with_id(10).c_str()).duplicate);
  CHECK(f.press(press_with_id(11).c_str()).duplicate);
}

// ---- matching ----

TEST(triggers_button_and_event_must_match) {
  Fixture f;
  f.configure({make_binding("b1press", "button:1", "press"), make_binding("b1double", "button:1", "double_press"),
               make_binding("b2press", "button:2", "press"), make_binding("b4hold", "button:4", "hold_press")});
  CHECK_EQ(fired_ids(f.press(kPressB1)), std::string("b1press"));
  CHECK_EQ(fired_ids(f.press(kDoubleB1)), std::string("b1double"));
  CHECK_EQ(fired_ids(f.press(kPressB2)), std::string("b2press"));
  CHECK_EQ(fired_ids(f.press(kHoldB4)), std::string("b4hold"));
}

TEST(triggers_nothing_fires_without_a_matching_binding) {
  Fixture f;
  f.configure({make_binding("b2press", "button:2", "press")});
  const ProcessResult result = f.press(kPressB1);
  CHECK(result.fired.empty());
  CHECK_EQ(result.observed.size(), static_cast<size_t>(1));
}

TEST(triggers_several_bindings_on_one_trigger_all_fire) {
  Fixture f;
  f.configure({make_binding("one", "button:1", "press"), make_binding("two", "button:1", "press")});
  CHECK_EQ(fired_ids(f.press(kPressB1)), std::string("one,two"));
}

TEST(triggers_disabled_binding_does_not_fire) {
  Fixture f;
  f.configure({make_binding("off", "button:1", "press", "desk", std::nullopt, 700, false),
               make_binding("on", "button:1", "press")});
  CHECK_EQ(fired_ids(f.press(kPressB1)), std::string("on"));
}

TEST(triggers_binding_for_a_deleted_button_is_ignored) {
  Fixture f;
  f.configure({make_binding("orphan", "button:1", "press", "gone")});
  CHECK(f.press(kPressB1).fired.empty());
}

TEST(triggers_unknown_device_is_observed_but_fires_nothing) {
  Fixture f;
  f.configure({make_binding("wake", "button:1", "press")});
  const ProcessResult result = f.press(kPressB1, "11:22:33:44:55:66");
  CHECK(!result.known_device);
  CHECK_EQ(result.observed.size(), static_cast<size_t>(1));
  CHECK(result.fired.empty());
}

TEST(triggers_channel_specific_and_any_channel) {
  Fixture f;
  f.configure({make_binding("ch1", "button:1", "press", "desk", 1),
               make_binding("any", "button:1", "press", "desk", std::nullopt, 0)});
  CHECK_EQ(fired_ids(f.press(kRcB1Ch0)), std::string("any"));
  CHECK_EQ(fired_ids(f.press(kRcB1Ch1)), std::string("ch1,any"));
}

TEST(triggers_channel_trigger_does_not_match_a_device_without_channel) {
  Fixture f;
  f.configure({make_binding("ch0", "button:1", "press", "desk", 0)});
  CHECK(f.press(kPressB1).fired.empty());
}

TEST(triggers_mac_comparison_is_case_insensitive) {
  Fixture f;
  f.configure({make_binding("wake", "button:1", "press")});
  CHECK_EQ(fired_ids(f.press(kPressB1, "aa:bb:cc:dd:ee:01")), std::string("wake"));
}

TEST(triggers_observed_event_carries_the_packet_details) {
  Fixture f;
  f.configure({});
  const ProcessResult result = f.press(kRcB1Ch1, kDesk, -55);
  CHECK_EQ(result.observed.size(), static_cast<size_t>(1));
  const Observed& o = result.observed[0];
  CHECK_EQ(o.mac, std::string(kDesk));
  CHECK_EQ(o.rssi, std::optional<int>(-55));
  CHECK_EQ(o.channel, std::optional<int>(1));
  CHECK_EQ(o.input, std::string("button:1"));
  CHECK_EQ(o.event, std::string("press"));
  CHECK_EQ(o.packet_id, std::optional<int>(95));
  CHECK_EQ(o.battery, std::optional<int>(100));
}

// ---- wheel ----

TEST(triggers_turn_fires_on_the_start_packet_only) {
  Fixture f;
  f.configure({make_binding("left", "wheel:left", "turn")});
  CHECK_EQ(fired_ids(f.press(kWheelLeftStart)), std::string("left"));
  f.clock.now += 0.5;
  const ProcessResult final_packet = f.press(kWheelLeftFinal);
  CHECK(final_packet.fired.empty());
  CHECK(final_packet.observed.empty());
  CHECK_EQ(final_packet.notes.size(), static_cast<size_t>(1));
  CHECK(final_packet.notes[0].find("final packet ignored") != std::string::npos);
}

TEST(triggers_wheel_direction_selects_the_binding) {
  Fixture f;
  f.configure({make_binding("left", "wheel:left", "turn"), make_binding("right", "wheel:right", "turn")});
  CHECK_EQ(fired_ids(f.press(kWheelRightStart)), std::string("right"));
}

TEST(triggers_wiggle_burst_is_absorbed_by_the_cooldown) {
  Fixture f;
  f.configure({make_binding("left", "wheel:left", "turn", "desk", std::nullopt, 700)});
  CHECK_EQ(fired_ids(f.press(kWheelLeftStart)), std::string("left"));
  f.clock.now += 0.12;
  const ProcessResult again = f.press("44008101643c01006000");  // id 129, left start
  CHECK(again.fired.empty());
  CHECK_EQ(again.suppressed.size(), static_cast<size_t>(1));
  CHECK_EQ(again.suppressed[0].first, std::string("left"));
  CHECK_EQ(again.suppressed[0].second, std::string("cooldown"));
}

TEST(triggers_cooldown_expires) {
  Fixture f;
  f.configure({make_binding("left", "wheel:left", "turn", "desk", std::nullopt, 700)});
  CHECK_EQ(fired_ids(f.press(kWheelLeftStart)), std::string("left"));
  f.clock.now += 0.8;
  CHECK_EQ(fired_ids(f.press("44008101643c01006000")), std::string("left"));
}

// ---- cooldown ----

TEST(triggers_zero_cooldown_fires_every_new_event) {
  Fixture f;
  f.configure({make_binding("wake", "button:1", "press", "desk", std::nullopt, 0)});
  CHECK_EQ(fired_ids(f.press(kPressB1)), std::string("wake"));
  CHECK_EQ(fired_ids(f.press(kPressB1Id47)), std::string("wake"));
}

TEST(triggers_cooldown_is_per_binding) {
  Fixture f;
  f.configure({make_binding("a", "button:1", "press", "desk", std::nullopt, 5000),
               make_binding("b", "button:1", "press", "desk", std::nullopt, 0)});
  f.press(kPressB1);
  f.clock.now += 1;
  const ProcessResult result = f.press(kPressB1Id47);
  CHECK_EQ(fired_ids(result), std::string("b"));
  CHECK_EQ(result.suppressed.size(), static_cast<size_t>(1));
  CHECK_EQ(result.suppressed[0].first, std::string("a"));
}

TEST(triggers_reconfiguring_keeps_the_cooldown_of_surviving_bindings) {
  Fixture f;
  f.configure({make_binding("a", "button:1", "press", "desk", std::nullopt, 5000)});
  f.press(kPressB1);
  f.clock.now += 1;
  f.configure({make_binding("a", "button:1", "press", "desk", std::nullopt, 5000)});
  CHECK(f.press(kPressB1Id47).fired.empty());
}

// ---- packets that cannot be used ----

TEST(triggers_encrypted_payload_is_reported) {
  Fixture f;
  f.configure({});
  const ProcessResult result = f.press("45aabbcc");
  CHECK(result.error.find("encrypted") != std::string::npos);
  CHECK(result.observed.empty());
}

TEST(triggers_empty_payload_is_reported) {
  Fixture f;
  f.configure({});
  CHECK(!f.engine.process(kDesk, -40, nullptr, 0).error.empty());
}

TEST(triggers_other_bthome_version_is_reported) {
  Fixture f;
  f.configure({});
  CHECK(f.press("24001f01643a01").error.find("version") != std::string::npos);
}

TEST(triggers_invalid_mac_is_reported) {
  Fixture f;
  f.configure({});
  CHECK(!f.press(kPressB1, "not-a-mac").error.empty());
}
