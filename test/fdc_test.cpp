/* Tests for FDC (uPD765A) command dispatch.
   Verifies status register, command parsing, seek, recalibrate, specify,
   sense interrupt, and invalid command handling through the public API.
*/

#include <gtest/gtest.h>

#include "koncepcja.h"
#include "disk.h"

extern t_CPC CPC;
extern t_FDC FDC;
extern t_drive driveA;
extern t_drive driveB;

// FDC-internal globals defined in fdc.cpp
extern t_drive *active_drive;
extern t_track *active_track;
extern dword read_status_delay;

class FdcTest : public testing::Test {
 protected:
  void SetUp() override {
    memset(&FDC, 0, sizeof(FDC));
    memset(&driveA, 0, sizeof(driveA));
    memset(&driveB, 0, sizeof(driveB));
    active_drive = &driveA;
    active_track = nullptr;
    read_status_delay = 0;

    // Give drives some tracks so they appear "ready"
    driveA.tracks = 40;
    driveA.sides = 1;
    driveA.current_track = 0;
    driveB.tracks = 40;
    driveB.sides = 1;
    driveB.current_track = 0;

    // Motor must be on for drive to be considered ready
    FDC.motor = 1;
  }
};

// ─────────────────────────────────────────────────
// Status register initial state
// ─────────────────────────────────────────────────

TEST_F(FdcTest, ReadStatus_InitialState) {
  // In command phase with byte_count=0, status should be 0x80 (RQM, ready)
  EXPECT_EQ(FDC.phase, CMD_PHASE);
  byte status = fdc_read_status();
  EXPECT_EQ(status, 0x80);
}

TEST_F(FdcTest, ReadStatus_ReceivingCommandParams) {
  // After writing the first byte of a multi-byte command, FDC should be busy
  fdc_write_data(0x03);  // Specify command (3 bytes total)
  // Now in CMD_PHASE but byte_count > 0 (waiting for parameters)
  byte status = fdc_read_status();
  EXPECT_EQ(status & 0x90, 0x90);  // RQM + busy
}

// ─────────────────────────────────────────────────
// Specify command (0x03) — 3 bytes, no result phase
// ─────────────────────────────────────────────────

TEST_F(FdcTest, Specify_NoResultPhase) {
  // Specify: command byte + 2 parameter bytes, returns to CMD_PHASE directly
  fdc_write_data(0x03);  // command
  fdc_write_data(0xDF);  // SRT/HUT
  fdc_write_data(0x02);  // HLT/ND

  // Should return to command phase (no result phase for specify)
  EXPECT_EQ(FDC.phase, CMD_PHASE);
  // Status should be ready again
  EXPECT_EQ(fdc_read_status(), 0x80);
}

// ─────────────────────────────────────────────────
// Sense Interrupt Status (0x08) — 1 byte cmd, 2 result bytes
// ─────────────────────────────────────────────────

TEST_F(FdcTest, SenseInterruptStatus_NoSeekPending) {
  // With no seek flags set, sense interrupt returns invalid (0x80)
  fdc_write_data(0x08);

  EXPECT_EQ(FDC.phase, RESULT_PHASE);

  byte st0 = fdc_read_data();
  EXPECT_EQ(st0, 0x80);  // Invalid Command response when no seek pending

  // After reading all result bytes, should return to command phase
  EXPECT_EQ(FDC.phase, CMD_PHASE);
}

TEST_F(FdcTest, SenseInterruptStatus_AfterSeek) {
  // Issue a seek on drive A, then sense interrupt
  fdc_write_data(0x0F);  // Seek command
  fdc_write_data(0x00);  // Drive A, head 0
  fdc_write_data(0x05);  // Track 5

  // Seek returns to CMD_PHASE (no result phase)
  EXPECT_EQ(FDC.phase, CMD_PHASE);

  // Now sense interrupt status
  fdc_write_data(0x08);
  EXPECT_EQ(FDC.phase, RESULT_PHASE);

  byte st0 = fdc_read_data();
  EXPECT_EQ(st0 & 0x20, 0x20);  // Seek End bit set

  byte pcn = fdc_read_data();
  EXPECT_EQ(pcn, 5);  // Present Cylinder Number = 5

  EXPECT_EQ(FDC.phase, CMD_PHASE);
}

// ─────────────────────────────────────────────────
// Recalibrate (0x07) — seeks to track 0
// ─────────────────────────────────────────────────

TEST_F(FdcTest, Recalibrate_SeeksToTrack0) {
  driveA.current_track = 20;

  fdc_write_data(0x07);  // Recalibrate
  fdc_write_data(0x00);  // Drive A

  EXPECT_EQ(FDC.phase, CMD_PHASE);  // no result phase
  EXPECT_EQ(driveA.current_track, 0u);
}

