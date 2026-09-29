// Quit policy: only a user gesture may raise the unsaved-disk dialog.
#include "quit_policy.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

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
  // The usual off spellings mean off, in any case: setting the variable to
  // "false" and getting dialogs suppressed anyway is a surprise nobody wants
  // to debug through a modal on their desktop.
  EXPECT_FALSE(koncpc_dialogs_suppressed_by_env("false"));
  EXPECT_FALSE(koncpc_dialogs_suppressed_by_env("FALSE"));
  EXPECT_FALSE(koncpc_dialogs_suppressed_by_env("no"));
  EXPECT_FALSE(koncpc_dialogs_suppressed_by_env("No"));
  EXPECT_FALSE(koncpc_dialogs_suppressed_by_env("off"));
  EXPECT_FALSE(koncpc_dialogs_suppressed_by_env("OFF"));
  // Anything else still suppresses — including a word that merely starts
  // with an off spelling.
  EXPECT_TRUE(koncpc_dialogs_suppressed_by_env("offline"));
  EXPECT_TRUE(koncpc_dialogs_suppressed_by_env("true"));
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

TEST(QuitMailbox, ExitCodesRoundTripThroughTheLowByte) {
  // The box carries the exit code in 8 bits because that is all a process
  // exit status has: _exit() hands the low byte to the parent's wait status,
  // so `quit -1` is 255 and `quit 256` is 0 either way. Pinned here so the
  // packing can never quietly start eating a bit of the code.
  const struct {
    int posted;
    int expected;
  } cases[] = {{0, 0},     {1, 1},      {3, 3},           {143, 143},
               {255, 255}, {256, 0},    {257, 1},         {-1, 255},
               {-2, 254},  {1000, 232}, {0x7FFFFFFF, 255}};
  for (const auto& c : cases) {
    QuitMailbox box;
    box.post(c.posted, false);
    auto quit = box.take();
    ASSERT_TRUE(quit.has_value()) << "posted " << c.posted;
    EXPECT_EQ(quit->code, c.expected) << "posted " << c.posted;
    EXPECT_FALSE(quit->ask_if_unsaved) << "posted " << c.posted;
    EXPECT_GE(quit->code, 0);
    EXPECT_LE(quit->code, 255);
  }
}

TEST(QuitMailbox, PostIsObservedAsAWholeRequest) {
  // Two threads quitting at the same instant must never have the main thread
  // read one poster's code beside the other's ask flag. Written as separate
  // atomics that mix happens often enough to catch here: an aux caller that
  // forgets ask_if_unsaved=false would then resurrect the dialog this policy
  // exists to suppress.
  //
  // Only two pairings are ever posted, so any other pairing is a tear.
  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};
  QuitMailbox box;

  std::thread poster_a([&] {
    while (!go.load()) {
    }
    while (!stop.load()) {
      box.post(7, false);
    }
  });
  std::thread poster_b([&] {
    while (!go.load()) {
    }
    while (!stop.load()) {
      box.post(200, true);
    }
  });

  go.store(true);
  int taken = 0;
  int torn = 0;
  for (int i = 0; i < 200000; ++i) {
    if (auto quit = box.take()) {
      ++taken;
      const bool a = quit->code == 7 && !quit->ask_if_unsaved;
      const bool b = quit->code == 200 && quit->ask_if_unsaved;
      if (!a && !b) {
        ++torn;
      }
    }
  }
  stop.store(true);
  poster_a.join();
  poster_b.join();

  EXPECT_GT(taken, 0) << "the racers never landed a request; test is vacuous";
  EXPECT_EQ(torn, 0);
}
