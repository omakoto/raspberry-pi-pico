// Tests for learn.cpp (the "Press a button" state machine) and eventlog.cpp.

#include <string>

#include "dispatcher/eventlog.hpp"
#include "dispatcher/learn.hpp"
#include "minitest.hpp"

using namespace dispatcher;

namespace {

const char* const kDesk = "AA:BB:CC:DD:EE:01";
const char* const kNeighbor = "AA:BB:CC:DD:EE:99";

struct FakeClock {
  double now = 100.0;
};

Observed obs(const char* mac = kDesk, std::optional<int> rssi = -40, std::optional<int> channel = std::nullopt,
             const char* input = "button:2", const char* event = "press", int packet_id = 1) {
  return Observed{mac, rssi, channel, input, event, 0, packet_id, 100};
}

struct Fixture {
  FakeClock clock;
  LearnSession learn{[this] { return clock.now; }};

  // Starts a session, feeds the given first presses and lets the collect window pass.
  void first_press(std::initializer_list<Observed> presses) {
    learn.start();
    for (const Observed& o : presses) learn.feed(o, "SBBT-104CUS");
    clock.now += LearnSession::kCollectWindowS + 0.1;
  }
};

bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

}  // namespace

TEST(learn_descriptions) {
  CHECK_EQ(describe(obs()), std::string("button 2, single press"));
  CHECK_EQ(describe(obs(kDesk, -40, 3, "button:1", "hold_press")), std::string("button 1, hold, channel 3"));
  CHECK_EQ(describe(obs(kDesk, -40, 0, "wheel:left", "turn")), std::string("wheel turned left, channel 0"));
}

TEST(learn_idle_by_default) {
  Fixture f;
  CHECK_EQ(f.learn.status().phase, std::string("idle"));
  CHECK(!f.learn.active());
}

TEST(learn_waits_for_the_first_press) {
  Fixture f;
  f.learn.start();
  const LearnStatus s = f.learn.status();
  CHECK_EQ(s.phase, std::string("waiting"));
  CHECK(!s.candidate.has_value());
  CHECK(f.learn.active());
}

TEST(learn_first_press_becomes_the_candidate_after_the_collect_window) {
  Fixture f;
  f.learn.start();
  f.learn.feed(obs(), "SBBT-104CUS");
  CHECK_EQ(f.learn.status().phase, std::string("waiting"));  // still collecting
  f.clock.now += LearnSession::kCollectWindowS + 0.1;
  const LearnStatus s = f.learn.status();
  CHECK_EQ(s.phase, std::string("confirm"));
  CHECK(s.candidate.has_value());
  CHECK_EQ(s.candidate->mac, std::string(kDesk));
  CHECK_EQ(s.candidate->name, std::string("SBBT-104CUS"));
  CHECK_EQ(s.candidate->description, std::string("button 2, single press"));
  CHECK(contains(s.message, "once more"));
}

TEST(learn_strongest_signal_wins_among_devices_pressed_together) {
  Fixture f;
  f.first_press({obs(kNeighbor, -85), obs(kDesk, -40)});
  CHECK_EQ(f.learn.status().candidate->mac, std::string(kDesk));
  // Pressing order does not matter; the signal does.
  f.learn.start();
  f.learn.feed(obs(kDesk, -40));
  f.learn.feed(obs(kNeighbor, -85));
  f.clock.now += 1;
  CHECK_EQ(f.learn.status().candidate->mac, std::string(kDesk));
}

TEST(learn_missing_signal_strength_loses) {
  Fixture f;
  f.first_press({obs(kNeighbor, std::nullopt), obs(kDesk, -90)});
  CHECK_EQ(f.learn.status().candidate->mac, std::string(kDesk));
}

TEST(learn_only_a_devices_first_press_counts) {
  Fixture f;
  f.first_press({obs(kDesk, -80, std::nullopt, "button:2"), obs(kDesk, -30, std::nullopt, "button:3")});
  CHECK_EQ(f.learn.status().candidate->input, std::string("button:2"));
}

TEST(learn_second_identical_press_confirms) {
  Fixture f;
  f.first_press({obs(kDesk, -40, std::nullopt, "button:2", "press", 1)});
  f.learn.feed(obs(kDesk, -40, std::nullopt, "button:2", "press", 2));
  const LearnStatus s = f.learn.status();
  CHECK_EQ(s.phase, std::string("confirmed"));
  CHECK_EQ(s.candidate->mac, std::string(kDesk));
  CHECK(!f.learn.active());
}

TEST(learn_other_device_cannot_confirm) {
  Fixture f;
  f.first_press({obs()});
  f.learn.feed(obs(kNeighbor, -30));
  CHECK_EQ(f.learn.status().phase, std::string("confirm"));
}

