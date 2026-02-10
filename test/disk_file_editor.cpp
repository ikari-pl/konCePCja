#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>

#include "disk_file_editor.h"
#include "disk_format.h"
#include "koncepcja.h"
#include "slotshandler.h"

extern t_drive driveA;
extern t_drive driveB;

namespace {

class DiskFileEditorTest : public testing::Test {
 protected:
    void SetUp() override {
        dsk_eject(&driveA);
        // Format drive A as DATA format
        std::string err = disk_format_drive('A', "data");
        ASSERT_EQ("", err);
    }

    void TearDown() override {
        dsk_eject(&driveA);
        dsk_eject(&driveB);
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

// -----------------------------------------------
// disk_to_cpc_filename tests
// -----------------------------------------------

TEST(CpcFilename, SimpleConversion) {
    EXPECT_EQ("HELLO.BAS", disk_to_cpc_filename("hello.bas"));
}

TEST(CpcFilename, NoExtension) {
    EXPECT_EQ("README", disk_to_cpc_filename("readme"));
}

TEST(CpcFilename, TruncatesLongName) {
    EXPECT_EQ("LONGFILE.TXT", disk_to_cpc_filename("longfilename.txt"));
}

TEST(CpcFilename, TruncatesLongExt) {
    EXPECT_EQ("FILE.BAS", disk_to_cpc_filename("file.basic"));
}

TEST(CpcFilename, StripsDirPath) {
    EXPECT_EQ("TEST.BIN", disk_to_cpc_filename("/path/to/test.bin"));
}

TEST(CpcFilename, EmptyFilename) {
    EXPECT_EQ("", disk_to_cpc_filename(""));
    EXPECT_EQ("", disk_to_cpc_filename("/path/to/"));
}

// -----------------------------------------------
// AMSDOS header tests
// -----------------------------------------------

TEST(AmsdosHeader, CreateAndParse) {
    auto hdr = disk_make_amsdos_header("TEST.BIN", AmsdosFileType::BINARY,
                                        0x4000, 0x4000, 1234);
    ASSERT_EQ(128u, hdr.size());

    auto info = disk_parse_amsdos_header(hdr);
    EXPECT_TRUE(info.valid);
    EXPECT_EQ(AmsdosFileType::BINARY, info.type);
    EXPECT_EQ(0x4000u, info.load_addr);
    EXPECT_EQ(0x4000u, info.exec_addr);
    EXPECT_EQ(1234u, info.file_length);
}

TEST(AmsdosHeader, BasicType) {
    auto hdr = disk_make_amsdos_header("PROG.BAS", AmsdosFileType::BASIC,
                                        0x0170, 0x0000, 500);
    auto info = disk_parse_amsdos_header(hdr);
    EXPECT_TRUE(info.valid);
    EXPECT_EQ(AmsdosFileType::BASIC, info.type);
    EXPECT_EQ(0x0170u, info.load_addr);
    EXPECT_EQ(500u, info.file_length);
}

TEST(AmsdosHeader, InvalidChecksum) {
    auto hdr = disk_make_amsdos_header("TEST.BIN", AmsdosFileType::BINARY,
                                        0x4000, 0x4000, 100);
    hdr[0] = 0xFF; // Corrupt the header
    auto info = disk_parse_amsdos_header(hdr);
    EXPECT_FALSE(info.valid);
}

TEST(AmsdosHeader, TooShort) {
    std::vector<uint8_t> short_data(64, 0);
    auto info = disk_parse_amsdos_header(short_data);
    EXPECT_FALSE(info.valid);
}

// -----------------------------------------------
// disk_list_files tests
// -----------------------------------------------

TEST_F(DiskFileEditorTest, EmptyDiscHasNoFiles) {
    std::string err;
    auto files = disk_list_files(&driveA, err);
    EXPECT_EQ("", err);
    EXPECT_TRUE(files.empty());
}

TEST_F(DiskFileEditorTest, ListAfterWrite) {
    std::vector<uint8_t> data(256, 0x42);
    std::string err = disk_write_file(&driveA, "TEST.BIN", data, true, 0x4000, 0x4000);
    ASSERT_EQ("", err);

    auto files = disk_list_files(&driveA, err);
    EXPECT_EQ("", err);
    ASSERT_EQ(1u, files.size());
    EXPECT_EQ("TEST.BIN", files[0].display_name);
    // File on disc includes 128-byte AMSDOS header + 256 bytes data = 384 bytes
    // Rounded up to nearest 128-byte record: 384 / 128 = 3 records = 384 bytes
    EXPECT_EQ(384u, files[0].size_bytes);
}

TEST_F(DiskFileEditorTest, NoDiskReturnsError) {
    dsk_eject(&driveA);
    std::string err;
    auto files = disk_list_files(&driveA, err);
    EXPECT_NE("", err);
    EXPECT_TRUE(files.empty());
}

// -----------------------------------------------
// disk_write_file / disk_read_file round-trip
// -----------------------------------------------

TEST_F(DiskFileEditorTest, WriteAndReadBackRaw) {
    std::vector<uint8_t> data(512, 0xAB);
    std::string err = disk_write_file(&driveA, "DATA.BIN", data, false);
    ASSERT_EQ("", err);

    auto read_data = disk_read_file(&driveA, "DATA.BIN", err);
    ASSERT_EQ("", err);
    ASSERT_EQ(512u, read_data.size());
    EXPECT_EQ(data, read_data);
}

TEST_F(DiskFileEditorTest, WriteWithHeaderAndReadBack) {
    std::vector<uint8_t> data(100, 0x55);
    std::string err = disk_write_file(&driveA, "HELLO.BIN", data, true, 0x8000, 0x8000);
    ASSERT_EQ("", err);

    auto raw = disk_read_file(&driveA, "HELLO.BIN", err);
    ASSERT_EQ("", err);
    // Raw data includes AMSDOS header (128) + data rounded to records.
    // 228 bytes rounds up to 256 (2 * 128-byte records).
    ASSERT_EQ(256u, raw.size());

    // Parse the AMSDOS header
    auto info = disk_parse_amsdos_header(raw);
    EXPECT_TRUE(info.valid);
    EXPECT_EQ(AmsdosFileType::BINARY, info.type);
    EXPECT_EQ(0x8000u, info.load_addr);
    EXPECT_EQ(0x8000u, info.exec_addr);
    EXPECT_EQ(100u, info.file_length);

    // Actual data starts at offset 128; only first 100 bytes are real data
    std::vector<uint8_t> payload(raw.begin() + 128, raw.begin() + 128 + 100);
    EXPECT_EQ(data, payload);
}

TEST_F(DiskFileEditorTest, WriteMultipleFiles) {
    std::vector<uint8_t> d1(100, 0x11);
    std::vector<uint8_t> d2(200, 0x22);
    std::vector<uint8_t> d3(300, 0x33);

    ASSERT_EQ("", disk_write_file(&driveA, "FILE1.BIN", d1, false));
    ASSERT_EQ("", disk_write_file(&driveA, "FILE2.BIN", d2, false));
    ASSERT_EQ("", disk_write_file(&driveA, "FILE3.BIN", d3, false));

    std::string err;
    auto files = disk_list_files(&driveA, err);
    EXPECT_EQ("", err);
    EXPECT_EQ(3u, files.size());
}

TEST_F(DiskFileEditorTest, DuplicateFileReturnsError) {
    std::vector<uint8_t> data(100, 0);
    ASSERT_EQ("", disk_write_file(&driveA, "DUP.BIN", data, false));
    std::string err = disk_write_file(&driveA, "DUP.BIN", data, false);
    EXPECT_NE("", err);
}

// -----------------------------------------------
// Multi-extent files (>16K)
// -----------------------------------------------

TEST_F(DiskFileEditorTest, LargeFileMultiExtent) {
    // 20K file -> needs 2 extents (each extent covers up to 16K)
    std::vector<uint8_t> data(20 * 1024);
    for (size_t i = 0; i < data.size(); i++) {
        data[i] = static_cast<uint8_t>(i & 0xFF);
    }

    std::string err = disk_write_file(&driveA, "BIG.BIN", data, false);
    ASSERT_EQ("", err);

    auto read_data = disk_read_file(&driveA, "BIG.BIN", err);
    ASSERT_EQ("", err);
    ASSERT_EQ(data.size(), read_data.size());
    EXPECT_EQ(data, read_data);
}

// -----------------------------------------------
// disk_delete_file tests
// -----------------------------------------------

TEST_F(DiskFileEditorTest, DeleteFile) {
    std::vector<uint8_t> data(100, 0x42);
    ASSERT_EQ("", disk_write_file(&driveA, "DEL.BIN", data, false));

    std::string err;
    auto files = disk_list_files(&driveA, err);
    ASSERT_EQ(1u, files.size());

    err = disk_delete_file(&driveA, "DEL.BIN");
    EXPECT_EQ("", err);

    files = disk_list_files(&driveA, err);
    EXPECT_TRUE(files.empty());
}

TEST_F(DiskFileEditorTest, DeleteNonexistentFile) {
    std::string err = disk_delete_file(&driveA, "NOPE.BIN");
    EXPECT_NE("", err);
}

TEST_F(DiskFileEditorTest, DeleteMultiExtentFile) {
    std::vector<uint8_t> data(20 * 1024, 0xBB);
    ASSERT_EQ("", disk_write_file(&driveA, "BIG.BIN", data, false));

    std::string err = disk_delete_file(&driveA, "BIG.BIN");
    EXPECT_EQ("", err);

    auto files = disk_list_files(&driveA, err);
    EXPECT_TRUE(files.empty());
}

TEST_F(DiskFileEditorTest, DeleteThenReuse) {
    std::vector<uint8_t> d1(100, 0x11);
    ASSERT_EQ("", disk_write_file(&driveA, "OLD.BIN", d1, false));
    ASSERT_EQ("", disk_delete_file(&driveA, "OLD.BIN"));

    // Should be able to write a new file with the same name
    std::vector<uint8_t> d2(200, 0x22);
    ASSERT_EQ("", disk_write_file(&driveA, "OLD.BIN", d2, false));

    std::string err;
    auto read_data = disk_read_file(&driveA, "OLD.BIN", err);
    EXPECT_EQ("", err);
    // Size rounded to 128-byte records: 200 -> 256
    ASSERT_EQ(256u, read_data.size());
    // First 200 bytes should match d2
    std::vector<uint8_t> trimmed(read_data.begin(), read_data.begin() + 200);
    EXPECT_EQ(d2, trimmed);
}

// -----------------------------------------------
// Error cases
// -----------------------------------------------

TEST_F(DiskFileEditorTest, ReadNonexistentFile) {
    std::string err;
    auto data = disk_read_file(&driveA, "NOPE.BIN", err);
    EXPECT_NE("", err);
    EXPECT_TRUE(data.empty());
}

TEST_F(DiskFileEditorTest, WriteInvalidFilename) {
    std::vector<uint8_t> data(100, 0);
    std::string err = disk_write_file(&driveA, "TOOLONGNAME.TOOLONG", data, false);
    EXPECT_NE("", err);
}

TEST_F(DiskFileEditorTest, DiscFullError) {
    // DATA format has 178 usable blocks (blocks 2-179), each 1K
    // Fill the disc completely
    // 178 blocks = 178K = 182272 bytes
    // Write files until disc is full
    int file_num = 0;
    while (file_num < 20) {
        std::string fname = "F" + std::to_string(file_num) + ".BIN";
        // Pad to 8.3
        while (fname.size() < 5) fname = fname; // already fine
        std::vector<uint8_t> data(10 * 1024, 0); // 10K each
        std::string err = disk_write_file(&driveA, fname, data, false);
        if (!err.empty()) break;
        file_num++;
    }
    // At some point we should get a "disc full" error
    EXPECT_LT(file_num, 20);
}

// -----------------------------------------------
// Save to file and load back
// -----------------------------------------------

TEST_F(DiskFileEditorTest, SaveLoadRoundTrip) {
    // Write a file to driveA
    std::vector<uint8_t> data(256, 0x42);
    ASSERT_EQ("", disk_write_file(&driveA, "SAVE.BIN", data, true, 0x1000, 0x2000));

    // Save the DSK
    std::string path = make_temp_path("roundtrip.dsk");
    ASSERT_EQ(0, dsk_save(path, &driveA));

    // Load into driveB
    ASSERT_EQ(0, dsk_load(path, &driveB));

    // Read the file back from driveB
    std::string err;
    auto read_data = disk_read_file(&driveB, "SAVE.BIN", err);
    ASSERT_EQ("", err);
    ASSERT_EQ(128u + 256u, read_data.size());

    auto info = disk_parse_amsdos_header(read_data);
    EXPECT_TRUE(info.valid);
    EXPECT_EQ(0x1000u, info.load_addr);
    EXPECT_EQ(0x2000u, info.exec_addr);
    EXPECT_EQ(256u, info.file_length);
}

// -----------------------------------------------
// R/O and SYS flags
// -----------------------------------------------

TEST_F(DiskFileEditorTest, ReadOnlyAndSystemFlags) {
    // Write a file, then manually set the R/O and SYS flags in the directory
    std::vector<uint8_t> data(100, 0);
    ASSERT_EQ("", disk_write_file(&driveA, "FLAGS.BIN", data, false));

    // First check default state: neither R/O nor SYS set
    std::string err;
    auto files = disk_list_files(&driveA, err);
    ASSERT_EQ(1u, files.size());
    EXPECT_FALSE(files[0].read_only);
    EXPECT_FALSE(files[0].system);

    // Now manually set R/O (bit 7 of byte 9) and SYS (bit 7 of byte 10)
    // in the directory entry on disc.
    // Directory is in block 0 (sectors C1, C2 on track 0).
    // Read block 0 (first 1K of directory)
    t_track& trk = driveA.track[0][0];
    uint8_t* dir_sector = nullptr;
    for (unsigned int s = 0; s < trk.sectors; s++) {
        if (trk.sector[s].CHRN[2] == 0xC1) {
            dir_sector = trk.sector[s].getDataForWrite();
            break;
        }
    }
    ASSERT_NE(nullptr, dir_sector);

    // Find the FLAGS.BIN entry in the directory sector
    // Each directory entry is 32 bytes. Scan for user=0, name matches.
    uint8_t* found_entry = nullptr;
    for (int i = 0; i < 16; i++) { // 512 / 32 = 16 entries per sector
        uint8_t* ent = dir_sector + i * 32;
        if (ent[0] == 0xE5) continue;
        if (ent[0] > 15) continue;
        // Check name: "FLAGS   BIN"
        if ((ent[1] & 0x7F) == 'F' && (ent[2] & 0x7F) == 'L' &&
            (ent[3] & 0x7F) == 'A' && (ent[4] & 0x7F) == 'G' &&
            (ent[5] & 0x7F) == 'S') {
            found_entry = ent;
            break;
        }
    }
    ASSERT_NE(nullptr, found_entry);

    // Set R/O bit (bit 7 of extension byte 0, which is entry byte 9)
    found_entry[9] |= 0x80;
    // Set SYS bit (bit 7 of extension byte 1, which is entry byte 10)
    found_entry[10] |= 0x80;

    // Re-list and verify flags are now true
    files = disk_list_files(&driveA, err);
    ASSERT_EQ("", err);
    ASSERT_EQ(1u, files.size());
    EXPECT_TRUE(files[0].read_only);
    EXPECT_TRUE(files[0].system);
}

}  // namespace
