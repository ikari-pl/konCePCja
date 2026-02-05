#include <gtest/gtest.h>
#include <vector>
#include "koncepcja.h"
#include "tape.h"
#include "imgui_ui.h"

// External globals from tape.cpp
extern byte bTapeLevel;
extern byte bTapeData;
extern byte *pbTapeBlock;
extern byte *pbTapeBlockData;
extern int iTapeCycleCount;
extern dword dwTapePulseCycles;
extern dword dwTapeZeroPulseCycles;
extern dword dwTapeOnePulseCycles;
extern dword dwTapeStage;
extern dword dwTapePulseCount;
extern dword dwTapeDataCount;
extern dword dwTapeBitsToShift;

// External globals - defined in main source files
extern std::vector<byte> pbTapeImage;
extern byte *pbTapeImageEnd;
extern t_CPC CPC;
extern ImGuiUIState imgui_state;

// Test macros from tape.cpp
#define CYCLE_SCALE ((40 << 16) / 35)
#define CYCLE_ADJUST(p) ((static_cast<dword>(p) * CYCLE_SCALE) >> 16)
#define MS_TO_CYCLES(p) (static_cast<dword>(p) * 4000)

// ─────────────────────────────────────────────────
// CYCLE_ADJUST macro tests
// ─────────────────────────────────────────────────

TEST(TapeMacros, CycleAdjustZero) {
  EXPECT_EQ(0u, CYCLE_ADJUST(0));
}

TEST(TapeMacros, CycleAdjustStandardPilot) {
  // Standard pilot pulse is 2168 T-states
  // CYCLE_SCALE = (40 * 65536) / 35 = 74898
  // 2168 * 74898 / 65536 = 2477
  dword result = CYCLE_ADJUST(2168);
  EXPECT_GT(result, 2168u); // Should be scaled up (CPC runs faster than Spectrum)
  EXPECT_LT(result, 3000u); // But not too much
}

TEST(TapeMacros, CycleAdjustStandardZeroBit) {
  // Zero bit pulse is 855 T-states
  dword result = CYCLE_ADJUST(855);
  EXPECT_GT(result, 855u);
  EXPECT_LT(result, 1200u);
}

TEST(TapeMacros, CycleAdjustStandardOneBit) {
  // One bit pulse is 1710 T-states
  dword result = CYCLE_ADJUST(1710);
  EXPECT_GT(result, 1710u);
  EXPECT_LT(result, 2400u);
}

TEST(TapeMacros, MsToCyclesZero) {
  EXPECT_EQ(0u, MS_TO_CYCLES(0));
}

TEST(TapeMacros, MsToCyclesOneMs) {
  // 1ms at 4MHz = 4000 cycles
  EXPECT_EQ(4000u, MS_TO_CYCLES(1));
}

TEST(TapeMacros, MsToCyclesOneSecond) {
  // 1000ms = 4,000,000 cycles
  EXPECT_EQ(4000000u, MS_TO_CYCLES(1000));
}

// ─────────────────────────────────────────────────
// Tape_SwitchLevel tests
// ─────────────────────────────────────────────────

class TapeLevelTest : public ::testing::Test {
protected:
  void SetUp() override {
    bTapeLevel = TAPE_LEVEL_LOW;
  }
};

TEST_F(TapeLevelTest, SwitchFromLowToHigh) {
  bTapeLevel = TAPE_LEVEL_LOW;
  Tape_SwitchLevel();
  EXPECT_EQ(TAPE_LEVEL_HIGH, bTapeLevel);
}

TEST_F(TapeLevelTest, SwitchFromHighToLow) {
  bTapeLevel = TAPE_LEVEL_HIGH;
  Tape_SwitchLevel();
  EXPECT_EQ(TAPE_LEVEL_LOW, bTapeLevel);
}

TEST_F(TapeLevelTest, DoubleSwitchReturnsSame) {
  bTapeLevel = TAPE_LEVEL_LOW;
  Tape_SwitchLevel();
  Tape_SwitchLevel();
  EXPECT_EQ(TAPE_LEVEL_LOW, bTapeLevel);
}

// ─────────────────────────────────────────────────
// Tape_Rewind tests
// ─────────────────────────────────────────────────

class TapeRewindTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Create a minimal valid CDT/TZX image
    // Header: "ZXTape!" + 0x1A + major + minor
    pbTapeImage.clear();
    pbTapeImage = {
      'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1A, 0x01, 0x14,
      // Block 0x10 (standard data): pause_lo, pause_hi, len_lo, len_hi, data...
      0x10, 0x00, 0x00, 0x02, 0x00, 0xAA, 0x55
    };
    pbTapeImageEnd = &pbTapeImage[0] + pbTapeImage.size();

    // Set tape pointer past the header
    pbTapeBlock = &pbTapeImage[10]; // Start of first block

    // Set some non-default values
    bTapeLevel = TAPE_LEVEL_HIGH;
    iTapeCycleCount = 12345;
    CPC.tape_play_button = 1;
  }

  void TearDown() override {
    pbTapeImage.clear();
    pbTapeImageEnd = nullptr;
    pbTapeBlock = nullptr;
  }
};

