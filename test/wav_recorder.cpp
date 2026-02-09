#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <cstring>

#include "wav_recorder.h"

namespace fs = std::filesystem;

class WavRecorderTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() / "wav_recorder_test";
        fs::create_directories(tmp_dir_);
    }

    void TearDown() override {
        // Stop recorder if still running
        recorder_.stop();
        fs::remove_all(tmp_dir_);
    }

    std::string tmp_path(const std::string& name) {
        return (tmp_dir_ / name).string();
    }

    // Read the full contents of a file into a vector
    std::vector<uint8_t> read_file(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                    std::istreambuf_iterator<char>());
    }

    // Read a little-endian uint16 from a byte buffer at offset
    uint16_t read_u16(const std::vector<uint8_t>& buf, size_t offset) {
        return static_cast<uint16_t>(buf[offset]) |
               (static_cast<uint16_t>(buf[offset + 1]) << 8);
    }

    // Read a little-endian uint32 from a byte buffer at offset
    uint32_t read_u32(const std::vector<uint8_t>& buf, size_t offset) {
        return static_cast<uint32_t>(buf[offset]) |
               (static_cast<uint32_t>(buf[offset + 1]) << 8) |
               (static_cast<uint32_t>(buf[offset + 2]) << 16) |
               (static_cast<uint32_t>(buf[offset + 3]) << 24);
    }

    WavRecorder recorder_;
    fs::path tmp_dir_;
};

TEST_F(WavRecorderTest, StartAndStopCreatesValidWavFile) {
    std::string path = tmp_path("test.wav");
    auto err = recorder_.start(path, 44100, 16, 2);
    ASSERT_TRUE(err.empty()) << "start failed: " << err;
    ASSERT_TRUE(recorder_.is_recording());

    uint32_t bytes = recorder_.stop();
    EXPECT_EQ(bytes, 0u);  // No samples written
    EXPECT_FALSE(recorder_.is_recording());

    // Verify the file exists and has a valid header (44 bytes minimum)
    auto data = read_file(path);
    ASSERT_GE(data.size(), 44u);

    // Check RIFF header
    EXPECT_EQ(data[0], 'R');
    EXPECT_EQ(data[1], 'I');
    EXPECT_EQ(data[2], 'F');
    EXPECT_EQ(data[3], 'F');

    // Check WAVE format
    EXPECT_EQ(data[8], 'W');
    EXPECT_EQ(data[9], 'A');
    EXPECT_EQ(data[10], 'V');
    EXPECT_EQ(data[11], 'E');

    // Check fmt sub-chunk
    EXPECT_EQ(data[12], 'f');
    EXPECT_EQ(data[13], 'm');
    EXPECT_EQ(data[14], 't');
    EXPECT_EQ(data[15], ' ');
}

TEST_F(WavRecorderTest, HeaderFieldsAreCorrect) {
    std::string path = tmp_path("header_test.wav");
    auto err = recorder_.start(path, 48000, 16, 2);
    ASSERT_TRUE(err.empty());
    recorder_.stop();

    auto data = read_file(path);
    ASSERT_GE(data.size(), 44u);

    // fmt chunk size = 16
    EXPECT_EQ(read_u32(data, 16), 16u);
    // Audio format = 1 (PCM)
    EXPECT_EQ(read_u16(data, 20), 1u);
    // Channels = 2
    EXPECT_EQ(read_u16(data, 22), 2u);
    // Sample rate = 48000
    EXPECT_EQ(read_u32(data, 24), 48000u);
    // Byte rate = 48000 * 2 * 2 = 192000
    EXPECT_EQ(read_u32(data, 28), 192000u);
    // Block align = 2 * 2 = 4
    EXPECT_EQ(read_u16(data, 32), 4u);
    // Bits per sample = 16
    EXPECT_EQ(read_u16(data, 34), 16u);

    // data sub-chunk marker
    EXPECT_EQ(data[36], 'd');
    EXPECT_EQ(data[37], 'a');
    EXPECT_EQ(data[38], 't');
    EXPECT_EQ(data[39], 'a');

    // data size = 0 (no samples)
    EXPECT_EQ(read_u32(data, 40), 0u);

    // RIFF size = 36 (44 - 8 + 0 data bytes)
    EXPECT_EQ(read_u32(data, 4), 36u);
}

TEST_F(WavRecorderTest, WriteSamplesUpdatesCount) {
    std::string path = tmp_path("samples.wav");
    auto err = recorder_.start(path, 44100, 16, 1);
    ASSERT_TRUE(err.empty());

    // Write 100 bytes of sample data
    std::vector<uint8_t> samples(100, 0x42);
    recorder_.write_samples(samples.data(), static_cast<uint32_t>(samples.size()));

    EXPECT_EQ(recorder_.bytes_written(), 100u);

    // Write more
    recorder_.write_samples(samples.data(), 50);
    EXPECT_EQ(recorder_.bytes_written(), 150u);

    uint32_t total = recorder_.stop();
    EXPECT_EQ(total, 150u);
}

