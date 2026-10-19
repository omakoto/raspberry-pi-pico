#include "dispatcher/learn.hpp"

#include <cmath>

namespace dispatcher {

namespace {

// A missing signal strength loses against any real one.
int rssi_or_lowest(const std::optional<int>& rssi) { return rssi ? *rssi : -127; }

}  // namespace

std::string event_label(const std::string& event) {
  if (event == "press") return "single press";
  if (event == "double_press") return "double press";
  if (event == "triple_press") return "triple press";
  if (event == "long_press") return "long press";
  if (event == "long_double_press") return "long double press";
  if (event == "long_triple_press") return "long triple press";
  if (event == "hold_press") return "hold";
  return event;
}

std::string describe(const Observed& observed) {
  const size_t colon = observed.input.find(':');
  const std::string kind = observed.input.substr(0, colon);
  const std::string which = colon == std::string::npos ? "" : observed.input.substr(colon + 1);
  std::string text;
  if (kind == "wheel") {
    text = "wheel turned " + which;
  } else {
    text = "button " + which + ", " + event_label(observed.event);
  }
  if (observed.channel) text += ", channel " + std::to_string(*observed.channel);
  return text;
}

void LearnSession::reset() {
  phase_ = "idle";
  deadline_ = 0.0;
  collect_until_.reset();
  candidates_.clear();
  candidate_.reset();
  candidate_name_.clear();
  mismatch_.clear();
}

void LearnSession::start() {
  reset();
  phase_ = "waiting";
  deadline_ = clock_() + kFirstPressTimeoutS;
}

void LearnSession::cancel() { reset(); }

void LearnSession::feed(const Observed& observed, const std::string& name) {
  advance();
  if (phase_ == "waiting") {
    if (!collect_until_) collect_until_ = clock_() + kCollectWindowS;
    // Only a device's first press counts, so the order heard decides ties.
    for (const auto& entry : candidates_) {
      if (entry.first.mac == observed.mac) return;
    }
    candidates_.emplace_back(observed, name);
  } else if (phase_ == "confirm" && candidate_ && observed.mac == candidate_->mac) {
    if (observed.channel == candidate_->channel && observed.input == candidate_->input &&
        observed.event == candidate_->event) {
      phase_ = "confirmed";
      mismatch_.clear();
    } else {
      mismatch_ = "That was " + describe(observed) + ", but the first press was " + describe(*candidate_) +
                  ". Press the same button once more.";
    }
  }
}

void LearnSession::advance() {
  const double now = clock_();
  if (phase_ == "waiting") {
    if (collect_until_ && now >= *collect_until_) {
      size_t best = 0;
      for (size_t i = 1; i < candidates_.size(); ++i) {
        if (rssi_or_lowest(candidates_[i].first.rssi) > rssi_or_lowest(candidates_[best].first.rssi)) best = i;
      }
      candidate_ = candidates_[best].first;
      candidate_name_ = candidates_[best].second;
      phase_ = "confirm";
      // Counted from the end of the collect window rather than from now, so the deadline does not
      // depend on how soon someone happens to look at the session.
      deadline_ = *collect_until_ + kConfirmTimeoutS;
    } else if (!collect_until_ && now >= deadline_) {
      phase_ = "timeout";
    }
  }
  if (phase_ == "confirm" && now >= deadline_) phase_ = "timeout";
}

LearnStatus LearnSession::status() {
  advance();
  LearnStatus status;
  status.phase = phase_;
  if (phase_ == "waiting") {
    status.message = "Press the button you want to add.";
  } else if (phase_ == "confirm") {
    status.message = mismatch_.empty() ? "Press the same button once more (a separate single press)." : mismatch_;
  } else if (phase_ == "confirmed") {
    status.message = "Confirmed.";
  } else if (phase_ == "timeout") {
    status.message = candidate_ ? "The button wasn't confirmed in time."
                                : "No button was heard. Check the batteries and the distance to the dispatcher.";
  }
  if (candidate_) {
    const Observed& c = *candidate_;
    status.candidate = LearnCandidate{c.mac, candidate_name_, c.rssi, c.channel, c.battery, c.input, c.event,
                                      describe(c)};
  }
  if (active()) status.seconds_left = std::max(0.0, std::round((deadline_ - clock_()) * 10.0) / 10.0);
  return status;
}

}  // namespace dispatcher
