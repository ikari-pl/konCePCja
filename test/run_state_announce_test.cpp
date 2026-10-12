/* The announced run state (beads-3o1.4).
 *
 * "Paused" and "running" are announced, not sampled: cpc_pause(), a resume
 * that is applied and the last pause lease letting go each compare the state
 * that now holds with the one last announced, and tell the observer only
 * when they differ. A lease that pauses a running machine and runs it again
 * is therefore never announced, which is what keeps a client's reads and
 * writes from making the run state flicker for everybody else.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <vector>

#include "koncepcja.h"
#include "z80_view.h"

extern t_CPC CPC;

namespace {

std::vector<CpcRunEvent>* g_seen = nullptr;

void record(const CpcRunEvent& event) { g_seen->push_back(event); }

std::string describe(const std::vector<CpcRunEvent>& events) {
  std::string out;
  for (const CpcRunEvent& e : events) {
    switch (e.kind) {
      case CpcRunEventKind::Stopped:
        out += "stopped(" + std::to_string(static_cast<int>(e.reason)) + ") ";
        break;
      case CpcRunEventKind::Resumed:
        out += "resumed ";
        break;
      case CpcRunEventKind::Reset:
        out += "reset ";
        break;
    }
  }
  return out;
}

CpcRunCause by(uint32_t client, uint32_t request) {
  CpcRunCause cause;
  cause.client = client;
  cause.request = request;
  return cause;
}

class RunStateAnnounceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // A running machine that is announced as running, and nobody listening
    // while it gets there.
    cpc_set_run_state_observer(nullptr);
    cpc_resume();
    cpc_pause();
    cpc_resume();
    ASSERT_FALSE(cpc_announced_stopped());
    ASSERT_FALSE(cpc_step_active());
    g_seen = &seen_;
    cpc_set_run_state_observer(&record);
  }
  void TearDown() override {
    cpc_set_run_state_observer(nullptr);
    g_z80_idle.store(true);
    cpc_walk_end();  // closes a walk a failed test left open
    cpc_pause();     // ends a step a test left in progress
    cpc_resume();
    g_seen = nullptr;
  }

  std::vector<CpcRunEvent> seen_;
};

}  // namespace

TEST_F(RunStateAnnounceTest, APauseAndAResumeAreAnnouncedOnceEach) {
  cpc_pause(by(3, 14));
  ASSERT_EQ(seen_.size(), 1U) << describe(seen_);
  EXPECT_EQ(seen_[0].kind, CpcRunEventKind::Stopped);
  EXPECT_EQ(seen_[0].reason, CpcStopReason::Paused);
  EXPECT_EQ(seen_[0].cause_client, 3U);
  EXPECT_EQ(seen_[0].cause_request, 14U);
  EXPECT_EQ(seen_[0].step_client, 0U);
  EXPECT_TRUE(cpc_announced_stopped());

  cpc_pause();  // already stopped: nothing changed
  EXPECT_EQ(seen_.size(), 1U) << describe(seen_);

  EXPECT_TRUE(cpc_resume_applied(by(3, 15)));
  ASSERT_EQ(seen_.size(), 2U) << describe(seen_);
  EXPECT_EQ(seen_[1].kind, CpcRunEventKind::Resumed);
  EXPECT_EQ(seen_[1].cause_client, 3U);
  EXPECT_EQ(seen_[1].cause_request, 15U);
  EXPECT_FALSE(cpc_announced_stopped());

  cpc_resume();  // already running
  EXPECT_EQ(seen_.size(), 2U) << describe(seen_);
}

TEST_F(RunStateAnnounceTest, ALeaseOnARunningMachineIsNeverAnnounced) {
  for (int i = 0; i < 3; ++i) {
    CpcPauseLease lease;
    EXPECT_TRUE(CPC.paused);
    lease.restore_run_state();
    EXPECT_FALSE(CPC.paused);
  }
  EXPECT_TRUE(seen_.empty()) << describe(seen_);
}

TEST_F(RunStateAnnounceTest, ALeaseOnAPausedMachineIsNeverAnnounced) {
  cpc_pause();
  seen_.clear();
  {
    CpcPauseLease lease;
    lease.restore_run_state();
  }
  EXPECT_TRUE(seen_.empty()) << describe(seen_);
  EXPECT_TRUE(cpc_announced_stopped());
}

TEST_F(RunStateAnnounceTest, NestedLeasesAnnounceNothingUntilTheLastLetsGo) {
  CpcPauseLease outer;
  {
    CpcPauseLease inner;
    inner.restore_run_state();
  }
  EXPECT_TRUE(seen_.empty()) << "the outer lease still holds the machine";
  outer.restore_run_state();
  EXPECT_TRUE(seen_.empty()) << describe(seen_);
  EXPECT_FALSE(CPC.paused);
}

// A stop that lands while leases are held (somebody's Pause, a breakpoint in
// the frame a lease waited out) is real, and is announced when the last
// holder lets go: with the cause the stop was given, not the holder's.
TEST_F(RunStateAnnounceTest, AStopUnderALeaseIsAnnouncedWhenTheLeaseGoes) {
  CpcPauseLease lease;
  cpc_pause(by(5, 77));
  EXPECT_TRUE(seen_.empty()) << "not settled while the lease is held";
  lease.restore_run_state();
  ASSERT_EQ(seen_.size(), 1U) << describe(seen_);
  EXPECT_EQ(seen_[0].kind, CpcRunEventKind::Stopped);
  EXPECT_EQ(seen_[0].cause_client, 5U);
  EXPECT_EQ(seen_[0].cause_request, 77U);
  EXPECT_TRUE(CPC.paused);
}

TEST_F(RunStateAnnounceTest, AnInnerStopIsAnnouncedByTheOuterHolder) {
  CpcPauseLease outer;
  {
    CpcPauseLease inner;
    inner.release_stopped(by(2, 9));
  }
  EXPECT_TRUE(seen_.empty());
  outer.restore_run_state();
  ASSERT_EQ(seen_.size(), 1U) << describe(seen_);
  EXPECT_EQ(seen_[0].kind, CpcRunEventKind::Stopped);
  EXPECT_EQ(seen_[0].cause_request, 9U);
}

// `reset --no-resume` and its KCPC twin: a lease that ends in a stop.
TEST_F(RunStateAnnounceTest, ResetThatStaysPausedAnnouncesTheResetThenTheStop) {
  {
    CpcPauseLease lease;
    cpc_announce_reset(by(4, 21));  // what emulator_reset() does at its end
    lease.release_stopped(by(4, 21));
  }
  ASSERT_EQ(seen_.size(), 2U) << describe(seen_);
  EXPECT_EQ(seen_[0].kind, CpcRunEventKind::Reset);
  EXPECT_EQ(seen_[0].cause_client, 4U);
  EXPECT_EQ(seen_[0].cause_request, 21U);
  EXPECT_EQ(seen_[1].kind, CpcRunEventKind::Stopped);
  EXPECT_EQ(seen_[1].reason, CpcStopReason::Paused);
  EXPECT_EQ(seen_[1].cause_request, 21U);

  // And the reset that resumes, from that stopped machine.
  seen_.clear();
  {
    CpcPauseLease lease;
    cpc_announce_reset(by(4, 22));
    lease.release_running(by(4, 22));
  }
  ASSERT_EQ(seen_.size(), 2U) << describe(seen_);
  EXPECT_EQ(seen_[0].kind, CpcRunEventKind::Reset);
  EXPECT_EQ(seen_[1].kind, CpcRunEventKind::Resumed);
  EXPECT_EQ(seen_[1].cause_request, 22U);

  // On a running machine a reset that resumes changes no run state.
  seen_.clear();
  {
    CpcPauseLease lease;
    cpc_announce_reset({});
    lease.release_running();
  }
  ASSERT_EQ(seen_.size(), 1U) << describe(seen_);
  EXPECT_EQ(seen_[0].kind, CpcRunEventKind::Reset);
}

// A single step of a paused machine changes no run state. It is announced
// all the same: the registers moved, and the client that asked waits for it.
TEST_F(RunStateAnnounceTest, AStepIsAnnouncedAlthoughTheMachineStaysStopped) {
  cpc_pause();
  seen_.clear();
  CpcRunCause step = by(6, 30);
  step.reason = CpcStopReason::StepDone;
  step.step = true;
  {
    CpcPauseLease lease;
    lease.release_stopped(step);
  }
  ASSERT_EQ(seen_.size(), 1U) << describe(seen_);
  EXPECT_EQ(seen_[0].kind, CpcRunEventKind::Stopped);
  EXPECT_EQ(seen_[0].reason, CpcStopReason::StepDone);
  EXPECT_EQ(seen_[0].cause_client, 6U);
  EXPECT_EQ(seen_[0].cause_request, 30U);
  EXPECT_EQ(seen_[0].step_client, 6U);
  EXPECT_FALSE(cpc_step_active());
}

// A frame step: a run that starts a step, and the next stop ends it.
TEST_F(RunStateAnnounceTest, TheStopThatEndsAStepNamesTheStep) {
  cpc_pause();
  seen_.clear();
  CpcRunCause step = by(8, 40);
  step.step = true;
  {
    CpcPauseLease lease;
    lease.release_running(step);
  }
  ASSERT_EQ(seen_.size(), 1U) << describe(seen_);
  EXPECT_EQ(seen_[0].kind, CpcRunEventKind::Resumed);
  EXPECT_EQ(seen_[0].cause_request, 40U);
  EXPECT_TRUE(cpc_step_active());

  // The frame runner, when the frames have run: it knows no client.
  CpcRunCause done;
  done.reason = CpcStopReason::StepDone;
  cpc_pause(done);
  ASSERT_EQ(seen_.size(), 2U) << describe(seen_);
  EXPECT_EQ(seen_[1].reason, CpcStopReason::StepDone);
  EXPECT_EQ(seen_[1].cause_client, 8U);
  EXPECT_EQ(seen_[1].cause_request, 40U);
  EXPECT_EQ(seen_[1].step_client, 8U);
  EXPECT_FALSE(cpc_step_active());
}

// Somebody's pause during a step: the stop is the pauser's, and the client
// that asked for the step is still the one whose step ended.
TEST_F(RunStateAnnounceTest, APauseDuringAStepIsThePausersAndEndsTheStep) {
  CpcRunCause step = by(8, 41);
  step.step = true;
  {
    CpcPauseLease lease;
    lease.release_running(step);
  }
  EXPECT_TRUE(seen_.empty()) << "the machine ran before and runs now";
  ASSERT_TRUE(cpc_step_active());
  cpc_pause(by(9, 50));
  ASSERT_EQ(seen_.size(), 1U) << describe(seen_);
  EXPECT_EQ(seen_[0].reason, CpcStopReason::Paused);
  EXPECT_EQ(seen_[0].cause_client, 9U);
  EXPECT_EQ(seen_[0].cause_request, 50U);
  EXPECT_EQ(seen_[0].step_client, 8U);
  EXPECT_FALSE(cpc_step_active());
}

// beads-pja3: a Run while another actor holds a lease used to be dropped.
TEST_F(RunStateAnnounceTest, ARunUnderAnothersLeaseTakesEffectWhenItGoes) {
  cpc_pause();
  seen_.clear();
  CpcPauseLease other;  // a read that fell back to a lease, the text IPC, ...
  EXPECT_FALSE(cpc_resume_applied()) << "the plain resume is refused";
  EXPECT_FALSE(cpc_run(by(1, 60))) << "deferred, not dropped";
  EXPECT_TRUE(CPC.paused);
  EXPECT_TRUE(seen_.empty());
  other.restore_run_state();
  EXPECT_FALSE(CPC.paused) << "the Run was lost";
  ASSERT_EQ(seen_.size(), 1U) << describe(seen_);
  EXPECT_EQ(seen_[0].kind, CpcRunEventKind::Resumed);
  EXPECT_EQ(seen_[0].cause_client, 1U);
  EXPECT_EQ(seen_[0].cause_request, 60U);

  EXPECT_TRUE(cpc_run()) << "with no lease held it runs at once";
  EXPECT_EQ(seen_.size(), 1U) << "and a running machine is not resumed again";
}

// A stop after a deferred Run stands: the latest request wins.
TEST_F(RunStateAnnounceTest, AStopAfterADeferredRunLeavesTheMachineStopped) {
  cpc_pause();
  seen_.clear();
  CpcPauseLease other;
  cpc_run(by(1, 61));
  cpc_pause(by(1, 62));
  other.restore_run_state();
  EXPECT_TRUE(CPC.paused);
  EXPECT_TRUE(seen_.empty())
      << "stopped before, stopped now: " << describe(seen_);
}

// A pause from another thread leaves the frame in flight to finish. The stop
// is announced when the runner parks, so that what the event says about the
// machine is what a read then finds.
TEST_F(RunStateAnnounceTest, AStopIsAnnouncedOnceTheRunnerHasParked) {
  g_z80_idle.store(false);  // a frame is running
  cpc_pause(by(2, 70));
  EXPECT_TRUE(seen_.empty()) << "announced while the frame still ran";
  EXPECT_FALSE(cpc_announced_stopped());

  g_z80_idle.store(true);  // the runner found the pause and parked
  cpc_runner_parked();
  ASSERT_EQ(seen_.size(), 1U) << describe(seen_);
  EXPECT_EQ(seen_[0].kind, CpcRunEventKind::Stopped);
  EXPECT_EQ(seen_[0].cause_request, 70U);
  EXPECT_TRUE(cpc_announced_stopped());

  cpc_runner_parked();  // it parks every millisecond: nothing more to say
  EXPECT_EQ(seen_.size(), 1U);
}

// A pause that is taken back before the frame ended never happened.
TEST_F(RunStateAnnounceTest, APauseUndoneBeforeItLandedIsNotAnnounced) {
  g_z80_idle.store(false);
  cpc_pause();
  cpc_resume();
  g_z80_idle.store(true);
  cpc_runner_parked();
  EXPECT_TRUE(seen_.empty()) << describe(seen_);
  EXPECT_FALSE(cpc_announced_stopped());
}

// -- walks (beads-3o1.5, beads-pja3) ------------------------------------------
//
// A step over a call, out of a routine or to an address runs and stops the
// machine as often as it needs to. To everybody else it is one step.

namespace {

// The stop of a one-shot breakpoint the walk ran to, or of a breakpoint
// somebody set: committed the way the bridge commits a probe hit.
void breakpoint_stop(uint64_t epoch, word pc) {
  CpcRunCause hit;
  hit.reason = CpcStopReason::Breakpoint;
  hit.has_pc = true;
  hit.pc = pc;
  hit.addr = pc;
  ASSERT_TRUE(cpc_commit_breakpoint_stop(epoch, z80_breakpoint_generation(),
                                         hit, pc, /*watchpoint=*/false));
}

}  // namespace

