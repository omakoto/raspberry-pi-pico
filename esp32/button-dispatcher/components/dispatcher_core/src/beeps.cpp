#include "dispatcher/beeps.hpp"

#include <algorithm>

namespace dispatcher {

int click_count(const std::string& event) {
  if (event == "double_press" || event == "long_double_press") return 2;
  if (event == "triple_press" || event == "long_triple_press") return 3;
  return 1;
}

std::vector<Pulse> BeepScheduler::schedule(double now_s, Beep kind, int count) {
  std::vector<Pulse> pulses;
  if (count <= 0) return pulses;
  count = std::min(count, kMaxCount);

  const double start_s = std::max(now_s, last_end_s_ + kSequenceGapS);
  if (start_s > now_s + kMaxDelayS) return pulses;

  const double duration_s = kind == Beep::kLong ? kLongS : kShortS;
  double at_s = start_s;
  for (int i = 0; i < count; ++i) {
    pulses.push_back({at_s, duration_s});
    last_end_s_ = at_s + duration_s;
    at_s = last_end_s_ + kPauseS;
  }
  return pulses;
}

}  // namespace dispatcher
