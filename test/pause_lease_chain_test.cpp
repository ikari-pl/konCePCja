/* CpcPauseLease with more than one holder at a time (beads-3o1.1).
 *
 * A lease pauses the machine for one operation and then puts the run state
 * back. Holders overlap as soon as there is more than one client: two KCPC
 * connections, a KCPC connection and the text IPC, the IPC and the UI. Before
 * this was fixed a second holder found CPC.paused set by the first one's
 * lease, concluded the machine "was paused" and left it that way, while the
 * first holder's own resume was refused because a lease was still held. Four
 * KCPC clients reading MACHINE_INFO left the machine stopped for good.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "koncepcja.h"

extern t_CPC CPC;

namespace {

class PauseLeaseChainTest : public ::testing::Test {
 protected:
  void SetUp() override { cpc_resume(); }
  void TearDown() override { cpc_resume(); }
};

}  // namespace

TEST_F(PauseLeaseChainTest, OverlappingHoldersLeaveARunningMachineRunning) {
  ASSERT_FALSE(CPC.paused);
  CpcPauseLease first;
  CpcPauseLease second;
  EXPECT_FALSE(first.was_paused());
  EXPECT_FALSE(second.was_paused())
      << "paused only by the first lease: nobody stopped the machine";

  first.restore_run_state();
  EXPECT_TRUE(CPC.paused) << "the second lease still holds the machine";
  second.restore_run_state();
  EXPECT_FALSE(CPC.paused) << "the last holder puts the run state back";
}

TEST_F(PauseLeaseChainTest, ReleaseOrderDoesNotMatter) {
  ASSERT_FALSE(CPC.paused);
  CpcPauseLease first;
  CpcPauseLease second;
  second.restore_run_state();
  EXPECT_TRUE(CPC.paused);
  first.restore_run_state();
  EXPECT_FALSE(CPC.paused);
}

TEST_F(PauseLeaseChainTest, OverlappingHoldersLeaveAPausedMachinePaused) {
  cpc_pause();
  {
    CpcPauseLease first;
    CpcPauseLease second;
    EXPECT_TRUE(first.was_paused());
    EXPECT_TRUE(second.was_paused());
    first.restore_run_state();
    second.restore_run_state();
  }
  EXPECT_TRUE(CPC.paused);
}

// A chain ends when the last lease goes; the next lease looks afresh.
TEST_F(PauseLeaseChainTest, ANewChainReadsTheRunStateAgain) {
  {
    CpcPauseLease lease;
    EXPECT_FALSE(lease.was_paused());
    lease.restore_run_state();
  }
  cpc_pause();
  {
    CpcPauseLease lease;
    EXPECT_TRUE(lease.was_paused());
    lease.restore_run_state();
  }
  EXPECT_TRUE(CPC.paused);
}

// A breakpoint in the frame a lease is waiting out, the end of a frame step,
// somebody pressing Pause: a stop that arrives while a lease is held is meant
// to last, and the lease must not run the machine again when it lets go.
TEST_F(PauseLeaseChainTest, AStopDuringALeaseOutlivesIt) {
  ASSERT_FALSE(CPC.paused);
  {
    CpcPauseLease lease;
    EXPECT_FALSE(lease.was_paused());
    cpc_pause();
    lease.restore_run_state();
  }
  EXPECT_TRUE(CPC.paused) << "the lease resumed over a real stop";
}

TEST_F(PauseLeaseChainTest, AStopDuringALeaseIsSeenByLaterHolders) {
  ASSERT_FALSE(CPC.paused);
  CpcPauseLease first;
  cpc_pause();
  CpcPauseLease second;
  EXPECT_TRUE(second.was_paused());
  second.restore_run_state();
  first.restore_run_state();
  EXPECT_TRUE(CPC.paused);
}

// cpc_stopped() is what code waiting for emulated time asks instead of
// CPC.paused: a lease is not a stop.
TEST_F(PauseLeaseChainTest, ALeaseIsNotAStop) {
  EXPECT_FALSE(cpc_stopped());
  {
    CpcPauseLease lease;
    EXPECT_TRUE(CPC.paused);
    EXPECT_FALSE(cpc_stopped()) << "paused by a lease only";
    lease.restore_run_state();
  }
  EXPECT_FALSE(cpc_stopped());
  cpc_pause();
  EXPECT_TRUE(cpc_stopped());
  {
    CpcPauseLease lease;
    EXPECT_FALSE(cpc_stopped()) << "a lease is held: the state is not final";
    lease.restore_run_state();
  }
  EXPECT_TRUE(cpc_stopped());
}

// cpc_read_while_stopped(): how a tool reads a stopped machine without
// holding a lease (beads-3o1.2). A KCPC client polling a paused machine
// through leases made the text IPC's `run` answer ERR 409 pause-lease-held,
// and cpc_resume() do nothing, whenever it landed on one.
TEST_F(PauseLeaseChainTest, AStoppedMachineIsReadWithoutALease) {
  int reads = 0;
  EXPECT_FALSE(cpc_read_while_stopped([&] { ++reads; }))
      << "running: there is nothing stopped to read";
  {
    CpcPauseLease lease;
    EXPECT_FALSE(cpc_read_while_stopped([&] { ++reads; }))
        << "paused by a lease only: not stopped";
    lease.restore_run_state();
  }
  EXPECT_EQ(reads, 0);

  cpc_pause();
  EXPECT_TRUE(cpc_read_while_stopped([&] {
    ++reads;
    EXPECT_TRUE(CPC.paused);
  }));
  EXPECT_EQ(reads, 1);
  EXPECT_TRUE(cpc_stopped()) << "reading left it stopped";
  {
    CpcPauseLease lease;
    EXPECT_FALSE(cpc_read_while_stopped([&] { ++reads; }))
        << "stopped, but a lease holder is at work on it";
    lease.restore_run_state();
  }
  EXPECT_EQ(reads, 1);
}

// The point of it: a Run that arrives during the read is neither refused nor
// lost. It waits for the read and then takes effect.
TEST_F(PauseLeaseChainTest, ARunDuringAStoppedReadWaitsAndThenApplies) {
  cpc_pause();
  std::atomic<bool> reading{false};
  std::atomic<bool> read_done{false};
  std::atomic<bool> resume_called{false};
  bool applied = false;
  bool read_was_done_when_resume_returned = false;

  std::thread resumer([&] {
    while (!reading.load()) std::this_thread::yield();
    resume_called.store(true);
    applied = cpc_resume_applied();
    read_was_done_when_resume_returned = read_done.load();
  });
  EXPECT_TRUE(cpc_read_while_stopped([&] {
    reading.store(true);
    while (!resume_called.load()) std::this_thread::yield();
    // Long enough for the resume to have gone ahead if nothing held it.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_TRUE(CPC.paused) << "resumed in the middle of the read";
    read_done.store(true);
  }));
  resumer.join();

  EXPECT_TRUE(applied) << "the Run was refused";
  EXPECT_TRUE(read_was_done_when_resume_returned);
  EXPECT_FALSE(CPC.paused) << "the Run was lost";
}

// --- How a holder lets go (beads-mk1w) --------------------------------------
//
// restore_run_state() is for an operation that borrowed the machine. Two
// kinds of operation mean something else, and used to say it by hand with
// release() and cpc_resume()/nothing, which is only right for a lone holder.

// A step is a stop somebody asked for. Alone, the machine stays paused
// either way; with another holder that found the machine running, only a
// stop that says so survives that holder's restore.
TEST_F(PauseLeaseChainTest, ReleaseStoppedIsARealStop) {
  ASSERT_FALSE(CPC.paused);
  {
    CpcPauseLease lease;
    lease.release_stopped();
    EXPECT_FALSE(lease.active());
  }
  EXPECT_TRUE(cpc_stopped());
  cpc_resume();

  CpcPauseLease other;
  CpcPauseLease stepper;
  stepper.release_stopped();
  EXPECT_FALSE(cpc_stopped()) << "the other lease is still at work";
  other.restore_run_state();
  EXPECT_TRUE(cpc_stopped()) << "the other holder ran a stepped machine";
}

// reset and the start of a frame step promise a running machine, whatever
// was there before.
TEST_F(PauseLeaseChainTest, ReleaseRunningRunsAPausedMachine) {
  cpc_pause();
  uint64_t const epoch = cpc_resume_epoch();
  {
    CpcPauseLease lease;
    EXPECT_TRUE(lease.was_paused());
    lease.release_running();
  }
  EXPECT_FALSE(CPC.paused);
  EXPECT_FALSE(g_emu_paused.load());
  EXPECT_EQ(epoch + 1, cpc_resume_epoch()) << "a real pause->run transition";
}

// cpc_resume() does nothing while a lease is held, so a holder that wanted
// the machine running and was not the last one never got it.
TEST_F(PauseLeaseChainTest, ReleaseRunningTakesEffectWhenTheLastHolderLetsGo) {
  cpc_pause();
  CpcPauseLease other;
  CpcPauseLease runner;
  EXPECT_TRUE(other.was_paused());
  runner.release_running();
  EXPECT_TRUE(CPC.paused) << "the other lease still holds the machine";
  EXPECT_FALSE(cpc_stopped());
  other.restore_run_state();
  EXPECT_FALSE(CPC.paused) << "the run was lost";
}

// Last request wins, as it does without leases.
TEST_F(PauseLeaseChainTest, AStopAfterReleaseRunningStands) {
  cpc_pause();
  CpcPauseLease other;
  CpcPauseLease runner;
  runner.release_running();
  cpc_pause();
  other.restore_run_state();
  EXPECT_TRUE(cpc_stopped());
}

TEST_F(PauseLeaseChainTest, ReleaseRunningAfterAStopRuns) {
  ASSERT_FALSE(CPC.paused);
  CpcPauseLease lease;
  cpc_pause();              // a breakpoint in the frame the lease waited out
  lease.release_running();  // then the reset the lease was for
  EXPECT_FALSE(CPC.paused);
}

// A holder whose own work cleared CPC.paused behind the lease's back
// (emulator_init() does, in a machine rebuild) still ends with both pause
// flags agreeing, running or stopped.
TEST_F(PauseLeaseChainTest, TheFlagsAgreeAfterAHolderClearedCpcPaused) {
  ASSERT_FALSE(CPC.paused);
  {
    CpcPauseLease lease;
    CPC.paused = false;
    lease.restore_run_state();
  }
  EXPECT_FALSE(CPC.paused);
  EXPECT_FALSE(g_emu_paused.load()) << "CPC.paused says running, the runner "
                                       "is still told to stay parked";
  cpc_pause();
  {
    CpcPauseLease lease;
    CPC.paused = false;
    lease.restore_run_state();
  }
  EXPECT_TRUE(CPC.paused);
  EXPECT_TRUE(g_emu_paused.load());
  {
    CpcPauseLease lease;
    CPC.paused = false;
    lease.release_stopped();
  }
  EXPECT_TRUE(cpc_stopped());
  EXPECT_TRUE(g_emu_paused.load());
}

// --- A reader among lease holders (beads-mk1w, from beads-3o1.2) ------------
//
// Leases do not exclude one another: two holders both see the runner idle
// and both go ahead. Harmless between two operations that each restore what
// they found, not between one that copies out of the machine and one that is
// rebuilding it. read_alone() is the reader's side: it runs only while its
// lease is the only one, and holds off new ones until it is done.

TEST_F(PauseLeaseChainTest, ALoneHolderReadsAtOnce) {
  CpcPauseLease lease;
  int reads = 0;
  EXPECT_TRUE(lease.read_alone([&] { ++reads; }, 0));
  EXPECT_EQ(1, reads);
  lease.restore_run_state();
  EXPECT_FALSE(CPC.paused);
}

TEST_F(PauseLeaseChainTest, AReaderWaitsForTheOtherHolderToFinish) {
  std::atomic<bool> held{false};
  std::atomic<bool> mutating{true};
  std::thread mutator([&] {
    CpcPauseLease lease;
    held.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    mutating.store(false);
    lease.restore_run_state();
  });
  while (!held.load()) std::this_thread::yield();

  CpcPauseLease reader;
  bool read_during_the_mutation = false;
  EXPECT_TRUE(reader.read_alone(
      [&] { read_during_the_mutation = mutating.load(); }, 2000));
  EXPECT_FALSE(read_during_the_mutation);
  reader.restore_run_state();
  mutator.join();
  EXPECT_FALSE(CPC.paused);
}

TEST_F(PauseLeaseChainTest, AReaderGivesUpOnAHolderThatStays) {
  CpcPauseLease other;
  CpcPauseLease reader;
  int reads = 0;
  auto const start = std::chrono::steady_clock::now();
  EXPECT_FALSE(reader.read_alone([&] { ++reads; }, 30));
  EXPECT_GE(std::chrono::steady_clock::now() - start,
            std::chrono::milliseconds(30));
  EXPECT_EQ(0, reads);
  reader.restore_run_state();
  other.restore_run_state();
  EXPECT_FALSE(CPC.paused);
}

TEST_F(PauseLeaseChainTest, ANewHolderWaitsForTheRead) {
  CpcPauseLease reader;
  std::atomic<bool> reading{false};
  std::atomic<bool> read_done{false};
  std::atomic<bool> asked{false};
  bool read_was_done_when_the_lease_came = false;
  std::thread mutator([&] {
    while (!reading.load()) std::this_thread::yield();
    asked.store(true);
    CpcPauseLease lease;
    read_was_done_when_the_lease_came = read_done.load();
    lease.restore_run_state();
  });
  EXPECT_TRUE(reader.read_alone(
      [&] {
        reading.store(true);
        while (!asked.load()) std::this_thread::yield();
        // Long enough for the lease to have been granted if nothing held it.
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        read_done.store(true);
      },
      0));
  reader.restore_run_state();
  mutator.join();
  EXPECT_TRUE(read_was_done_when_the_lease_came);
  EXPECT_FALSE(CPC.paused);
}

TEST_F(PauseLeaseChainTest, ALeaseThatIsNotIdleDoesNotRead) {
  cpc_set_idle_timeout_ms(30);
  g_z80_idle.store(false);
  {
    CpcPauseLease lease;
    ASSERT_FALSE(lease.idle());
    int reads = 0;
    EXPECT_FALSE(lease.read_alone([&] { ++reads; }, 0));
    EXPECT_EQ(0, reads);
    lease.restore_run_state();
  }
  g_z80_idle.store(true);
  {
    CpcPauseLease clear;  // clears the "known stuck" latch
    clear.restore_run_state();
  }
  cpc_set_idle_timeout_ms(kCpcIdleTimeoutMs);
}

// Two holders on two threads that both change the machine (beads-nhfy): the
// text IPC loading a snapshot while the main thread rebuilds the machine, a
// KCPC reset beside a DevTools disc format. Each used to find the runner idle
// and go ahead; the second now waits for the first to let go.
TEST_F(PauseLeaseChainTest, TwoChangingHoldersOnTwoThreadsTakeTurns) {
  std::atomic<bool> held{false};
  std::atomic<bool> changing{false};
  std::thread first([&] {
    CpcPauseLease lease;
    ASSERT_TRUE(lease.idle());
    changing.store(true);
    held.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    changing.store(false);
    lease.restore_run_state();
  });
  while (!held.load()) std::this_thread::yield();

  CpcPauseLease second;
  EXPECT_TRUE(second.idle());
  EXPECT_FALSE(changing.load());
  second.restore_run_state();
  first.join();
  EXPECT_FALSE(CPC.paused);
}

// The wait for the other holder is bounded like the wait for the runner, and
// fails closed the same way: not idle, so the holder must not touch anything.
TEST_F(PauseLeaseChainTest, AHolderGivesUpOnAnotherThatStays) {
  std::atomic<bool> held{false};
  std::atomic<bool> done{false};
  std::thread first([&] {
    CpcPauseLease lease;
    held.store(true);
    while (!done.load()) std::this_thread::yield();
    lease.restore_run_state();
  });
  while (!held.load()) std::this_thread::yield();

  auto const start = std::chrono::steady_clock::now();
  CpcPauseLease second(CpcPauseLeaseMode::WaitImmediately, 30);
  EXPECT_FALSE(second.idle());
  EXPECT_GE(std::chrono::steady_clock::now() - start,
            std::chrono::milliseconds(30));
  second.restore_run_state();
  done.store(true);
  first.join();
  EXPECT_FALSE(CPC.paused);
}

// A thread that already holds the machine does not wait for itself: a
// cartridge insert resets under its own lease, a rebuild reapplies media.
TEST_F(PauseLeaseChainTest, ANestedHolderOnTheSameThreadGoesAhead) {
  CpcPauseLease outer;
  ASSERT_TRUE(outer.idle());
  CpcPauseLease inner(CpcPauseLeaseMode::WaitImmediately, 30);
  EXPECT_TRUE(inner.idle());
  inner.restore_run_state();
  CpcPauseLease again(CpcPauseLeaseMode::WaitImmediately, 30);
  EXPECT_TRUE(again.idle());
  again.restore_run_state();
  outer.restore_run_state();
  EXPECT_FALSE(CPC.paused);
}

// A lease that only pauses and never waits (a window screenshot, which waits
// for the main thread to render) does not keep anybody from the machine.
TEST_F(PauseLeaseChainTest, APauseOnlyHolderThatNeverWaitsKeepsNobodyOut) {
  CpcPauseLease pausing(CpcPauseLeaseMode::PauseOnly);
  bool idle = false;
  std::thread other([&] {
    CpcPauseLease lease(CpcPauseLeaseMode::WaitImmediately, 1000);
    idle = lease.idle();
    lease.restore_run_state();
  });
  other.join();
  EXPECT_TRUE(idle);
  pausing.restore_run_state();
  EXPECT_FALSE(CPC.paused);
}

// The cooperating pair: IPC `profile load` holds a lease while the main
// thread rebuilds the machine under its own, and waits for it. The holder
// lends the machine for the length of that wait and has it back afterwards;
// the machine stays paused throughout.
TEST_F(PauseLeaseChainTest, AHolderLendsTheMachineToTheThreadItWaitsFor) {
  CpcPauseLease lease;
  ASSERT_TRUE(lease.idle());
  bool rebuilt = false;
  bool paused_while_lent = false;
  EXPECT_TRUE(lease.lend([&] {
    std::thread main_thread([&] {
      CpcPauseLease rebuild(CpcPauseLeaseMode::WaitImmediately, 1000);
      rebuilt = rebuild.idle();
      paused_while_lent = CPC.paused;
      rebuild.restore_run_state();
    });
    main_thread.join();
  }));
  EXPECT_TRUE(rebuilt);
  EXPECT_TRUE(paused_while_lent);
  EXPECT_TRUE(lease.idle());

  // And it is the holder again: another thread waits for it.
  bool other_idle = true;
  std::thread other([&] {
    CpcPauseLease late(CpcPauseLeaseMode::WaitImmediately, 30);
    other_idle = late.idle();
    late.restore_run_state();
  });
  other.join();
  EXPECT_FALSE(other_idle);
  lease.restore_run_state();
  EXPECT_FALSE(CPC.paused);
}
