#include <gtest/gtest.h>

#include "asic.h"
#include "asic_debug.h"
#include "koncepcja.h"

extern byte *pbRegisterPage;
extern t_CRTC CRTC;

namespace {

class AsicDebugTest : public testing::Test {
 protected:
  void SetUp() override {
    asic_reset();
    memset(&CRTC, 0, sizeof(CRTC));
    // Allocate a register page if needed
    if (!reg_page_allocated_) {
      reg_page_ = new byte[16 * 1024]();
      reg_page_allocated_ = true;
    }
    pbRegisterPage = reg_page_;
  }

  static void TearDownTestSuite() {
    if (reg_page_allocated_) {
      delete[] reg_page_;
      reg_page_ = nullptr;
      reg_page_allocated_ = false;
      pbRegisterPage = nullptr;
    }
  }

  static byte *reg_page_;
  static bool reg_page_allocated_;
};

byte *AsicDebugTest::reg_page_ = nullptr;
bool AsicDebugTest::reg_page_allocated_ = false;

TEST_F(AsicDebugTest, DmaDumpAfterReset) {
  std::string result = asic_dump_dma();
  EXPECT_NE(result.find("ch0: addr=0000 prescaler=00 enabled=0 pause=0 loop_count=0"), std::string::npos);
  EXPECT_NE(result.find("ch1: addr=0000 prescaler=00 enabled=0 pause=0 loop_count=0"), std::string::npos);
  EXPECT_NE(result.find("ch2: addr=0000 prescaler=00 enabled=0 pause=0 loop_count=0"), std::string::npos);
}

TEST_F(AsicDebugTest, DmaDumpWithState) {
  asic.dma.ch[0].source_address = 0x1234;
  asic.dma.ch[0].prescaler = 0x0A;
  asic.dma.ch[0].enabled = true;
  asic.dma.ch[0].pause_ticks = 5;
  asic.dma.ch[0].loops = 3;

  asic.dma.ch[2].source_address = 0xABCD;
  asic.dma.ch[2].enabled = true;

  std::string result = asic_dump_dma();
  EXPECT_NE(result.find("ch0: addr=1234 prescaler=0A enabled=1 pause=1 loop_count=3"), std::string::npos);
  EXPECT_NE(result.find("ch1: addr=0000 prescaler=00 enabled=0 pause=0 loop_count=0"), std::string::npos);
  EXPECT_NE(result.find("ch2: addr=ABCD prescaler=00 enabled=1 pause=0 loop_count=0"), std::string::npos);
}

TEST_F(AsicDebugTest, SpritesDumpAfterReset) {
  std::string result = asic_dump_sprites();
  EXPECT_NE(result.find("spr0: x=0 y=0 mag_x=0 mag_y=0"), std::string::npos);
  EXPECT_NE(result.find("spr15: x=0 y=0 mag_x=0 mag_y=0"), std::string::npos);
}

TEST_F(AsicDebugTest, SpritesDumpWithPositions) {
  asic.sprites_x[0] = 100;
  asic.sprites_y[0] = 200;
  asic.sprites_mag_x[0] = 2;
  asic.sprites_mag_y[0] = 4;

  asic.sprites_x[15] = -32;
  asic.sprites_y[15] = 512;

  std::string result = asic_dump_sprites();
  EXPECT_NE(result.find("spr0: x=100 y=200 mag_x=2 mag_y=4"), std::string::npos);
  EXPECT_NE(result.find("spr15: x=-32 y=512 mag_x=0 mag_y=0"), std::string::npos);
}

TEST_F(AsicDebugTest, InterruptsDumpAfterReset) {
  std::string result = asic_dump_interrupts();
  EXPECT_NE(result.find("raster_interrupt: line=0 enabled=0"), std::string::npos);
  EXPECT_NE(result.find("dma_interrupt: ch0=0 ch1=0 ch2=0"), std::string::npos);
  // Interrupt vector after reset has D0=1
  EXPECT_NE(result.find("interrupt_vector: 01"), std::string::npos);
  EXPECT_NE(result.find("dcsr: 00"), std::string::npos);
}

TEST_F(AsicDebugTest, InterruptsDumpWithState) {
  CRTC.interrupt_sl = 42;
  asic.dma.ch[0].interrupt = true;
  asic.dma.ch[1].enabled = true;
  asic.dma.ch[2].interrupt = true;
  asic.dma.ch[2].enabled = true;
  asic.interrupt_vector = 0xF8;

  std::string result = asic_dump_interrupts();
  EXPECT_NE(result.find("raster_interrupt: line=42 enabled=1"), std::string::npos);
  EXPECT_NE(result.find("dma_interrupt: ch0=1 ch1=0 ch2=1"), std::string::npos);
  EXPECT_NE(result.find("interrupt_vector: F8"), std::string::npos);
  // DCSR: ch1 enabled=bit1, ch2 enabled=bit2, ch0 int=bit6, ch2 int=bit4
  // = 0x02 | 0x04 | 0x40 | 0x10 = 0x56
  EXPECT_NE(result.find("dcsr: 56"), std::string::npos);
}

TEST_F(AsicDebugTest, PaletteDumpAllZeros) {
  std::string result = asic_dump_palette();
  EXPECT_NE(result.find("pen0=0000"), std::string::npos);
  EXPECT_NE(result.find("pen15=0000"), std::string::npos);
  EXPECT_NE(result.find("ink0=0000"), std::string::npos);
  EXPECT_NE(result.find("ink15=0000"), std::string::npos);
}

TEST_F(AsicDebugTest, PaletteDumpWithColors) {
  // Set pen0 to R=F, G=0, B=0 => even byte = 0xF0, odd byte = 0x00
  int offset = 0x2400;
  pbRegisterPage[offset] = 0xF0;     // R=F, B=0
  pbRegisterPage[offset + 1] = 0x00; // G=0

  // Set pen1 to R=0, G=F, B=0 => even byte = 0x00, odd byte = 0x0F
  pbRegisterPage[offset + 2] = 0x00; // R=0, B=0
  pbRegisterPage[offset + 3] = 0x0F; // G=F

  // Set pen2 to R=0, G=0, B=F => even byte = 0x0F, odd byte = 0x00
  pbRegisterPage[offset + 4] = 0x0F; // R=0, B=F
  pbRegisterPage[offset + 5] = 0x00; // G=0

  // Set ink0 (entry 16) to white R=F, G=F, B=F => even = 0xFF, odd = 0x0F
  int ink0_offset = offset + (16 * 2);
  pbRegisterPage[ink0_offset] = 0xFF;     // R=F, B=F
  pbRegisterPage[ink0_offset + 1] = 0x0F; // G=F

  std::string result = asic_dump_palette();
  EXPECT_NE(result.find("pen0=00F0"), std::string::npos);  // 0GRB: 0=0, G=0, R=F, B=0
  EXPECT_NE(result.find("pen1=0F00"), std::string::npos);  // 0GRB: 0=0, G=F, R=0, B=0
  EXPECT_NE(result.find("pen2=000F"), std::string::npos);  // 0GRB: 0=0, G=0, R=0, B=F
  EXPECT_NE(result.find("ink0=0FFF"), std::string::npos);  // 0GRB: 0=0, G=F, R=F, B=F
}

TEST_F(AsicDebugTest, PaletteDumpNullRegPage) {
  pbRegisterPage = nullptr;
  std::string result = asic_dump_palette();
  EXPECT_NE(result.find("pen0=0000"), std::string::npos);
  // Restore for other tests
  pbRegisterPage = reg_page_;
}

TEST_F(AsicDebugTest, AllDumpContainsSections) {
  std::string result = asic_dump_all();
  EXPECT_NE(result.find("locked="), std::string::npos);
  EXPECT_NE(result.find("hscroll="), std::string::npos);
  EXPECT_NE(result.find("[sprites]"), std::string::npos);
  EXPECT_NE(result.find("[dma]"), std::string::npos);
  EXPECT_NE(result.find("[interrupts]"), std::string::npos);
  EXPECT_NE(result.find("[palette]"), std::string::npos);
}

TEST_F(AsicDebugTest, AllDumpReflectsLockState) {
  asic.locked = false;
  std::string result = asic_dump_all();
  EXPECT_NE(result.find("locked=0"), std::string::npos);

  asic.locked = true;
  result = asic_dump_all();
  EXPECT_NE(result.find("locked=1"), std::string::npos);
}

}  // namespace
