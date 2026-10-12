// konCePCja — tests for the KCPC machine gate (beads-3o1.2).
//
// The gate gets the machine's pause state, its stopped-read and its pause
// lease through GateHost, so these tests run real reader and runner threads
// against a fake machine. The fake keeps the one promise the real seams keep:
// a frame, the boundary that follows it and a reader's own work never overlap.
// Every read below writes to plain, unsynchronised memory, so a gate that ran
// two of them at once, or one of them during a frame, is a data race
// ThreadSanitizer reports.

#include "kcpc/kcpc_gate.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using kcpc::Gate;
using kcpc::GateResult;
using kcpc::Stamp;

class FakeMachine {
 public:
  kcpc::GateHost host() {
    kcpc::GateHost h;
    // What g_emu_paused says: a stop and a lease look the same.
    h.paused = [this] { return paused.load() || foreign_lease.load(); };
    // What cpc_read_while_stopped() gives: only a real stop, held meanwhile.
    h.read_stopped = [this](const std::function<void()>& work) {
      std::scoped_lock const lock(clocking);
      if (!paused.load() || foreign_lease.load()) return false;
      stopped_reads.fetch_add(1);
      work();
      return true;
    };
    h.with_lease = [this](const std::function<void(bool)>& work) {
      if (!goes_idle.load()) return false;
      // What CpcPauseLease gives a holder: no frame runs meanwhile.
      std::scoped_lock const lock(clocking);
      leases.fetch_add(1);
      work(paused.load());
      return true;
    };
    return h;
  }

  // One frame and its boundary, as the runner does them.
  void run_frame(Gate& gate) {
    std::scoped_lock const lock(clocking);
    ++ticks;  // "clocking the machine": races with any read that overlaps it
    gate.service_frame();
  }

  std::atomic<bool> paused{false};         // somebody stopped the machine
  std::atomic<bool> foreign_lease{false};  // somebody else's lease holds it
  std::atomic<bool> goes_idle{true};       // a lease of ours gets the machine
  std::atomic<int> leases{0};
  std::atomic<int> stopped_reads{0};
  std::mutex clocking;
  std::uint64_t ticks = 0;  // guarded by `clocking`, like machine state
};

// A runner thread that turns out frames, back to back, until stopped. It
// yields instead of sleeping: a sleep is 15 ms long on Windows.
class Runner {
 public:
  Runner(FakeMachine& machine, Gate& gate)
      : thread_([this, &machine, &gate] {
          while (!stop_.load()) {
            if (!machine.paused.load()) machine.run_frame(gate);
            std::this_thread::yield();
          }
        }) {}
  ~Runner() {
    stop_.store(true);
    thread_.join();
  }
  Runner(const Runner&) = delete;
  Runner& operator=(const Runner&) = delete;

