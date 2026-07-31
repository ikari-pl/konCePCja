#include "launch_echo.h"

#include <gtest/gtest.h>

namespace {
constexpr std::uint64_t kWindow = 5000;
constexpr std::uint64_t kStart = 1000;
}  // namespace

// The one thing this exists for: the window manager hands a launch file back
// as a drop, and handling it would re-load it — over the second disk, in a
// two-drive launch.
TEST(LaunchEchoFilter, SwallowsAnEchoOfALaunchFile) {
  LaunchEchoFilter f;
  f.arm({"/games/a.dsk", "/games/b.dsk"}, kStart, kWindow);

  EXPECT_TRUE(f.consume("/games/a.dsk", kStart + 50));
  EXPECT_TRUE(f.consume("/games/b.dsk", kStart + 60));
}

// Consumed once each: the second drop of the same file is the user asking for
// it, and must be loaded and reported like any other drop.
TEST(LaunchEchoFilter, SwallowsEachFileOnlyOnce) {
  LaunchEchoFilter f;
  f.arm({"/games/a.dsk"}, kStart, kWindow);

  ASSERT_TRUE(f.consume("/games/a.dsk", kStart + 50));
  EXPECT_FALSE(f.consume("/games/a.dsk", kStart + 100))
      << "a deliberate re-drop was swallowed";
}

// The bug the old filename-matching guard caused: a different file that merely
// shares a basename with a mounted one was silently ignored — no load, no
// message, nothing on the status bar.
TEST(LaunchEchoFilter, DoesNotSwallowADifferentFileWithTheSameName) {
  LaunchEchoFilter f;
  f.arm({"/games/a.dsk"}, kStart, kWindow);

  EXPECT_FALSE(f.consume("/elsewhere/a.dsk", kStart + 50))
      << "same basename, different file — this is a real drop";
}

// After the launch moment, nothing is an echo. Without this the filter could
// eat one deliberate drop hours later on a platform that never echoes.
TEST(LaunchEchoFilter, StopsSwallowingOnceTheWindowCloses) {
  LaunchEchoFilter f;
  f.arm({"/games/a.dsk"}, kStart, kWindow);

  EXPECT_FALSE(f.consume("/games/a.dsk", kStart + kWindow + 1));
  EXPECT_FALSE(f.armed()) << "the expired window should disarm the filter";
  EXPECT_FALSE(f.consume("/games/a.dsk", kStart + 10))
      << "a closed window must not reopen";
}

// A launch with no files must never swallow anything.
TEST(LaunchEchoFilter, UnarmedSwallowsNothing) {
  LaunchEchoFilter f;
  EXPECT_FALSE(f.consume("/games/a.dsk", kStart));

  f.arm({}, kStart, kWindow);
  EXPECT_FALSE(f.consume("/games/a.dsk", kStart + 50));
}

// A file that was never on the command line is always the user's.
TEST(LaunchEchoFilter, DoesNotSwallowAnUnregisteredFile) {
  LaunchEchoFilter f;
  f.arm({"/games/a.dsk"}, kStart, kWindow);

  EXPECT_FALSE(f.consume("/games/other.dsk", kStart + 50));
}
