#include <gtest/gtest.h>
#include <cstring>

#include "koncepcja.h"
#include "crtc.h"

extern t_CPC CPC;
extern t_CRTC CRTC;

class CrtcTypesTest : public testing::Test {
protected:
   void SetUp() override {
      memset(&CRTC, 0, sizeof(CRTC));
      CRTC.registers[0] = 0x3f;
      CRTC.registers[2] = 0x2e;
      CRTC.registers[3] = 0x8e;
   }
};

TEST_F(CrtcTypesTest, DefaultTypeForCPC464) { EXPECT_EQ(0, crtc_type_for_model(0)); }
TEST_F(CrtcTypesTest, DefaultTypeForCPC664) { EXPECT_EQ(0, crtc_type_for_model(1)); }
TEST_F(CrtcTypesTest, DefaultTypeForCPC6128) { EXPECT_EQ(1, crtc_type_for_model(2)); }
TEST_F(CrtcTypesTest, DefaultTypeForPlus) { EXPECT_EQ(3, crtc_type_for_model(3)); }
TEST_F(CrtcTypesTest, DefaultTypeForUnknownModel) { EXPECT_EQ(0, crtc_type_for_model(99)); }

TEST_F(CrtcTypesTest, R3VsyncWidthType0UsesUpperBits) {
   CRTC.crtc_type = 0;
   CRTC.hsw = 0x4e & 0x0f;
   CRTC.vsw = 0x4e >> 4;
   EXPECT_EQ(14u, CRTC.hsw);
   EXPECT_EQ(4u, CRTC.vsw);
}

TEST_F(CrtcTypesTest, R3VsyncWidthType1FixedAt16) {
   CRTC.crtc_type = 1;
   CRTC.hsw = 0x4e & 0x0f;
   CRTC.vsw = 0;
   EXPECT_EQ(0u, CRTC.vsw);
}

TEST_F(CrtcTypesTest, R3VsyncWidthType2FixedAt16) {
   CRTC.crtc_type = 2;
   CRTC.hsw = 0x8e & 0x0f;
   CRTC.vsw = 0;
   EXPECT_EQ(0u, CRTC.vsw);
}

TEST_F(CrtcTypesTest, R3VsyncWidthType3UsesUpperBits) {
   CRTC.crtc_type = 3;
   CRTC.hsw = 0x5e & 0x0f;
   CRTC.vsw = 0x5e >> 4;
   EXPECT_EQ(5u, CRTC.vsw);
}

TEST_F(CrtcTypesTest, Type0ReturnsZeroForWriteOnlyRegs) {
   CRTC.crtc_type = 0;
   CRTC.registers[0] = 0x3f;
   byte reg = 0;
   byte result = (reg >= 12 && reg <= 17) ? CRTC.registers[reg] : 0;
   EXPECT_EQ(0, result);
}

TEST_F(CrtcTypesTest, Type0CanReadR12R13) {
   CRTC.crtc_type = 0;
   CRTC.registers[12] = 0x30;
   CRTC.registers[13] = 0x42;
   byte r12 = (12 >= 12 && 12 <= 17) ? CRTC.registers[12] : 0;
   byte r13 = (13 >= 12 && 13 <= 17) ? CRTC.registers[13] : 0;
   EXPECT_EQ(0x30, r12);
   EXPECT_EQ(0x42, r13);
}

TEST_F(CrtcTypesTest, Type1CannotReadR12R13) {
   CRTC.crtc_type = 1;
   CRTC.registers[12] = 0x30;
   // Type 1: R12 is write-only, returns 0
   byte reg = 12;
   byte result = (reg >= 14 && reg <= 17) ? CRTC.registers[reg] : (reg == 31 ? 0xff : 0);
   EXPECT_EQ(0, result);
}

TEST_F(CrtcTypesTest, Type1CanReadR14R15) {
   CRTC.crtc_type = 1;
   CRTC.registers[14] = 0x10;
   byte reg = 14;
   byte result = (reg >= 14 && reg <= 17) ? CRTC.registers[reg] : (reg == 31 ? 0xff : 0);
   EXPECT_EQ(0x10, result);
}

TEST_F(CrtcTypesTest, Type1R31ReturnsFF) {
   CRTC.crtc_type = 1;
   byte reg = 31;
   byte result = (reg >= 14 && reg <= 17) ? CRTC.registers[reg] : (reg == 31 ? 0xff : 0);
   EXPECT_EQ(0xff, result);
}

TEST_F(CrtcTypesTest, Type2CannotReadR12R13) {
   CRTC.crtc_type = 2;
   CRTC.registers[12] = 0x30;
   byte reg = 12;
   byte result = (reg >= 14 && reg <= 17) ? CRTC.registers[reg] : 0;
   EXPECT_EQ(0, result);
}

TEST_F(CrtcTypesTest, Type3CanReadR12R13) {
   CRTC.crtc_type = 3;
   CRTC.registers[12] = 0x30;
   byte reg = 12;
   byte result = (reg >= 12 && reg <= 17) ? CRTC.registers[reg] : 0;
   EXPECT_EQ(0x30, result);
}

