#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <cstring>

#include "avi_recorder.h"

namespace fs = std::filesystem;

class AviRecorderTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() / "avi_recorder_test";
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

    uint16_t read_u16(const std::vector<uint8_t>& buf, size_t offset) {
        return static_cast<uint16_t>(buf[offset]) |
               (static_cast<uint16_t>(buf[offset + 1]) << 8);
    }

    uint32_t read_u32(const std::vector<uint8_t>& buf, size_t offset) {
        return static_cast<uint32_t>(buf[offset]) |
               (static_cast<uint32_t>(buf[offset + 1]) << 8) |
               (static_cast<uint32_t>(buf[offset + 2]) << 16) |
               (static_cast<uint32_t>(buf[offset + 3]) << 24);
    }

    bool bytes_match(const std::vector<uint8_t>& buf, size_t offset,
                     const char* str, size_t len) {
        if (offset + len > buf.size()) return false;
        return memcmp(buf.data() + offset, str, len) == 0;
    }

    // Create a simple RGBA test frame (solid color)
    std::vector<uint8_t> make_test_frame(int width, int height,
                                          uint8_t r, uint8_t g, uint8_t b) {
        std::vector<uint8_t> frame(static_cast<size_t>(width * height * 4));
        for (int i = 0; i < width * height; i++) {
            frame[static_cast<size_t>(i * 4 + 0)] = r;
            frame[static_cast<size_t>(i * 4 + 1)] = g;
            frame[static_cast<size_t>(i * 4 + 2)] = b;
            frame[static_cast<size_t>(i * 4 + 3)] = 255;
        }
        return frame;
    }

    AviRecorder recorder_;
    fs::path tmp_dir_;
};

// === Tests that work regardless of HAS_LIBJPEG ===

#ifndef HAS_LIBJPEG

TEST_F(AviRecorderTest, StartReturnsErrorWithoutLibjpeg) {
    std::string path = tmp_path("test.avi");
    auto err = recorder_.start(path);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("libjpeg"), std::string::npos);
    EXPECT_FALSE(recorder_.is_recording());
}

#else  // HAS_LIBJPEG

