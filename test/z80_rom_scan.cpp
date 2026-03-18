#include <gtest/gtest.h>
#include <cstring>
#include "z80.h"

// The ROM scanner looks for the boot print loop pattern in pbROMlo:
//   7E 23 B7 C8 CD xx xx 18 F7
// and returns the 16-bit address at the CALL target (xx xx).

extern byte *pbROMlo;

class RomTxtOutputScanTest : public ::testing::Test {
protected:
    byte rom_buf_[16384] = {};
    byte *saved_rom_ = nullptr;

    void SetUp() override {
        saved_rom_ = pbROMlo;
        pbROMlo = rom_buf_;
    }
    void TearDown() override {
        pbROMlo = saved_rom_;
    }

    // Plant the print loop pattern at a given offset with a given target address
    void plant_pattern(int offset, uint16_t target) {
        rom_buf_[offset + 0] = 0x7E;  // LD A,(HL)
        rom_buf_[offset + 1] = 0x23;  // INC HL
        rom_buf_[offset + 2] = 0xB7;  // OR A
        rom_buf_[offset + 3] = 0xC8;  // RET Z
        rom_buf_[offset + 4] = 0xCD;  // CALL
        rom_buf_[offset + 5] = target & 0xFF;
        rom_buf_[offset + 6] = (target >> 8) & 0xFF;
        rom_buf_[offset + 7] = 0x18;  // JR
        rom_buf_[offset + 8] = 0xF7;  // -9 (back to start)
    }
};

TEST_F(RomTxtOutputScanTest, Finds6128Address) {
    plant_pattern(0x06FC, 0x13FE);
    EXPECT_EQ(z80_find_rom_txt_output(), 0x13FE);
}

TEST_F(RomTxtOutputScanTest, Finds464Address) {
    plant_pattern(0x06EB, 0x1400);
    EXPECT_EQ(z80_find_rom_txt_output(), 0x1400);
}

TEST_F(RomTxtOutputScanTest, Finds664Address) {
    plant_pattern(0x06EC, 0x13FA);
    EXPECT_EQ(z80_find_rom_txt_output(), 0x13FA);
}

TEST_F(RomTxtOutputScanTest, ReturnsZeroWhenNotFound) {
    // Empty ROM — no pattern
    EXPECT_EQ(z80_find_rom_txt_output(), 0);
}

TEST_F(RomTxtOutputScanTest, ReturnsZeroWhenNullRom) {
    pbROMlo = nullptr;
    EXPECT_EQ(z80_find_rom_txt_output(), 0);
}

TEST_F(RomTxtOutputScanTest, FindsFirstMatch) {
    // Two patterns — should return the first one found
    plant_pattern(0x0100, 0x1234);
    plant_pattern(0x0200, 0x5678);
    EXPECT_EQ(z80_find_rom_txt_output(), 0x1234);
}
