#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "subcycle_bridge.h"

TEST(Scanlines, DimsEveryRgb24Row) {
  // subcycle_bridge_apply_scanlines_rgb24 builds a fully-dimmed variant of
  // the whole native frame -- blit_fb composites it onto only the spare
  // destination rows (see the ScanlineGapRow tests below), so the helper
  // itself must darken every row, not pick rows by parity (beads: a row
  // dimmed here used to land on real picture content one-to-one; picking
  // *which* row is the renderer's job now).
  std::array<uint8_t, 12> pixels = {
      10, 20, 30, 40, 50, 60,  // row 0
      80, 40, 20, 12, 8,  4,   // row 1
  };

  subcycle_bridge_apply_scanlines_rgb24(pixels.data(), 2, 2, 25);

  EXPECT_EQ((std::array<uint8_t, 12>{
                7,  15, 22, 30, 37, 45,  // row 0 at 75%
                60, 30, 15, 9,  6,  3,   // row 1 at 75%
            }),
            pixels);
}

TEST(Scanlines, ClampsIntensityToOneHundredPercent) {
  std::array<uint8_t, 6> pixels = {1, 2, 3, 255, 128, 64};

  subcycle_bridge_apply_scanlines_rgb24(pixels.data(), 1, 2, 150);

  EXPECT_EQ((std::array<uint8_t, 6>{0, 0, 0, 0, 0, 0}), pixels);
}

TEST(ScanlineGapRow, NoGapAtOneToOneScale) {
  // Every source row maps to exactly one destination row -- nothing spare
  // to darken (this is the case the pre-fix code got wrong: it dimmed
  // real picture rows here instead of leaving them alone).
  int source_row = -1;
  for (int d = 0; d < 4; ++d) {
    EXPECT_FALSE(subcycle_bridge_scanline_gap_row(d, 4, 4, &source_row));
  }
}

TEST(ScanlineGapRow, LastOfCleanTwoRowSpanIsTheGap) {
  // src_h=2, dst_h=4: source row 0 -> dst rows {0,1}, source row 1 -> {2,3}.
  // The trailing row of each span (1 and 3) is the gap; the leading row
  // (0 and 2) must stay untouched (real content).
  int source_row = -1;
  EXPECT_FALSE(subcycle_bridge_scanline_gap_row(0, 2, 4, &source_row));
  EXPECT_TRUE(subcycle_bridge_scanline_gap_row(1, 2, 4, &source_row));
  EXPECT_EQ(source_row, 0);
  EXPECT_FALSE(subcycle_bridge_scanline_gap_row(2, 2, 4, &source_row));
  EXPECT_TRUE(subcycle_bridge_scanline_gap_row(3, 2, 4, &source_row));
  EXPECT_EQ(source_row, 1);
}

TEST(ScanlineGapRow, UnevenSpanStillPicksOnlyTheTrailingRow) {
  // src_h=2, dst_h=5 (2.5x, not a clean integer factor): floor(d*2/5) gives
  // source row 0 for d={0,1,2} and source row 1 for d={3,4} -- an uneven
  // 3-row/2-row split. Only the LAST row of each span (2 and 4) is a gap;
  // a real-world non-integer window size must not dim more than one
  // destination row per source scanline.
  int source_row = -1;
  EXPECT_FALSE(subcycle_bridge_scanline_gap_row(0, 2, 5, &source_row));
  EXPECT_FALSE(subcycle_bridge_scanline_gap_row(1, 2, 5, &source_row));
  EXPECT_TRUE(subcycle_bridge_scanline_gap_row(2, 2, 5, &source_row));
  EXPECT_EQ(source_row, 0);
  EXPECT_FALSE(subcycle_bridge_scanline_gap_row(3, 2, 5, &source_row));
  EXPECT_TRUE(subcycle_bridge_scanline_gap_row(4, 2, 5, &source_row));
  EXPECT_EQ(source_row, 1);
}

TEST(ScanlineGapRow, RejectsOutOfRangeInputs) {
  EXPECT_FALSE(subcycle_bridge_scanline_gap_row(-1, 2, 4, nullptr));
  EXPECT_FALSE(subcycle_bridge_scanline_gap_row(4, 2, 4, nullptr));
  EXPECT_FALSE(subcycle_bridge_scanline_gap_row(0, 0, 4, nullptr));
  EXPECT_FALSE(subcycle_bridge_scanline_gap_row(0, 2, 0, nullptr));
}