TEST_F(RunStateAnnounceTest, AWalkIsOneResumeAndOneStop) {
  cpc_pause();
  seen_.clear();
  cpc_walk_begin(by(7, 70));
  EXPECT_TRUE(cpc_step_active());
  EXPECT_TRUE(seen_.empty()) << describe(seen_);

  // Twice over: run to a one-shot breakpoint, as a step out does for every
  // call it skips.
  for (int leg = 0; leg < 2; ++leg) {
    uint64_t epoch = 0;
    ASSERT_EQ(cpc_walk_run(epoch), CpcWalkRun::Running);
    EXPECT_FALSE(cpc_announced_stopped());
    breakpoint_stop(epoch, 0x4000);
    // What the walk waits for, without a lock: stopped, and not merely
    // leased.
    EXPECT_TRUE(cpc_announced_stopped());
  }
  ASSERT_EQ(seen_.size(), 1U) << describe(seen_);
  EXPECT_EQ(seen_[0].kind, CpcRunEventKind::Resumed);
  EXPECT_EQ(seen_[0].cause_client, 7U);
  EXPECT_EQ(seen_[0].cause_request, 70U);

  cpc_walk_verdict(CpcStopReason::StepDone);
  cpc_walk_end();
  ASSERT_EQ(seen_.size(), 2U) << describe(seen_);
  EXPECT_EQ(seen_[1].kind, CpcRunEventKind::Stopped);
  EXPECT_EQ(seen_[1].reason, CpcStopReason::StepDone);
  EXPECT_EQ(seen_[1].cause_client, 7U);
  EXPECT_EQ(seen_[1].cause_request, 70U);
  EXPECT_EQ(seen_[1].step_client, 7U);
  EXPECT_EQ(seen_[1].addr, 0U) << "the one-shot breakpoint is not the news";
  EXPECT_FALSE(cpc_step_active());
  EXPECT_TRUE(cpc_announced_stopped());
}

