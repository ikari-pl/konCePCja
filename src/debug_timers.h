#ifndef DEBUG_TIMERS_H
#define DEBUG_TIMERS_H

#include <cstdint>
#include <map>

struct DebugTimer {
  uint64_t start_tstate = 0;
  bool running = false;
  uint32_t count = 0;
  uint32_t last_us = 0;
  uint32_t min_us = UINT32_MAX;
  uint32_t max_us = 0;
  uint64_t total_us = 0;
};

class DebugTimerManager {
public:
  // Start a timer. Returns 0 (for use in expressions).
  int32_t timer_start(int32_t id, uint64_t tstate);

  // Stop a timer. Returns elapsed microseconds (T-states / 4).
  int32_t timer_stop(int32_t id, uint64_t tstate);

  // Clear all timers.
  void clear();

  // Access timers for listing.
  const std::map<int32_t, DebugTimer>& timers() const { return timers_; }

private:
  std::map<int32_t, DebugTimer> timers_;
};

extern DebugTimerManager g_debug_timers;

#endif
