/* imgui_lease_ready(): the fail-closed guard every destructive UI action
 * goes through.
 *
 * The UI takes a CpcPauseLease before it shuts the video plugin down, ejects
 * a disk, formats one, or pulls/pushes a disc-tools image. When the Z80
 * thread will not leave its frame the lease comes back not-idle, and the work
 * must be skipped: touching machine state then is the use-after-free / stale
 * FDC bug the bound was added for. The IPC side of the same guard is covered
 * by IpcServerTest.LeaseTakingCommandsFailClosedOnAStuckZ80Thread; this is
 * the UI side, which had no coverage at all.
 */

#include <gtest/gtest.h>

#include "imgui_state.h"
#include "imgui_ui.h"
#include "imgui_ui_testable.h"
#include "koncepcja.h"
#include "stuck_z80_thread.h"

extern t_CPC CPC;

namespace {

using koncpc_test::StuckZ80Thread;

class LeaseGuardTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_toasts_ = imgui_state.toasts;
    imgui_state.toasts.clear();
    cpc_resume();
  }
  void TearDown() override {
    imgui_state.toasts = saved_toasts_;
    cpc_resume();
  }

 private:
  std::deque<ImGuiUIState::Toast> saved_toasts_;
};

}  // namespace

TEST_F(LeaseGuardTest, AnIdleLeaseLetsTheWorkThroughWithoutAToast) {
  ASSERT_FALSE(CPC.paused);
  {
    CpcPauseLease lease;
    EXPECT_TRUE(imgui_lease_ready(lease));
    EXPECT_TRUE(CPC.paused) << "the guard must leave the lease holding";
    lease.restore_run_state();
  }
  EXPECT_TRUE(imgui_state.toasts.empty()) << "nothing failed; no toast";
  EXPECT_FALSE(CPC.paused);
}

TEST_F(LeaseGuardTest, AStuckZ80ThreadSkipsTheWorkAndSaysSo) {
  ASSERT_FALSE(CPC.paused);
  {
    StuckZ80Thread const stuck(50);
    CpcPauseLease lease;
    ASSERT_FALSE(lease.idle());
    EXPECT_FALSE(imgui_lease_ready(lease))
        << "the caller must be told to skip its destructive work";
    EXPECT_FALSE(CPC.paused)
        << "the guard must hand back the run state it found";
  }
  ASSERT_EQ(1u, imgui_state.toasts.size()) << "the user was told nothing";
  EXPECT_EQ(rebuild_failure_text(ERR_Z80_NOT_IDLE, ""),
            imgui_state.toasts.front().message);
  EXPECT_EQ(ImGuiUIState::ToastLevel::Error, imgui_state.toasts.front().level);
}
