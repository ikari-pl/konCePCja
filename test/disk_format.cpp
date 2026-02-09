#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>

#include "disk_format.h"
#include "koncepcja.h"
#include "slotshandler.h"

extern t_drive driveA;
extern t_drive driveB;

namespace {

// -----------------------------------------------
// disk_format_index_by_name tests
// -----------------------------------------------

TEST(DiskFormatIndex, DataReturnsZero) {
    EXPECT_EQ(0, disk_format_index_by_name("data"));
}

TEST(DiskFormatIndex, VendorReturnsOne) {
    EXPECT_EQ(1, disk_format_index_by_name("vendor"));
}

TEST(DiskFormatIndex, CaseInsensitive) {
    EXPECT_EQ(0, disk_format_index_by_name("DATA"));
    EXPECT_EQ(0, disk_format_index_by_name("Data"));
    EXPECT_EQ(1, disk_format_index_by_name("VENDOR"));
    EXPECT_EQ(1, disk_format_index_by_name("Vendor"));
}

TEST(DiskFormatIndex, UnknownReturnsNegOne) {
    EXPECT_EQ(-1, disk_format_index_by_name(""));
    EXPECT_EQ(-1, disk_format_index_by_name("nonexistent"));
    EXPECT_EQ(-1, disk_format_index_by_name("ibm"));
}

TEST(DiskFormatIndex, MatchByLabelPrefix) {
    // "178K Data Format" starts with "178k" (case-insensitive)
    EXPECT_EQ(0, disk_format_index_by_name("178K"));
    // "169K Vendor Format" starts with "169k"
    EXPECT_EQ(1, disk_format_index_by_name("169K"));
}

// -----------------------------------------------
// disk_format_names tests
// -----------------------------------------------

TEST(DiskFormatNames, ContainsBuiltinFormats) {
    auto names = disk_format_names();
    ASSERT_GE(names.size(), 2u);
    EXPECT_EQ("data", names[0]);
    EXPECT_EQ("vendor", names[1]);
}

// -----------------------------------------------
// disk_create_new tests
// -----------------------------------------------

class DiskCreateNewTest : public testing::Test {
 protected:
    void TearDown() override {
        // Clean up any created files.
        for (const auto& f : created_files) {
            std::filesystem::remove(f);
        }
    }

    std::string make_temp_path(const std::string& name) {
        auto p = std::filesystem::temp_directory_path() / name;
        created_files.push_back(p);
        return p.string();
    }

