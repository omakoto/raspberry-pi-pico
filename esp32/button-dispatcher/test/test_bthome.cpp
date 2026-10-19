// Tests for bthome.cpp, using packets captured from real Shelly BLU remotes.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "dispatcher/bthome.hpp"
#include "minitest.hpp"
#include "test_util.hpp"

using dispatcher::bthome::decode;
using dispatcher::bthome::InputEvent;
using dispatcher::bthome::input_events;
using dispatcher::bthome::Packet;
using test_util::from_hex;

namespace {

Packet packet_of(const char* hex) {
  const std::vector<uint8_t> bytes = from_hex(hex);
  auto packet = decode(bytes.data(), bytes.size());
  if (!packet) {
    std::printf("  FAIL: %s did not decode\n", hex);
    ++minitest::failures();
    return Packet();
  }
  return *packet;
}

// "button:2/press/0 wheel:left/turn/2": a compact rendering of a packet's events for comparison.
std::string events_of(const char* hex) {
  std::string out;
  for (const InputEvent& e : input_events(packet_of(hex))) {
    if (!out.empty()) out += ' ';
    out += e.input + "/" + e.event + "/" + std::to_string(e.steps);
  }
  return out;
}

}  // namespace

// ---- BLU RC Button 4 (four button slots per packet) ----

TEST(bthome_four_button_remote_press) {
  const Packet p = packet_of("44001f01643a013a003a003a00");
  CHECK_EQ(p.version, 2);
  CHECK(!p.encrypted);
  CHECK(p.trigger_based);
  CHECK_EQ(p.packet_id, std::optional<int>(31));
  CHECK_EQ(p.battery, std::optional<int>(100));
  CHECK_EQ(p.channel, std::optional<int>());
  CHECK_EQ(p.buttons, (std::vector<int>{1, 0, 0, 0}));
  CHECK(p.complete);
}

TEST(bthome_four_button_remote_event_codes) {
  CHECK_EQ(events_of("44001f01643a013a003a003a00"), std::string("button:1/press/0"));
  CHECK_EQ(events_of("44002001643a023a003a003a00"), std::string("button:1/double_press/0"));
  CHECK_EQ(events_of("44002101643a003a033a003a00"), std::string("button:2/triple_press/0"));
  CHECK_EQ(events_of("44002301643a003a003a003a03"), std::string("button:4/triple_press/0"));
  CHECK_EQ(events_of("44002401643a803a003a003a00"), std::string("button:1/hold_press/0"));
}

TEST(bthome_no_event_in_any_slot) {
  CHECK_EQ(events_of("44001f01643a003a003a003a00"), std::string(""));
}

TEST(bthome_two_buttons_at_once) {
  CHECK_EQ(events_of("44001f01643a013a013a003a00"), std::string("button:1/press/0 button:2/press/0"));
}

TEST(bthome_unknown_event_code_gets_a_name) {
  CHECK_EQ(events_of("44001f01643a7f3a003a003a00"), std::string("button:1/event_0x7f/0"));
}

// ---- BLU Remote Control (two button slots, a wheel, and a channel) ----

TEST(bthome_remote_control_buttons_and_channel) {
  const Packet p = packet_of("44005901643a013a006000");
  CHECK_EQ(p.packet_id, std::optional<int>(89));
  CHECK_EQ(p.channel, std::optional<int>(0));
  CHECK_EQ(p.buttons, (std::vector<int>{1, 0}));
  CHECK_EQ(events_of("44005901643a013a006000"), std::string("button:1/press/0"));
  CHECK_EQ(events_of("44006001643a003a016001"), std::string("button:2/press/0"));
  CHECK_EQ(packet_of("44006001643a003a016001").channel, std::optional<int>(1));
}

TEST(bthome_remote_control_wheel_start_and_final_packets) {
  // Start packet: direction with 0 steps. Final packet: direction with the step count.
  CHECK_EQ(events_of("44005b01643c01006000"), std::string("wheel:left/turn/0"));
  CHECK_EQ(events_of("44005c01643c01026000"), std::string("wheel:left/turn/2"));
  CHECK_EQ(events_of("44006f01643c02006003"), std::string("wheel:right/turn/0"));
  CHECK_EQ(events_of("44007001643c02016003"), std::string("wheel:right/turn/1"));
}

TEST(bthome_wheel_step_count_is_one_byte) {
  const Packet p = packet_of("44007201643c017c6003");
  CHECK_EQ(p.wheel_event, std::optional<int>(1));
  CHECK_EQ(p.wheel_steps, 124);
  CHECK_EQ(p.channel, std::optional<int>(3));
  CHECK_EQ(packet_of("44007401643c02986003").wheel_steps, 152);
}

TEST(bthome_wheel_event_none_is_not_an_input) {
  CHECK_EQ(events_of("44005b01643c00006000"), std::string(""));
}

// ---- robustness ----

TEST(bthome_empty_payload_does_not_decode) {
  CHECK(!decode(nullptr, 0).has_value());
  const uint8_t none = 0;
  CHECK(!decode(&none, 0).has_value());
}

TEST(bthome_encrypted_payload_is_not_parsed) {
  const Packet p = packet_of("45aabbcc");
  CHECK(p.encrypted);
  CHECK(p.buttons.empty());
  CHECK_EQ(p.packet_id, std::optional<int>());
}

TEST(bthome_unknown_object_stops_decoding) {
  const Packet p = packet_of("44001f01643a01ff1122");
  CHECK(!p.complete);
  // What was decoded before the unknown object is kept.
  CHECK_EQ(p.packet_id, std::optional<int>(31));
  CHECK_EQ(p.buttons, (std::vector<int>{1}));
}

TEST(bthome_truncated_object_stops_decoding) {
  const Packet p = packet_of("44001f3c01");
  CHECK(!p.complete);
  CHECK_EQ(p.packet_id, std::optional<int>(31));
  CHECK_EQ(p.wheel_event, std::optional<int>());
}

TEST(bthome_known_objects_of_other_devices_are_skipped_by_length) {
  // A temperature object (id 0x02, 2 bytes) before the button event must not break parsing.
  const Packet p = packet_of("44001f02e8033a01");
  CHECK(p.complete);
  CHECK_EQ(p.buttons, (std::vector<int>{1}));
}

TEST(bthome_other_version_is_reported) {
  CHECK_EQ(packet_of("24001f").version, 1);
}

TEST(bthome_all_button_events_are_the_named_codes) {
  const std::vector<std::string>& events = dispatcher::bthome::all_button_events();
  CHECK_EQ(events.size(), static_cast<size_t>(7));
  CHECK_EQ(events.front(), std::string("press"));
  CHECK_EQ(events.back(), std::string("hold_press"));
  for (int code : {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x80}) {
    const std::string name = dispatcher::bthome::button_event_name(code);
    CHECK(std::find(events.begin(), events.end(), name) != events.end());
  }
}
