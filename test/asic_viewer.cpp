#include <gtest/gtest.h>

#include "asic.h"
#include "asic_debug.h"
#include "koncepcja.h"

extern byte *pbRegisterPage;
extern t_CRTC CRTC;

namespace {

class AsicViewerTest : public testing::Test {
 protected:
  void SetUp() override {
    asic_reset();
    memset(&CRTC, 0, sizeof(CRTC));
    if (!reg_page_) {
      reg_page_ = new byte[16 * 1024]();
    }
    memset(reg_page_, 0, 16 * 1024);
    pbRegisterPage = reg_page_;
  }

  static void TearDownTestSuite() {
    delete[] reg_page_;
    reg_page_ = nullptr;
    pbRegisterPage = nullptr;
  }

  static byte *reg_page_;
};

byte *AsicViewerTest::reg_page_ = nullptr;

// --- asic_dump_dma_channel tests ---

TEST_F(AsicViewerTest, DmaChannelInRange) {
  asic.dma.ch[0].source_address = 0x1234;
  asic.dma.ch[0].loop_address = 0x1000;
  asic.dma.ch[0].prescaler = 0x05;
  asic.dma.ch[0].enabled = true;
  asic.dma.ch[0].interrupt = false;
  asic.dma.ch[0].pause_ticks = 0;
  asic.dma.ch[0].tick_cycles = 3;
  asic.dma.ch[0].loops = 2;

  std::string result = asic_dump_dma_channel(0);
  EXPECT_NE(result.find("ch0:"), std::string::npos);
  EXPECT_NE(result.find("addr=1234"), std::string::npos);
  EXPECT_NE(result.find("loop_addr=1000"), std::string::npos);
  EXPECT_NE(result.find("prescaler=05"), std::string::npos);
  EXPECT_NE(result.find("enabled=1"), std::string::npos);
  EXPECT_NE(result.find("interrupt=0"), std::string::npos);
  EXPECT_NE(result.find("pause=0"), std::string::npos);
  EXPECT_NE(result.find("tick_cycles=03"), std::string::npos);
  EXPECT_NE(result.find("loop_count=2"), std::string::npos);
}

TEST_F(AsicViewerTest, DmaChannelWithPause) {
  asic.dma.ch[1].source_address = 0x4000;
  asic.dma.ch[1].enabled = true;
  asic.dma.ch[1].interrupt = true;
  asic.dma.ch[1].pause_ticks = 10;
  asic.dma.ch[1].loops = 7;

  std::string result = asic_dump_dma_channel(1);
  EXPECT_NE(result.find("ch1:"), std::string::npos);
  EXPECT_NE(result.find("enabled=1"), std::string::npos);
  EXPECT_NE(result.find("interrupt=1"), std::string::npos);
  EXPECT_NE(result.find("pause=1"), std::string::npos);
  EXPECT_NE(result.find("loop_count=7"), std::string::npos);
}

TEST_F(AsicViewerTest, DmaChannelOutOfRange) {
  EXPECT_TRUE(asic_dump_dma_channel(3).empty());
  EXPECT_TRUE(asic_dump_dma_channel(-1).empty());
}

// --- asic_dump_sprite tests ---

TEST_F(AsicViewerTest, SpriteInRange) {
  asic.sprites_x[0] = 100;
  asic.sprites_y[0] = 200;
  asic.sprites_mag_x[0] = 2;
  asic.sprites_mag_y[0] = 1;

  std::string result = asic_dump_sprite(0);
  EXPECT_NE(result.find("spr0: x=100 y=200 mag_x=2 mag_y=1 enabled=1"), std::string::npos);
  // Header line + 16 rows of pixel data = 16 newlines
  int newlines = 0;
  for (char c : result) if (c == '\n') newlines++;
  EXPECT_EQ(newlines, 16);
}

TEST_F(AsicViewerTest, SpriteDisabledWhenMagZero) {
  asic.sprites_x[5] = 50;
  asic.sprites_y[5] = 60;
  asic.sprites_mag_x[5] = 0;
  asic.sprites_mag_y[5] = 0;

  std::string result = asic_dump_sprite(5);
  EXPECT_NE(result.find("enabled=0"), std::string::npos);
}

TEST_F(AsicViewerTest, SpriteOutOfRange) {
  EXPECT_TRUE(asic_dump_sprite(16).empty());
  EXPECT_TRUE(asic_dump_sprite(-1).empty());
}

TEST_F(AsicViewerTest, SpritePixelDataAllZeros) {
  std::string result = asic_dump_sprite(0);
  EXPECT_NE(result.find("0000000000000000"), std::string::npos);
}

TEST_F(AsicViewerTest, SpritePixelDataWithColors) {
  // sprites[id][x][y]: color+16 when color>0, 0 when transparent
  asic.sprites[3][0][0] = 17;  // palette index 1
  asic.sprites[3][1][0] = 31;  // palette index 15 (F)
  asic.sprites[3][2][0] = 0;   // transparent

  std::string result = asic_dump_sprite(3);
  // First row: 1, F, 0, then thirteen 0s
  EXPECT_NE(result.find("1F00000000000000"), std::string::npos);
}

// --- Palette dump ---

TEST_F(AsicViewerTest, PaletteHas32Entries) {
  std::string result = asic_dump_palette();
  EXPECT_NE(result.find("pen0="), std::string::npos);
  EXPECT_NE(result.find("pen15="), std::string::npos);
  EXPECT_NE(result.find("ink0="), std::string::npos);
  EXPECT_NE(result.find("ink15="), std::string::npos);
}

// --- Full dump ---

TEST_F(AsicViewerTest, DumpAllContainsSections) {
  std::string result = asic_dump_all();
  EXPECT_NE(result.find("[sprites]"), std::string::npos);
  EXPECT_NE(result.find("[dma]"), std::string::npos);
  EXPECT_NE(result.find("[interrupts]"), std::string::npos);
  EXPECT_NE(result.find("[palette]"), std::string::npos);
  EXPECT_NE(result.find("locked="), std::string::npos);
}

TEST_F(AsicViewerTest, DumpAllShowsUnlocked) {
  asic.locked = false;
  std::string result = asic_dump_all();
  EXPECT_NE(result.find("locked=0"), std::string::npos);
}

TEST_F(AsicViewerTest, DumpAllShowsLocked) {
  asic.locked = true;
  std::string result = asic_dump_all();
  EXPECT_NE(result.find("locked=1"), std::string::npos);
}

}  // namespace
