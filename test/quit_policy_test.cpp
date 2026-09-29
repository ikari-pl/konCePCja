// Quit policy: only a user gesture may raise the unsaved-disk dialog.
#include "quit_policy.h"

#include <gtest/gtest.h>

TEST(QuitPolicy, PromptsOnlyForAGestureWithADirtyDiskInTheGui) {
  EXPECT_TRUE(koncpc_quit_should_prompt(/*headless=*/false,
                                        /*ask_if_unsaved=*/true,
                                        /*drive_altered=*/true,
                                        /*dialogs_suppressed=*/false));
}

TEST(QuitPolicy, ProgrammaticQuitNeverPrompts) {
  // IPC `quit`, SIGTERM, --exit-after and --exit-on-break all pass
  // ask_if_unsaved=false. This is the flag that used to be lost on the hop
  // from an auxiliary thread to the main thread.
  EXPECT_FALSE(
      koncpc_quit_should_prompt(false, /*ask_if_unsaved=*/false, true, false));
}

TEST(QuitPolicy, HeadlessNeverPrompts) {
  EXPECT_FALSE(koncpc_quit_should_prompt(/*headless=*/true, true, true, false));
}

TEST(QuitPolicy, CleanDiskNeverPrompts) {
  EXPECT_FALSE(
      koncpc_quit_should_prompt(false, true, /*drive_altered=*/false, false));
}

TEST(QuitPolicy, EnvSuppressionWinsOverAGesture) {
  EXPECT_FALSE(koncpc_quit_should_prompt(false, true, true,
                                         /*dialogs_suppressed=*/true));
}

TEST(QuitPolicy, EnvVariableParsing) {
  EXPECT_FALSE(koncpc_dialogs_suppressed_by_env(nullptr));
  EXPECT_FALSE(koncpc_dialogs_suppressed_by_env(""));
  EXPECT_FALSE(koncpc_dialogs_suppressed_by_env("0"));
  EXPECT_TRUE(koncpc_dialogs_suppressed_by_env("1"));
  EXPECT_TRUE(koncpc_dialogs_suppressed_by_env("yes"));
}

TEST(QuitPolicy, ExitActionAsksOnlyOnTheMainThread) {
  // F10, the ImGui menu and the native Quit item dispatch KONCPC_EXIT on the
  // main thread; `-a KONCPC_EXIT` is replayed on the Z80 thread and used to
  // raise the dialog too.
  EXPECT_TRUE(koncpc_exit_action_asks(/*on_main_thread=*/true));
  EXPECT_FALSE(koncpc_exit_action_asks(/*on_main_thread=*/false));
}

TEST(QuitPolicy, SignalExitCodeFollowsTheShellConvention) {
  EXPECT_EQ(koncpc_signal_exit_code(2), 130);   // SIGINT
  EXPECT_EQ(koncpc_signal_exit_code(15), 143);  // SIGTERM
  EXPECT_NE(koncpc_signal_exit_code(15), 0);    // never reads as success
}

TEST(QuitMailbox, EmptyUntilPosted) {
  QuitMailbox box;
  EXPECT_FALSE(box.take().has_value());
}

TEST(QuitMailbox, CarriesCodeAndAskAcrossTheHop) {
  // The flag that used to be lost: an IPC `quit 3` posts ask=false, and the
  // main thread must read exactly that, not the gesture default.
  QuitMailbox box;
  box.post(3, /*ask_if_unsaved=*/false);
  auto quit = box.take();
  ASSERT_TRUE(quit.has_value());
  EXPECT_EQ(quit->code, 3);
  EXPECT_FALSE(quit->ask_if_unsaved);
}

TEST(QuitMailbox, TakingEmptiesTheBox) {
  // A second QUIT (SDL raising one on its own) must not replay the first
  // request's code or its ask=false.
  QuitMailbox box;
  box.post(1, false);
  ASSERT_TRUE(box.take().has_value());
  EXPECT_FALSE(box.take().has_value());
}

TEST(QuitMailbox, LatestPostWins) {
  QuitMailbox box;
  box.post(1, true);
  box.post(4, false);
  auto quit = box.take();
  ASSERT_TRUE(quit.has_value());
  EXPECT_EQ(quit->code, 4);
  EXPECT_FALSE(quit->ask_if_unsaved);
}
