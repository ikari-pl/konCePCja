#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "subcycle_bridge.h"

TEST(Scanlines, DimsOnlyOddRgb24Rows) {
  std::array<uint8_t, 12> pixels = {
      10, 20, 30, 40, 50, 60,  // row 0
      80, 40, 20, 12, 8,  4,   // row 1
  };

  subcycle_bridge_apply_scanlines_rgb24(pixels.data(), 2, 2, 25);

  EXPECT_EQ((std::array<uint8_t, 12>{
                10,
                20,
                30,
                40,
                50,
                60,  // unchanged even row
                60,
                30,
                15,
                9,
                6,
                3,  // odd row at 75%
            }),
            pixels);
}

TEST(Scanlines, ClampsIntensityToOneHundredPercent) {
  std::array<uint8_t, 6> pixels = {1, 2, 3, 255, 128, 64};

  subcycle_bridge_apply_scanlines_rgb24(pixels.data(), 1, 2, 150);

  EXPECT_EQ((std::array<uint8_t, 6>{1, 2, 3, 0, 0, 0}), pixels);
}
