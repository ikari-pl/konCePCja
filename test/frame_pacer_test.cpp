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

// beads-drzi: a stop of 150 ms (under the quarter second) was repaid as seven
// frames released at once.
TEST(FramePacer, AStopIsNotCaughtUp) {
  FramePacer p;
  p.arrive(5000, kFreq);  // next line: 5020
  p.stopped();
  // The first frame after the stop arrives 150 ticks past its line.
  EXPECT_EQ(5170u, p.arrive(5170, kFreq));
  EXPECT_FALSE(p.late());
  EXPECT_EQ(5170 + kPeriod, p.arrive(5171, kFreq)) << "paced from the restart";
  EXPECT_FALSE(p.late());
  // The stop is over: a later overshoot is jitter again, and is made up.
  EXPECT_EQ(5170 + 2 * kPeriod, p.arrive(5170 + 2 * kPeriod + 5, kFreq));
  EXPECT_EQ(5170 + 3 * kPeriod, p.arrive(5170 + 2 * kPeriod + 6, kFreq));
}

// A stop that is over before the frame's deadline must not release that frame
// early.
TEST(FramePacer, AStopThatEndsBeforeTheDeadlineKeepsTheGrid) {
  FramePacer p;
  p.arrive(5000, kFreq);  // next line: 5020
  p.stopped();
  EXPECT_EQ(5020u, p.arrive(5008, kFreq));
  EXPECT_EQ(5040u, p.arrive(5023, kFreq));
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

// PacingThreadRule: when the frame runner asks the OS to wake it on time
// (host_pacing_thread, beads-fijd).

TEST(PacingThreadRule, PacedFramesThatFitArePacedFromTheFirst) {
  PacingThreadRule r;
  EXPECT_TRUE(r.frame(true, 500));    // Fast
  EXPECT_TRUE(r.frame(true, 14000));  // Faithful
  EXPECT_TRUE(r.frame(true, 19999));
}

TEST(PacingThreadRule, AnUnpacedFrameIsNeverPaced) {
  PacingThreadRule r;
  EXPECT_FALSE(r.frame(false, 500));
  EXPECT_TRUE(r.frame(true, 500));  // pacing back on: nothing was owed
}

TEST(PacingThreadRule, AFrameThatFillsThePeriodStopsPacingForAWhile) {
  PacingThreadRule r;
  ASSERT_TRUE(r.frame(true, 500));
  // The runner cannot keep up: it would never block.
  EXPECT_FALSE(r.frame(true, PacingThreadRule::kPeriodUs));
  // It stays off until kRetryFrames frames in a row fit again.
  for (unsigned i = 1; i < PacingThreadRule::kRetryFrames; ++i) {
    EXPECT_FALSE(r.frame(true, 500)) << i;
  }
  EXPECT_TRUE(r.frame(true, 500));
}

TEST(PacingThreadRule, ALongFrameRestartsTheWait) {
  PacingThreadRule r;
  r.frame(true, 30000);
  for (unsigned i = 0; i + 1 < PacingThreadRule::kRetryFrames; ++i) {
    r.frame(true, 500);
  }
  EXPECT_FALSE(r.frame(true, 25000));  // one frame short of the retry
  for (unsigned i = 1; i < PacingThreadRule::kRetryFrames; ++i) {
    EXPECT_FALSE(r.frame(true, 500)) << i;
  }
  EXPECT_TRUE(r.frame(true, 500));
}
