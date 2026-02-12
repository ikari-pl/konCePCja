#include <gtest/gtest.h>
#include "session_recording.h"
#include <cstring>
#include <filesystem>
#include <fstream>

namespace {

class SessionRecorderTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = std::filesystem::temp_directory_path() / "ksr_test";
        std::filesystem::create_directories(tmp_dir_);
        rec_ = SessionRecorder();
    }
    void TearDown() override {
        // Ensure any open recording/playback files are closed before cleanup
        // (Windows locks open files, preventing remove_all)
        rec_.stop_recording();
        rec_.stop_playback();
        std::filesystem::remove_all(tmp_dir_);
    }

    // Create a minimal fake SNA file for testing
    std::string create_fake_sna() {
        std::string path = (tmp_dir_ / "test.sna").string();
        std::ofstream f(path, std::ios::binary);
        // Minimal SNA: 256 bytes of header + 64K RAM
        std::vector<uint8_t> data(256 + 65536, 0);
        memcpy(data.data(), "MV - SNA", 8);
        data[16] = 3; // version 3
        f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        return path;
    }

    std::filesystem::path tmp_dir_;
    SessionRecorder rec_;
};

TEST_F(SessionRecorderTest, InitialStateIsIdle) {
    EXPECT_EQ(rec_.state(), SessionState::IDLE);
    EXPECT_EQ(rec_.frame_count(), 0u);
    EXPECT_EQ(rec_.event_count(), 0u);
}

TEST_F(SessionRecorderTest, StartRecordingChangesState) {
    std::string snap = create_fake_sna();
    std::string ksr = (tmp_dir_ / "test.ksr").string();
    ASSERT_TRUE(rec_.start_recording(ksr, snap));
    EXPECT_EQ(rec_.state(), SessionState::RECORDING);
}

TEST_F(SessionRecorderTest, StartRecordingFailsIfNotIdle) {
    std::string snap = create_fake_sna();
    std::string ksr = (tmp_dir_ / "test.ksr").string();
    ASSERT_TRUE(rec_.start_recording(ksr, snap));
    std::string ksr2 = (tmp_dir_ / "test2.ksr").string();
    EXPECT_FALSE(rec_.start_recording(ksr2, snap));
}

TEST_F(SessionRecorderTest, RecordAndStopUpdatesEventCount) {
    std::string snap = create_fake_sna();
    std::string ksr = (tmp_dir_ / "test.ksr").string();
    ASSERT_TRUE(rec_.start_recording(ksr, snap));

    rec_.record_event(SessionEventType::KEY_DOWN, 0x1234);
    rec_.record_event(SessionEventType::KEY_UP, 0x1234);
    rec_.record_frame_sync();
    rec_.record_event(SessionEventType::KEY_DOWN, 0x5678);
    rec_.record_frame_sync();

    EXPECT_EQ(rec_.event_count(), 5u);  // 2 key + 2 sync + 1 key
    EXPECT_EQ(rec_.frame_count(), 2u);

    ASSERT_TRUE(rec_.stop_recording());
    EXPECT_EQ(rec_.state(), SessionState::IDLE);
}

