/* A Run the user asks for while another actor holds a pause lease
 * (beads-jlbg).
 *
 * cpc_resume() does nothing while any CpcPauseLease is held. The GUI's Run
 * actions, closing the pause menu and the M4 web interface's pause toggle
 * all resumed with it, so a Run that arrived while a KCPC client was writing
 * memory (or the text IPC was holding the machine for a `reg set`) vanished:
 * the machine stayed paused and nothing said why. These tests are the second
 * actor: a lease held across the user's action and let go afterwards.
 */

#include <gtest/gtest.h>

#include "imgui_state.h"
#include "imgui_ui.h"
#include "koncepcja.h"
#include "m4board_http.h"

extern t_CPC CPC;

namespace {

class GuiRunUnderLeaseTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_menu_ = imgui_state.show_menu;
    saved_just_opened_ = imgui_state.menu_just_opened;
    saved_options_ = imgui_state.show_options;
    saved_quit_ = imgui_state.show_quit_confirm;
    imgui_state.show_menu = false;
    imgui_state.show_options = false;
    imgui_state.show_quit_confirm = false;
    g_m4_http.pending_pause_toggle.store(false);
    cpc_resume();
    ASSERT_FALSE(CPC.paused);
  }
  void TearDown() override {
    imgui_state.show_menu = saved_menu_;
    imgui_state.menu_just_opened = saved_just_opened_;
    imgui_state.show_options = saved_options_;
    imgui_state.show_quit_confirm = saved_quit_;
    g_m4_http.pending_pause_toggle.store(false);
    cpc_resume();
  }

 private:
  bool saved_menu_ = false;
  bool saved_just_opened_ = false;
  bool saved_options_ = false;
  bool saved_quit_ = false;
};

}  // namespace

// The DevTools Run/Pause button, the Disassembly window's Run and the command
// palette's "Pause / Resume" all go through dbg_run_pause_toggle().
TEST_F(GuiRunUnderLeaseTest, ADevToolsRunIsNotLostToAnothersLease) {
  cpc_pause();
  ASSERT_TRUE(cpc_stopped());
  CpcPauseLease other;  // a KCPC MEM_WRITE, a text IPC `reg set`, ...
  dbg_run_pause_toggle();
  EXPECT_TRUE(CPC.paused) << "the lease still holds the machine";
  other.restore_run_state();
  EXPECT_FALSE(CPC.paused) << "the user's Run was dropped";
}

// Closing the pause menu (Esc, Resume, a menu action) is the user asking for
// the machine back.
TEST_F(GuiRunUnderLeaseTest, ClosingTheMenuIsNotLostToAnothersLease) {
  imgui_open_menu();
  ASSERT_TRUE(cpc_stopped());
  CpcPauseLease other;
  imgui_close_menu();
  EXPECT_FALSE(imgui_state.show_menu);
  other.restore_run_state();
  EXPECT_FALSE(CPC.paused) << "the menu closed and the machine stayed paused";
}

// The menu opened while the lease was already held, on a running machine:
// the same lost Run, reached the other way round.
TEST_F(GuiRunUnderLeaseTest, AMenuOpenedAndClosedUnderALeaseLeavesItRunning) {
  CpcPauseLease other;
  imgui_open_menu();
  imgui_close_menu();
  other.restore_run_state();
  EXPECT_FALSE(CPC.paused) << "the menu closed and the machine stayed paused";
}

// A dialog that keeps the machine paused still does, lease or no lease.
TEST_F(GuiRunUnderLeaseTest, AMenuClosedOverAnOpenDialogStaysPaused) {
  imgui_open_menu();
  imgui_state.show_options = true;
  CpcPauseLease other;
  imgui_close_menu();
  other.restore_run_state();
  EXPECT_TRUE(cpc_stopped()) << "Options is open: it keeps the machine";
}

// The M4 web interface's pause button, drained on the main thread.
TEST_F(GuiRunUnderLeaseTest, AnM4ResumeIsNotLostToAnothersLease) {
  cpc_pause();
  ASSERT_TRUE(cpc_stopped());
  CpcPauseLease other;
  g_m4_http.pending_pause_toggle.store(true);
  g_m4_http.drain_pending();
  other.restore_run_state();
  EXPECT_FALSE(CPC.paused) << "the web interface's resume was dropped";
}

// A stop that comes after the deferred Run stands: the latest request wins.
TEST_F(GuiRunUnderLeaseTest, APauseAfterADeferredRunLeavesItStopped) {
  cpc_pause();
  CpcPauseLease other;
  dbg_run_pause_toggle();
  cpc_pause();
  other.restore_run_state();
  EXPECT_TRUE(cpc_stopped());
}

// One actor: nothing changes.
TEST_F(GuiRunUnderLeaseTest, WithNoLeaseHeldTheToggleActsAtOnce) {
  dbg_run_pause_toggle();
  EXPECT_TRUE(cpc_stopped());
  dbg_run_pause_toggle();
  EXPECT_FALSE(CPC.paused);
}
