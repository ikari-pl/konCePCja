/* system.auto_pause (beads-z6yz).
 *
 * The main window losing focus or the pointer pauses the machine; getting it
 * back runs it again. It used to run it whatever had stopped it: a machine
 * stopped at a breakpoint, by the user's Pause or by a text-IPC or KCPC
 * PAUSE ran again as soon as the pointer crossed back into the window.
 * Getting the window back may only undo the pause that losing it applied,
 * and only while that pause is still why the machine is stopped.
 */

#include <gtest/gtest.h>

#include "koncepcja.h"

extern t_CPC CPC;

namespace {

class AutoPauseTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_auto_pause_ = CPC.auto_pause;
    CPC.auto_pause = 1;
    cpc_run();
  }
  void TearDown() override {
    // Drop whatever claim a test left behind, then leave the machine running
    // as the other suites expect.
    cpc_pause();
    cpc_auto_pause(CpcAutoPauseSignal::Returned);
    cpc_run();
    CPC.auto_pause = saved_auto_pause_;
  }
  unsigned int saved_auto_pause_ = 0;
};

}  // namespace

TEST_F(AutoPauseTest, LeavingPausesAndReturningRuns) {
  cpc_auto_pause(CpcAutoPauseSignal::Left);
  EXPECT_TRUE(cpc_stopped());
  cpc_auto_pause(CpcAutoPauseSignal::Returned);
  EXPECT_FALSE(cpc_stopped());
}

// FOCUS_LOST and MOUSE_LEAVE both arrive when the user clicks another app.
TEST_F(AutoPauseTest, LeavingTwiceStillReturnsToARun) {
  cpc_auto_pause(CpcAutoPauseSignal::Left);
  cpc_auto_pause(CpcAutoPauseSignal::Left);
  cpc_auto_pause(CpcAutoPauseSignal::Returned);
  EXPECT_FALSE(cpc_stopped());
}

TEST_F(AutoPauseTest, ReturningDoesNotRunAMachineTheUserPaused) {
  cpc_pause();
  cpc_auto_pause(CpcAutoPauseSignal::Left);
  cpc_auto_pause(CpcAutoPauseSignal::Returned);
  EXPECT_TRUE(cpc_stopped()) << "auto-pause did not stop it";
}

TEST_F(AutoPauseTest, ReturningDoesNotRunAMachineStoppedAtABreakpoint) {
  CpcRunCause hit;
  hit.reason = CpcStopReason::Breakpoint;
  cpc_pause(hit);
  cpc_auto_pause(CpcAutoPauseSignal::Left);
  cpc_auto_pause(CpcAutoPauseSignal::Returned);
  EXPECT_TRUE(cpc_stopped());
}

// A text-IPC or KCPC PAUSE while the window is away: the machine is meant to
// stay stopped when the window comes back.
TEST_F(AutoPauseTest, APauseWhileAwayOutlivesTheReturn) {
  cpc_auto_pause(CpcAutoPauseSignal::Left);
  CpcRunCause kcpc;
  kcpc.client = 7;
  kcpc.request = 1;
  cpc_pause(kcpc);
  cpc_auto_pause(CpcAutoPauseSignal::Returned);
  EXPECT_TRUE(cpc_stopped());
}

TEST_F(AutoPauseTest, ARunAndAPauseWhileAwayOutliveTheReturn) {
  cpc_auto_pause(CpcAutoPauseSignal::Left);
  cpc_run();
  cpc_pause();
  cpc_auto_pause(CpcAutoPauseSignal::Returned);
  EXPECT_TRUE(cpc_stopped());
}

// Somebody ran the machine while the window was away: returning leaves it
// running, and the claim is gone, so a later stop is not undone either.
TEST_F(AutoPauseTest, ARunWhileAwayEndsTheClaim) {
  cpc_auto_pause(CpcAutoPauseSignal::Left);
  cpc_run();
  cpc_auto_pause(CpcAutoPauseSignal::Returned);
  EXPECT_FALSE(cpc_stopped());
  cpc_pause();
  cpc_auto_pause(CpcAutoPauseSignal::Returned);
  EXPECT_TRUE(cpc_stopped());
}

// FOCUS_GAINED and MOUSE_ENTER both arrive: the first one runs the machine
// and uses the claim up.
TEST_F(AutoPauseTest, AClaimIsUsedOnce) {
  cpc_auto_pause(CpcAutoPauseSignal::Left);
  cpc_auto_pause(CpcAutoPauseSignal::Returned);
  ASSERT_FALSE(cpc_stopped());
  cpc_pause();
  cpc_auto_pause(CpcAutoPauseSignal::Returned);
  EXPECT_TRUE(cpc_stopped());
}

// Another actor holds a pause lease (a KCPC read, a memory write) when the
// window goes: the auto-pause is a real stop that outlives the lease, and
// returning runs the machine again.
TEST_F(AutoPauseTest, LeavingUnderAnotherActorsLeaseStillReturns) {
  {
    CpcPauseLease lease;
    cpc_auto_pause(CpcAutoPauseSignal::Left);
    lease.restore_run_state();
  }
  EXPECT_TRUE(cpc_stopped());
  cpc_auto_pause(CpcAutoPauseSignal::Returned);
  EXPECT_FALSE(cpc_stopped());
}

TEST_F(AutoPauseTest, OffDoesNothing) {
  CPC.auto_pause = 0;
  cpc_auto_pause(CpcAutoPauseSignal::Left);
  EXPECT_FALSE(cpc_stopped());
  cpc_pause();
  cpc_auto_pause(CpcAutoPauseSignal::Returned);
  EXPECT_TRUE(cpc_stopped());
}