TEST_F(RunStateAnnounceTest, AWalkThatNeverRanTheMachineStillEndsWithAStop) {
  cpc_pause();
  seen_.clear();
  cpc_walk_begin(by(7, 71));
  cpc_walk_verdict(CpcStopReason::StepDone);  // one instruction, stepped
  cpc_walk_end();
  ASSERT_EQ(seen_.size(), 1U) << describe(seen_);
  EXPECT_EQ(seen_[0].kind, CpcRunEventKind::Stopped);
  EXPECT_EQ(seen_[0].reason, CpcStopReason::StepDone);
  EXPECT_EQ(seen_[0].cause_request, 71U);
}

TEST_F(RunStateAnnounceTest, AWalkStoppedBySomethingElseSaysWhat) {
  cpc_pause();
  seen_.clear();
  cpc_walk_begin(by(7, 72));
  uint64_t epoch = 0;
  ASSERT_EQ(cpc_walk_run(epoch), CpcWalkRun::Running);
  // A watchpoint somebody set fires on the way.
  CpcRunCause hit;
  hit.reason = CpcStopReason::Watchpoint;
  hit.addr = 0x9000;
  hit.access = 2;
  hit.value = 0x5A;
  hit.old_value = 0x11;
  ASSERT_TRUE(cpc_commit_breakpoint_stop(epoch, z80_breakpoint_generation(),
                                         hit, 0x9000, /*watchpoint=*/true));
  cpc_walk_stopped_by_other();
  cpc_walk_end();
  ASSERT_EQ(seen_.size(), 2U) << describe(seen_);
  EXPECT_EQ(seen_[1].reason, CpcStopReason::Watchpoint);
  EXPECT_EQ(seen_[1].addr, 0x9000);
  EXPECT_EQ(seen_[1].access, 2);
  EXPECT_EQ(seen_[1].value, 0x5A);
  EXPECT_EQ(seen_[1].old_value, 0x11);
  // The step ran into it: the stop is the step's.
  EXPECT_EQ(seen_[1].cause_client, 7U);
  EXPECT_EQ(seen_[1].cause_request, 72U);
  EXPECT_EQ(seen_[1].step_client, 7U);
}