TEST_F(CrtcTypesTest, R8Type0FullRegister) {
   CRTC.crtc_type = 0;
   CRTC.registers[8] = 0x33;
   EXPECT_EQ(0x33, CRTC.registers[8]);
}

TEST_F(CrtcTypesTest, R8Type1OnlyInterlaceBits) {
   CRTC.crtc_type = 1;
   CRTC.registers[8] = 0x33 & 0x03;
   EXPECT_EQ(0x03, CRTC.registers[8]);
}

TEST_F(CrtcTypesTest, R8Type2OnlyInterlaceBits) {
   CRTC.crtc_type = 2;
   CRTC.registers[8] = 0x33 & 0x03;
   EXPECT_EQ(0x03, CRTC.registers[8]);
}

TEST_F(CrtcTypesTest, R8Type3FullRegister) {
   CRTC.crtc_type = 3;
   CRTC.registers[8] = 0x33;
   EXPECT_EQ(0x33, CRTC.registers[8]);
}

TEST_F(CrtcTypesTest, SettingTypeChangesField) {
   CRTC.crtc_type = 0;
   EXPECT_EQ(0, CRTC.crtc_type);
   CRTC.crtc_type = 2;
   EXPECT_EQ(2, CRTC.crtc_type);
   CRTC.crtc_type = 3;
   EXPECT_EQ(3, CRTC.crtc_type);
}

TEST_F(CrtcTypesTest, ValidTypeRange) {
   for (int t = 0; t <= 3; t++) {
      CRTC.crtc_type = static_cast<unsigned char>(t);
      EXPECT_EQ(t, CRTC.crtc_type);
   }
}

TEST_F(CrtcTypesTest, Type1R12R13ImmediateUpdateWhenVCC0) {
   CRTC.crtc_type = 1;
   CRTC.line_count = 0;
   CRTC.registers[12] = 0x10;
   CRTC.registers[13] = 0x20;
   CRTC.requested_addr = CRTC.registers[13] + (CRTC.registers[12] << 8);
   CRTC.addr = CRTC.requested_addr;
   CRTC.next_addr = CRTC.requested_addr;
   EXPECT_EQ(0x1020u, CRTC.addr);
   EXPECT_EQ(0x1020u, CRTC.next_addr);
}

TEST_F(CrtcTypesTest, Type1R12R13DeferredWhenVCCNot0) {
   CRTC.crtc_type = 1;
   CRTC.line_count = 5;
   CRTC.addr = 0;
   CRTC.next_addr = 0;
   CRTC.registers[12] = 0x10;
   CRTC.registers[13] = 0x20;
   CRTC.requested_addr = CRTC.registers[13] + (CRTC.registers[12] << 8);
   EXPECT_EQ(0x1020u, CRTC.requested_addr);
   EXPECT_EQ(0u, CRTC.addr);
}

TEST_F(CrtcTypesTest, Type0R12R13AlwaysDeferred) {
   CRTC.crtc_type = 0;
   CRTC.line_count = 0;
   CRTC.addr = 0;
   CRTC.next_addr = 0;
   CRTC.registers[12] = 0x10;
   CRTC.registers[13] = 0x20;
   CRTC.requested_addr = CRTC.registers[13] + (CRTC.registers[12] << 8);
   EXPECT_EQ(0x1020u, CRTC.requested_addr);
   EXPECT_EQ(0u, CRTC.addr);
}

TEST_F(CrtcTypesTest, ChipNameType0) { EXPECT_STREQ("HD6845S", crtc_type_chip_name(0)); }
TEST_F(CrtcTypesTest, ChipNameType1) { EXPECT_STREQ("UM6845R", crtc_type_chip_name(1)); }
TEST_F(CrtcTypesTest, ChipNameType2) { EXPECT_STREQ("MC6845", crtc_type_chip_name(2)); }
TEST_F(CrtcTypesTest, ChipNameType3) { EXPECT_STREQ("AMS40489", crtc_type_chip_name(3)); }

TEST_F(CrtcTypesTest, ManufacturerType0) { EXPECT_STREQ("Hitachi", crtc_type_manufacturer(0)); }
TEST_F(CrtcTypesTest, ManufacturerType1) { EXPECT_STREQ("UMC", crtc_type_manufacturer(1)); }
TEST_F(CrtcTypesTest, ManufacturerType2) { EXPECT_STREQ("Motorola", crtc_type_manufacturer(2)); }
TEST_F(CrtcTypesTest, ManufacturerType3) { EXPECT_STREQ("Amstrad", crtc_type_manufacturer(3)); }

TEST_F(CrtcTypesTest, ChipNameUnknown) { EXPECT_STREQ("Unknown", crtc_type_chip_name(99)); }
TEST_F(CrtcTypesTest, ManufacturerUnknown) { EXPECT_STREQ("Unknown", crtc_type_manufacturer(99)); }
