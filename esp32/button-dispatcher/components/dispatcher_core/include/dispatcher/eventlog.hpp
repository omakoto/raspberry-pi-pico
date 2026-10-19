// A small in-memory log of what the dispatcher heard and did, shown on the web page.
//
// Mirrors pi/button-dispatcher/button_dispatcher/eventlog.py. Entries get increasing sequence numbers so
// the page can poll for news. The time stored is the injected clock's value when the entry was added; on
// the device that is a monotonic clock, and the web layer turns it into wall-clock time when the entry is
// read, so entries logged before the clock was set still show the right time afterwards.

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace dispatcher {

struct LogEntry {
  uint32_t seq = 0;
  double time = 0.0;
  std::string kind;  // ble, fire, action, error or info
  std::string text;
};

class EventLog {
 public:
  using Clock = std::function<double()>;

  EventLog(size_t capacity, Clock clock) : capacity_(capacity), clock_(std::move(clock)) {}

  void add(const std::string& kind, const std::string& text);
  // Entries newer than `seq`, oldest first.
  std::vector<LogEntry> since(uint32_t seq) const;
  uint32_t last_seq() const { return seq_; }
  size_t size() const { return entries_.size(); }

 private:
  size_t capacity_;
  Clock clock_;
  uint32_t seq_ = 0;
  std::deque<LogEntry> entries_;
};

}  // namespace dispatcher
