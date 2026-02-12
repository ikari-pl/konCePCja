#pragma once
#include <cstdint>
#include <string>
#include <vector>

// konCePCja Session Recording (.ksr format)
//
// File layout:
//   [32-byte header]
//   [SNA snapshot data (variable length)]
//   [event records until EOF]
//
// Header (32 bytes):
//   0-3:   magic "KSR\x1A"
//   4:     version (1)
//   5-7:   reserved
//   8-11:  SNA size (LE32)
//   12-15: event count (LE32) — filled on close
//   16-31: reserved

constexpr uint32_t KSR_MAGIC = 0x1A52534B;  // "KSR\x1A" little-endian
constexpr uint8_t KSR_VERSION = 1;
constexpr size_t KSR_HEADER_SIZE = 32;

// Event types
enum class SessionEventType : uint8_t {
    FRAME_SYNC = 0x00,    // 1 byte: type only (marks frame boundary)
    KEY_DOWN   = 0x01,    // 3 bytes: type + CPC key code (uint16_t LE)
    KEY_UP     = 0x02,    // 3 bytes: type + CPC key code (uint16_t LE)
    JOY_STATE  = 0x03,    // 3 bytes: type + joystick bitmask (uint16_t LE)
};

struct SessionEvent {
    SessionEventType type;
    uint16_t data;        // key code or joystick state
};

// Recording states
enum class SessionState {
    IDLE,
    RECORDING,
    PLAYING,
};

class SessionRecorder {
public:
    SessionRecorder() = default;
    ~SessionRecorder();

    // Recording
    bool start_recording(const std::string& path, const std::string& snap_path);
    void record_event(SessionEventType type, uint16_t data = 0);
    void record_frame_sync();
    bool stop_recording();

    // Playback
    bool start_playback(const std::string& path, std::string& snap_path_out);
    // Get next event for current frame. Returns false when no more events this frame.
    bool next_event(SessionEvent& evt);
    // Advance to next frame. Returns false if recording is finished.
    bool advance_frame();
    bool stop_playback();

    // State
    SessionState state() const { return state_; }
    uint32_t frame_count() const { return frame_count_; }
    uint32_t event_count() const { return event_count_; }
    uint32_t total_frames() const { return total_frames_; }
    const std::string& path() const { return path_; }

private:
    SessionState state_ = SessionState::IDLE;
    std::string path_;
    uint32_t frame_count_ = 0;
    uint32_t event_count_ = 0;
    uint32_t total_frames_ = 0;

    // Recording state
    FILE* rec_file_ = nullptr;

    // Playback state
    std::vector<SessionEvent> events_;  // all events loaded at once
    size_t play_pos_ = 0;              // current position in events_
};

extern SessionRecorder g_session;
