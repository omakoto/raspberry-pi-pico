// The beeps of the "ack" buzzer: how many beeps a press earns, and when each beep sounds.
//
// The buzzer is active (it has its own oscillator), so it has one pitch and beeps differ only in length and
// count: short beeps acknowledge a press and report success, long beeps report a failure.

#pragma once

#include <string>
#include <vector>

namespace dispatcher {

enum class Beep { kShort, kLong };

// The number of beeps that acknowledge a press: 1 for a single press, 2 for a double press, 3 for a triple
// press. The long-press variants and the wheel count like their short counterparts.
int click_count(const std::string& event);

// One sound of the buzzer, on a monotonic clock.
struct Pulse {
  double start_s;
  double duration_s;
};

// Lays beeps out in time. Each sequence of beeps starts at least kSequenceGapS after the previous sequence
// ended, so that an acknowledgement and the result that follows it are heard as separate things.
// Not thread-safe; the caller serializes access.
class BeepScheduler {
 public:
  static constexpr double kShortS = 0.08;
  static constexpr double kLongS = 0.4;
  // Silence between the beeps of one sequence.
  static constexpr double kPauseS = 0.15;
  // Silence between two sequences, measured from the end of the last beep.
  static constexpr double kSequenceGapS = 1.0;
  static constexpr int kMaxCount = 9;
  // A sequence that would start later than this after `now` is dropped, so that a burst of presses cannot
  // queue up beeps that sound long after the events they report.
  static constexpr double kMaxDelayS = 10.0;

  // The pulses for `count` beeps requested at monotonic time `now_s`; empty if there is nothing to play.
  std::vector<Pulse> schedule(double now_s, Beep kind, int count);

 private:
  double last_end_s_ = -1e9;
};

}  // namespace dispatcher
