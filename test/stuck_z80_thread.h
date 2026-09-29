/* A Z80 thread that never leaves its frame, for the tests that pin the
 * bounded quiescence wait (beads-csl7.5).
 *
 * The production bound is kCpcIdleTimeoutMs (5 s); a test lowers it so it can
 * prove the cap without waiting it out. The destructor puts the timeout back,
 * clears the "known stuck" latch, and — importantly — hands the machine back
 * running: a helper that left CPC.paused true would leak a paused machine
 * into whatever test --gtest_shuffle runs next.
 */

#pragma once

#include <atomic>

#include "koncepcja.h"

namespace koncpc_test {

struct StuckZ80Thread {
  explicit StuckZ80Thread(int bound_ms) {
    cpc_set_idle_timeout_ms(bound_ms);
    g_z80_idle.store(false, std::memory_order_release);
  }
  ~StuckZ80Thread() {
    g_z80_idle.store(true, std::memory_order_release);
    // A successful wait clears the "known stuck" latch for later tests; the
    // lease must also resume, or the machine stays paused after this scope.
    {
      CpcPauseLease clear;
      clear.restore_run_state();
    }
    cpc_set_idle_timeout_ms(kCpcIdleTimeoutMs);
  }
  StuckZ80Thread(const StuckZ80Thread&) = delete;
  StuckZ80Thread& operator=(const StuckZ80Thread&) = delete;
};

}  // namespace koncpc_test