// The walk single-steps onto a breakpoint. The probe is blind there, so the
// walk itself says what it met.
TEST_F(RunStateAnnounceTest, AWalkCanNameTheBreakpointItSteppedOnto) {
  cpc_pause();
  seen_.clear();
  cpc_walk_begin(by(7, 73));
  CpcRunCause met;
  met.reason = CpcStopReason::Breakpoint;
  met.has_pc = true;
  met.pc = 0x8014;
  met.addr = 0x8014;
  cpc_walk_verdict(met);
  cpc_walk_end();
  ASSERT_EQ(seen_.size(), 1U) << describe(seen_);
  EXPECT_EQ(seen_[0].reason, CpcStopReason::Breakpoint);
  EXPECT_EQ(seen_[0].pc, 0x8014);
  EXPECT_EQ(seen_[0].addr, 0x8014);
  EXPECT_EQ(seen_[0].cause_request, 73U);
}

// A pause while the walk has the machine stopped (it single-steps a stopped
// machine) changes no run state. The walk is told all the same, and may not
// run the machine again.
TEST_F(RunStateAnnounceTest, APauseInterruptsAWalkEvenOnAStoppedMachine) {
  cpc_pause();
  seen_.clear();
  cpc_walk_begin(by(7, 74));
  EXPECT_FALSE(cpc_walk_interrupted());
  cpc_pause(by(9, 90));
  EXPECT_TRUE(cpc_walk_interrupted());
  EXPECT_TRUE(seen_.empty()) << "the walk announces how its step ended";
  uint64_t epoch = 0;
  EXPECT_EQ(cpc_walk_run(epoch), CpcWalkRun::Interrupted);
  EXPECT_TRUE(CPC.paused);
  cpc_walk_stopped_by_other();
  cpc_walk_end();
  ASSERT_EQ(seen_.size(), 1U) << describe(seen_);
  EXPECT_EQ(seen_[0].reason, CpcStopReason::Paused);
  EXPECT_EQ(seen_[0].cause_client, 9U) << "the stop is the pauser's";
  EXPECT_EQ(seen_[0].cause_request, 90U);
  EXPECT_EQ(seen_[0].step_client, 7U) << "and the step that ended is 7's";
  EXPECT_FALSE(cpc_walk_interrupted()) << "the next walk starts clean";
}