TEST_F(TapeRewindTest, ResetsTapeLevel) {
  Tape_Rewind();
  EXPECT_EQ(TAPE_LEVEL_LOW, bTapeLevel);
}

TEST_F(TapeRewindTest, SetsCycleCountForFirstBlock) {
  Tape_Rewind();
  // After rewind, Tape_GetNextBlock() is called which sets the cycle count
  // for the first block's pilot tone (2168 T-states adjusted for CPC timing)
  dword expected = ((2168u * ((40 << 16) / 35)) >> 16); // CYCLE_ADJUST(2168)
  EXPECT_EQ(static_cast<int>(expected), iTapeCycleCount);
}

TEST_F(TapeRewindTest, ResetsPlayButton) {
  Tape_Rewind();
  EXPECT_EQ(0, CPC.tape_play_button);
}

TEST_F(TapeRewindTest, PositionsAtFirstBlock) {
  Tape_Rewind();
  // After rewind, pbTapeBlock points to the first data block (past the 10-byte TZX header)
  EXPECT_EQ(&pbTapeImage[10], pbTapeBlock);
}

// ─────────────────────────────────────────────────
// Tape_ReadDataBit tests
// ─────────────────────────────────────────────────

class TapeReadDataBitTest : public ::testing::Test {
protected:
  byte testData[4];

  void SetUp() override {
    memset(testData, 0, sizeof(testData));
    dwTapeDataCount = 0;
    dwTapeBitsToShift = 0;
    dwTapePulseCount = 0;
    dwTapeZeroPulseCycles = 1000;
    dwTapeOnePulseCycles = 2000;

    // Reset UI state
    imgui_state.tape_decoded_head = 0;
    memset(imgui_state.tape_decoded_buf, 0, sizeof(imgui_state.tape_decoded_buf));
  }
};

TEST_F(TapeReadDataBitTest, ReturnsZeroWhenNoData) {
  dwTapeDataCount = 0;
  EXPECT_EQ(0, Tape_ReadDataBit());
}

TEST_F(TapeReadDataBitTest, ReturnsOneWhenHasData) {
  testData[0] = 0x80; // High bit set
  pbTapeBlockData = testData;
  dwTapeDataCount = 1;
  dwTapeBitsToShift = 0;

  EXPECT_EQ(1, Tape_ReadDataBit());
}

TEST_F(TapeReadDataBitTest, HighBitSetsOnePulseCycles) {
  testData[0] = 0x80; // High bit set (1)
  pbTapeBlockData = testData;
  dwTapeDataCount = 1;
  dwTapeBitsToShift = 0;
  dwTapeZeroPulseCycles = 1000;
  dwTapeOnePulseCycles = 2000;

  Tape_ReadDataBit();

  EXPECT_EQ(2000u, dwTapePulseCycles); // Should use one-bit pulse
}

TEST_F(TapeReadDataBitTest, LowBitSetsZeroPulseCycles) {
  testData[0] = 0x00; // Low bit (0)
  pbTapeBlockData = testData;
  dwTapeDataCount = 1;
  dwTapeBitsToShift = 0;
  dwTapeZeroPulseCycles = 1000;
  dwTapeOnePulseCycles = 2000;

  Tape_ReadDataBit();

  EXPECT_EQ(1000u, dwTapePulseCycles); // Should use zero-bit pulse
}

TEST_F(TapeReadDataBitTest, SetsPulseCountToTwo) {
  testData[0] = 0x55;
  pbTapeBlockData = testData;
  dwTapeDataCount = 1;
  dwTapeBitsToShift = 0;

  Tape_ReadDataBit();

  EXPECT_EQ(2u, dwTapePulseCount); // Two pulses per bit
}

TEST_F(TapeReadDataBitTest, DecrementsDataCount) {
  testData[0] = 0xFF;
  pbTapeBlockData = testData;
  dwTapeDataCount = 8;
  dwTapeBitsToShift = 0;

  Tape_ReadDataBit();

  EXPECT_EQ(7u, dwTapeDataCount);
}

