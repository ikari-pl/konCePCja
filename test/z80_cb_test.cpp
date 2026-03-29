#include <gtest/gtest.h>
#include "z80.h"
#include "koncepcja.h"

#include "z80_macros.h"

extern byte *membank_read[4];
extern t_z80regs z80;

namespace {

class Z80CBTest : public testing::Test {
 public:
  static void SetUpTestCase() { z80_init_tables(); }

 protected:
  // Helper: place a CB-prefixed instruction and execute it.
  // membank0 must outlive the call (caller owns it).
  void execCB(byte *membank0, byte cb_opcode) {
    membank0[0] = 0xCB;
    membank0[1] = cb_opcode;
    _PC = 0;
    _R = 0;
    membank_read[0] = membank0;
    z80_execute_instruction();
  }
};

// ---------------------------------------------------------------------------
// RLC B (CB 00)
// ---------------------------------------------------------------------------

TEST_F(Z80CBTest, RLC_B_Basic) {
  byte mem[2];
  _B = 0x85;  // 1000 0101
  _F = 0;
  execCB(mem, 0x00);
  // Rotate left: bit 7 (1) goes to carry and bit 0
  // Result: 0000 1011 = 0x0B
  EXPECT_EQ(0x0B, _B);
  EXPECT_TRUE(_F & Cflag);    // bit 7 was 1
  EXPECT_FALSE(_F & Zflag);   // result non-zero
  EXPECT_FALSE(_F & Nflag);   // N always cleared
  EXPECT_FALSE(_F & Hflag);   // H always cleared
}

TEST_F(Z80CBTest, RLC_B_CarryClear) {
  byte mem[2];
  _B = 0x01;  // 0000 0001
  _F = Cflag;  // pre-existing carry should be replaced
  execCB(mem, 0x00);
  // Result: 0000 0010 = 0x02, old bit 7 was 0
  EXPECT_EQ(0x02, _B);
  EXPECT_FALSE(_F & Cflag);
  EXPECT_FALSE(_F & Zflag);
}

TEST_F(Z80CBTest, RLC_B_Zero) {
  byte mem[2];
  _B = 0x00;
  _F = 0;
  execCB(mem, 0x00);
  EXPECT_EQ(0x00, _B);
  EXPECT_TRUE(_F & Zflag);
  EXPECT_FALSE(_F & Cflag);
}

// ---------------------------------------------------------------------------
// RRC B (CB 08)
// ---------------------------------------------------------------------------

TEST_F(Z80CBTest, RRC_B_Basic) {
  byte mem[2];
  _B = 0x85;  // 1000 0101
  _F = 0;
  execCB(mem, 0x08);
  // Rotate right: bit 0 (1) goes to carry and bit 7
  // Result: 1100 0010 = 0xC2
  EXPECT_EQ(0xC2, _B);
  EXPECT_TRUE(_F & Cflag);    // bit 0 was 1
  EXPECT_FALSE(_F & Zflag);
}

TEST_F(Z80CBTest, RRC_B_CarryClear) {
  byte mem[2];
  _B = 0x80;  // 1000 0000
  _F = Cflag;
  execCB(mem, 0x08);
  // Result: 0100 0000 = 0x40, bit 0 was 0
  EXPECT_EQ(0x40, _B);
  EXPECT_FALSE(_F & Cflag);
  EXPECT_FALSE(_F & Zflag);
}

TEST_F(Z80CBTest, RRC_B_Zero) {
  byte mem[2];
  _B = 0x00;
  _F = 0;
  execCB(mem, 0x08);
  EXPECT_EQ(0x00, _B);
  EXPECT_TRUE(_F & Zflag);
  EXPECT_FALSE(_F & Cflag);
}

// ---------------------------------------------------------------------------
// RL B (CB 10) - rotate left through carry
// ---------------------------------------------------------------------------

TEST_F(Z80CBTest, RL_B_CarryInAndOut) {
  byte mem[2];
  _B = 0x80;  // 1000 0000
  _F = Cflag;  // carry is set
  execCB(mem, 0x10);
  // bit 7 -> carry (set), old carry -> bit 0
  // Result: 0000 0001 = 0x01
  EXPECT_EQ(0x01, _B);
  EXPECT_TRUE(_F & Cflag);   // bit 7 was 1
  EXPECT_FALSE(_F & Zflag);
}

TEST_F(Z80CBTest, RL_B_NoCarry) {
  byte mem[2];
  _B = 0x40;  // 0100 0000
  _F = 0;      // no carry in
  execCB(mem, 0x10);
  // Result: 1000 0000 = 0x80, bit 7 was 0
  EXPECT_EQ(0x80, _B);
  EXPECT_FALSE(_F & Cflag);
  EXPECT_TRUE(_F & Sflag);   // result has bit 7 set
}

TEST_F(Z80CBTest, RL_B_Zero) {
  byte mem[2];
  _B = 0x00;
  _F = 0;  // no carry in
  execCB(mem, 0x10);
  EXPECT_EQ(0x00, _B);
  EXPECT_TRUE(_F & Zflag);
  EXPECT_FALSE(_F & Cflag);
}

// ---------------------------------------------------------------------------
// RR B (CB 18) - rotate right through carry
// ---------------------------------------------------------------------------

TEST_F(Z80CBTest, RR_B_CarryInAndOut) {
  byte mem[2];
  _B = 0x01;  // 0000 0001
  _F = Cflag;  // carry is set
  execCB(mem, 0x18);
  // bit 0 -> carry (set), old carry -> bit 7
  // Result: 1000 0000 = 0x80
  EXPECT_EQ(0x80, _B);
  EXPECT_TRUE(_F & Cflag);   // bit 0 was 1
  EXPECT_TRUE(_F & Sflag);   // bit 7 set in result
}

TEST_F(Z80CBTest, RR_B_NoCarry) {
  byte mem[2];
  _B = 0x02;  // 0000 0010
  _F = 0;      // no carry in
  execCB(mem, 0x18);
  // Result: 0000 0001 = 0x01, bit 0 was 0
  EXPECT_EQ(0x01, _B);
  EXPECT_FALSE(_F & Cflag);
  EXPECT_FALSE(_F & Zflag);
}

TEST_F(Z80CBTest, RR_B_Zero) {
  byte mem[2];
  _B = 0x00;
  _F = 0;
  execCB(mem, 0x18);
  EXPECT_EQ(0x00, _B);
  EXPECT_TRUE(_F & Zflag);
  EXPECT_FALSE(_F & Cflag);
}

// ---------------------------------------------------------------------------
// SLA B (CB 20) - shift left arithmetic
// ---------------------------------------------------------------------------

TEST_F(Z80CBTest, SLA_B_Basic) {
  byte mem[2];
  _B = 0x42;  // 0100 0010
  _F = 0;
  execCB(mem, 0x20);
  // Shift left: bit 7 (0) -> carry, bit 0 = 0
  // Result: 1000 0100 = 0x84
  EXPECT_EQ(0x84, _B);
  EXPECT_FALSE(_F & Cflag);
  EXPECT_TRUE(_F & Sflag);
}

TEST_F(Z80CBTest, SLA_B_CarryOut) {
  byte mem[2];
  _B = 0x81;  // 1000 0001
  _F = 0;
  execCB(mem, 0x20);
  // bit 7 (1) -> carry
  // Result: 0000 0010 = 0x02
  EXPECT_EQ(0x02, _B);
  EXPECT_TRUE(_F & Cflag);
  EXPECT_FALSE(_F & Zflag);
}

TEST_F(Z80CBTest, SLA_B_Zero) {
  byte mem[2];
  _B = 0x80;  // 1000 0000
  _F = 0;
  execCB(mem, 0x20);
  // Result: 0x00, carry set from bit 7
  EXPECT_EQ(0x00, _B);
  EXPECT_TRUE(_F & Zflag);
  EXPECT_TRUE(_F & Cflag);
}

// ---------------------------------------------------------------------------
// SRA B (CB 28) - shift right arithmetic (sign bit preserved)
// ---------------------------------------------------------------------------

TEST_F(Z80CBTest, SRA_B_PreservesSign) {
  byte mem[2];
  _B = 0x84;  // 1000 0100
  _F = 0;
  execCB(mem, 0x28);
  // bit 0 (0) -> carry, bit 7 preserved (1)
  // Result: 1100 0010 = 0xC2
  EXPECT_EQ(0xC2, _B);
  EXPECT_FALSE(_F & Cflag);
  EXPECT_TRUE(_F & Sflag);
}

TEST_F(Z80CBTest, SRA_B_CarryOut) {
  byte mem[2];
  _B = 0x03;  // 0000 0011
  _F = 0;
  execCB(mem, 0x28);
  // bit 0 (1) -> carry, bit 7 preserved (0)
  // Result: 0000 0001 = 0x01
  EXPECT_EQ(0x01, _B);
  EXPECT_TRUE(_F & Cflag);
  EXPECT_FALSE(_F & Sflag);
}

TEST_F(Z80CBTest, SRA_B_NegativeToMinusOne) {
  byte mem[2];
  _B = 0xFF;  // 1111 1111 = -1 signed
  _F = 0;
  execCB(mem, 0x28);
  // SRA of -1 stays -1: 0xFF, carry = 1 (bit 0 was 1)
  EXPECT_EQ(0xFF, _B);
  EXPECT_TRUE(_F & Cflag);
  EXPECT_TRUE(_F & Sflag);
}

// ---------------------------------------------------------------------------
// SRL B (CB 38) - shift right logical
// ---------------------------------------------------------------------------

TEST_F(Z80CBTest, SRL_B_Basic) {
  byte mem[2];
  _B = 0x84;  // 1000 0100
  _F = 0;
  execCB(mem, 0x38);
  // bit 0 (0) -> carry, bit 7 = 0
  // Result: 0100 0010 = 0x42
  EXPECT_EQ(0x42, _B);
  EXPECT_FALSE(_F & Cflag);
  EXPECT_FALSE(_F & Sflag);  // bit 7 is always cleared
}

TEST_F(Z80CBTest, SRL_B_CarryOut) {
  byte mem[2];
  _B = 0x01;  // 0000 0001
  _F = 0;
  execCB(mem, 0x38);
  // bit 0 (1) -> carry, bit 7 = 0
  // Result: 0x00
  EXPECT_EQ(0x00, _B);
  EXPECT_TRUE(_F & Cflag);
  EXPECT_TRUE(_F & Zflag);
}

TEST_F(Z80CBTest, SRL_B_HighBitCleared) {
  byte mem[2];
  _B = 0x80;  // 1000 0000
  _F = 0;
  execCB(mem, 0x38);
  // Result: 0100 0000 = 0x40, carry = 0
  EXPECT_EQ(0x40, _B);
  EXPECT_FALSE(_F & Cflag);
  EXPECT_FALSE(_F & Sflag);
}

// ---------------------------------------------------------------------------
// BIT 0,B (CB 40) - test bit 0
// ---------------------------------------------------------------------------

TEST_F(Z80CBTest, BIT0_B_BitSet) {
  byte mem[2];
  _B = 0x01;  // bit 0 is set
  _F = Cflag;  // carry should be preserved
  execCB(mem, 0x40);
  // Z flag cleared (bit is set), H flag set, carry preserved
  EXPECT_FALSE(_F & Zflag);
  EXPECT_TRUE(_F & Hflag);
  EXPECT_TRUE(_F & Cflag);   // carry preserved
  EXPECT_EQ(0x01, _B);       // register unchanged
}

TEST_F(Z80CBTest, BIT0_B_BitClear) {
  byte mem[2];
  _B = 0xFE;  // bit 0 is clear
  _F = 0;
  execCB(mem, 0x40);
  // Z flag set (bit is 0), H flag set
  EXPECT_TRUE(_F & Zflag);
  EXPECT_TRUE(_F & Hflag);
  EXPECT_FALSE(_F & Cflag);  // carry was 0, preserved
  EXPECT_EQ(0xFE, _B);       // register unchanged
}

// ---------------------------------------------------------------------------
// BIT 7,B (CB 78) - test bit 7
// ---------------------------------------------------------------------------

TEST_F(Z80CBTest, BIT7_B_BitSet) {
  byte mem[2];
  _B = 0x80;  // bit 7 is set
  _F = 0;
  execCB(mem, 0x78);
  EXPECT_FALSE(_F & Zflag);
  EXPECT_TRUE(_F & Sflag);   // BIT 7 sets S flag when bit is set
  EXPECT_TRUE(_F & Hflag);
  EXPECT_EQ(0x80, _B);
}

TEST_F(Z80CBTest, BIT7_B_BitClear) {
  byte mem[2];
  _B = 0x7F;  // bit 7 is clear
  _F = Cflag;
  execCB(mem, 0x78);
  EXPECT_TRUE(_F & Zflag);
  EXPECT_FALSE(_F & Sflag);
  EXPECT_TRUE(_F & Hflag);
  EXPECT_TRUE(_F & Cflag);   // carry preserved
  EXPECT_EQ(0x7F, _B);
}

// ---------------------------------------------------------------------------
// BIT 3,B (CB 58) - test bit 3 (middle bit)
// ---------------------------------------------------------------------------

TEST_F(Z80CBTest, BIT3_B_BitSet) {
  byte mem[2];
  _B = 0x08;  // only bit 3 set
  _F = 0;
  execCB(mem, 0x58);
  EXPECT_FALSE(_F & Zflag);
  EXPECT_TRUE(_F & Hflag);
  EXPECT_EQ(0x08, _B);
}

TEST_F(Z80CBTest, BIT3_B_BitClear) {
  byte mem[2];
  _B = 0xF7;  // all bits set except bit 3
  _F = 0;
  execCB(mem, 0x58);
  EXPECT_TRUE(_F & Zflag);
  EXPECT_TRUE(_F & Hflag);
  EXPECT_EQ(0xF7, _B);
}

// ---------------------------------------------------------------------------
// SET 0,B (CB C0) - set bit 0
// ---------------------------------------------------------------------------

TEST_F(Z80CBTest, SET0_B_AlreadyClear) {
  byte mem[2];
  _B = 0x00;
  execCB(mem, 0xC0);
  EXPECT_EQ(0x01, _B);
}

TEST_F(Z80CBTest, SET0_B_AlreadySet) {
  byte mem[2];
  _B = 0xFF;
  execCB(mem, 0xC0);
  EXPECT_EQ(0xFF, _B);  // unchanged
}

// ---------------------------------------------------------------------------
// SET 7,B (CB F8) - set bit 7
// ---------------------------------------------------------------------------

TEST_F(Z80CBTest, SET7_B_AlreadyClear) {
  byte mem[2];
  _B = 0x00;
  execCB(mem, 0xF8);
  EXPECT_EQ(0x80, _B);
}

TEST_F(Z80CBTest, SET7_B_OtherBitsPreserved) {
  byte mem[2];
  _B = 0x55;  // 0101 0101
  execCB(mem, 0xF8);
  EXPECT_EQ(0xD5, _B);  // 1101 0101
}

// ---------------------------------------------------------------------------
// RES 0,B (CB 80) - reset bit 0
// ---------------------------------------------------------------------------

TEST_F(Z80CBTest, RES0_B_BitWasSet) {
  byte mem[2];
  _B = 0xFF;
  execCB(mem, 0x80);
  EXPECT_EQ(0xFE, _B);
}

TEST_F(Z80CBTest, RES0_B_BitWasClear) {
  byte mem[2];
  _B = 0xFE;
  execCB(mem, 0x80);
  EXPECT_EQ(0xFE, _B);  // unchanged
}

// ---------------------------------------------------------------------------
// RES 7,B (CB B8) - reset bit 7
// ---------------------------------------------------------------------------

TEST_F(Z80CBTest, RES7_B_BitWasSet) {
  byte mem[2];
  _B = 0xFF;
  execCB(mem, 0xB8);
  EXPECT_EQ(0x7F, _B);
}

TEST_F(Z80CBTest, RES7_B_OtherBitsPreserved) {
  byte mem[2];
  _B = 0xAA;  // 1010 1010
  execCB(mem, 0xB8);
  EXPECT_EQ(0x2A, _B);  // 0010 1010
}

// ---------------------------------------------------------------------------
// PC advancement: CB prefix + sub-opcode = 2 bytes
// ---------------------------------------------------------------------------

TEST_F(Z80CBTest, PC_AdvancesBy2) {
  byte mem[2];
  _B = 0x01;
  _F = 0;
  execCB(mem, 0x00);  // RLC B
  EXPECT_EQ(2, _PC);
}

// ---------------------------------------------------------------------------
// R register: incremented twice (once for CB prefix, once for sub-opcode)
// ---------------------------------------------------------------------------

TEST_F(Z80CBTest, R_IncrementedTwice) {
  byte mem[2];
  _B = 0x01;
  _F = 0;
  execCB(mem, 0x00);  // RLC B
  EXPECT_EQ(2, _R);
}

// ---------------------------------------------------------------------------
// Cross-register: RLC A (CB 07)
// ---------------------------------------------------------------------------

TEST_F(Z80CBTest, RLC_A) {
  byte mem[2];
  _A = 0x81;  // 1000 0001
  _F = 0;
  execCB(mem, 0x07);
  // Result: 0000 0011 = 0x03, carry from bit 7
  EXPECT_EQ(0x03, _A);
  EXPECT_TRUE(_F & Cflag);
}

// ---------------------------------------------------------------------------
// Cross-register: SLA C (CB 21)
// ---------------------------------------------------------------------------

TEST_F(Z80CBTest, SLA_C) {
  byte mem[2];
  _C = 0xC0;  // 1100 0000
  _F = 0;
  execCB(mem, 0x21);
  // bit 7 -> carry, result = 1000 0000 = 0x80
  EXPECT_EQ(0x80, _C);
  EXPECT_TRUE(_F & Cflag);
  EXPECT_TRUE(_F & Sflag);
}

// ---------------------------------------------------------------------------
// Parity flag: SZP table includes parity
// ---------------------------------------------------------------------------

TEST_F(Z80CBTest, RLC_B_ParityEven) {
  byte mem[2];
  _B = 0x41;  // 0100 0001
  _F = 0;
  execCB(mem, 0x00);
  // RLC: result = 1000 0010 = 0x82 (two 1-bits = even parity)
  EXPECT_EQ(0x82, _B);
  EXPECT_FALSE(_F & Cflag);
  EXPECT_TRUE(_F & Pflag);  // even parity -> P flag set
}

TEST_F(Z80CBTest, RLC_B_ParityOdd) {
  byte mem[2];
  _B = 0x40;  // 0100 0000
  _F = 0;
  execCB(mem, 0x00);
  // RLC: result = 1000 0000 = 0x80 (one 1-bit = odd parity)
  EXPECT_EQ(0x80, _B);
  EXPECT_FALSE(_F & Cflag);
  EXPECT_FALSE(_F & Pflag);  // odd parity -> P flag cleared
}

// ---------------------------------------------------------------------------
// SET/RES do not affect flags
// ---------------------------------------------------------------------------

TEST_F(Z80CBTest, SET_DoesNotAffectFlags) {
  byte mem[2];
  _B = 0x00;
  _F = Zflag | Cflag;  // set some flags
  byte saved_f = _F;
  execCB(mem, 0xC0);  // SET 0,B
  EXPECT_EQ(0x01, _B);
  EXPECT_EQ(saved_f, _F);  // flags unchanged
}

TEST_F(Z80CBTest, RES_DoesNotAffectFlags) {
  byte mem[2];
  _B = 0xFF;
  _F = Sflag | Hflag;
  byte saved_f = _F;
  execCB(mem, 0x80);  // RES 0,B
  EXPECT_EQ(0xFE, _B);
  EXPECT_EQ(saved_f, _F);  // flags unchanged
}

// ---------------------------------------------------------------------------
// BIT preserves carry flag
// ---------------------------------------------------------------------------

TEST_F(Z80CBTest, BIT_PreservesCarrySet) {
  byte mem[2];
  _B = 0x00;
  _F = Cflag;
  execCB(mem, 0x40);  // BIT 0,B
  EXPECT_TRUE(_F & Cflag);
  EXPECT_TRUE(_F & Zflag);
}

TEST_F(Z80CBTest, BIT_PreservesCarryClear) {
  byte mem[2];
  _B = 0x00;
  _F = 0;
  execCB(mem, 0x40);  // BIT 0,B
  EXPECT_FALSE(_F & Cflag);
  EXPECT_TRUE(_F & Zflag);
}

// ---------------------------------------------------------------------------
// Shift edge cases
// ---------------------------------------------------------------------------

TEST_F(Z80CBTest, SRA_B_PositiveValue) {
  byte mem[2];
  _B = 0x7E;  // 0111 1110 (positive, bit 7 = 0)
  _F = 0;
  execCB(mem, 0x28);
  // bit 0 (0) -> carry, bit 7 stays 0
  // Result: 0011 1111 = 0x3F
  EXPECT_EQ(0x3F, _B);
  EXPECT_FALSE(_F & Cflag);
  EXPECT_FALSE(_F & Sflag);
}

TEST_F(Z80CBTest, SRL_B_AllOnes) {
  byte mem[2];
  _B = 0xFF;
  _F = 0;
  execCB(mem, 0x38);
  // bit 0 (1) -> carry, bit 7 = 0
  // Result: 0111 1111 = 0x7F
  EXPECT_EQ(0x7F, _B);
  EXPECT_TRUE(_F & Cflag);
  EXPECT_FALSE(_F & Sflag);
}

}  // namespace
