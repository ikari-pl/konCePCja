#include "debug_timers.h"

DebugTimerManager g_debug_timers;

int32_t DebugTimerManager::timer_start(int32_t id, uint64_t tstate) {
  auto& t = timers_[id];
  t.start_tstate = tstate;
  t.running = true;
  return 0;
}

int32_t DebugTimerManager::timer_stop(int32_t id, uint64_t tstate) {
  auto it = timers_.find(id);
  if (it == timers_.end() || !it->second.running) return 0;

  auto& t = it->second;
  t.running = false;

  uint64_t elapsed_tstates = tstate - t.start_tstate;
  // CPC runs at 4MHz → 1 T-state = 0.25µs → elapsed_µs = tstates / 4
  uint32_t us = static_cast<uint32_t>(elapsed_tstates / 4);

  t.last_us = us;
  t.count++;
  t.total_us += us;
  if (us < t.min_us) t.min_us = us;
  if (us > t.max_us) t.max_us = us;

  return static_cast<int32_t>(us);
}

void DebugTimerManager::clear() {
  timers_.clear();
}
