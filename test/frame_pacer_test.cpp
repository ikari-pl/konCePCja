// FramePacer: the 50 Hz deadline both emulation loops pace to (beads-gnx3),
// and whose lateness verdict drives auto frameskip (beads-af0k, beads-o478).
// A synthetic clock makes every case exact: 1000 ticks a second, 20 a frame.

#include "frame_pacer.h"

#include <gtest/gtest.h>

namespace {

constexpr uint64_t kFreq = 1000;
constexpr uint64_t kPeriod = kFreq / 50;

TEST(FramePacer, FirstFrameIsReleasedAtOnceAndIsNotLate) {
  FramePacer p;
  EXPECT_EQ(5000u, p.arrive(5000, kFreq));
  EXPECT_FALSE(p.late());
}

TEST(FramePacer, OnTimeFramesAreHeldToTheFiftyHertzGrid) {
  FramePacer p;
  p.arrive(5000, kFreq);
  // A frame that took 3 ticks waits for the next grid line, 20 after start.
  EXPECT_EQ(5000 + kPeriod, p.arrive(5003, kFreq));
  EXPECT_FALSE(p.late());
  // The grid is fixed: an oversleep to 5021 does not shift the next line.
  EXPECT_EQ(5000 + 2 * kPeriod, p.arrive(5021 + 3, kFreq));
  EXPECT_FALSE(p.late());
}

TEST(FramePacer, LessThanAPeriodBehindIsJitterNotLateness) {
  FramePacer p;
  p.arrive(5000, kFreq);  // next line: 5020
  // An oversleep left this frame 15 ticks past its line: released at once and
  // made up by the next frame, but not late.
  EXPECT_EQ(5020u, p.arrive(5035, kFreq));
  EXPECT_FALSE(p.late());
  EXPECT_EQ(5040u, p.arrive(5036, kFreq));
  EXPECT_FALSE(p.late());
}

TEST(FramePacer, MoreThanAPeriodBehindIsLateUntilCaughtUp) {
  FramePacer p;
  p.arrive(5000, kFreq);                           // next line: 5020
  uint64_t const release = p.arrive(5070, kFreq);  // 50 ticks behind
  EXPECT_TRUE(p.late());
  EXPECT_EQ(5020u, release) << "a late frame is released at once";
  // Drift correction: the following frames keep the old grid (5040, 5060,
  // 5080) and stay late while more than a period behind it.
  p.arrive(5071, kFreq);  // 31 behind 5040
  EXPECT_TRUE(p.late());
  p.arrive(5072, kFreq);  // 12 behind 5060: within a period
  EXPECT_FALSE(p.late());
  EXPECT_EQ(5080u, p.arrive(5073, kFreq)) << "caught up: back on the grid";
  EXPECT_FALSE(p.late());
}

TEST(FramePacer, ALongStallResyncsAndIsNotLate) {
  FramePacer p;
  p.arrive(5000, kFreq);  // next line: 5020
  // More than a quarter second past the line: a pause or a debugger stop.
  uint64_t const now = 5020 + kFreq / 4 + 1;
  EXPECT_EQ(now, p.arrive(now, kFreq));
  EXPECT_FALSE(p.late()) << "the first frame after a pause must render";
  EXPECT_EQ(now + kPeriod, p.arrive(now + 1, kFreq));
}

TEST(FramePacer, AnUnpacedFrameRestartsPacing) {
  FramePacer p;
  p.arrive(5000, kFreq);
  p.arrive(5070, kFreq);  // 50 behind 5020
  ASSERT_TRUE(p.late());
  p.unpaced();
  EXPECT_FALSE(p.late());
  EXPECT_EQ(9000u, p.arrive(9000, kFreq));
  EXPECT_FALSE(p.late());
}

}  // namespace