 private:
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

bool wait_for_queued(const Gate& gate, std::size_t count) {
  auto const deadline = Clock::now() + std::chrono::seconds(5);
  while (gate.queued() != count) {
    if (Clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  return true;
}

kcpc::GateTuning tuning(int fallback_ms, int poll_ms = 1) {
  kcpc::GateTuning t;
  t.fallback_ms = fallback_ms;
  t.poll_ms = poll_ms;
  return t;
}

TEST(KcpcGate, AStoppedMachineIsReadOnTheCallerWithoutALease) {
  FakeMachine machine;
  machine.paused.store(true);
  Gate gate(machine.host());

  int runs = 0;
  Stamp seen;
  std::thread::id ran_on;
  EXPECT_EQ(gate.read(16,
                      [&](const Stamp& stamp) {
                        ++runs;
                        seen = stamp;
                        ran_on = std::this_thread::get_id();
                      }),
            GateResult::kDone);
  EXPECT_EQ(runs, 1);
  EXPECT_EQ(ran_on, std::this_thread::get_id());
  EXPECT_TRUE(seen.paused);
  EXPECT_EQ(seen.frame, gate.frame());
  EXPECT_EQ(machine.stopped_reads.load(), 1);
  EXPECT_EQ(machine.leases.load(), 0)
      << "a lease makes a Run be refused; a stopped machine needs none";
  EXPECT_EQ(gate.counters().while_paused, 1U);
  EXPECT_EQ(gate.counters().at_boundary, 0U);
}

// Somebody else's lease holds the machine (the text IPC in the middle of a
// command, say). It is paused, but not stopped. The reader does not answer
// with a lease of its own: readers doing that keep one another's leases alive,
// and while any lease is held a Run is refused. It waits for the lease to
// pass, and then the machine is stopped...
TEST(KcpcGate, AReadWaitsOutAnotherLeaseOverAStoppedMachine) {
  FakeMachine machine;
  machine.paused.store(true);
  machine.foreign_lease.store(true);
  Gate gate(machine.host(), tuning(/*fallback_ms=*/60000));

  std::atomic<int> runs{0};
  Stamp seen;
  GateResult result = GateResult::kBusy;
  std::thread reader([&] {
    result = gate.read(16, [&](const Stamp& stamp) {
      runs.fetch_add(1);
      seen = stamp;
    });
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_EQ(runs.load(), 0) << "read while a lease holder was at work";
  machine.foreign_lease.store(false);
  reader.join();

  EXPECT_EQ(result, GateResult::kDone);
  EXPECT_EQ(runs.load(), 1);
  EXPECT_TRUE(seen.paused);
  EXPECT_EQ(machine.stopped_reads.load(), 1);
  EXPECT_EQ(machine.leases.load(), 0);
}

// ...or running again, and the read is taken at a frame boundary.
TEST(KcpcGate, AReadWaitsOutAnotherLeaseOverARunningMachine) {
  FakeMachine machine;
  machine.foreign_lease.store(true);
  Gate gate(machine.host(), tuning(/*fallback_ms=*/60000));

  std::atomic<int> runs{0};
  Stamp seen{0, true};
  GateResult result = GateResult::kBusy;
  std::thread reader([&] {
    result = gate.read(16, [&](const Stamp& stamp) {
      runs.fetch_add(1);
      seen = stamp;
    });
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_EQ(runs.load(), 0);
  machine.foreign_lease.store(false);
  ASSERT_TRUE(wait_for_queued(gate, 1));
  machine.run_frame(gate);
  reader.join();

  EXPECT_EQ(result, GateResult::kDone);
  EXPECT_EQ(runs.load(), 1);
  EXPECT_FALSE(seen.paused);
  EXPECT_EQ(machine.leases.load(), 0);
  EXPECT_EQ(gate.counters().at_boundary, 1U);
}

// A lease that lasts longer than the fallback: now a lease of our own is the
// way in, and it says what the run state really was.
TEST(KcpcGate, ALeaseThatLastsIsMetWithALeaseAfterTheFallback) {
  FakeMachine machine;
  machine.foreign_lease.store(true);  // over a running machine
  Gate gate(machine.host(), tuning(/*fallback_ms=*/30));

  int runs = 0;
  Stamp seen{0, true};
  auto const begin = Clock::now();
  EXPECT_EQ(gate.read(16,
                      [&](const Stamp& stamp) {
                        ++runs;
                        seen = stamp;
                      }),
            GateResult::kDone);
  EXPECT_GE(Clock::now() - begin, std::chrono::milliseconds(30));
  EXPECT_EQ(runs, 1);
  EXPECT_FALSE(seen.paused) << "held by a lease is not paused";
  EXPECT_EQ(machine.stopped_reads.load(), 0);
  EXPECT_EQ(machine.leases.load(), 1);
  EXPECT_EQ(gate.counters().fallback, 1U);
}

// A host that offers no stopped-read still works: everything goes through
// the lease.
TEST(KcpcGate, WithoutAStoppedReadAPausedMachineIsReadUnderALease) {
  FakeMachine machine;
  machine.paused.store(true);
  kcpc::GateHost host = machine.host();
  host.read_stopped = nullptr;
  Gate gate(host);
  Stamp seen;
  EXPECT_EQ(gate.read(1, [&](const Stamp& stamp) { seen = stamp; }),
            GateResult::kDone);
  EXPECT_TRUE(seen.paused);
  EXPECT_EQ(machine.leases.load(), 1);
}

// Two reads of a paused machine describe the same instant.
TEST(KcpcGate, TheStampStandsStillWhilePaused) {
  FakeMachine machine;
  Gate gate(machine.host());
  for (int i = 0; i < 7; ++i) machine.run_frame(gate);
  machine.paused.store(true);

  Stamp first;
  Stamp second;
  ASSERT_EQ(gate.read(1, [&](const Stamp& s) { first = s; }),
            GateResult::kDone);
  ASSERT_EQ(gate.read(1, [&](const Stamp& s) { second = s; }),
            GateResult::kDone);
  EXPECT_EQ(first.frame, 7U);
  EXPECT_EQ(second.frame, 7U);
  EXPECT_TRUE(first.paused && second.paused);
}

TEST(KcpcGate, ARunningMachineIsReadByTheRunnerAtTheFrameBoundary) {
  FakeMachine machine;
  Gate gate(machine.host(), tuning(/*fallback_ms=*/60000));

  int runs = 0;
  Stamp seen;
  std::thread::id ran_on;
  GateResult result = GateResult::kBusy;
  std::thread reader([&] {
    result = gate.read(64, [&](const Stamp& stamp) {
      ++runs;
      seen = stamp;
      ran_on = std::this_thread::get_id();
    });
  });
  ASSERT_TRUE(wait_for_queued(gate, 1));
  EXPECT_EQ(runs, 0) << "a queued read runs at a boundary, not before";

  machine.run_frame(gate);
  std::uint64_t const boundary = gate.frame();
  reader.join();

  EXPECT_EQ(result, GateResult::kDone);
  EXPECT_EQ(runs, 1);
  EXPECT_EQ(ran_on, std::this_thread::get_id()) << "the runner copies";
  EXPECT_FALSE(seen.paused);
  EXPECT_EQ(seen.frame, boundary);
  EXPECT_EQ(machine.leases.load(), 0) << "a live read never takes a lease";
  EXPECT_EQ(gate.queued(), 0U);
  EXPECT_EQ(gate.counters().at_boundary, 1U);
}

TEST(KcpcGate, AnEmptyQueueOnlyCountsTheFrame) {
  FakeMachine machine;
  Gate gate(machine.host());
  EXPECT_EQ(gate.frame(), 0U);
  for (int i = 0; i < 1000; ++i) gate.service_frame();
  EXPECT_EQ(gate.frame(), 1000U);
  EXPECT_EQ(gate.counters().at_boundary, 0U);
}

// The tier is too slow to finish a frame, or the runner is gone: the reader
// stops waiting and runs the read itself under a lease.
TEST(KcpcGate, AReadNoBoundaryServesFallsBackToTheLease) {
  FakeMachine machine;
  Gate gate(machine.host(), tuning(/*fallback_ms=*/40));

  int runs = 0;
  Stamp seen{99, true};
  auto const begin = Clock::now();
  EXPECT_EQ(gate.read(8,
                      [&](const Stamp& stamp) {
                        ++runs;
                        seen = stamp;
                      }),
            GateResult::kDone);
  auto const waited = Clock::now() - begin;

  EXPECT_EQ(runs, 1);
  EXPECT_GE(waited, std::chrono::milliseconds(40));
  EXPECT_FALSE(seen.paused) << "the lease found the machine running";
  EXPECT_EQ(machine.leases.load(), 1);
  EXPECT_EQ(gate.queued(), 0U) << "the job was taken back off the queue";
  EXPECT_EQ(gate.counters().fallback, 1U);

  // The boundary that comes too late finds nothing to run.
  machine.run_frame(gate);
  EXPECT_EQ(runs, 1);
  EXPECT_EQ(gate.counters().at_boundary, 0U);
}

// Acceptance 4: a read queued while the machine ran is completed when the
// machine is paused under it, exactly once, and without sitting out the whole
// fallback.
TEST(KcpcGate, AQueuedReadIsCompletedWhenTheMachinePausesUnderIt) {
  FakeMachine machine;
  Gate gate(machine.host(), tuning(/*fallback_ms=*/60000, /*poll_ms=*/2));

  std::atomic<int> runs{0};
  Stamp seen;
  GateResult result = GateResult::kBusy;
  std::thread reader([&] {
    result = gate.read(8, [&](const Stamp& stamp) {
      runs.fetch_add(1);
      seen = stamp;
    });
  });
  ASSERT_TRUE(wait_for_queued(gate, 1));

  auto const begin = Clock::now();
  machine.paused.store(true);  // a breakpoint: no boundary will come
  reader.join();
  auto const waited = Clock::now() - begin;

  EXPECT_EQ(result, GateResult::kDone);
  EXPECT_EQ(runs.load(), 1);
  EXPECT_TRUE(seen.paused);
  EXPECT_LT(waited, std::chrono::seconds(5))
      << "the reader noticed the pause; it did not wait for the fallback";
  EXPECT_EQ(machine.stopped_reads.load(), 1);
  EXPECT_EQ(machine.leases.load(), 0);
  EXPECT_EQ(gate.counters().while_paused, 1U);
  EXPECT_EQ(gate.counters().fallback, 0U) << "a pause is not a timeout";

  machine.paused.store(false);
  machine.run_frame(gate);
  EXPECT_EQ(runs.load(), 1) << "the runner must not run it a second time";
}

TEST(KcpcGate, ARefusedLeaseAnswersBusyAndRunsNothing) {
  FakeMachine machine;
  machine.goes_idle.store(false);
  Gate gate(machine.host(), tuning(/*fallback_ms=*/20));
  int runs = 0;
  auto const work = [&](const Stamp&) { ++runs; };

  machine.foreign_lease.store(true);
  EXPECT_EQ(gate.read(1, work), GateResult::kBusy) << "held by a lease";
  machine.foreign_lease.store(false);
  EXPECT_EQ(gate.read(1, work), GateResult::kBusy) << "the fallback path";

  EXPECT_EQ(runs, 0);
  EXPECT_EQ(gate.queued(), 0U);
  EXPECT_EQ(gate.counters().busy, 2U);
}

// The budget bounds what one boundary costs the runner, but never splits a
// read and never starves one.
TEST(KcpcGate, TheFrameBudgetSpreadsReadsOverBoundaries) {
  FakeMachine machine;
  kcpc::GateTuning t = tuning(/*fallback_ms=*/60000);
  t.frame_budget = 1000;
  Gate gate(machine.host(), t);

  std::atomic<int> runs{0};
  std::vector<std::thread> readers;
  for (int i = 0; i < 3; ++i) {
    readers.emplace_back([&] {
      EXPECT_EQ(gate.read(600, [&](const Stamp&) { runs.fetch_add(1); }),
                GateResult::kDone);
    });
    ASSERT_TRUE(wait_for_queued(gate, static_cast<std::size_t>(i) + 1));
  }

  machine.run_frame(gate);
  EXPECT_EQ(runs.load(), 1) << "600 + 600 is over the budget of 1000";
  EXPECT_EQ(gate.queued(), 2U);
  machine.run_frame(gate);
  EXPECT_EQ(runs.load(), 2);
  machine.run_frame(gate);
  EXPECT_EQ(runs.load(), 3);
  for (auto& reader : readers) reader.join();
}

TEST(KcpcGate, SmallReadsShareABoundaryAndAHugeOneStillRuns) {
  FakeMachine machine;
  kcpc::GateTuning t = tuning(/*fallback_ms=*/60000);
  t.frame_budget = 1000;
  Gate gate(machine.host(), t);

  std::atomic<int> runs{0};
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back([&] {
      EXPECT_EQ(gate.read(250, [&](const Stamp&) { runs.fetch_add(1); }),
                GateResult::kDone);
    });
  }
  ASSERT_TRUE(wait_for_queued(gate, 4));
  machine.run_frame(gate);
  EXPECT_EQ(runs.load(), 4) << "4 x 250 fits the budget exactly";
  for (auto& reader : readers) reader.join();

  // Far over the budget, and still one boundary: all its bytes are from the
  // same instant.
  std::thread big([&] {
    EXPECT_EQ(gate.read(50000, [&](const Stamp&) { runs.fetch_add(1); }),
              GateResult::kDone);
  });
  ASSERT_TRUE(wait_for_queued(gate, 1));
  machine.run_frame(gate);
  EXPECT_EQ(runs.load(), 5);
  big.join();
}

TEST(KcpcGate, ShutDownFailsQueuedAndLaterReads) {
  FakeMachine machine;
  Gate gate(machine.host(), tuning(/*fallback_ms=*/60000));
  std::atomic<int> runs{0};
  auto const work = [&](const Stamp&) { runs.fetch_add(1); };

  GateResult queued = GateResult::kDone;
  std::thread reader([&] { queued = gate.read(1, work); });
  ASSERT_TRUE(wait_for_queued(gate, 1));
  auto const begin = Clock::now();
  gate.shut_down();
  reader.join();
  EXPECT_LT(Clock::now() - begin, std::chrono::seconds(5));

  EXPECT_EQ(queued, GateResult::kShuttingDown);
  EXPECT_EQ(gate.read(1, work), GateResult::kShuttingDown);
  machine.paused.store(true);
  EXPECT_EQ(gate.read(1, work), GateResult::kShuttingDown);
  EXPECT_EQ(runs.load(), 0);
  EXPECT_EQ(gate.queued(), 0U);
  EXPECT_EQ(machine.leases.load(), 0);
  EXPECT_EQ(machine.stopped_reads.load(), 0);

  gate.reopen();
  EXPECT_EQ(gate.read(1, work), GateResult::kDone);
  EXPECT_EQ(runs.load(), 1);
}

// A machine paused and resumed while a read waits: the read follows it from
// the queue to the stopped path and back, and still runs once.
TEST(KcpcGate, AReadFollowsTheMachineThroughPauseAndResume) {
  FakeMachine machine;
  Gate gate(machine.host(), tuning(/*fallback_ms=*/60000));
  std::atomic<int> runs{0};
  Stamp seen{0, true};
  GateResult result = GateResult::kBusy;
  std::thread reader([&] {
    result = gate.read(16, [&](const Stamp& stamp) {
      runs.fetch_add(1);
      seen = stamp;
    });
  });
  ASSERT_TRUE(wait_for_queued(gate, 1));
  // Paused, but a lease holder is at work, so the read cannot be taken...
  machine.foreign_lease.store(true);
  machine.paused.store(true);
  ASSERT_TRUE(wait_for_queued(gate, 0)) << "the reader took its job back";
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_EQ(runs.load(), 0);
  // ...and the lease holder resumes the machine before letting go.
  machine.paused.store(false);
  machine.foreign_lease.store(false);
  ASSERT_TRUE(wait_for_queued(gate, 1)) << "queued again for a boundary";
  machine.run_frame(gate);
  reader.join();

  EXPECT_EQ(result, GateResult::kDone);
  EXPECT_EQ(runs.load(), 1);
  EXPECT_FALSE(seen.paused);
  EXPECT_EQ(machine.leases.load(), 0);
  EXPECT_EQ(machine.stopped_reads.load(), 0);
}

// Every path at once: a runner turning out frames back to back, readers that
// give up after two milliseconds, and a machine that is paused and resumed
// underneath them. Each read must run exactly once, and never while a frame
// or another read is running.
TEST(KcpcGate, EveryReadRunsExactlyOnceUnderContention) {
  constexpr int kReaders = 6;
  FakeMachine machine;
  Gate gate(machine.host(), tuning(/*fallback_ms=*/2));

  std::uint64_t unguarded = 0;  // "machine state": only exclusion protects it
  std::atomic<std::uint64_t> reads{0};
  std::atomic<int> failures{0};
  std::atomic<bool> stop{false};
  {
    Runner const runner(machine, gate);
    std::vector<std::thread> readers;
    for (int r = 0; r < kReaders; ++r) {
      readers.emplace_back([&] {
        while (!stop.load()) {
          int ran = 0;
          GateResult const result = gate.read(100, [&](const Stamp& stamp) {
            ++ran;
            unguarded += stamp.frame;
          });
          if (result != GateResult::kDone || ran != 1) failures.fetch_add(1);
          reads.fetch_add(1);
        }
      });
    }
    for (int cycle = 0; cycle < 40; ++cycle) {
      machine.paused.store(true);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      machine.paused.store(false);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    stop.store(true);
    for (auto& reader : readers) reader.join();
  }

  EXPECT_EQ(failures.load(), 0);
  Gate::Counters const c = gate.counters();
  EXPECT_EQ(c.at_boundary + c.while_paused + c.fallback, reads.load());
  EXPECT_GT(c.at_boundary, 0U) << "the runner served none: nothing was tested";
  EXPECT_GT(c.while_paused + c.fallback, 0U)
      << "no read took the lease path: nothing was tested";
  EXPECT_EQ(gate.queued(), 0U);
  RecordProperty("at_boundary", static_cast<int>(c.at_boundary));
  RecordProperty("while_paused", static_cast<int>(c.while_paused));
  RecordProperty("fallback", static_cast<int>(c.fallback));
}

}  // namespace