    std::vector<std::filesystem::path> created_files;
};

TEST_F(DiskCreateNewTest, CreatesDataFormatDsk) {
    std::string path = make_temp_path("test_data.dsk");
    std::string err = disk_create_new(path, "data");
    EXPECT_EQ("", err);
    EXPECT_TRUE(std::filesystem::exists(path));
    // A 40-track, 1-side, 9-sector, 512-byte DSK should be roughly:
    // header (256) + 40 * (track_header(256) + 9*512)
    // = 256 + 40 * (256 + 4608) = 256 + 40 * 4864 = 256 + 194560 = 194816
    auto size = std::filesystem::file_size(path);
    EXPECT_GT(size, 100000u);  // Sanity: at least 100KB
    EXPECT_LT(size, 300000u);  // Sanity: under 300KB
}

TEST_F(DiskCreateNewTest, CreatesVendorFormatDsk) {
    std::string path = make_temp_path("test_vendor.dsk");
    std::string err = disk_create_new(path, "vendor");
    EXPECT_EQ("", err);
    EXPECT_TRUE(std::filesystem::exists(path));
    auto size = std::filesystem::file_size(path);
    EXPECT_GT(size, 100000u);
}

TEST_F(DiskCreateNewTest, DefaultFormatIsData) {
    std::string path = make_temp_path("test_default.dsk");
    std::string err = disk_create_new(path);
    EXPECT_EQ("", err);
    EXPECT_TRUE(std::filesystem::exists(path));
}

TEST_F(DiskCreateNewTest, UnknownFormatReturnsError) {
    std::string path = make_temp_path("test_bad.dsk");
    std::string err = disk_create_new(path, "nonexistent");
    EXPECT_NE("", err);
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(DiskCreateNewTest, InvalidPathReturnsError) {
    std::string err = disk_create_new("/nonexistent_dir/subdir/test.dsk", "data");
    EXPECT_NE("", err);
}

TEST_F(DiskCreateNewTest, DskHeaderIsValid) {
    constexpr size_t kDskSignatureSize = 34;
    constexpr size_t kCreatorOffset = 34;
    constexpr size_t kCreatorSize = 14;
    constexpr size_t kTracksOffset = 48;
    constexpr size_t kSidesOffset = 49;

    std::string path = make_temp_path("test_header.dsk");
    std::string err = disk_create_new(path, "data");
    ASSERT_EQ("", err);

    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in.is_open());

    char header[256];
    in.read(header, 256);
    ASSERT_TRUE(in.good());

    // Check EXTENDED CPC DSK signature.
    EXPECT_EQ(0, std::memcmp(header, "EXTENDED CPC DSK File\r\nDisk-Info\r\n", kDskSignatureSize));

    // Check creator string contains "konCePCja".
    std::string creator(header + kCreatorOffset, kCreatorSize);
    EXPECT_NE(std::string::npos, creator.find("konCePCja"));

    // Check tracks = 40, sides = 1.
    EXPECT_EQ(40, static_cast<unsigned char>(header[kTracksOffset]));
    EXPECT_EQ(1, static_cast<unsigned char>(header[kSidesOffset]));
}

// -----------------------------------------------
// disk_format_drive tests
// -----------------------------------------------

class DiskFormatDriveTest : public testing::Test {
 protected:
    void SetUp() override {
        // Ensure drives are clean.
        dsk_eject(&driveA);
        dsk_eject(&driveB);
    }

    void TearDown() override {
        dsk_eject(&driveA);
        dsk_eject(&driveB);
    }
};

TEST_F(DiskFormatDriveTest, FormatDriveAData) {
    std::string err = disk_format_drive('A', "data");
    EXPECT_EQ("", err);
    EXPECT_EQ(40u, driveA.tracks);
    EXPECT_EQ(0u, driveA.sides);  // 0-based: 0 means 1 side
    EXPECT_TRUE(driveA.altered);
}

TEST_F(DiskFormatDriveTest, FormatDriveBVendor) {
    std::string err = disk_format_drive('B', "vendor");
    EXPECT_EQ("", err);
    EXPECT_EQ(40u, driveB.tracks);
    EXPECT_EQ(0u, driveB.sides);
    EXPECT_TRUE(driveB.altered);
}

TEST_F(DiskFormatDriveTest, LowercaseDriveLetter) {
    std::string err = disk_format_drive('a', "data");
    EXPECT_EQ("", err);
    EXPECT_EQ(40u, driveA.tracks);
}

TEST_F(DiskFormatDriveTest, InvalidDriveLetterReturnsError) {
    std::string err = disk_format_drive('C', "data");
    EXPECT_NE("", err);
}

TEST_F(DiskFormatDriveTest, InvalidFormatReturnsError) {
    std::string err = disk_format_drive('A', "nonexistent");
    EXPECT_NE("", err);
}

TEST_F(DiskFormatDriveTest, ReformatClearsOldData) {
    // Format as data first.
    std::string err = disk_format_drive('A', "data");
    ASSERT_EQ("", err);
    EXPECT_EQ(40u, driveA.tracks);

    // Re-format as vendor.
    err = disk_format_drive('A', "vendor");
    EXPECT_EQ("", err);
    EXPECT_EQ(40u, driveA.tracks);
    // Vendor format uses sector IDs starting at 0x41 (side 0, sector 0).
    EXPECT_EQ(0x41, driveA.track[0][0].sector[0].CHRN[2]);
}

}  // namespace
