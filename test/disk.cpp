#include <gtest/gtest.h>

#include "disk.h"

namespace
{

class SectorReadTest : public testing::Test
{
  public:
    SectorReadTest()
    {
      data[0] = 1;
      data[512] = 2;
      sector.setData(data);
    }

  protected:
    t_sector sector;
    unsigned char data[1024] = {0};
};

TEST_F(SectorReadTest, NormalSector)
{
  sector.setSizes(1024, 1024);

  unsigned char* read1 = sector.getDataForRead();
  unsigned char* read2 = sector.getDataForRead();

  ASSERT_EQ(1, read1[0]);
  ASSERT_EQ(1, read2[0]);
}

TEST_F(SectorReadTest, WeakSector)
{
  sector.setSizes(512, 1024);

  unsigned char* read1 = sector.getDataForRead();
  unsigned char* read2 = sector.getDataForRead();

  // There's no reason to force a given order as long as 2 consecutive reads return 2 different versions
  ASSERT_NE(read1[0], read2[0]);
  // And the value is one of the 2 provided
  ASSERT_TRUE(read1[0] == 1 || read1[0] == 2);
  ASSERT_TRUE(read2[0] == 1 || read2[0] == 2);
}

TEST_F(SectorReadTest, LongSector)
{
  // Should work just as a normal sector of size 512
  sector.setSizes(1024, 512);

  unsigned char* read1 = sector.getDataForRead();
  unsigned char* read2 = sector.getDataForRead();

  ASSERT_EQ(1, read1[0]);
  ASSERT_EQ(1, read2[0]);
  ASSERT_EQ(512, sector.getTotalSize());
}

// ─────────────────────────────────────────────────
// chrn_to_string tests
// ─────────────────────────────────────────────────

TEST(ChrnToString, AllZeros) {
  unsigned char chrn[4] = {0, 0, 0, 0};
  EXPECT_EQ("0-0-0-0", chrn_to_string(chrn));
}

TEST(ChrnToString, StandardFormat) {
  unsigned char chrn[4] = {1, 0, 0xC1, 2}; // Track 1, Side 0, Sector ID 0xC1, Size 2 (512 bytes)
  EXPECT_EQ("1-0-193-2", chrn_to_string(chrn));
}

TEST(ChrnToString, MaxValues) {
  unsigned char chrn[4] = {255, 255, 255, 255};
  EXPECT_EQ("255-255-255-255", chrn_to_string(chrn));
}

TEST(ChrnToString, TypicalAmstradFormat) {
  // Typical Amstrad CPC DATA format: Track 0, Side 0, Sector C1, Size 2
  unsigned char chrn[4] = {0, 0, 0xC1, 2};
  EXPECT_EQ("0-0-193-2", chrn_to_string(chrn));
}

// ─────────────────────────────────────────────────
// t_sector additional tests
// ─────────────────────────────────────────────────

TEST(SectorTest, SetSizesNormal) {
  t_sector sector;
  unsigned char data[1024] = {0};
  sector.setData(data);
  sector.setSizes(512, 512);
  EXPECT_EQ(512u, sector.getTotalSize());
}

TEST(SectorTest, SetSizesWithMultipleWeakVersions) {
  t_sector sector;
  unsigned char data[2048] = {0};
  sector.setData(data);
  // 4 weak versions: total_size / size = 2048 / 512 = 4
  sector.setSizes(512, 2048);
  EXPECT_EQ(2048u, sector.getTotalSize());
}

TEST(SectorTest, GetDataForWriteReturnsBasePointer) {
  t_sector sector;
  unsigned char data[512] = {0};
  data[0] = 0xAA;
  sector.setData(data);
  sector.setSizes(512, 512);

  unsigned char* write_ptr = sector.getDataForWrite();
  EXPECT_EQ(data, write_ptr);
  EXPECT_EQ(0xAA, write_ptr[0]);
}

TEST(SectorTest, MultipleWeakReadsReturnDifferentVersions) {
  t_sector sector;
  unsigned char data[1024] = {0};
  data[0] = 0xAA;     // Version 0
  data[512] = 0xBB;   // Version 1
  sector.setData(data);
  sector.setSizes(512, 1024); // 2 weak versions

  // After 2 reads, we should have seen both versions
  unsigned char* read1 = sector.getDataForRead();
  unsigned char* read2 = sector.getDataForRead();

  // The values should be different (cycling through versions)
  EXPECT_NE(read1, read2);
}

TEST(SectorTest, CycleThroughAllWeakVersions) {
  t_sector sector;
  unsigned char data[1536] = {0};
  data[0] = 1;        // Version 0
  data[512] = 2;      // Version 1
  data[1024] = 3;     // Version 2
  sector.setData(data);
  sector.setSizes(512, 1536); // 3 weak versions

  // After 3 reads, we should cycle back
  unsigned char* r1 = sector.getDataForRead();
  unsigned char* r2 = sector.getDataForRead();
  unsigned char* r3 = sector.getDataForRead();
  unsigned char* r4 = sector.getDataForRead(); // Should cycle back

  EXPECT_EQ(r1, r4); // Same version after cycling
}

// ─────────────────────────────────────────────────
// t_track tests (struct initialization)
// ─────────────────────────────────────────────────

TEST(TrackTest, DefaultInitialization) {
  t_track track = {};
  EXPECT_EQ(0u, track.sectors);
  EXPECT_EQ(0u, track.size);
  EXPECT_EQ(nullptr, track.data);
}

// ─────────────────────────────────────────────────
// t_drive tests (struct initialization)
// ─────────────────────────────────────────────────

TEST(DriveTest, DefaultInitialization) {
  t_drive drive = {};
  EXPECT_EQ(0u, drive.tracks);
  EXPECT_EQ(0u, drive.current_track);
  EXPECT_EQ(0u, drive.sides);
  EXPECT_EQ(0u, drive.current_side);
  EXPECT_FALSE(drive.altered);
  EXPECT_EQ(0u, drive.write_protected);
}

// ─────────────────────────────────────────────────
// t_disk_format tests
// ─────────────────────────────────────────────────

TEST(DiskFormatTest, DefaultConstruction) {
  t_disk_format format;
  EXPECT_EQ(0u, format.tracks);
  EXPECT_EQ(0u, format.sides);
  EXPECT_EQ(0u, format.sectors);
  EXPECT_EQ(0u, format.sector_size);
  EXPECT_EQ(0u, format.gap3_length);
  EXPECT_EQ(0, format.filler_byte);
}

TEST(DiskFormatTest, AmstradDataFormat) {
  t_disk_format format;
  format.label = "DATA";
  format.tracks = 40;
  format.sides = 1;
  format.sectors = 9;
  format.sector_size = 2; // N=2 means 512 bytes
  format.gap3_length = 0x4E;
  format.filler_byte = 0xE5;

  EXPECT_EQ("DATA", format.label);
  EXPECT_EQ(40u, format.tracks);
  EXPECT_EQ(1u, format.sides);
  EXPECT_EQ(9u, format.sectors);
}

// ─────────────────────────────────────────────────
// DSK header constants tests
// ─────────────────────────────────────────────────

TEST(DskConstants, MaximumValues) {
  // Verify DSK format constraints
  EXPECT_EQ(8192, DSK_BPTMAX);       // Max bytes per track
  EXPECT_EQ(102, DSK_TRACKMAX);      // Max tracks
  EXPECT_EQ(2, DSK_SIDEMAX);         // Max sides
  EXPECT_EQ(29, DSK_SECTORMAX);      // Max sectors per track
}

TEST(DskConstants, FdcDirections) {
  EXPECT_EQ(0, FDC_TO_CPU);
  EXPECT_EQ(1, CPU_TO_FDC);
}

TEST(DskConstants, FdcPhases) {
  EXPECT_EQ(0, CMD_PHASE);
  EXPECT_EQ(1, EXEC_PHASE);
  EXPECT_EQ(2, RESULT_PHASE);
}

// ─────────────────────────────────────────────────
// DSK header struct tests
// ─────────────────────────────────────────────────

TEST(DskHeaderTest, SizeCheck) {
  // DSK header should be 256 bytes
  // id(34) + unused1(14) + tracks(1) + sides(1) + unused2(2) + track_size(204) = 256
  t_DSK_header header = {};
  EXPECT_EQ(256u, sizeof(header));
}

TEST(TrackHeaderTest, SizeCheck) {
  // Track header: id(12) + unused1(4) + track(1) + side(1) + unused2(2) + bps(1) + sectors(1) + gap3(1) + filler(1) + sector(29*8)
  // = 12 + 4 + 1 + 1 + 2 + 1 + 1 + 1 + 1 + 232 = 256
  t_track_header header = {};
  EXPECT_EQ(256u, sizeof(header));
}

}
