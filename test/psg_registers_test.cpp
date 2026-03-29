/* Tests for PSG (AY-3-8912) register handling.
   Verifies masking, envelope reset, chip reset, and level table init.
*/

#include <gtest/gtest.h>

#include "koncepcja.h"
#include "types.h"

extern t_CPC CPC;
extern t_PSG PSG;

// PSG internals defined in psg.cpp — needed for verification
extern int Level_AL[32];
extern int Level_AR[32];
extern int Level_BL[32];
extern int Level_BR[32];
extern int Level_CL[32];
extern int Level_CR[32];
extern int64_t LoopCountInit;
extern bool Ton_EnA, Ton_EnB, Ton_EnC;
extern bool Noise_EnA, Noise_EnB, Noise_EnC;
extern bool Envelope_EnA, Envelope_EnB, Envelope_EnC;

// Union types matching psg.cpp definitions
union TNoise {
   struct {
      word Low;
      word Val;
   };
   dword Seed;
};
extern TNoise Noise;

union TCounter {
   struct {
      word Lo;
      word Hi;
   };
   dword Re;
};
extern TCounter Ton_Counter_A, Ton_Counter_B, Ton_Counter_C, Noise_Counter;

union TEnvelopeCounter {
   struct {
      dword Lo;
      dword Hi;
   };
   int64_t Re;
};
extern TEnvelopeCounter Envelope_Counter;

extern byte Ton_A, Ton_B, Ton_C;
extern int Left_Chan, Right_Chan;

extern byte Index_AL, Index_AR, Index_BL, Index_BR, Index_CL, Index_CR;
extern int PreAmpMax;

extern dword freq_table[];

class PsgRegisterTest : public testing::Test {
 protected:
  void SetUp() override {
    memset(&PSG.RegisterAY, 0, sizeof(PSG.RegisterAY));
    PSG.AmplitudeEnv = 0;
    PSG.FirstPeriod = false;
    PSG.buffer_full = 0;
  }
};

// ─────────────────────────────────────────────────
// Register write masking tests
// ─────────────────────────────────────────────────

TEST_F(PsgRegisterTest, ToneALowByte_NoMask) {
  // Register 0: tone period A low byte — full 8 bits, no mask
  SetAYRegister(0, 0xFF);
  EXPECT_EQ(PSG.RegisterAY.Index[0], 0xFF);
}

TEST_F(PsgRegisterTest, ToneAHighByte_Masked4Bits) {
  // Register 1: tone period A high — masked to 4 bits
  SetAYRegister(1, 0xFF);
  EXPECT_EQ(PSG.RegisterAY.Index[1], 0x0F);
}

TEST_F(PsgRegisterTest, ToneBLowByte_NoMask) {
  // Register 2: tone period B low byte — full 8 bits
  SetAYRegister(2, 0xFF);
  EXPECT_EQ(PSG.RegisterAY.Index[2], 0xFF);
}

TEST_F(PsgRegisterTest, ToneBHighByte_Masked4Bits) {
  // Register 3: tone period B high — masked to 4 bits
  SetAYRegister(3, 0xFF);
  EXPECT_EQ(PSG.RegisterAY.Index[3], 0x0F);
}

TEST_F(PsgRegisterTest, ToneCLowByte_NoMask) {
  // Register 4: tone period C low byte — full 8 bits
  SetAYRegister(4, 0xFF);
  EXPECT_EQ(PSG.RegisterAY.Index[4], 0xFF);
}

TEST_F(PsgRegisterTest, ToneCHighByte_Masked4Bits) {
  // Register 5: tone period C high — masked to 4 bits
  SetAYRegister(5, 0xFF);
  EXPECT_EQ(PSG.RegisterAY.Index[5], 0x0F);
}

TEST_F(PsgRegisterTest, NoisePeriod_Masked5Bits) {
  // Register 6: noise period — masked to 5 bits
  SetAYRegister(6, 0xFF);
  EXPECT_EQ(PSG.RegisterAY.Noise, 0x1F);
}

TEST_F(PsgRegisterTest, Mixer_Masked6Bits) {
  // Register 7: mixer — masked to 6 bits
  SetAYRegister(7, 0xFF);
  EXPECT_EQ(PSG.RegisterAY.Mixer, 0x3F);
}

TEST_F(PsgRegisterTest, AmplitudeA_Masked5Bits) {
  // Register 8: channel A amplitude — masked to 5 bits
  SetAYRegister(8, 0xFF);
  EXPECT_EQ(PSG.RegisterAY.AmplitudeA, 0x1F);
}

TEST_F(PsgRegisterTest, AmplitudeB_Masked5Bits) {
  // Register 9: channel B amplitude — masked to 5 bits
  SetAYRegister(9, 0xFF);
  EXPECT_EQ(PSG.RegisterAY.AmplitudeB, 0x1F);
}