// beads-pja3: another actor's pause lease is not a stop. It pauses the
// machine for the length of a memory write; a walk that polled g_emu_paused
// took that for the end of its run.
TEST_F(RunStateAnnounceTest, ALeaseDuringAWalkIsNeitherAStopNorANewRun) {
  cpc_pause();
  seen_.clear();
  cpc_walk_begin(by(7, 75));
  uint64_t epoch = 0;
  ASSERT_EQ(cpc_walk_run(epoch), CpcWalkRun::Running);
  for (int i = 0; i < 3; ++i) {
    CpcPauseLease lease;
    EXPECT_TRUE(g_emu_paused.load()) << "what the walk used to look at";
    EXPECT_FALSE(cpc_announced_stopped()) << "what it looks at now";
    uint64_t ignored = 0;
    EXPECT_EQ(cpc_walk_run(ignored), CpcWalkRun::LeaseHeld);
    lease.restore_run_state();
  }
  EXPECT_FALSE(CPC.paused);
  EXPECT_FALSE(cpc_walk_interrupted());
  EXPECT_EQ(cpc_resume_epoch(), epoch)
      << "the lease put the walk's run back: it is still that run";
  EXPECT_TRUE(cpc_pause_if_epoch(epoch)) << "and the walk may still end it";
  cpc_walk_verdict(CpcStopReason::StepTimeout);
  cpc_walk_end();
  ASSERT_EQ(seen_.size(), 2U) << describe(seen_);
  EXPECT_EQ(seen_[0].kind, CpcRunEventKind::Resumed);
  EXPECT_EQ(seen_[1].reason, CpcStopReason::StepTimeout);
  EXPECT_EQ(seen_[1].cause_request, 75U);
}

