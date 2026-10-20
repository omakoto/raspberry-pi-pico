// Tests for beeps.cpp (the ack buzzer's beep counts and timing) and for the beeps the press flow asks for.

#include <string>
#include <vector>

#include "dispatcher/app.hpp"
#include "dispatcher/beeps.hpp"
#include "fake_platform.hpp"
#include "minitest.hpp"
#include "test_util.hpp"

using namespace dispatcher;

namespace {

const char* const kDesk = "AA:BB:CC:DD:EE:01";
const char* const kPressB2 = "44002101643a003a013a003a00";        // id 33, button 2 press
const char* const kDoublePressB2 = "44002401643a003a023a003a00";  // id 36, button 2 double press
const char* const kTriplePressB2 = "44002501643a003a033a003a00";  // id 37, button 2 triple press

constexpr int kFastIterations = 2;

Binding http_binding(const std::string& id, const std::string& event) {
  Binding binding;
  binding.id = id;
  binding.name = "Hit " + id;
  binding.cooldown_ms = 0;
  binding.trigger = {"desk", "button:2", event, std::nullopt};
  HttpAction http;
  http.url = "http://wol.local/" + id;
  binding.action = http;
  return binding;
}

struct Fixture {
  test_support::FakePlatform platform;
  AppCore app{platform, kFastIterations};

  Fixture() {
    platform.http_handler = [](const test_support::RecordedRequest&) {
      HttpResponse response;
      response.status = 200;
      response.reason = "OK";
      return response;
    };
    app.init();
  }

  void configure(std::vector<Binding> bindings) {
    std::lock_guard<std::mutex> lock(app.mutex);
    app.config.buttons = {{"desk", "Desk remote", kDesk, "SBBT-104CUS"}};
    app.config.bindings = std::move(bindings);
    std::string error;
    CHECK(app.commit_config(&error));
  }

  void press(const char* hex) {
    const std::vector<uint8_t> bytes = test_util::from_hex(hex);
    app.handle_packet(kDesk, -40, bytes.data(), bytes.size());
  }

  // The beeps heard so far as "short x1, long x3, ..." for readable failures.
  std::string heard() const {
    std::string text;
    for (const auto& beep : platform.beeps) {
      text += std::string(beep.first == Beep::kShort ? "short" : "long") + " x" + std::to_string(beep.second) + ", ";
    }
    return text;
  }
};

}  // namespace

// ---- counts ----

TEST(beeps_click_count_follows_the_number_of_presses) {
  CHECK_EQ(click_count("press"), 1);
  CHECK_EQ(click_count("double_press"), 2);
  CHECK_EQ(click_count("triple_press"), 3);
  CHECK_EQ(click_count("long_press"), 1);
  CHECK_EQ(click_count("long_double_press"), 2);
  CHECK_EQ(click_count("long_triple_press"), 3);
  CHECK_EQ(click_count("hold_press"), 1);
  CHECK_EQ(click_count("turn"), 1);
}

// ---- timing ----

TEST(beeps_a_sequence_is_laid_out_with_pauses) {
  BeepScheduler scheduler;
  const std::vector<Pulse> pulses = scheduler.schedule(50.0, Beep::kShort, 3);
  CHECK_EQ(pulses.size(), static_cast<size_t>(3));
  CHECK_EQ(pulses[0].start_s, 50.0);
  CHECK_EQ(pulses[0].duration_s, BeepScheduler::kShortS);
  CHECK_EQ(pulses[1].start_s, 50.0 + BeepScheduler::kShortS + BeepScheduler::kPauseS);
  CHECK(pulses[2].start_s > pulses[1].start_s + pulses[1].duration_s);
}

TEST(beeps_long_beeps_are_longer_than_short_ones) {
  BeepScheduler scheduler;
  const std::vector<Pulse> shorts = scheduler.schedule(0.0, Beep::kShort, 1);
  const std::vector<Pulse> longs = scheduler.schedule(100.0, Beep::kLong, 1);
  CHECK(longs[0].duration_s > 3 * shorts[0].duration_s);
}

TEST(beeps_the_next_sequence_waits_a_second_after_the_previous_one_ended) {
  BeepScheduler scheduler;
  const std::vector<Pulse> ack = scheduler.schedule(10.0, Beep::kShort, 2);
  const double ack_end = ack.back().start_s + ack.back().duration_s;
  // The result arrives while the acknowledgement is still sounding.
  const std::vector<Pulse> result = scheduler.schedule(10.1, Beep::kShort, 1);
  CHECK_EQ(result.size(), static_cast<size_t>(1));
  CHECK(result[0].start_s >= ack_end + BeepScheduler::kSequenceGapS - 1e-9);
}

