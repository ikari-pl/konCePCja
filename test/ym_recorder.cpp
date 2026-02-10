#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <cstring>

#include "ym_recorder.h"

namespace fs = std::filesystem;

class YmRecorderTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() / "ym_recorder_test";
        fs::create_directories(tmp_dir_);
    }

    void TearDown() override {
        recorder_.stop();
        fs::remove_all(tmp_dir_);
    }

    std::string tmp_path(const std::string& name) {
        return (tmp_dir_ / name).string();
    }

    std::vector<uint8_t> read_file(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                    std::istreambuf_iterator<char>());
    }

    // Read a big-endian uint16 from a byte buffer at offset
    uint16_t read_be_u16(const std::vector<uint8_t>& buf, size_t offset) {
        return (static_cast<uint16_t>(buf[offset]) << 8) |
               static_cast<uint16_t>(buf[offset + 1]);
    }

    // Read a big-endian uint32 from a byte buffer at offset
    uint32_t read_be_u32(const std::vector<uint8_t>& buf, size_t offset) {
        return (static_cast<uint32_t>(buf[offset]) << 24) |
               (static_cast<uint32_t>(buf[offset + 1]) << 16) |
               (static_cast<uint32_t>(buf[offset + 2]) << 8) |
               static_cast<uint32_t>(buf[offset + 3]);
    }

    // Check that bytes at offset match a string
    bool bytes_match(const std::vector<uint8_t>& buf, size_t offset, const char* str, size_t len) {
        if (offset + len > buf.size()) return false;
        return memcmp(buf.data() + offset, str, len) == 0;
    }

    YmRecorder recorder_;
    fs::path tmp_dir_;
};

TEST_F(YmRecorderTest, StartAndStopLifecycle) {
    EXPECT_FALSE(recorder_.is_recording());
    EXPECT_EQ(recorder_.frame_count(), 0u);
    EXPECT_TRUE(recorder_.current_path().empty());

    std::string path = tmp_path("lifecycle.ym");
    auto err = recorder_.start(path);
    ASSERT_TRUE(err.empty()) << "start failed: " << err;
    EXPECT_TRUE(recorder_.is_recording());
    EXPECT_EQ(recorder_.current_path(), path);
    EXPECT_EQ(recorder_.frame_count(), 0u);

    uint32_t frames = recorder_.stop();
    EXPECT_EQ(frames, 0u);
    EXPECT_FALSE(recorder_.is_recording());
    EXPECT_TRUE(recorder_.current_path().empty());
}

TEST_F(YmRecorderTest, CaptureFrameStoresRegisters) {
    std::string path = tmp_path("capture.ym");
    auto err = recorder_.start(path);
    ASSERT_TRUE(err.empty());

    uint8_t regs1[14] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
    recorder_.capture_frame(regs1);
    EXPECT_EQ(recorder_.frame_count(), 1u);

    uint8_t regs2[14] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
                         0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD};
    recorder_.capture_frame(regs2);
    EXPECT_EQ(recorder_.frame_count(), 2u);

    uint32_t frames = recorder_.stop();
    EXPECT_EQ(frames, 2u);
}

TEST_F(YmRecorderTest, CaptureFrameIgnoredWhenNotRecording) {
    uint8_t regs[14] = {};
    recorder_.capture_frame(regs);
    EXPECT_EQ(recorder_.frame_count(), 0u);
}

TEST_F(YmRecorderTest, Ym5FileHasCorrectMagicAndCheckString) {
    std::string path = tmp_path("magic.ym");
    recorder_.start(path);
    recorder_.stop();

    auto data = read_file(path);
    ASSERT_GE(data.size(), 12u);

    // "YM5!" at offset 0
    EXPECT_TRUE(bytes_match(data, 0, "YM5!", 4));

    // "LeOnArD!" at offset 4
    EXPECT_TRUE(bytes_match(data, 4, "LeOnArD!", 8));
}

TEST_F(YmRecorderTest, Ym5FileHeaderFields) {
    std::string path = tmp_path("header.ym");
    recorder_.start(path);

    // Capture 3 frames
    uint8_t regs[14] = {};
    recorder_.capture_frame(regs);
    recorder_.capture_frame(regs);
    recorder_.capture_frame(regs);
    recorder_.stop();

    auto data = read_file(path);
    // Header is at least: 4 + 8 + 4 + 4 + 2 + 4 + 2 + 4 + 2 = 34 bytes
    // Plus 3 null-terminated strings + data + "End!"
    ASSERT_GE(data.size(), 34u);

    size_t off = 12; // after magic + check string

    // Number of frames = 3
    EXPECT_EQ(read_be_u32(data, off), 3u);
    off += 4;

    // Song attributes = 1 (interleaved)
    EXPECT_EQ(read_be_u32(data, off), 1u);
    off += 4;

    // Number of digidrums = 0
    EXPECT_EQ(read_be_u16(data, off), 0u);
    off += 2;

    // Master clock = 1000000
    EXPECT_EQ(read_be_u32(data, off), 1000000u);
    off += 4;

    // Player frequency = 50
    EXPECT_EQ(read_be_u16(data, off), 50u);
    off += 2;

    // VBL loop frame = 0
    EXPECT_EQ(read_be_u32(data, off), 0u);
    off += 4;

    // Additional data size = 0
    EXPECT_EQ(read_be_u16(data, off), 0u);
}