TEST_F(WavRecorderTest, WriteSamplesProducesCorrectFile) {
    std::string path = tmp_path("pcm_data.wav");
    auto err = recorder_.start(path, 22050, 8, 1);
    ASSERT_TRUE(err.empty());

    // Write known data pattern
    uint8_t pattern[] = {0x80, 0x90, 0xA0, 0xB0, 0xC0};
    recorder_.write_samples(pattern, 5);
    recorder_.stop();

    auto data = read_file(path);
    ASSERT_EQ(data.size(), 44u + 5u);

    // Verify PCM data after header
    EXPECT_EQ(data[44], 0x80);
    EXPECT_EQ(data[45], 0x90);
    EXPECT_EQ(data[46], 0xA0);
    EXPECT_EQ(data[47], 0xB0);
    EXPECT_EQ(data[48], 0xC0);

    // Verify header sizes are patched correctly
    EXPECT_EQ(read_u32(data, 40), 5u);        // data chunk size
    EXPECT_EQ(read_u32(data, 4), 36u + 5u);   // RIFF size

    // Verify audio format fields
    EXPECT_EQ(read_u32(data, 24), 22050u);     // sample rate
    EXPECT_EQ(read_u32(data, 28), 22050u);     // byte rate (22050 * 1 * 1)
    EXPECT_EQ(read_u16(data, 32), 1u);         // block align
    EXPECT_EQ(read_u16(data, 34), 8u);         // bits per sample
    EXPECT_EQ(read_u16(data, 22), 1u);         // channels
}

TEST_F(WavRecorderTest, DoubleStartReturnsError) {
    std::string path1 = tmp_path("double1.wav");
    std::string path2 = tmp_path("double2.wav");

    auto err1 = recorder_.start(path1, 44100, 16, 2);
    ASSERT_TRUE(err1.empty());

    auto err2 = recorder_.start(path2, 44100, 16, 2);
    EXPECT_FALSE(err2.empty());
    EXPECT_NE(err2.find("already recording"), std::string::npos);

    recorder_.stop();
}

TEST_F(WavRecorderTest, StopWhenNotRecordingReturnsZero) {
    EXPECT_FALSE(recorder_.is_recording());
    uint32_t bytes = recorder_.stop();
    EXPECT_EQ(bytes, 0u);
}

TEST_F(WavRecorderTest, StatusReporting) {
    EXPECT_FALSE(recorder_.is_recording());
    EXPECT_TRUE(recorder_.current_path().empty());
    EXPECT_EQ(recorder_.bytes_written(), 0u);

    std::string path = tmp_path("status.wav");
    recorder_.start(path, 44100, 16, 2);

    EXPECT_TRUE(recorder_.is_recording());
    EXPECT_EQ(recorder_.current_path(), path);

    uint8_t data[10] = {};
    recorder_.write_samples(data, 10);
    EXPECT_EQ(recorder_.bytes_written(), 10u);

    recorder_.stop();

    EXPECT_FALSE(recorder_.is_recording());
    EXPECT_TRUE(recorder_.current_path().empty());
}

TEST_F(WavRecorderTest, StartWithInvalidPathReturnsError) {
    std::string bad_path = "/nonexistent_dir_xyz/test.wav";
    auto err = recorder_.start(bad_path, 44100, 16, 2);
    EXPECT_FALSE(err.empty());
    EXPECT_FALSE(recorder_.is_recording());
}

TEST_F(WavRecorderTest, MonoStereoConfigurations) {
    // Test mono 8-bit
    {
        std::string path = tmp_path("mono8.wav");
        auto err = recorder_.start(path, 11025, 8, 1);
        ASSERT_TRUE(err.empty());
        recorder_.stop();
        auto data = read_file(path);
        ASSERT_GE(data.size(), 44u);
        EXPECT_EQ(read_u16(data, 22), 1u);          // channels
        EXPECT_EQ(read_u16(data, 34), 8u);           // bits
        EXPECT_EQ(read_u32(data, 24), 11025u);       // sample rate
        EXPECT_EQ(read_u32(data, 28), 11025u);       // byte rate
        EXPECT_EQ(read_u16(data, 32), 1u);            // block align
    }

    // Test stereo 16-bit
    {
        std::string path = tmp_path("stereo16.wav");
        auto err = recorder_.start(path, 96000, 16, 2);
        ASSERT_TRUE(err.empty());
        recorder_.stop();
        auto data = read_file(path);
        ASSERT_GE(data.size(), 44u);
        EXPECT_EQ(read_u16(data, 22), 2u);            // channels
        EXPECT_EQ(read_u16(data, 34), 16u);            // bits
        EXPECT_EQ(read_u32(data, 24), 96000u);         // sample rate
        EXPECT_EQ(read_u32(data, 28), 96000u * 2 * 2); // byte rate
        EXPECT_EQ(read_u16(data, 32), 4u);              // block align
    }
}

TEST_F(WavRecorderTest, WriteZeroLengthIsNoop) {
    std::string path = tmp_path("zero.wav");
    recorder_.start(path, 44100, 16, 2);

    uint8_t dummy = 0;
    recorder_.write_samples(&dummy, 0);
    EXPECT_EQ(recorder_.bytes_written(), 0u);

    recorder_.write_samples(nullptr, 0);
    EXPECT_EQ(recorder_.bytes_written(), 0u);

    recorder_.stop();
}

TEST_F(WavRecorderTest, DestructorStopsRecording) {
    std::string path = tmp_path("destructor.wav");
    {
        WavRecorder local_rec;
        auto err = local_rec.start(path, 44100, 16, 1);
        ASSERT_TRUE(err.empty());
        uint8_t data[20] = {};
        local_rec.write_samples(data, 20);
        // Destructor should call stop() and finalize the file
    }

    // Verify file is valid after destructor
    auto data = read_file(path);
    ASSERT_GE(data.size(), 44u + 20u);
    EXPECT_EQ(read_u32(data, 40), 20u);  // data size
}
