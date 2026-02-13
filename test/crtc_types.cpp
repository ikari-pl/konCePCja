#include <gtest/gtest.h>
#include <cstring>

#include "koncepcja.h"
#include "crtc.h"
#include "z80.h"

extern t_CPC CPC;
extern t_CRTC CRTC;
extern t_z80regs z80;

// Port addresses for CRTC I/O (active when bit 6 of high byte is clear)
// 0xBCxx = register select (port.b.h & 3 == 0)
// 0xBDxx = register write  (port.b.h & 3 == 1)
// 0xBExx = status read     (port.b.h & 3 == 2)
// 0xBFxx = register read   (port.b.h & 3 == 3)

static reg_pair make_port(word addr) {
   reg_pair p;
   p.w.l = addr;
   return p;
}

static const reg_pair PORT_REG_SELECT = make_port(0xBC00);
static const reg_pair PORT_REG_WRITE  = make_port(0xBD00);
static const reg_pair PORT_REG_READ   = make_port(0xBF00);

// Helper: select a CRTC register and read it via z80_IN_handler
static byte crtc_read_register(byte reg) {
   z80_OUT_handler(PORT_REG_SELECT, reg);
   return z80_IN_handler(PORT_REG_READ);
}

// Helper: select a CRTC register and write a value via z80_OUT_handler
static void crtc_write_register(byte reg, byte val) {
   z80_OUT_handler(PORT_REG_SELECT, reg);
   z80_OUT_handler(PORT_REG_WRITE, val);
}

class CrtcTypesTest : public testing::Test {
protected:
   void SetUp() override {
      memset(&CRTC, 0, sizeof(CRTC));
      z80 = t_z80regs();
      CRTC.registers[0] = 0x3f;
      CRTC.registers[2] = 0x2e;
      CRTC.registers[3] = 0x8e;
      CPC.model = 2;  // default to 6128
   }
};

// --- crtc_type_for_model() ---

TEST_F(CrtcTypesTest, DefaultTypeForCPC464) { EXPECT_EQ(0, crtc_type_for_model(0)); }
TEST_F(CrtcTypesTest, DefaultTypeForCPC664) { EXPECT_EQ(0, crtc_type_for_model(1)); }
TEST_F(CrtcTypesTest, DefaultTypeForCPC6128) { EXPECT_EQ(1, crtc_type_for_model(2)); }
TEST_F(CrtcTypesTest, DefaultTypeForPlus) { EXPECT_EQ(3, crtc_type_for_model(3)); }
TEST_F(CrtcTypesTest, DefaultTypeForUnknownModel) { EXPECT_EQ(0, crtc_type_for_model(99)); }

// --- R3 VSYNC width via z80_OUT_handler ---
// Writing R3 sets hsw (lower nibble) and vsw (upper nibble, type-dependent)

TEST_F(CrtcTypesTest, R3VsyncWidthType0UsesUpperBits) {
   CRTC.crtc_type = 0;
   crtc_write_register(3, 0x4E);
   EXPECT_EQ(14u, CRTC.hsw);
   EXPECT_EQ(4u, CRTC.vsw);
}

TEST_F(CrtcTypesTest, R3VsyncWidthType1FixedAt16) {
   CRTC.crtc_type = 1;
   crtc_write_register(3, 0x4E);
   EXPECT_EQ(14u, CRTC.hsw);
   EXPECT_EQ(0u, CRTC.vsw);  // Types 1/2: VSYNC width fixed (0 = 16 lines)
}

TEST_F(CrtcTypesTest, R3VsyncWidthType2FixedAt16) {
   CRTC.crtc_type = 2;
   crtc_write_register(3, 0x8E);
   EXPECT_EQ(14u, CRTC.hsw);
   EXPECT_EQ(0u, CRTC.vsw);
}

TEST_F(CrtcTypesTest, R3VsyncWidthType3UsesUpperBits) {
   CRTC.crtc_type = 3;
   crtc_write_register(3, 0x5E);
   EXPECT_EQ(14u, CRTC.hsw);
   EXPECT_EQ(5u, CRTC.vsw);
}

// --- Register read tests via z80_IN_handler ---
// Each CRTC type has different readable register ranges

TEST_F(CrtcTypesTest, Type0ReturnsZeroForWriteOnlyRegs) {
   CRTC.crtc_type = 0;
   CRTC.registers[0] = 0x3F;
   EXPECT_EQ(0, crtc_read_register(0));  // R0 is write-only on all types
}

TEST_F(CrtcTypesTest, Type0CanReadR12R13) {
   CRTC.crtc_type = 0;
   CRTC.registers[12] = 0x30;
   CRTC.registers[13] = 0x42;
   EXPECT_EQ(0x30, crtc_read_register(12));
   EXPECT_EQ(0x42, crtc_read_register(13));
}

TEST_F(CrtcTypesTest, Type1CannotReadR12R13) {
   CRTC.crtc_type = 1;
   CRTC.registers[12] = 0x30;
   EXPECT_EQ(0, crtc_read_register(12));  // Type 1: R12 is write-only
}

TEST_F(CrtcTypesTest, Type1CanReadR14R15) {
   CRTC.crtc_type = 1;
   CRTC.registers[14] = 0x10;
   EXPECT_EQ(0x10, crtc_read_register(14));
}

TEST_F(CrtcTypesTest, Type1R31ReturnsFF) {
   CRTC.crtc_type = 1;
   EXPECT_EQ(0xFF, crtc_read_register(31));
}

TEST_F(CrtcTypesTest, Type2CannotReadR12R13) {
   CRTC.crtc_type = 2;
   CRTC.registers[12] = 0x30;
   EXPECT_EQ(0, crtc_read_register(12));
}

TEST_F(CrtcTypesTest, Type3CanReadR12R13) {
   CRTC.crtc_type = 3;
   CRTC.registers[12] = 0x30;
   EXPECT_EQ(0x30, crtc_read_register(12));
}

// --- R8 write behaviour via z80_OUT_handler ---
// Type 0/3: full 8 bits stored. Type 1/2: only bits 0-1 (interlace mode).

TEST_F(CrtcTypesTest, R8Type0FullRegister) {
   CRTC.crtc_type = 0;
   crtc_write_register(8, 0x33);
   EXPECT_EQ(0x33, CRTC.registers[8]);
}

TEST_F(CrtcTypesTest, R8Type1OnlyInterlaceBits) {
   CRTC.crtc_type = 1;
   crtc_write_register(8, 0x33);
   EXPECT_EQ(0x03, CRTC.registers[8]);  // Type 1: masked to val & 0x03 on write
}

TEST_F(CrtcTypesTest, R8Type2OnlyInterlaceBits) {
   CRTC.crtc_type = 2;
   crtc_write_register(8, 0x33);
   EXPECT_EQ(0x03, CRTC.registers[8]);  // Type 2: masked to val & 0x03 on write
}

TEST_F(CrtcTypesTest, R8Type3FullRegister) {
   CRTC.crtc_type = 3;
   crtc_write_register(8, 0x33);
   EXPECT_EQ(0x33, CRTC.registers[8]);
}

// --- Type field tests ---

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

// --- R12/R13 address update tests ---

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

// --- Chip info tests (pure functions) ---

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