TEST_F(FdcTest, Recalibrate_DriveB) {
  driveB.current_track = 15;

  fdc_write_data(0x07);  // Recalibrate
  fdc_write_data(0x01);  // Drive B

  EXPECT_EQ(FDC.phase, CMD_PHASE);
  EXPECT_EQ(driveB.current_track, 0u);
}

// ─────────────────────────────────────────────────
// Seek (0x0F) — updates current_track
// ─────────────────────────────────────────────────

TEST_F(FdcTest, Seek_UpdatesCurrentTrack) {
  fdc_write_data(0x0F);  // Seek
  fdc_write_data(0x00);  // Drive A
  fdc_write_data(0x25);  // Track 37

  EXPECT_EQ(FDC.phase, CMD_PHASE);
  EXPECT_EQ(driveA.current_track, 37u);
}

TEST_F(FdcTest, Seek_DriveB) {
  fdc_write_data(0x0F);  // Seek
  fdc_write_data(0x01);  // Drive B
  fdc_write_data(0x0A);  // Track 10

  EXPECT_EQ(FDC.phase, CMD_PHASE);
  EXPECT_EQ(driveB.current_track, 10u);
}

TEST_F(FdcTest, Seek_ClampsToMaxTrack) {
  // Track number beyond DSK_TRACKMAX should be clamped
  fdc_write_data(0x0F);
  fdc_write_data(0x00);
  fdc_write_data(0xFF);  // 255, beyond DSK_TRACKMAX (102)

  EXPECT_EQ(FDC.phase, CMD_PHASE);
  EXPECT_EQ(driveA.current_track, (unsigned int)(DSK_TRACKMAX - 1));
}

// ─────────────────────────────────────────────────
// Seek on empty drive (no disk)
// ─────────────────────────────────────────────────

TEST_F(FdcTest, Seek_NoDisk_SetsNotReady) {
  driveA.tracks = 0;  // no disk loaded

  fdc_write_data(0x0F);
  fdc_write_data(0x00);  // Drive A
  fdc_write_data(0x05);  // Track 5

  // Track should NOT be updated when drive is not ready
  // (init_status_regs returns non-zero -> seek body skipped)
  // But SEEKDRVA flag is still set
  EXPECT_EQ(FDC.phase, CMD_PHASE);

  // Sense interrupt to read status
  fdc_write_data(0x08);
  byte st0 = fdc_read_data();
  // Not ready flag should be present
  EXPECT_EQ(st0 & 0x08, 0x08);
  fdc_read_data();  // consume PCN
}

// ─────────────────────────────────────────────────
// Invalid command
// ─────────────────────────────────────────────────

TEST_F(FdcTest, InvalidCommand_Returns0x80) {
  fdc_write_data(0xFF);  // Unknown command

  EXPECT_EQ(FDC.phase, RESULT_PHASE);
  EXPECT_EQ(FDC.res_length, 1);

  byte result = fdc_read_data();
  EXPECT_EQ(result, 0x80);
  EXPECT_EQ(FDC.phase, CMD_PHASE);
}

TEST_F(FdcTest, InvalidCommand_AnotherValue) {
  fdc_write_data(0x00);  // Also not a valid FDC command

  EXPECT_EQ(FDC.phase, RESULT_PHASE);
  byte result = fdc_read_data();
  EXPECT_EQ(result, 0x80);
  EXPECT_EQ(FDC.phase, CMD_PHASE);
}

// ─────────────────────────────────────────────────
// Sense Device Status (0x04) — 2 byte cmd, 1 result byte
// ─────────────────────────────────────────────────

TEST_F(FdcTest, SenseDeviceStatus_ReadyDriveAtTrack0) {
  driveA.current_track = 0;
  driveA.write_protected = 0;

  fdc_write_data(0x04);  // Sense Device Status
  fdc_write_data(0x00);  // Drive A, head 0

  EXPECT_EQ(FDC.phase, RESULT_PHASE);

  byte st3 = fdc_read_data();
  EXPECT_EQ(st3 & 0x10, 0x10);  // Track 0 flag
  EXPECT_EQ(st3 & 0x20, 0x20);  // Ready flag
  EXPECT_EQ(st3 & 0x40, 0x00);  // Write Protect clear

  EXPECT_EQ(FDC.phase, CMD_PHASE);
}

TEST_F(FdcTest, SenseDeviceStatus_WriteProtected) {
  driveA.write_protected = 1;

  fdc_write_data(0x04);
  fdc_write_data(0x00);

  byte st3 = fdc_read_data();
  EXPECT_EQ(st3 & 0x48, 0x48);  // Write Protect + Two Sided bits
}

TEST_F(FdcTest, SenseDeviceStatus_NotAtTrack0) {
  driveA.current_track = 10;

  fdc_write_data(0x04);
  fdc_write_data(0x00);

  byte st3 = fdc_read_data();
  EXPECT_EQ(st3 & 0x10, 0x00);  // Track 0 flag should be clear
}