TEST_F(AviRecorderTest, StartAndStopLifecycle) {
    EXPECT_FALSE(recorder_.is_recording());
    EXPECT_EQ(recorder_.frame_count(), 0u);
    EXPECT_TRUE(recorder_.current_path().empty());

    std::string path = tmp_path("lifecycle.avi");
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

TEST_F(AviRecorderTest, DoubleStartReturnsError) {
    std::string path1 = tmp_path("double1.avi");
    std::string path2 = tmp_path("double2.avi");

    auto err1 = recorder_.start(path1);
    ASSERT_TRUE(err1.empty());

    auto err2 = recorder_.start(path2);
    EXPECT_FALSE(err2.empty());
    EXPECT_NE(err2.find("already recording"), std::string::npos);

    recorder_.stop();
}

TEST_F(AviRecorderTest, StopWithoutStartReturnsZero) {
    EXPECT_FALSE(recorder_.is_recording());
    uint32_t frames = recorder_.stop();
    EXPECT_EQ(frames, 0u);
}

TEST_F(AviRecorderTest, StartWithInvalidPathReturnsError) {
    std::string bad_path = "/nonexistent_dir_xyz/test.avi";
    auto err = recorder_.start(bad_path);
    EXPECT_FALSE(err.empty());
    EXPECT_FALSE(recorder_.is_recording());
}

TEST_F(AviRecorderTest, RiffHeaderMagic) {
    std::string path = tmp_path("magic.avi");
    auto err = recorder_.start(path);
    ASSERT_TRUE(err.empty());
    recorder_.stop();

    auto data = read_file(path);
    ASSERT_GE(data.size(), 12u);

    // "RIFF" at offset 0
    EXPECT_TRUE(bytes_match(data, 0, "RIFF", 4));
    // "AVI " at offset 8
    EXPECT_TRUE(bytes_match(data, 8, "AVI ", 4));
}

TEST_F(AviRecorderTest, HasValidStreamHeaders) {
    std::string path = tmp_path("streams.avi");
    auto err = recorder_.start(path);
    ASSERT_TRUE(err.empty());
    recorder_.stop();

    auto data = read_file(path);

    // Search for "vids" and "MJPG" in the file
    bool found_vids = false;
    bool found_mjpg = false;
    bool found_auds = false;
    for (size_t i = 0; i + 4 <= data.size(); i++) {
        if (memcmp(data.data() + i, "vids", 4) == 0) found_vids = true;
        if (memcmp(data.data() + i, "MJPG", 4) == 0) found_mjpg = true;
        if (memcmp(data.data() + i, "auds", 4) == 0) found_auds = true;
    }
    EXPECT_TRUE(found_vids) << "Video stream header 'vids' not found";
    EXPECT_TRUE(found_mjpg) << "MJPG codec identifier not found";
    EXPECT_TRUE(found_auds) << "Audio stream header 'auds' not found";
}

TEST_F(AviRecorderTest, AudioStreamFormatIsPCM) {
    std::string path = tmp_path("pcm.avi");
    auto err = recorder_.start(path, 85, 44100, 2, 16);
    ASSERT_TRUE(err.empty());
    recorder_.stop();

    auto data = read_file(path);

    // Find the audio strf chunk - look for the second "strf" occurrence
    int strf_count = 0;
    size_t audio_strf_offset = 0;
    for (size_t i = 0; i + 4 <= data.size(); i++) {
        if (memcmp(data.data() + i, "strf", 4) == 0) {
            strf_count++;
            if (strf_count == 2) {
                audio_strf_offset = i + 8;  // skip "strf" + size field
                break;
            }
        }
    }
    ASSERT_GT(strf_count, 1) << "Could not find audio strf chunk";
    ASSERT_LT(audio_strf_offset + 18, data.size());

    // wFormatTag = 1 (PCM)
    EXPECT_EQ(read_u16(data, audio_strf_offset), 1u);
    // nChannels = 2
    EXPECT_EQ(read_u16(data, audio_strf_offset + 2), 2u);
    // nSamplesPerSec = 44100
    EXPECT_EQ(read_u32(data, audio_strf_offset + 4), 44100u);
    // nAvgBytesPerSec = 44100 * 2 * 2 = 176400
    EXPECT_EQ(read_u32(data, audio_strf_offset + 8), 176400u);
    // nBlockAlign = 4
    EXPECT_EQ(read_u16(data, audio_strf_offset + 12), 4u);
    // wBitsPerSample = 16
    EXPECT_EQ(read_u16(data, audio_strf_offset + 14), 16u);
}

TEST_F(AviRecorderTest, VideoFrameCaptureIncrementsCount) {
    std::string path = tmp_path("frames.avi");
    auto err = recorder_.start(path);
    ASSERT_TRUE(err.empty());

    auto frame = make_test_frame(64, 48, 255, 0, 0);
    recorder_.capture_video_frame(frame.data(), 64, 48, 64 * 4);
    EXPECT_EQ(recorder_.frame_count(), 1u);

    recorder_.capture_video_frame(frame.data(), 64, 48, 64 * 4);
    EXPECT_EQ(recorder_.frame_count(), 2u);

    recorder_.capture_video_frame(frame.data(), 64, 48, 64 * 4);
    EXPECT_EQ(recorder_.frame_count(), 3u);

    uint32_t total = recorder_.stop();
    EXPECT_EQ(total, 3u);
}

TEST_F(AviRecorderTest, BytesWrittenIncreases) {
    std::string path = tmp_path("bytes.avi");
    auto err = recorder_.start(path);
    ASSERT_TRUE(err.empty());

    uint64_t initial = recorder_.bytes_written();
    EXPECT_GT(initial, 0u);  // headers written

    auto frame = make_test_frame(64, 48, 0, 255, 0);
    recorder_.capture_video_frame(frame.data(), 64, 48, 64 * 4);

    EXPECT_GT(recorder_.bytes_written(), initial);

    recorder_.stop();
}

TEST_F(AviRecorderTest, DestructorFinalizesFile) {
    std::string path = tmp_path("destructor.avi");
    {
        AviRecorder local_rec;
        auto err = local_rec.start(path);
        ASSERT_TRUE(err.empty());
        auto frame = make_test_frame(64, 48, 0, 0, 255);
        local_rec.capture_video_frame(frame.data(), 64, 48, 64 * 4);
        // Destructor should call stop() and finalize the file
    }

    auto data = read_file(path);
    ASSERT_GE(data.size(), 12u);
    EXPECT_TRUE(bytes_match(data, 0, "RIFF", 4));
    EXPECT_TRUE(bytes_match(data, 8, "AVI ", 4));

    // Verify RIFF size is patched (non-zero)
    uint32_t riff_size = read_u32(data, 4);
    EXPECT_GT(riff_size, 0u);
    EXPECT_EQ(riff_size, static_cast<uint32_t>(data.size() - 8));
}

TEST_F(AviRecorderTest, HasIdx1Index) {
    std::string path = tmp_path("index.avi");
    auto err = recorder_.start(path);
    ASSERT_TRUE(err.empty());

    auto frame = make_test_frame(64, 48, 128, 128, 128);
    recorder_.capture_video_frame(frame.data(), 64, 48, 64 * 4);
    recorder_.stop();

    auto data = read_file(path);

    // Search for "idx1" chunk
    bool found_idx1 = false;
    for (size_t i = 0; i + 4 <= data.size(); i++) {
        if (memcmp(data.data() + i, "idx1", 4) == 0) {
            found_idx1 = true;
            // Verify it has at least one 16-byte index entry
            uint32_t idx_size = read_u32(data, i + 4);
            EXPECT_GE(idx_size, 16u);
            break;
        }
    }
    EXPECT_TRUE(found_idx1) << "idx1 index chunk not found";
}

TEST_F(AviRecorderTest, AudioCaptureWritesData) {
    std::string path = tmp_path("audio.avi");
    auto err = recorder_.start(path, 85, 44100, 2, 16);
    ASSERT_TRUE(err.empty());

    // Write some audio samples
    std::vector<int16_t> samples(1024, 0x1234);
    recorder_.capture_audio_samples(samples.data(), samples.size());

    uint64_t bytes_after_audio = recorder_.bytes_written();

    auto frame = make_test_frame(64, 48, 255, 255, 0);
    recorder_.capture_video_frame(frame.data(), 64, 48, 64 * 4);

    uint64_t bytes_after_video = recorder_.bytes_written();
    EXPECT_GT(bytes_after_video, bytes_after_audio);

    recorder_.stop();

    // Verify file has both "00dc" and "01wb" chunks
    auto data = read_file(path);
    bool found_00dc = false;
    bool found_01wb = false;
    for (size_t i = 0; i + 4 <= data.size(); i++) {
        if (memcmp(data.data() + i, "00dc", 4) == 0) found_00dc = true;
        if (memcmp(data.data() + i, "01wb", 4) == 0) found_01wb = true;
    }
    EXPECT_TRUE(found_00dc) << "Video data chunk '00dc' not found";
    EXPECT_TRUE(found_01wb) << "Audio data chunk '01wb' not found";
}

TEST_F(AviRecorderTest, StatusReportsCorrectValues) {
    EXPECT_FALSE(recorder_.is_recording());
    EXPECT_TRUE(recorder_.current_path().empty());
    EXPECT_EQ(recorder_.frame_count(), 0u);
    EXPECT_EQ(recorder_.bytes_written(), 0u);

    std::string path = tmp_path("status.avi");
    recorder_.start(path);
    EXPECT_TRUE(recorder_.is_recording());
    EXPECT_EQ(recorder_.current_path(), path);

    auto frame = make_test_frame(64, 48, 100, 100, 100);
    recorder_.capture_video_frame(frame.data(), 64, 48, 64 * 4);
    EXPECT_EQ(recorder_.frame_count(), 1u);
    EXPECT_GT(recorder_.bytes_written(), 0u);

    recorder_.stop();
    EXPECT_FALSE(recorder_.is_recording());
    EXPECT_TRUE(recorder_.current_path().empty());
}

#endif  // HAS_LIBJPEG
