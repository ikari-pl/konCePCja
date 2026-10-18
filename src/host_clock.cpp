#include "host_clock.h"

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#elif defined(__linux__)
#include <sys/prctl.h>
#endif

#ifdef KONCPC_SDL

#include <SDL3/SDL_timer.h>

uint64_t host_ticks_ms() { return SDL_GetTicks(); }
uint64_t host_perf_counter() { return SDL_GetPerformanceCounter(); }
uint64_t host_perf_frequency() { return SDL_GetPerformanceFrequency(); }
void host_delay_ms(uint32_t ms) { SDL_Delay(ms); }

#else

#include <chrono>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;

// SDL_GetTicks counts from SDL_Init; this counts from the first reading, which
// the startup path takes before the loop starts.
const Clock::time_point& clock_origin() {
  static const Clock::time_point origin = Clock::now();
  return origin;
}
}  // namespace

uint64_t host_ticks_ms() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                            clock_origin())
          .count());
}

uint64_t host_perf_counter() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now().time_since_epoch())
          .count());
}

uint64_t host_perf_frequency() { return 1000000000ULL; }

void host_delay_ms(uint32_t ms) {
  if (ms == 0) {
    std::this_thread::yield();
    return;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

#endif

#if defined(__APPLE__)

void host_pacing_thread(bool on, uint32_t period_us, uint32_t busy_us) {
  thread_port_t const self = mach_thread_self();
  if (on) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    auto const ticks = [&tb](uint32_t us) {
      return static_cast<uint32_t>(static_cast<uint64_t>(us) * 1000U *
                                   tb.denom / tb.numer);
    };
    thread_time_constraint_policy_data_t policy;
    policy.period = ticks(period_us);
    policy.computation = ticks(busy_us);
    policy.constraint = ticks(period_us);
    policy.preemptible = 1;
    thread_policy_set(self, THREAD_TIME_CONSTRAINT_POLICY,
                      reinterpret_cast<thread_policy_t>(&policy),
                      THREAD_TIME_CONSTRAINT_POLICY_COUNT);
  } else {
    thread_standard_policy_data_t policy{};
    thread_policy_set(self, THREAD_STANDARD_POLICY,
                      reinterpret_cast<thread_policy_t>(&policy),
                      THREAD_STANDARD_POLICY_COUNT);
  }
  mach_port_deallocate(mach_task_self(), self);
}

#elif defined(__linux__)

void host_pacing_thread(bool on, uint32_t, uint32_t) {
  // 1 ns is the least slack there is; 0 puts back the thread's default.
  prctl(PR_SET_TIMERSLACK, on ? 1UL : 0UL, 0UL, 0UL, 0UL);
}

#else

void host_pacing_thread(bool, uint32_t, uint32_t) {}

#endif