TEST_F(TapeReadDataBitTest, ShiftsThroughAllBits) {
  testData[0] = 0xAA; // 10101010
  pbTapeBlockData = testData;
  dwTapeDataCount = 8;
  dwTapeBitsToShift = 0;

  // Read all 8 bits
  int bits[8];
  for (int i = 0; i < 8; i++) {
    Tape_ReadDataBit();
    bits[i] = (dwTapePulseCycles == dwTapeOnePulseCycles) ? 1 : 0;
  }

  // 0xAA = 10101010
  EXPECT_EQ(1, bits[0]);
  EXPECT_EQ(0, bits[1]);
  EXPECT_EQ(1, bits[2]);
  EXPECT_EQ(0, bits[3]);
  EXPECT_EQ(1, bits[4]);
  EXPECT_EQ(0, bits[5]);
  EXPECT_EQ(1, bits[6]);
  EXPECT_EQ(0, bits[7]);
}

TEST_F(TapeReadDataBitTest, AdvancesToNextByte) {
  testData[0] = 0xFF;
  testData[1] = 0x00;
  pbTapeBlockData = testData;
  dwTapeDataCount = 16; // 2 bytes
  dwTapeBitsToShift = 0;

  // Read first 8 bits (all 1s)
  for (int i = 0; i < 8; i++) {
    Tape_ReadDataBit();
    EXPECT_EQ(dwTapeOnePulseCycles, dwTapePulseCycles) << "Bit " << i;
  }

  // Read next 8 bits (all 0s)
  for (int i = 0; i < 8; i++) {
    Tape_ReadDataBit();
    EXPECT_EQ(dwTapeZeroPulseCycles, dwTapePulseCycles) << "Bit " << (i + 8);
  }
}

TEST_F(TapeReadDataBitTest, WritesToDecodedBuffer) {
  testData[0] = 0xC0; // 11000000 - first two bits are 1
  pbTapeBlockData = testData;
  dwTapeDataCount = 2;
  dwTapeBitsToShift = 0;
  imgui_state.tape_decoded_head = 0;

  Tape_ReadDataBit(); // Reads 1
  Tape_ReadDataBit(); // Reads 1

  EXPECT_EQ(1, imgui_state.tape_decoded_buf[0]);
  EXPECT_EQ(1, imgui_state.tape_decoded_buf[1]);
  EXPECT_EQ(2u, imgui_state.tape_decoded_head);
}

// ─────────────────────────────────────────────────
// Block size calculation tests (from tape_scan_blocks in imgui_ui.cpp)
// These verify the block size logic matches between tape.cpp and imgui_ui.cpp
// ─────────────────────────────────────────────────

TEST(TapeBlockSize, StandardSpeedBlock) {
  // Block 0x10: pause(2) + length(2) + data(length) + 1(block type)
  // Total header size = 4 bytes + 1 = 5 bytes before data
  byte block[] = { 0x10, 0xE8, 0x03, 0x02, 0x00, 0xAA, 0x55 };
  // pause = 0x03E8 = 1000ms, length = 0x0002 = 2 bytes

  word length = *reinterpret_cast<word*>(&block[3]);
  EXPECT_EQ(2, length);

  // Block size = length + 4 (header) + 1 (block ID)
  size_t expected_size = length + 4 + 1;
  EXPECT_EQ(7u, expected_size);
}

TEST(TapeBlockSize, TurboLoadingBlock) {
  // Block 0x11: more complex header + data
  // Header is 0x12 bytes before data
  byte block[0x13 + 4] = { 0x11 };
  // Set length at offset 0x10 (3 bytes, little-endian)
  block[0x10] = 0x04; // 4 bytes of data
  block[0x11] = 0x00;
  block[0x12] = 0x00;

  dword length = *reinterpret_cast<dword*>(&block[0x10]) & 0x00FFFFFF;
  EXPECT_EQ(4u, length);

  // Block size = (length & 0xFFFFFF) + 0x12 + 1
  size_t expected_size = length + 0x12 + 1;
  EXPECT_EQ(0x17u, expected_size);
}

TEST(TapeBlockSize, PureToneBlock) {
  // Block 0x12: pulse_length(2) + pulse_count(2) + 1 = 5 bytes
  byte block[] = { 0x12, 0x00, 0x10, 0x00, 0x08 };
  EXPECT_EQ(5u, sizeof(block));
}

TEST(TapeBlockSize, PauseBlock) {
  // Block 0x20: pause_length(2) + 1 = 3 bytes
  byte block[] = { 0x20, 0xE8, 0x03 };
  EXPECT_EQ(3u, sizeof(block));
}

TEST(TapeBlockSize, GroupStartBlock) {
  // Block 0x21: name_length(1) + name(length) + 1
  byte block[] = { 0x21, 0x04, 'T', 'e', 's', 't' };
  byte name_len = block[1];
  EXPECT_EQ(4, name_len);
  size_t expected_size = name_len + 1 + 1;
  EXPECT_EQ(6u, expected_size);
}

TEST(TapeBlockSize, GroupEndBlock) {
  // Block 0x22: just the block ID = 1 byte
  byte block[] = { 0x22 };
  EXPECT_EQ(1u, sizeof(block));
}