TEST_F(SessionRecorderTest, RecordAndPlaybackRoundTrip) {
    std::string snap = create_fake_sna();
    std::string ksr = (tmp_dir_ / "test.ksr").string();

    // Record a session
    ASSERT_TRUE(rec_.start_recording(ksr, snap));
    rec_.record_event(SessionEventType::KEY_DOWN, 0x00FF);  // row 0, value 0xFF
    rec_.record_frame_sync();
    rec_.record_event(SessionEventType::KEY_DOWN, 0x01FE);  // row 1, value 0xFE
    rec_.record_event(SessionEventType::KEY_DOWN, 0x02FD);  // row 2, value 0xFD
    rec_.record_frame_sync();
    rec_.record_frame_sync();  // empty frame
    ASSERT_TRUE(rec_.stop_recording());

    // Playback
    std::string snap_out;
    ASSERT_TRUE(rec_.start_playback(ksr, snap_out));
    EXPECT_EQ(rec_.state(), SessionState::PLAYING);
    EXPECT_EQ(rec_.total_frames(), 3u);

    // Frame 1: one KEY_DOWN event
    SessionEvent evt;
    EXPECT_TRUE(rec_.next_event(evt));
    EXPECT_EQ(evt.type, SessionEventType::KEY_DOWN);
    EXPECT_EQ(evt.data, 0x00FF);
    EXPECT_FALSE(rec_.next_event(evt));  // hit frame boundary
    EXPECT_TRUE(rec_.advance_frame());

    // Frame 2: two KEY_DOWN events
    EXPECT_TRUE(rec_.next_event(evt));
    EXPECT_EQ(evt.data, 0x01FE);
    EXPECT_TRUE(rec_.next_event(evt));
    EXPECT_EQ(evt.data, 0x02FD);
    EXPECT_FALSE(rec_.next_event(evt));
    EXPECT_TRUE(rec_.advance_frame());

    // Frame 3: empty
    EXPECT_FALSE(rec_.next_event(evt));
    EXPECT_FALSE(rec_.advance_frame());  // end of recording

    // Clean up temp SNA
    std::filesystem::remove(snap_out);
}

TEST_F(SessionRecorderTest, PlaybackRejectsInvalidFile) {
    std::string bad = (tmp_dir_ / "bad.ksr").string();
    std::ofstream f(bad, std::ios::binary);
    f << "NOT_A_KSR_FILE";
    f.close();

    std::string snap_out;
    EXPECT_FALSE(rec_.start_playback(bad, snap_out));
    EXPECT_EQ(rec_.state(), SessionState::IDLE);
}

TEST_F(SessionRecorderTest, StopPlaybackReturnsToIdle) {
    std::string snap = create_fake_sna();
    std::string ksr = (tmp_dir_ / "test.ksr").string();
    ASSERT_TRUE(rec_.start_recording(ksr, snap));
    rec_.record_frame_sync();
    ASSERT_TRUE(rec_.stop_recording());

    std::string snap_out;
    ASSERT_TRUE(rec_.start_playback(ksr, snap_out));
    ASSERT_TRUE(rec_.stop_playback());
    EXPECT_EQ(rec_.state(), SessionState::IDLE);
    std::filesystem::remove(snap_out);
}

TEST_F(SessionRecorderTest, HeaderMagicAndVersion) {
    std::string snap = create_fake_sna();
    std::string ksr = (tmp_dir_ / "test.ksr").string();
    ASSERT_TRUE(rec_.start_recording(ksr, snap));
    rec_.record_frame_sync();
    ASSERT_TRUE(rec_.stop_recording());

    // Verify file header
    std::ifstream f(ksr, std::ios::binary);
    uint8_t header[KSR_HEADER_SIZE];
    f.read(reinterpret_cast<char*>(header), KSR_HEADER_SIZE);
    EXPECT_EQ(header[0], 'K');
    EXPECT_EQ(header[1], 'S');
    EXPECT_EQ(header[2], 'R');
    EXPECT_EQ(header[3], 0x1A);
    EXPECT_EQ(header[4], KSR_VERSION);
}

TEST_F(SessionRecorderTest, StartRecordingFailsWithBadSnapPath) {
    std::string ksr = (tmp_dir_ / "test.ksr").string();
    EXPECT_FALSE(rec_.start_recording(ksr, "/nonexistent/path.sna"));
    EXPECT_EQ(rec_.state(), SessionState::IDLE);
}

TEST_F(SessionRecorderTest, Constants) {
    EXPECT_EQ(KSR_HEADER_SIZE, 32u);
    EXPECT_EQ(KSR_VERSION, 1);
}

} // namespace