// A run somebody asked for while a lease was held is a new run, though: the
// holder that lets go last starts it.
TEST_F(RunStateAnnounceTest, ARunDeferredByALeaseIsANewEpoch) {
  cpc_pause();
  uint64_t const epoch = cpc_resume_epoch();
  {
    CpcPauseLease lease;
    EXPECT_FALSE(cpc_run());
    lease.restore_run_state();
  }
  EXPECT_FALSE(CPC.paused);
  EXPECT_EQ(cpc_resume_epoch(), epoch + 1);
}

TEST_F(RunStateAnnounceTest, NestedWalksAnnounceOnce) {
  cpc_pause();
  seen_.clear();
  cpc_walk_begin(by(7, 76));
  {
    CpcWalk const inner;  // the run to an address inside a step out
    cpc_walk_verdict(CpcStopReason::StepDone);
  }
  EXPECT_TRUE(seen_.empty()) << describe(seen_);
  EXPECT_TRUE(cpc_step_active());
  cpc_walk_verdict(CpcStopReason::StepStalled);  // the latest verdict stands
  cpc_walk_end();
  ASSERT_EQ(seen_.size(), 1U) << describe(seen_);
  EXPECT_EQ(seen_[0].reason, CpcStopReason::StepStalled);
  EXPECT_EQ(seen_[0].cause_request, 76U);
}