TEST_F(PsgRegisterTest, AmplitudeC_Masked5Bits) {
  // Register 10: channel C amplitude — masked to 5 bits
  SetAYRegister(10, 0xFF);
  EXPECT_EQ(PSG.RegisterAY.AmplitudeC, 0x1F);
}

TEST_F(PsgRegisterTest, EnvelopeLow_NoMask) {
  // Register 11: envelope period low — full 8 bits
  SetAYRegister(11, 0xAB);
  EXPECT_EQ(PSG.RegisterAY.Index[11], 0xAB);
}

TEST_F(PsgRegisterTest, EnvelopeHigh_NoMask) {
  // Register 12: envelope period high — full 8 bits
  SetAYRegister(12, 0xCD);
  EXPECT_EQ(PSG.RegisterAY.Index[12], 0xCD);
}

// ─────────────────────────────────────────────────
// Envelope shape register (13) triggers reset
// ─────────────────────────────────────────────────

TEST_F(PsgRegisterTest, EnvelopeShape_TriggersReset) {
  // Writing to register 13 should reset envelope state
  PSG.FirstPeriod = false;
  PSG.AmplitudeEnv = 15;
  Envelope_Counter.Hi = 999;

  SetAYRegister(13, 0);  // shape 0: attack bit clear -> AmplitudeEnv starts at 32
  EXPECT_TRUE(PSG.FirstPeriod);
  EXPECT_EQ(Envelope_Counter.Hi, 0u);
  // Value is masked to 4 bits, shape 0 has attack bit (bit 2) clear -> AmplitudeEnv = 32
  EXPECT_EQ(PSG.AmplitudeEnv, 32);
  EXPECT_EQ(PSG.RegisterAY.EnvType, 0);
}

TEST_F(PsgRegisterTest, EnvelopeShape_AttackBitSet) {
  // Shape with bit 2 set -> AmplitudeEnv starts at -1
  SetAYRegister(13, 0x0C);  // 0x0C & 0x0F = 0x0C, bit 2 set
  EXPECT_TRUE(PSG.FirstPeriod);
  EXPECT_EQ(PSG.AmplitudeEnv, -1);
  EXPECT_EQ(PSG.RegisterAY.EnvType, 0x0C);
}

TEST_F(PsgRegisterTest, EnvelopeShape_ValueMaskedTo4Bits) {
  // Register 13 value is masked to lower 4 bits before processing
  SetAYRegister(13, 0xF8);  // 0xF8 & 0x0F = 0x08
  EXPECT_EQ(PSG.RegisterAY.EnvType, 0x08);
}

// ─────────────────────────────────────────────────
// Mixer sets tone/noise enable flags
// ─────────────────────────────────────────────────

TEST_F(PsgRegisterTest, Mixer_ToneNoiseFlags) {
  // All channels enabled (bits clear = enabled)
  SetAYRegister(7, 0x00);
  EXPECT_TRUE(Ton_EnA);
  EXPECT_TRUE(Ton_EnB);
  EXPECT_TRUE(Ton_EnC);
  EXPECT_TRUE(Noise_EnA);
  EXPECT_TRUE(Noise_EnB);
  EXPECT_TRUE(Noise_EnC);

  // All channels disabled
  SetAYRegister(7, 0x3F);
  EXPECT_FALSE(Ton_EnA);
  EXPECT_FALSE(Ton_EnB);
  EXPECT_FALSE(Ton_EnC);
  EXPECT_FALSE(Noise_EnA);
  EXPECT_FALSE(Noise_EnB);
  EXPECT_FALSE(Noise_EnC);
}

TEST_F(PsgRegisterTest, Mixer_SelectiveToneNoise) {
  // 0x26 = 0b00_100_110: bits 1,2 set (tone B/C off), bit 5 set (noise C off)
  // bits 0,3,4 clear (tone A on, noise A on, noise B on)
  SetAYRegister(7, 0x26);
  EXPECT_TRUE(Ton_EnA);    // bit 0 clear
  EXPECT_FALSE(Ton_EnB);   // bit 1 set
  EXPECT_FALSE(Ton_EnC);   // bit 2 set
  EXPECT_TRUE(Noise_EnA);  // bit 3 clear
  EXPECT_TRUE(Noise_EnB);  // bit 4 clear
  EXPECT_FALSE(Noise_EnC); // bit 5 set
}

// ─────────────────────────────────────────────────
// Amplitude registers set envelope enable flags
// ─────────────────────────────────────────────────

TEST_F(PsgRegisterTest, AmplitudeA_EnvelopeEnable) {
  // bit 4 clear -> Envelope_EnA = true (envelope drives amplitude)
  SetAYRegister(8, 0x0F);
  EXPECT_TRUE(Envelope_EnA);

  // bit 4 set -> Envelope_EnA = false (envelope mode)
  SetAYRegister(8, 0x1F);
  EXPECT_FALSE(Envelope_EnA);
}

