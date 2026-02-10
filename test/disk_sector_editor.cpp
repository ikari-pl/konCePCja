#include <gtest/gtest.h>

#include <cstring>
#include <set>

#include "disk_sector_editor.h"
#include "disk_format.h"
#include "koncepcja.h"
#include "slotshandler.h"

extern t_drive driveA;
extern t_drive driveB;

namespace {

class DiskSectorEditorTest : public testing::Test {
 protected:
    void SetUp() override {
        dsk_eject(&driveA);
        dsk_eject(&driveB);
        // Format drive A as DATA format (40 tracks, 1 side, 9 sectors per track)
        std::string err = disk_format_drive('A', "data");
        ASSERT_EQ("", err);
    }

    void TearDown() override {
        dsk_eject(&driveA);
        dsk_eject(&driveB);
    }
};

// -----------------------------------------------
// disk_sector_info tests
// -----------------------------------------------

TEST_F(DiskSectorEditorTest, InfoListsSectorsOnTrack0) {
    std::string err;
    auto sectors = disk_sector_info(&driveA, 0, 0, err);
    EXPECT_EQ("", err);
    // DATA format: 9 sectors per track, sector IDs C1..C9 (interleaved order)
    ASSERT_EQ(9u, sectors.size());

    // Verify all expected sector IDs are present (order may be interleaved)
    std::set<uint8_t> found_ids;
    for (const auto& s : sectors) {
        EXPECT_EQ(0, s.C);    // Cylinder 0
        EXPECT_EQ(0, s.H);    // Head 0
        EXPECT_EQ(2, s.N);    // N=2 means 512 bytes
        EXPECT_EQ(512u, s.size);
        found_ids.insert(s.R);
    }
    // All sector IDs C1..C9 should be present
    for (uint8_t id = 0xC1; id <= 0xC9; id++) {
        EXPECT_TRUE(found_ids.count(id))
            << "Missing sector ID 0x" << std::hex
            << static_cast<unsigned>(id);
    }
}

TEST_F(DiskSectorEditorTest, InfoListsSectorsOnTrack5) {
    std::string err;
    auto sectors = disk_sector_info(&driveA, 5, 0, err);
    EXPECT_EQ("", err);
    ASSERT_EQ(9u, sectors.size());
    for (unsigned int i = 0; i < 9; i++) {
        EXPECT_EQ(5, sectors[i].C);    // Cylinder 5
    }
}

TEST_F(DiskSectorEditorTest, InfoBadTrackReturnsError) {
    std::string err;
    auto sectors = disk_sector_info(&driveA, 99, 0, err);
    EXPECT_NE("", err);
    EXPECT_TRUE(sectors.empty());
}

TEST_F(DiskSectorEditorTest, InfoBadSideReturnsError) {
    std::string err;
    // DATA format is single-sided (sides=0 in zero-based), so side 1 is invalid
    auto sectors = disk_sector_info(&driveA, 0, 1, err);
    EXPECT_NE("", err);
    EXPECT_TRUE(sectors.empty());
}

TEST_F(DiskSectorEditorTest, InfoNoDiscReturnsError) {
    dsk_eject(&driveA);
    std::string err;
    auto sectors = disk_sector_info(&driveA, 0, 0, err);
    EXPECT_NE("", err);
    EXPECT_TRUE(sectors.empty());
}

// -----------------------------------------------
// disk_sector_read tests
// -----------------------------------------------

TEST_F(DiskSectorEditorTest, ReadSectorReturns512Bytes) {
    std::string err;
    auto data = disk_sector_read(&driveA, 0, 0, 0xC1, err);
    EXPECT_EQ("", err);
    EXPECT_EQ(512u, data.size());
}

TEST_F(DiskSectorEditorTest, ReadAllSectorsOnTrack) {
    std::string err;
    // Read all 9 sectors on track 0
    for (uint8_t id = 0xC1; id <= 0xC9; id++) {
        auto data = disk_sector_read(&driveA, 0, 0, id, err);
        EXPECT_EQ("", err) << "Failed to read sector 0x"
                           << std::hex << static_cast<unsigned>(id);
        EXPECT_EQ(512u, data.size());
    }
}

TEST_F(DiskSectorEditorTest, ReadBadSectorIdReturnsError) {
    std::string err;
    // Sector 0x01 does not exist in DATA format
    auto data = disk_sector_read(&driveA, 0, 0, 0x01, err);
    EXPECT_NE("", err);
    EXPECT_TRUE(data.empty());
}

TEST_F(DiskSectorEditorTest, ReadBadTrackReturnsError) {
    std::string err;
    auto data = disk_sector_read(&driveA, 99, 0, 0xC1, err);
    EXPECT_NE("", err);
    EXPECT_TRUE(data.empty());
}

TEST_F(DiskSectorEditorTest, ReadBadSideReturnsError) {
    std::string err;
    auto data = disk_sector_read(&driveA, 0, 1, 0xC1, err);
    EXPECT_NE("", err);
    EXPECT_TRUE(data.empty());
}

TEST_F(DiskSectorEditorTest, ReadNoDiscReturnsError) {
    dsk_eject(&driveA);
    std::string err;
    auto data = disk_sector_read(&driveA, 0, 0, 0xC1, err);
    EXPECT_NE("", err);
    EXPECT_TRUE(data.empty());
}

// -----------------------------------------------
// disk_sector_write tests
// -----------------------------------------------

TEST_F(DiskSectorEditorTest, WriteAndReadBack) {
    // Write a known pattern to sector C1 on track 0
    std::vector<uint8_t> write_data(512, 0);
    for (size_t i = 0; i < write_data.size(); i++) {
        write_data[i] = static_cast<uint8_t>(i & 0xFF);
    }

    std::string err = disk_sector_write(&driveA, 0, 0, 0xC1, write_data);
    EXPECT_EQ("", err);

    // Read it back
    auto read_data = disk_sector_read(&driveA, 0, 0, 0xC1, err);
    EXPECT_EQ("", err);
    ASSERT_EQ(512u, read_data.size());
    EXPECT_EQ(write_data, read_data);
}

TEST_F(DiskSectorEditorTest, WriteDoesNotAffectOtherSectors) {
    // Read sector C2 first
    std::string err;
    auto original_c2 = disk_sector_read(&driveA, 0, 0, 0xC2, err);
    ASSERT_EQ("", err);

    // Write to sector C1
    std::vector<uint8_t> write_data(512, 0xAA);
    err = disk_sector_write(&driveA, 0, 0, 0xC1, write_data);
    EXPECT_EQ("", err);

    // C2 should be unchanged
    auto after_c2 = disk_sector_read(&driveA, 0, 0, 0xC2, err);
    EXPECT_EQ("", err);
    EXPECT_EQ(original_c2, after_c2);
}

TEST_F(DiskSectorEditorTest, WriteSetsAlteredFlag) {
    driveA.altered = false;
    std::vector<uint8_t> data(512, 0x55);
    std::string err = disk_sector_write(&driveA, 0, 0, 0xC1, data);
    EXPECT_EQ("", err);
    EXPECT_TRUE(driveA.altered);
}

TEST_F(DiskSectorEditorTest, WriteSizeMismatchReturnsError) {
    // Try to write 256 bytes to a 512-byte sector
    std::vector<uint8_t> short_data(256, 0xBB);
    std::string err = disk_sector_write(&driveA, 0, 0, 0xC1, short_data);
    EXPECT_NE("", err);
}

TEST_F(DiskSectorEditorTest, WriteBadSectorIdReturnsError) {
    std::vector<uint8_t> data(512, 0);
    std::string err = disk_sector_write(&driveA, 0, 0, 0x01, data);
    EXPECT_NE("", err);
}

TEST_F(DiskSectorEditorTest, WriteBadTrackReturnsError) {
    std::vector<uint8_t> data(512, 0);
    std::string err = disk_sector_write(&driveA, 99, 0, 0xC1, data);
    EXPECT_NE("", err);
}

TEST_F(DiskSectorEditorTest, WriteBadSideReturnsError) {
    std::vector<uint8_t> data(512, 0);
    std::string err = disk_sector_write(&driveA, 0, 1, 0xC1, data);
    EXPECT_NE("", err);
}

TEST_F(DiskSectorEditorTest, WriteNoDiscReturnsError) {
    dsk_eject(&driveA);
    std::vector<uint8_t> data(512, 0);
    std::string err = disk_sector_write(&driveA, 0, 0, 0xC1, data);
    EXPECT_NE("", err);
}

// -----------------------------------------------
// Round-trip: write and read across multiple tracks
// -----------------------------------------------

TEST_F(DiskSectorEditorTest, WriteAndReadAcrossTracks) {
    // Write distinct patterns to sectors on different tracks
    for (unsigned int t = 0; t < 5; t++) {
        std::vector<uint8_t> data(512, static_cast<uint8_t>(t + 1));
        std::string err = disk_sector_write(&driveA, t, 0, 0xC1, data);
        EXPECT_EQ("", err) << "Failed to write track " << t;
    }

    // Read them back and verify
    for (unsigned int t = 0; t < 5; t++) {
        std::string err;
        auto data = disk_sector_read(&driveA, t, 0, 0xC1, err);
        EXPECT_EQ("", err);
        ASSERT_EQ(512u, data.size());
        // All bytes in this sector should be (t+1)
        for (size_t i = 0; i < data.size(); i++) {
            EXPECT_EQ(static_cast<uint8_t>(t + 1), data[i])
                << "Mismatch at track " << t << " byte " << i;
        }
    }
}

// -----------------------------------------------
// Vendor format tests
// -----------------------------------------------

TEST_F(DiskSectorEditorTest, VendorFormatSectors) {
    // Format as vendor format
    dsk_eject(&driveA);
    std::string err = disk_format_drive('A', "vendor");
    ASSERT_EQ("", err);

    // Vendor format: 40 tracks, 1 side, 9 sectors (IDs 41..49, interleaved)
    auto sectors = disk_sector_info(&driveA, 0, 0, err);
    EXPECT_EQ("", err);
    ASSERT_EQ(9u, sectors.size());

    // Verify all expected vendor sector IDs are present
    std::set<uint8_t> found_ids;
    for (const auto& s : sectors) {
        found_ids.insert(s.R);
    }
    for (uint8_t id = 0x41; id <= 0x49; id++) {
        EXPECT_TRUE(found_ids.count(id))
            << "Missing vendor sector ID 0x" << std::hex
            << static_cast<unsigned>(id);
    }
}

// -----------------------------------------------
// NullDrive tests
// -----------------------------------------------

TEST(DiskSectorEditorNoDrive, ReadNullDrive) {
    std::string err;
    auto data = disk_sector_read(nullptr, 0, 0, 0xC1, err);
    EXPECT_NE("", err);
    EXPECT_TRUE(data.empty());
}

TEST(DiskSectorEditorNoDrive, WriteNullDrive) {
    std::vector<uint8_t> data(512, 0);
    std::string err = disk_sector_write(nullptr, 0, 0, 0xC1, data);
    EXPECT_NE("", err);
}

TEST(DiskSectorEditorNoDrive, InfoNullDrive) {
    std::string err;
    auto sectors = disk_sector_info(nullptr, 0, 0, err);
    EXPECT_NE("", err);
    EXPECT_TRUE(sectors.empty());
}

}  // namespace
