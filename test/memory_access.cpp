#include <gtest/gtest.h>

#include <cstring>

#include "asic.h"
#include "koncepcja.h"
#include "layered_memory.h"
#include "memory_bus.h"
#include "z80.h"

extern byte* membank_read[4];
extern byte* membank_write[4];
extern t_MemBankConfig membank_config;
extern t_GateArray GateArray;
extern byte* pbMF2ROM;
extern dword dwMF2Flags;
extern MemoryBus g_memory_bus;

namespace {

class MemoryAccessTest : public testing::Test {
 protected:
  void SetUp() override {
    // Set up a flat 64KB RAM layout: all 4 banks point to different
    // 16KB slices of a contiguous 64KB buffer.
    for (int i = 0; i < 4; i++) {
      membank_read[i] = &ram_[i * 16384];
      membank_write[i] = &ram_[i * 16384];
    }
    // Initialize membank_config for ASIC DMA tests
    for (int cfg = 0; cfg < 8; cfg++) {
      for (int bank = 0; bank < 4; bank++) {
        membank_config[cfg][bank] = &ram_[bank * 16384];
      }
    }
    // Reset MF2 state
    dwMF2Flags = 0;
    pbMF2ROM = mf2_rom_;
    std::memset(mf2_rom_, 0, sizeof(mf2_rom_));
    // Reset ASIC state
    GateArray.registerPageOn = false;
    asic.locked = false;
    std::memset(ram_, 0, sizeof(ram_));
  }

  byte ram_[65536]{};
  byte mf2_rom_[16384]{};  // MF2 ROM (0x0000-0x1FFF) + RAM (0x2000-0x3FFF)
};

TEST_F(MemoryAccessTest, CpuWriteGoesToBus) {
  z80_cpu_write_mem(0x1234, 0xAB);
  EXPECT_EQ(ram_[0x1234], 0xAB);
}

TEST_F(MemoryAccessTest, CpuWriteToMf2RangeWithoutMf2ActiveGoesToBus) {
  dwMF2Flags = 0;  // MF2 not active
  z80_cpu_write_mem(0x2000, 0x42);
  EXPECT_EQ(ram_[0x2000], 0x42);
  EXPECT_EQ(mf2_rom_[0x2000], 0);  // MF2 SRAM untouched
}

TEST_F(MemoryAccessTest, CpuWriteToMf2RangeWithMf2ActiveGoesToMf2Sram) {
  dwMF2Flags = 0x01;  // MF2_ACTIVE
  z80_cpu_write_mem(0x2000, 0x42);
  EXPECT_EQ(mf2_rom_[0x2000], 0x42);
  EXPECT_EQ(ram_[0x2000], 0);  // CPC RAM untouched
}

TEST_F(MemoryAccessTest, CpuWriteToMf2RangeBoundary) {
  dwMF2Flags = 0x01;
  // 0x1FFF is below MF2 range — should go to bus
  z80_cpu_write_mem(0x1FFF, 0x11);
  EXPECT_EQ(ram_[0x1FFF], 0x11);
  // 0x2000 is start of MF2 range
  z80_cpu_write_mem(0x2000, 0x22);
  EXPECT_EQ(mf2_rom_[0x2000], 0x22);
  // 0x3FFF is end of MF2 range
  z80_cpu_write_mem(0x3FFF, 0x33);
  EXPECT_EQ(mf2_rom_[0x3FFF], 0x33);
  // 0x4000 is above MF2 range — should go to bus
  z80_cpu_write_mem(0x4000, 0x44);
  EXPECT_EQ(ram_[0x4000], 0x44);
}

TEST_F(MemoryAccessTest, CpuWriteVsDirectWriteMf2Behavior) {
  dwMF2Flags = 0x01;
  // z80_write_mem (direct) should bypass MF2 and write to bus
  z80_write_mem(0x2000, 0xAA);
  EXPECT_EQ(ram_[0x2000], 0xAA);
  // z80_cpu_write_mem should redirect to MF2 SRAM
  z80_cpu_write_mem(0x2500, 0xBB);
  EXPECT_EQ(mf2_rom_[0x2500], 0xBB);
  EXPECT_EQ(ram_[0x2500], 0);  // bus untouched
}

TEST_F(MemoryAccessTest, CpuReadReturnsBusValue) {
  ram_[0x4000] = 0x77;
  EXPECT_EQ(z80_cpu_read_mem(0x4000), 0x77);
}

TEST_F(MemoryAccessTest, CpuReadVsDirectReadIdentical) {
  ram_[0x5000] = 0x99;
  EXPECT_EQ(z80_cpu_read_mem(0x5000), z80_read_mem(0x5000));
}

TEST_F(MemoryAccessTest, LayeredMemoryCpuViewWriteMf2) {
  dwMF2Flags = 0x01;
  LayeredMemory mem;
  mem.write_cpu(0x3000, 0xEE);
  EXPECT_EQ(mf2_rom_[0x3000], 0xEE);
  EXPECT_EQ(ram_[0x3000], 0);
}

TEST_F(MemoryAccessTest, LayeredMemoryDirectWriteBypassesMf2) {
  dwMF2Flags = 0x01;
  LayeredMemory mem;
  mem.write_direct(0x3000, 0xEE);
  EXPECT_EQ(ram_[0x3000], 0xEE);
  EXPECT_EQ(mf2_rom_[0x3000], 0);
}

TEST_F(MemoryAccessTest, LayeredMemoryRawWriteBypassesEverything) {
  dwMF2Flags = 0x01;
  LayeredMemory mem;
  mem.write_raw(0x3000, 0xFF);
  EXPECT_EQ(ram_[0x3000], 0xFF);
  EXPECT_EQ(mf2_rom_[0x3000], 0);
}

}  // namespace