// A step request pauses a running machine under a lease, opens the walk and
// lets go of the lease as part of the step. Nobody is told of that pause:
// the machine was running, and the next thing to say is how the step ended.
TEST_F(RunStateAnnounceTest, TheLeaseAStepRequestLetsGoOfIsNotAnnounced) {
  CpcRunCause step = by(7, 77);
  step.step = true;
  {
    CpcPauseLease lease;
    cpc_walk_begin(step);
    lease.release_stopped(step);
  }
  EXPECT_TRUE(seen_.empty()) << describe(seen_);
  EXPECT_TRUE(CPC.paused);
  EXPECT_TRUE(cpc_announced_stopped());
  EXPECT_FALSE(cpc_walk_interrupted()) << "that was not somebody's pause";
  uint64_t epoch = 0;
  ASSERT_EQ(cpc_walk_run(epoch), CpcWalkRun::Running);
  EXPECT_TRUE(seen_.empty()) << "it ran before the step and runs again";
  breakpoint_stop(epoch, 0x4003);
  cpc_walk_verdict(CpcStopReason::StepDone);
  cpc_walk_end();
  ASSERT_EQ(seen_.size(), 1U) << describe(seen_);
  EXPECT_EQ(seen_[0].kind, CpcRunEventKind::Stopped);
  EXPECT_EQ(seen_[0].reason, CpcStopReason::StepDone);
  EXPECT_EQ(seen_[0].cause_request, 77U);
}

// Somebody stopped and ran the machine under the walk. The walk gives up
// with nothing to announce; the step ends with the next stop, as a frame
// step does.
TEST_F(RunStateAnnounceTest, AWalkLeftRunningEndsWithTheNextStop) {
  cpc_pause();
  seen_.clear();
  cpc_walk_begin(by(7, 78));
  uint64_t epoch = 0;
  ASSERT_EQ(cpc_walk_run(epoch), CpcWalkRun::Running);
  cpc_pause();
  EXPECT_TRUE(cpc_resume_applied());
  EXPECT_NE(cpc_resume_epoch(), epoch);
  EXPECT_FALSE(cpc_pause_if_epoch(epoch)) << "not the walk's run any more";
  cpc_walk_stopped_by_other();
  cpc_walk_end();
  ASSERT_EQ(seen_.size(), 1U) << describe(seen_);
  EXPECT_EQ(seen_[0].kind, CpcRunEventKind::Resumed);
  EXPECT_TRUE(cpc_step_active());

  cpc_pause(by(3, 33));
  ASSERT_EQ(seen_.size(), 2U) << describe(seen_);
  EXPECT_EQ(seen_[1].reason, CpcStopReason::Paused);
  EXPECT_EQ(seen_[1].cause_client, 3U);
  EXPECT_EQ(seen_[1].step_client, 7U);
  EXPECT_FALSE(cpc_step_active());
}

// A core that is quitting ends the walk without pausing anything.
TEST_F(RunStateAnnounceTest, AWalkCanBeInterruptedWithoutAPause) {
  cpc_walk_interrupt();
  EXPECT_FALSE(cpc_walk_interrupted()) << "there is no walk to interrupt";
  cpc_pause();
  cpc_walk_begin();
  cpc_walk_interrupt();
  EXPECT_TRUE(cpc_walk_interrupted());
  cpc_walk_end();
  EXPECT_FALSE(cpc_walk_interrupted());
}
