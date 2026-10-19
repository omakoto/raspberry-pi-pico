#include "dispatcher/eventlog.hpp"

namespace dispatcher {

void EventLog::add(const std::string& kind, const std::string& text) {
  ++seq_;
  entries_.push_back({seq_, clock_(), kind, text});
  while (entries_.size() > capacity_) entries_.pop_front();
}

std::vector<LogEntry> EventLog::since(uint32_t seq) const {
  std::vector<LogEntry> newer;
  for (const LogEntry& entry : entries_) {
    if (entry.seq > seq) newer.push_back(entry);
  }
  return newer;
}

}  // namespace dispatcher