TEST_F(YmRecorderTest, Ym5FileInterleavedDataAndEndMarker) {
    std::string path = tmp_path("interleaved.ym");
    recorder_.start(path);

    // Frame 0: registers 0-13 = 0x10, 0x11, ..., 0x1D
    uint8_t regs0[14];
    for (int i = 0; i < 14; i++) regs0[i] = static_cast<uint8_t>(0x10 + i);
    recorder_.capture_frame(regs0);

    // Frame 1: registers 0-13 = 0x20, 0x21, ..., 0x2D
    uint8_t regs1[14];
    for (int i = 0; i < 14; i++) regs1[i] = static_cast<uint8_t>(0x20 + i);
    recorder_.capture_frame(regs1);

    recorder_.stop();

    auto data = read_file(path);

    // Find the start of register data.
    // Header: 4(YM5!) + 8(LeOnArD!) + 4(frames) + 4(attr) + 2(digidrums) +
    //         4(clock) + 2(freq) + 4(loop) + 2(additional) = 34
    // Then: song name "konCePCja recording\0" = 20 bytes
    //       author "\0" = 1 byte
    //       comment "\0" = 1 byte
    // Total header = 34 + 20 + 1 + 1 = 56
    size_t data_start = 56;
    size_t num_frames = 2;

    // Verify interleaved format: for each register, 2 bytes (one per frame)
    // Register 0: frame0=0x10, frame1=0x20
    EXPECT_EQ(data[data_start + 0 * num_frames + 0], 0x10);
    EXPECT_EQ(data[data_start + 0 * num_frames + 1], 0x20);

    // Register 1: frame0=0x11, frame1=0x21
    EXPECT_EQ(data[data_start + 1 * num_frames + 0], 0x11);
    EXPECT_EQ(data[data_start + 1 * num_frames + 1], 0x21);

    // Register 13: frame0=0x1D, frame1=0x2D
    EXPECT_EQ(data[data_start + 13 * num_frames + 0], 0x1D);
    EXPECT_EQ(data[data_start + 13 * num_frames + 1], 0x2D);

    // End marker: "End!" at the very end
    size_t end_offset = data.size() - 4;
    EXPECT_TRUE(bytes_match(data, end_offset, "End!", 4));
}

TEST_F(YmRecorderTest, Ym5FileEndMarkerWithZeroFrames) {
    std::string path = tmp_path("empty.ym");
    recorder_.start(path);
    recorder_.stop();

    auto data = read_file(path);
    // Should still have "End!" at the end
    ASSERT_GE(data.size(), 4u);
    size_t end_offset = data.size() - 4;
    EXPECT_TRUE(bytes_match(data, end_offset, "End!", 4));
}

TEST_F(YmRecorderTest, DoubleStartReturnsError) {
    std::string path1 = tmp_path("double1.ym");
    std::string path2 = tmp_path("double2.ym");

    auto err1 = recorder_.start(path1);
    ASSERT_TRUE(err1.empty());

    auto err2 = recorder_.start(path2);
    EXPECT_FALSE(err2.empty());
    EXPECT_NE(err2.find("already recording"), std::string::npos);

    recorder_.stop();
}

TEST_F(YmRecorderTest, StopWithoutStartReturnsZero) {
    EXPECT_FALSE(recorder_.is_recording());
    uint32_t frames = recorder_.stop();
    EXPECT_EQ(frames, 0u);
}

TEST_F(YmRecorderTest, StartWithInvalidPathReturnsError) {
    std::string bad_path = "/nonexistent_dir_xyz/test.ym";
    auto err = recorder_.start(bad_path);
    EXPECT_FALSE(err.empty());
    EXPECT_FALSE(recorder_.is_recording());
}

TEST_F(YmRecorderTest, DestructorStopsRecording) {
    std::string path = tmp_path("destructor.ym");
    {
        YmRecorder local_rec;
        auto err = local_rec.start(path);
        ASSERT_TRUE(err.empty());
        uint8_t regs[14] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
        local_rec.capture_frame(regs);
        // Destructor should call stop() and write the file
    }

    auto data = read_file(path);
    ASSERT_GE(data.size(), 4u);
    // Verify it has the magic
    EXPECT_TRUE(bytes_match(data, 0, "YM5!", 4));
    // And the end marker
    size_t end_offset = data.size() - 4;
    EXPECT_TRUE(bytes_match(data, end_offset, "End!", 4));
}

TEST_F(YmRecorderTest, SongNameInHeader) {
    std::string path = tmp_path("songname.ym");
    recorder_.start(path);
    recorder_.stop();

    auto data = read_file(path);
    // Song name starts at offset 34 (after fixed header fields)
    // Should be "konCePCja recording\0"
    const char* expected = "konCePCja recording";
    size_t name_offset = 34;
    ASSERT_GT(data.size(), name_offset + strlen(expected));
    EXPECT_TRUE(bytes_match(data, name_offset, expected, strlen(expected)));
    EXPECT_EQ(data[name_offset + strlen(expected)], 0); // null terminator
}