// ─────────────────────────────────────────────────
// ResetAYChipEmulation
// ─────────────────────────────────────────────────

TEST_F(PsgRegisterTest, ResetAYChipEmulation_NoiseSeed) {
  Noise.Seed = 0;
  ResetAYChipEmulation();
  EXPECT_EQ(Noise.Seed, 0xFFFFu);
}

TEST_F(PsgRegisterTest, ResetAYChipEmulation_CountersZeroed) {
  Ton_Counter_A.Re = 0xDEAD;
  Ton_Counter_B.Re = 0xBEEF;
  Ton_Counter_C.Re = 0xCAFE;
  Noise_Counter.Re = 0xF00D;
  Envelope_Counter.Re = 0x1234;
  Ton_A = 1;
  Ton_B = 1;
  Ton_C = 1;
  Left_Chan = 999;
  Right_Chan = 888;

  ResetAYChipEmulation();

  EXPECT_EQ(Ton_Counter_A.Re, 0u);
  EXPECT_EQ(Ton_Counter_B.Re, 0u);
  EXPECT_EQ(Ton_Counter_C.Re, 0u);
  EXPECT_EQ(Noise_Counter.Re, 0u);
  EXPECT_EQ(Envelope_Counter.Re, 0);
  EXPECT_EQ(Ton_A, 0);
  EXPECT_EQ(Ton_B, 0);
  EXPECT_EQ(Ton_C, 0);
  EXPECT_EQ(Left_Chan, 0);
  EXPECT_EQ(Right_Chan, 0);
}

// ─────────────────────────────────────────────────
// Calculate_Level_Tables
// ─────────────────────────────────────────────────

TEST_F(PsgRegisterTest, CalculateLevelTables_NonZeroValues) {
  // Set up minimal CPC config for level table calculation
  CPC.snd_bits = 1;       // 16-bit
  CPC.snd_stereo = 1;     // stereo
  CPC.snd_volume = 80;    // audible volume

  // Set channel indices (as InitAY does)
  Index_AL = 255;
  Index_AR = 13;
  Index_BL = 170;
  Index_BR = 170;
  Index_CL = 13;
  Index_CR = 255;
  PreAmpMax = 100;

  Calculate_Level_Tables();

  // At volume>0 with non-zero channel indices, higher amplitude entries
  // should produce non-zero levels
  EXPECT_NE(Level_AL[30], 0);  // amplitude index 15 (max) * 2
  EXPECT_NE(Level_AR[30], 0);
  EXPECT_NE(Level_BL[30], 0);
  EXPECT_NE(Level_BR[30], 0);
  EXPECT_NE(Level_CL[30], 0);
  EXPECT_NE(Level_CR[30], 0);

  // Index 0 should be zero (amplitude 0)
  EXPECT_EQ(Level_AL[0], 0);
  EXPECT_EQ(Level_AR[0], 0);
}

// ─────────────────────────────────────────────────
// InitAYCounterVars
// ─────────────────────────────────────────────────

TEST_F(PsgRegisterTest, InitAYCounterVars_SetsLoopCountInit) {
  CPC.speed = 4;             // default speed
  CPC.snd_playback_rate = 2; // index 2 = 44100 Hz
  LoopCountInit = 0;

  InitAYCounterVars();

  EXPECT_NE(LoopCountInit, 0);
}

// ─────────────────────────────────────────────────
// Register write does not affect unrelated registers
// ─────────────────────────────────────────────────

TEST_F(PsgRegisterTest, RegisterWrite_Isolation) {
  // Write to register 0, verify other registers untouched
  memset(&PSG.RegisterAY, 0, sizeof(PSG.RegisterAY));
  SetAYRegister(0, 0xAB);
  EXPECT_EQ(PSG.RegisterAY.Index[0], 0xAB);
  EXPECT_EQ(PSG.RegisterAY.Index[1], 0x00);
  EXPECT_EQ(PSG.RegisterAY.Index[2], 0x00);
  EXPECT_EQ(PSG.RegisterAY.Index[6], 0x00);
}

// ─────────────────────────────────────────────────
// Out-of-range register number is ignored
// ─────────────────────────────────────────────────

TEST_F(PsgRegisterTest, InvalidRegisterNumber_Ignored) {
  // Registers 14 and 15 (PortA, PortB) are not handled by SetAYRegister
  // Writing to them should not crash; they fall through the switch
  memset(&PSG.RegisterAY, 0xAA, sizeof(PSG.RegisterAY));
  byte saved[16];
  memcpy(saved, PSG.RegisterAY.Index, 16);

  SetAYRegister(14, 0x55);
  SetAYRegister(15, 0x55);

  // Port registers are not modified by SetAYRegister (no case for 14/15)
  EXPECT_EQ(PSG.RegisterAY.Index[14], 0xAA);
  EXPECT_EQ(PSG.RegisterAY.Index[15], 0xAA);
}
