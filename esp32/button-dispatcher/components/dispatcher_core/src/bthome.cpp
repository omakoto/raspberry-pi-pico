#include "dispatcher/bthome.hpp"

#include <cstdio>

namespace dispatcher::bthome {

namespace {

constexpr uint8_t kObjPacketId = 0x00;
constexpr uint8_t kObjBattery = 0x01;
constexpr uint8_t kObjButton = 0x3A;
constexpr uint8_t kObjDimmer = 0x3C;
constexpr uint8_t kObjChannel = 0x60;

// Value length in bytes of an object, or -1 if the id is unknown. Objects other than the ones this
// module interprets are listed so that their length is known and decoding can continue past them.
int object_length(uint8_t id) {
  switch (id) {
    case 0x00:  // packet id
    case 0x01:  // battery %
    case 0x10:  // power
    case 0x11:  // opening
    case 0x15:  // battery low
    case 0x21:  // motion
    case 0x2D:  // window
    case 0x2E:  // humidity (8 bit)
    case 0x2F:  // moisture (8 bit)
    case 0x3A:  // button event
    case 0x60:  // channel
      return 1;
    case 0x02:  // temperature
    case 0x03:  // humidity
    case 0x08:  // dew point
    case 0x0C:  // voltage
    case 0x3C:  // dimmer: event, steps
    case 0x3F:  // rotation
    case 0x40:  // distance
    case 0x45:  // temperature (0.1)
      return 2;
    case 0x04:  // pressure
    case 0x05:  // illuminance
      return 3;
    default:
      return -1;
  }
}

// The dimmer object's event byte: BTHome calls these rotate left / rotate right.
const char* wheel_direction(int event) {
  switch (event) {
    case 0x01: return "left";
    case 0x02: return "right";
    default: return nullptr;
  }
}

}  // namespace

std::string button_event_name(int code) {
  switch (code) {
    case 0x01: return "press";
    case 0x02: return "double_press";
    case 0x03: return "triple_press";
    case 0x04: return "long_press";
    case 0x05: return "long_double_press";
    case 0x06: return "long_triple_press";
    case 0x80: return "hold_press";
    default: {
      char text[24];
      std::snprintf(text, sizeof(text), "event_0x%02x", code & 0xFF);
      return text;
    }
  }
}

const std::vector<std::string>& all_button_events() {
  static const std::vector<std::string> events = {"press",      "double_press",      "triple_press", "long_press",
                                                  "long_double_press", "long_triple_press", "hold_press"};
  return events;
}

std::optional<Packet> decode(const uint8_t* data, size_t length) {
  if (data == nullptr || length == 0) return std::nullopt;
  Packet packet;
  const uint8_t info = data[0];
  packet.version = info >> 5;
  packet.encrypted = (info & 1) != 0;
  packet.trigger_based = (info & 4) != 0;
  packet.raw.assign(data, data + length);
  // The payload of an encrypted packet is sealed with a per-device key, so none of it can be read.
  if (packet.encrypted) return packet;

  size_t i = 1;
  while (i < length) {
    const uint8_t id = data[i];
    const int value_length = object_length(id);
    if (value_length < 0 || i + 1 + static_cast<size_t>(value_length) > length) {
      packet.complete = false;
      break;
    }
    const uint8_t* value = data + i + 1;
    switch (id) {
      case kObjPacketId: packet.packet_id = value[0]; break;
      case kObjBattery: packet.battery = value[0]; break;
      case kObjChannel: packet.channel = value[0]; break;
      case kObjButton: packet.buttons.push_back(value[0]); break;
      case kObjDimmer:
        packet.wheel_event = value[0];
        packet.wheel_steps = value[1];
        break;
      default: break;
    }
    i += 1 + static_cast<size_t>(value_length);
  }
  return packet;
}

std::vector<InputEvent> input_events(const Packet& packet) {
  std::vector<InputEvent> events;
  int slot = 1;
  for (int code : packet.buttons) {
    if (code != 0) events.push_back({"button:" + std::to_string(slot), button_event_name(code), 0});
    ++slot;
  }
  if (packet.wheel_event) {
    if (const char* direction = wheel_direction(*packet.wheel_event)) {
      events.push_back({std::string("wheel:") + direction, kWheelEvent, packet.wheel_steps});
    }
  }
  return events;
}

}  // namespace dispatcher::bthome
