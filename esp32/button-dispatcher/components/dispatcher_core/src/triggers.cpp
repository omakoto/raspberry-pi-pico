#include "dispatcher/triggers.hpp"

#include <algorithm>

namespace dispatcher {

bool trigger_matches(const Binding& binding, const Button& button, const Observed& observed) {
  const Trigger& t = binding.trigger;
  return button.mac == observed.mac && t.input == observed.input && t.event == observed.event &&
         (!t.channel || t.channel == observed.channel);
}

void TriggerEngine::configure(std::vector<Button> buttons, std::vector<Binding> bindings) {
  buttons_ = std::move(buttons);
  bindings_ = std::move(bindings);
  for (auto it = last_fired_.begin(); it != last_fired_.end();) {
    const std::string& id = it->first;
    const bool still_exists = std::any_of(bindings_.begin(), bindings_.end(),
                                          [&id](const Binding& b) { return b.id == id; });
    it = still_exists ? std::next(it) : last_fired_.erase(it);
  }
}

bool TriggerEngine::is_known(const std::string& mac) const {
  return std::any_of(buttons_.begin(), buttons_.end(), [&mac](const Button& b) { return b.mac == mac; });
}

ProcessResult TriggerEngine::process(const std::string& mac_text, std::optional<int> rssi, const uint8_t* data,
                                     size_t length) {
  ProcessResult result;
  const std::optional<std::string> mac = normalize_mac(mac_text);
  if (!mac) {
    result.error = "invalid MAC address: '" + mac_text + "'";
    return result;
  }
  std::optional<bthome::Packet> packet = bthome::decode(data, length);
  if (!packet) {
    result.error = "empty BTHome payload";
    return result;
  }
  result.known_device = is_known(*mac);
  if (packet->encrypted) {
    result.error = "encrypted payload (unsupported)";
    result.packet = std::move(packet);
    return result;
  }
  if (packet->version != 2) {
    result.error = "unsupported BTHome version " + std::to_string(packet->version);
    result.packet = std::move(packet);
    return result;
  }

  const double now = clock_();
  if (is_duplicate(*mac, *packet, now)) {
    result.duplicate = true;
    result.packet = std::move(packet);
    return result;
  }

  for (const bthome::InputEvent& event : bthome::input_events(*packet)) {
    if (event.input.rfind("wheel:", 0) == 0 && event.steps != 0) {
      result.notes.push_back("wheel final packet ignored (" + event.input + ", " + std::to_string(event.steps) +
                             " steps)");
      continue;
    }
    result.observed.push_back(
        {*mac, rssi, packet->channel, event.input, event.event, event.steps, packet->packet_id, packet->battery});
  }
  result.packet = std::move(packet);

  if (result.known_device) {
    for (const Observed& observed : result.observed) fire(observed, now, result);
  }
  return result;
}

bool TriggerEngine::is_duplicate(const std::string& mac, const bthome::Packet& packet, double now) {
  // Without a packet id the whole payload identifies the event.
  std::string key;
  if (packet.packet_id) {
    key = "id:" + std::to_string(*packet.packet_id);
  } else {
    key = "raw:";
    key.append(packet.raw.begin(), packet.raw.end());
  }

  auto it = std::find_if(devices_.begin(), devices_.end(), [&mac](const Device& d) { return d.mac == mac; });
  if (it == devices_.end()) {
    devices_.push_front(Device{mac, {}});
    it = devices_.begin();
    while (devices_.size() > kMaxTrackedDevices) devices_.pop_back();
  } else {
    devices_.splice(devices_.begin(), devices_, it);
    it = devices_.begin();
  }

  // Entries older than the window are gone, so a recycled packet id counts as new again.
  std::vector<Seen>& recent = it->recent;
  recent.erase(std::remove_if(recent.begin(), recent.end(),
                              [now](const Seen& s) { return now - s.time > kDedupeWindowS; }),
               recent.end());

  for (Seen& seen : recent) {
    if (seen.key == key) {
      // The window slides with every copy, so a long burst of repeats stays one event.
      seen.time = now;
      return true;
    }
  }
  recent.push_back({key, now});
  if (recent.size() > kRecentIdsPerDevice) recent.erase(recent.begin());
  return false;
}

std::vector<Binding> TriggerEngine::matching_bindings(const Observed& observed) const {
  std::vector<Binding> matches;
  for (const Binding& binding : bindings_) {
    if (!binding.enabled) continue;
    auto button = std::find_if(buttons_.begin(), buttons_.end(),
                               [&binding](const Button& b) { return b.id == binding.trigger.button; });
    if (button != buttons_.end() && trigger_matches(binding, *button, observed)) matches.push_back(binding);
  }
  return matches;
}

void TriggerEngine::fire(const Observed& observed, double now, ProcessResult& result) {
  for (Binding& binding : matching_bindings(observed)) {
    auto last = last_fired_.find(binding.id);
    if (last != last_fired_.end() && now - last->second < binding.cooldown_ms / 1000.0) {
      result.suppressed.emplace_back(binding.id, "cooldown");
      continue;
    }
    last_fired_[binding.id] = now;
    result.fired.push_back({std::move(binding), observed});
  }
}

}  // namespace dispatcher
