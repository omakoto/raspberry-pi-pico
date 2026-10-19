// BTHome v2 decoding for BLE buttons and remotes.
//
// A BTHome advertisement carries its payload as service data under the 16-bit UUID 0xFCD2. The first
// byte describes the packet (version, encryption, trigger-based); the rest is a sequence of objects,
// each an id byte followed by a fixed-length little-endian value.
//
// Only what button-like devices send is interpreted (packet id, battery, button events, the wheel
// "dimmer" object and the channel). Other objects are skipped by length so that devices which mix them
// in still decode; an object id this module does not know ends decoding, because object lengths depend
// on the id and the rest of the payload cannot be split safely.
//
// Mirrors pi/button-dispatcher/button_dispatcher/bthome.py. Measured behavior of the Shelly BLU remotes
// is in ~/history/reports/electronics/shelly-remote-switches.md.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dispatcher::bthome {

constexpr uint16_t kServiceUuid16 = 0xFCD2;
constexpr const char* kWheelEvent = "turn";

// A decoded BTHome service-data payload.
struct Packet {
  int version = 0;
  bool encrypted = false;
  bool trigger_based = false;
  std::optional<int> packet_id;
  std::optional<int> battery;
  std::optional<int> channel;
  // Event code of each button slot, in slot order (slot 1 first); 0 means no event.
  std::vector<int> buttons;
  std::optional<int> wheel_event;
  int wheel_steps = 0;
  // False when decoding stopped early at an unknown or truncated object.
  bool complete = true;
  std::vector<uint8_t> raw;
};

// One user action in a packet: a button event, or a wheel turn.
struct InputEvent {
  std::string input;  // "button:N" (N counts from 1) or "wheel:left" / "wheel:right"
  std::string event;  // "press", "double_press", ..., or kWheelEvent
  int steps = 0;      // wheel only: the step count (0 in the "start" packet of a turn)
};

// Decodes a payload; returns std::nullopt if it is empty.
std::optional<Packet> decode(const uint8_t* data, size_t length);

// The user actions a packet reports (button slots with an event, and a wheel turn).
std::vector<InputEvent> input_events(const Packet& packet);

// "press", "double_press", ... for a button event code; "event_0x7f" style for unknown codes.
std::string button_event_name(int code);

// The names of all known button events, in menu order: press, double_press, triple_press, long_press,
// long_double_press, long_triple_press, hold_press.
const std::vector<std::string>& all_button_events();

}  // namespace dispatcher::bthome
