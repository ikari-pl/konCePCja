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