TEST(beeps_a_sequence_after_a_long_silence_starts_at_once) {
  BeepScheduler scheduler;
  scheduler.schedule(10.0, Beep::kShort, 3);
  const std::vector<Pulse> later = scheduler.schedule(60.0, Beep::kLong, 3);
  CHECK_EQ(later[0].start_s, 60.0);
}

TEST(beeps_nothing_is_scheduled_for_a_zero_count_and_the_count_is_capped) {
  BeepScheduler scheduler;
  CHECK(scheduler.schedule(1.0, Beep::kShort, 0).empty());
  CHECK(scheduler.schedule(1.0, Beep::kShort, -2).empty());
  CHECK_EQ(scheduler.schedule(1.0, Beep::kShort, 500).size(), static_cast<size_t>(BeepScheduler::kMaxCount));
}

TEST(beeps_a_sequence_that_would_sound_much_too_late_is_dropped) {
  BeepScheduler scheduler;
  // Each long sequence of nine beeps takes several seconds; a burst soon pushes the next start out of reach.
  bool dropped = false;
  for (int i = 0; i < 20 && !dropped; ++i) dropped = scheduler.schedule(1.0, Beep::kLong, 9).empty();
  CHECK(dropped);
  // Once the time has passed, beeping works again.
  CHECK(!scheduler.schedule(1000.0, Beep::kShort, 1).empty());
}

// ---- the press flow ----

TEST(beeps_a_single_press_beeps_once_and_a_success_beeps_once_more) {
  Fixture f;
  f.configure({http_binding("a", "press")});
  f.press(kPressB2);
  CHECK_EQ(f.platform.beeps.size(), static_cast<size_t>(2));
  CHECK(f.platform.beeps[0] == std::make_pair(Beep::kShort, 1));
  CHECK(f.platform.beeps[1] == std::make_pair(Beep::kShort, 1));
}

TEST(beeps_a_double_and_a_triple_press_beep_that_many_times) {
  Fixture f;
  f.configure({http_binding("a", "double_press"), http_binding("b", "triple_press")});
  f.press(kDoublePressB2);
  CHECK(f.platform.beeps[0] == std::make_pair(Beep::kShort, 2));
  f.platform.beeps.clear();
  f.press(kTriplePressB2);
  CHECK(f.platform.beeps[0] == std::make_pair(Beep::kShort, 3));
}

TEST(beeps_a_failed_action_beeps_three_long_beeps) {
  Fixture f;
  f.platform.http_handler = [](const test_support::RecordedRequest&) {
    HttpResponse response;
    response.failure = HttpResponse::Failure::kConnect;
    return response;
  };
  f.configure({http_binding("a", "press")});
  f.press(kPressB2);
  CHECK_EQ(f.platform.beeps.size(), static_cast<size_t>(2));
  CHECK(f.platform.beeps[0] == std::make_pair(Beep::kShort, 1));
  CHECK(f.platform.beeps[1] == std::make_pair(Beep::kLong, 3));
}

TEST(beeps_the_acknowledgement_comes_before_the_action_runs) {
  Fixture f;
  f.platform.run_background_immediately = false;
  f.configure({http_binding("a", "press")});
  f.press(kPressB2);
  CHECK_EQ(f.platform.beeps.size(), static_cast<size_t>(1));  // only the acknowledgement so far
  f.platform.run_queued_background();
  CHECK_EQ(f.platform.beeps.size(), static_cast<size_t>(2));
}

TEST(beeps_presses_that_fire_nothing_are_silent) {
  Fixture f;
  f.configure({http_binding("a", "double_press")});
  f.press(kPressB2);  // a single press; only the double press has a binding
  CHECK(f.platform.beeps.empty());
}

TEST(beeps_repeated_copies_of_a_press_beep_once) {
  Fixture f;
  f.configure({http_binding("a", "press")});
  f.press(kPressB2);
  f.press(kPressB2);
  f.press(kPressB2);
  CHECK_EQ(f.platform.beeps.size(), static_cast<size_t>(2));  // one acknowledgement and one result
}

TEST(beeps_two_bindings_on_one_press_give_one_acknowledgement) {
  Fixture f;
  f.configure({http_binding("a", "press"), http_binding("b", "press")});
  f.press(kPressB2);
  int acks = 0;
  for (const auto& beep : f.platform.beeps) {
    if (beep.first == Beep::kShort && beep.second == 1) ++acks;
  }
  // One acknowledgement plus one result beep per action.
  CHECK_EQ(acks, 3);
  CHECK_EQ(f.platform.requests.size(), static_cast<size_t>(2));
}
