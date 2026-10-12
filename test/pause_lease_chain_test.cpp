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
