// host_pacing_thread: the frame runner asks the OS to wake it on time from
// its sleep to the 50 Hz deadline (beads-fijd). The OS must take the setting,
// and on macOS a paced 19 ms sleep must end on time: an ordinary thread of a
// process macOS treats as background was measured waking ~100 ms late (timer
// coalescing), which is what made KCPC reads and EVT_FRAME come in bursts.
// Every platform prints what it measured, paced and not, as the record of
// what its host does.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "frame_pacer.h"
#include "host_clock.h"

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <sys/resource.h>
#elif defined(__linux__)
#include <sys/prctl.h>
#endif

namespace {

constexpr uint32_t kBusyUs = 10000;
constexpr uint32_t kSleepMs = 19;  // what a Fast frame sleeps of its 20 ms
constexpr int kSleeps = 10;

// How late each of kSleeps sleeps of kSleepMs ended, in microseconds, sorted.
std::vector<uint64_t> oversleeps() {
  uint64_t const freq = host_perf_frequency();
  std::vector<uint64_t> late;
  for (int i = 0; i < kSleeps; ++i) {
    uint64_t const start = host_perf_counter();
    host_delay_ms(kSleepMs);
    uint64_t const slept_us = ((host_perf_counter() - start) * 1000000) / freq;
    // host_delay_ms promises at least kSleepMs; allow the counters 1 ms of
    // disagreement.
    EXPECT_GE(slept_us + 1000, uint64_t{kSleepMs} * 1000) << i;
    late.push_back(slept_us > kSleepMs * 1000 ? slept_us - kSleepMs * 1000 : 0);
  }
  std::sort(late.begin(), late.end());
  return late;
}

void print(const char* what, const std::vector<uint64_t>& late) {
  std::printf(
      "[ pacing   ] %s: %d sleeps of %u ms, late by median %.2f ms, "
      "max %.2f ms\n",
      what, kSleeps, kSleepMs, late[late.size() / 2] / 1000.0,
      late.back() / 1000.0);
}

#if defined(__APPLE__)
// The calling thread's time-constraint policy; false when it has none.
bool time_constraint(thread_time_constraint_policy_data_t& policy) {
  mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
  boolean_t get_default = FALSE;
  thread_port_t const self = mach_thread_self();
  kern_return_t const kr = thread_policy_get(
      self, THREAD_TIME_CONSTRAINT_POLICY,
      reinterpret_cast<thread_policy_t>(&policy), &count, &get_default);
  mach_port_deallocate(mach_task_self(), self);
  return kr == KERN_SUCCESS && !get_default;
}

// Whether the OS runs the calling thread in the background band
// (`taskpolicy -b`, `make e2e_test_slow` style, or a background thread). The
// band throttles timers, and a time-constraint policy does not lift that:
// measured ~195 ms late paced or not (beads-py5c).
bool in_background_band() {
  return getpriority(PRIO_DARWIN_PROCESS, 0) > 0 ||
         getpriority(PRIO_DARWIN_THREAD, 0) > 0;
}
#endif

// Runs `body` on a thread of its own: a new thread starts as an ordinary one,
// whatever an earlier test left the test runner's main thread as (paced
// bridge frames make it a pacing thread).
template <typename Body>
void on_new_thread(Body body) {
  std::thread(body).join();
}

TEST(HostPacingThread, TheOsTakesItAndGivesItBack) {
  on_new_thread([] {
#if defined(__APPLE__)
    thread_time_constraint_policy_data_t policy{};
    EXPECT_FALSE(time_constraint(policy));
    host_pacing_thread(true, PacingThreadRule::kPeriodUs, kBusyUs);
    EXPECT_TRUE(time_constraint(policy));
    EXPECT_GT(policy.computation, 0u);
    EXPECT_LT(policy.computation, policy.period);
    EXPECT_EQ(policy.constraint, policy.period);
    host_pacing_thread(false, PacingThreadRule::kPeriodUs, kBusyUs);
    EXPECT_FALSE(time_constraint(policy));
#elif defined(__linux__)
    int const before = prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0);
    EXPECT_GT(before, 0);
    host_pacing_thread(true, PacingThreadRule::kPeriodUs, kBusyUs);
    EXPECT_EQ(1, prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0));
    host_pacing_thread(false, PacingThreadRule::kPeriodUs, kBusyUs);
    EXPECT_EQ(before, prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0));
#else
    // Nothing to set: SDL_Delay already waits on a high-resolution timer.
    host_pacing_thread(true, PacingThreadRule::kPeriodUs, kBusyUs);
    host_pacing_thread(false, PacingThreadRule::kPeriodUs, kBusyUs);
#endif
  });
}

TEST(HostPacingThread, APacedSleepEndsOnTime) {
  on_new_thread([] {
    print("ordinary thread", oversleeps());
    host_pacing_thread(true, PacingThreadRule::kPeriodUs, kBusyUs);
    std::vector<uint64_t> const paced = oversleeps();
    host_pacing_thread(false, PacingThreadRule::kPeriodUs, kBusyUs);
    print("pacing thread  ", paced);
#if defined(__APPLE__)
    if (in_background_band()) {
      GTEST_SKIP() << "the process runs in the macOS background band, which "
                      "throttles timers whatever the thread's policy: the "
                      "measurement above is the record (beads-py5c)";
    }
    // Measured 0.01 ms paced, against ~100 ms on a coalescing host. A virtual
    // machine (CI) does not coalesce but has its own jitter: 5 ms paced or not.
    EXPECT_LT(paced[paced.size() / 2], 15000u);
#endif
  });
}

}  // namespace