TEST(learn_different_press_from_the_same_device_explains_the_mismatch) {
  Fixture f;
  f.first_press({obs(kDesk, -40, std::nullopt, "button:2", "press")});
  f.learn.feed(obs(kDesk, -40, std::nullopt, "button:3", "press", 2));
  const LearnStatus s = f.learn.status();
  CHECK_EQ(s.phase, std::string("confirm"));
  CHECK(contains(s.message, "button 3"));
  CHECK(contains(s.message, "button 2"));
  // A matching press afterwards still confirms, and clears the complaint.
  f.learn.feed(obs(kDesk, -40, std::nullopt, "button:2", "press", 3));
  CHECK_EQ(f.learn.status().phase, std::string("confirmed"));
}

TEST(learn_channel_must_match_too) {
  Fixture f;
  f.first_press({obs(kDesk, -40, 1)});
  f.learn.feed(obs(kDesk, -40, 2, "button:2", "press", 2));
  CHECK_EQ(f.learn.status().phase, std::string("confirm"));
  f.learn.feed(obs(kDesk, -40, 1, "button:2", "press", 3));
  CHECK_EQ(f.learn.status().phase, std::string("confirmed"));
}

TEST(learn_times_out_without_any_press) {
  Fixture f;
  f.learn.start();
  f.clock.now += LearnSession::kFirstPressTimeoutS + 1;
  const LearnStatus s = f.learn.status();
  CHECK_EQ(s.phase, std::string("timeout"));
  CHECK(contains(s.message, "No button was heard"));
}

TEST(learn_times_out_when_not_confirmed) {
  Fixture f;
  f.first_press({obs()});
  f.clock.now += LearnSession::kConfirmTimeoutS + 1;
  const LearnStatus s = f.learn.status();
  CHECK_EQ(s.phase, std::string("timeout"));
  CHECK(contains(s.message, "wasn't confirmed"));
}

TEST(learn_press_after_timeout_is_ignored) {
  Fixture f;
  f.first_press({obs()});
  f.clock.now += LearnSession::kConfirmTimeoutS + 1;
  f.learn.feed(obs(kDesk, -40, std::nullopt, "button:2", "press", 2));
  CHECK_EQ(f.learn.status().phase, std::string("timeout"));
}

TEST(learn_presses_are_ignored_when_not_searching) {
  Fixture f;
  f.learn.feed(obs());
  CHECK_EQ(f.learn.status().phase, std::string("idle"));
}

TEST(learn_cancel_and_restart) {
  Fixture f;
  f.first_press({obs()});
  f.learn.cancel();
  CHECK_EQ(f.learn.status().phase, std::string("idle"));
  f.learn.start();
  CHECK(!f.learn.status().candidate.has_value());
}

TEST(learn_seconds_left_counts_down_while_active) {
  Fixture f;
  f.learn.start();
  CHECK_EQ(f.learn.status().seconds_left, LearnSession::kFirstPressTimeoutS);
  f.clock.now += 10.04;
  CHECK_EQ(f.learn.status().seconds_left, 20.0);
  f.clock.now += 100;
  CHECK_EQ(f.learn.status().seconds_left, 0.0);
}

// ---- event log ----

TEST(eventlog_sequence_numbers_and_since) {
  FakeClock clock;
  EventLog log(10, [&clock] { return clock.now; });
  CHECK_EQ(log.last_seq(), 0u);
  log.add("info", "one");
  clock.now += 1;
  log.add("ble", "two");
  CHECK_EQ(log.last_seq(), 2u);
  CHECK_EQ(log.since(0).size(), static_cast<size_t>(2));
  const auto newer = log.since(1);
  CHECK_EQ(newer.size(), static_cast<size_t>(1));
  CHECK_EQ(newer[0].text, std::string("two"));
  CHECK_EQ(newer[0].kind, std::string("ble"));
  CHECK_EQ(newer[0].time, 101.0);
  CHECK(log.since(2).empty());
}

TEST(eventlog_keeps_only_the_newest_entries) {
  FakeClock clock;
  EventLog log(3, [&clock] { return clock.now; });
  for (int i = 1; i <= 5; ++i) log.add("info", std::to_string(i));
  CHECK_EQ(log.size(), static_cast<size_t>(3));
  const auto all = log.since(0);
  CHECK_EQ(all.front().text, std::string("3"));
  CHECK_EQ(all.back().text, std::string("5"));
  CHECK_EQ(log.last_seq(), 5u);
}

TEST(learn_event_labels) {
  CHECK_EQ(event_label("press"), std::string("single press"));
  CHECK_EQ(event_label("hold_press"), std::string("hold"));
  CHECK_EQ(event_label("turn"), std::string("turn"));
  CHECK_EQ(event_label("event_0x7f"), std::string("event_0x7f"));
}
