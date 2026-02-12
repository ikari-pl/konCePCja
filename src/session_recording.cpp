#include "session_recording.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

SessionRecorder g_session;

// Portable little-endian helpers
static void write_le32(FILE* f, uint32_t val) {
    uint8_t b[4] = {
        static_cast<uint8_t>(val),
        static_cast<uint8_t>(val >> 8),
        static_cast<uint8_t>(val >> 16),
        static_cast<uint8_t>(val >> 24)
    };
    fwrite(b, 1, 4, f);
}

static void write_le16(FILE* f, uint16_t val) {
    uint8_t b[2] = {
        static_cast<uint8_t>(val),
        static_cast<uint8_t>(val >> 8)
    };
    fwrite(b, 1, 2, f);
}

static uint32_t read_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

static uint16_t read_le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
}

SessionRecorder::~SessionRecorder() {
    if (state_ == SessionState::RECORDING) stop_recording();
    if (state_ == SessionState::PLAYING) stop_playback();
}

bool SessionRecorder::start_recording(const std::string& path, const std::string& snap_path) {
    if (state_ != SessionState::IDLE) return false;

    // Read the SNA file that was saved before calling us
    FILE* snap_f = fopen(snap_path.c_str(), "rb");
    if (!snap_f) return false;
    fseek(snap_f, 0, SEEK_END);
    long snap_size = ftell(snap_f);
    fseek(snap_f, 0, SEEK_SET);
    if (snap_size <= 0) { fclose(snap_f); return false; }

    std::vector<uint8_t> snap_data(static_cast<size_t>(snap_size));
    if (fread(snap_data.data(), 1, snap_data.size(), snap_f) != snap_data.size()) {
        fclose(snap_f);
        return false;
    }
    fclose(snap_f);

    // Open the KSR file
    rec_file_ = fopen(path.c_str(), "wb");
    if (!rec_file_) return false;

    // Write header (32 bytes)
    uint8_t header[KSR_HEADER_SIZE] = {};
    header[0] = 'K'; header[1] = 'S'; header[2] = 'R'; header[3] = 0x1A;
    header[4] = KSR_VERSION;
    // SNA size at offset 8 (LE32)
    header[8]  = static_cast<uint8_t>(snap_size);
    header[9]  = static_cast<uint8_t>(snap_size >> 8);
    header[10] = static_cast<uint8_t>(snap_size >> 16);
    header[11] = static_cast<uint8_t>(snap_size >> 24);
    // Event count at offset 12 will be filled on stop
    if (fwrite(header, 1, KSR_HEADER_SIZE, rec_file_) != KSR_HEADER_SIZE) {
        fclose(rec_file_);
        rec_file_ = nullptr;
        return false;
    }

    // Write embedded SNA
    if (fwrite(snap_data.data(), 1, snap_data.size(), rec_file_) != snap_data.size()) {
        fclose(rec_file_);
        rec_file_ = nullptr;
        return false;
    }

    path_ = path;
    state_ = SessionState::RECORDING;
    frame_count_ = 0;
    event_count_ = 0;
    return true;
}

void SessionRecorder::record_event(SessionEventType type, uint16_t data) {
    if (state_ != SessionState::RECORDING || !rec_file_) return;
    uint8_t t = static_cast<uint8_t>(type);
    fwrite(&t, 1, 1, rec_file_);
    if (type != SessionEventType::FRAME_SYNC) {
        write_le16(rec_file_, data);
    }
    event_count_++;
}

void SessionRecorder::record_frame_sync() {
    record_event(SessionEventType::FRAME_SYNC);
    frame_count_++;
}

bool SessionRecorder::stop_recording() {
    if (state_ != SessionState::RECORDING) return false;
    if (rec_file_) {
        // Update event count in header (offset 12)
        fseek(rec_file_, 12, SEEK_SET);
        write_le32(rec_file_, event_count_);
        fclose(rec_file_);
        rec_file_ = nullptr;
    }
    state_ = SessionState::IDLE;
    return true;
}

bool SessionRecorder::start_playback(const std::string& path, std::string& snap_path_out) {
    if (state_ != SessionState::IDLE) return false;

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    // Read header
    uint8_t header[KSR_HEADER_SIZE];
    if (fread(header, 1, KSR_HEADER_SIZE, f) != KSR_HEADER_SIZE) {
        fclose(f);
        return false;
    }
    if (header[0] != 'K' || header[1] != 'S' || header[2] != 'R' || header[3] != 0x1A) {
        fclose(f);
        return false;
    }
    if (header[4] != KSR_VERSION) {
        fclose(f);
        return false;
    }

    uint32_t sna_size = read_le32(header + 8);
    uint32_t evt_count = read_le32(header + 12);

    // Read and write SNA to temp file
    std::vector<uint8_t> sna_data(sna_size);
    if (fread(sna_data.data(), 1, sna_size, f) != sna_size) {
        fclose(f);
        return false;
    }

    // Write SNA to a temp file for snapshot_load
    snap_path_out = path + ".sna";
    FILE* snap_f = fopen(snap_path_out.c_str(), "wb");
    if (!snap_f) { fclose(f); return false; }
    if (fwrite(sna_data.data(), 1, sna_size, snap_f) != sna_size) {
        fclose(snap_f);
        fclose(f);
        return false;
    }
    fclose(snap_f);

    // Read all events
    events_.clear();
    events_.reserve(evt_count);
    total_frames_ = 0;

    while (!feof(f)) {
        uint8_t type_byte;
        if (fread(&type_byte, 1, 1, f) != 1) break;

        SessionEvent evt;
        evt.type = static_cast<SessionEventType>(type_byte);
        evt.data = 0;

        if (evt.type != SessionEventType::FRAME_SYNC) {
            uint8_t d[2];
            if (fread(d, 1, 2, f) != 2) break;
            evt.data = read_le16(d);
        } else {
            total_frames_++;
        }
        events_.push_back(evt);
    }
    fclose(f);

    path_ = path;
    state_ = SessionState::PLAYING;
    frame_count_ = 0;
    event_count_ = static_cast<uint32_t>(events_.size());
    play_pos_ = 0;
    return true;
}

bool SessionRecorder::next_event(SessionEvent& evt) {
    if (state_ != SessionState::PLAYING) return false;
    while (play_pos_ < events_.size()) {
        if (events_[play_pos_].type == SessionEventType::FRAME_SYNC) {
            return false;  // hit frame boundary, caller should call advance_frame()
        }
        evt = events_[play_pos_++];
        return true;
    }
    return false;  // end of recording
}

bool SessionRecorder::advance_frame() {
    if (state_ != SessionState::PLAYING) return false;
    // Skip to the FRAME_SYNC and past it
    while (play_pos_ < events_.size()) {
        if (events_[play_pos_].type == SessionEventType::FRAME_SYNC) {
            play_pos_++;
            frame_count_++;
            // If no more events remain, the recording is finished
            if (play_pos_ >= events_.size()) {
                stop_playback();
                return false;
            }
            return true;
        }
        play_pos_++;  // skip events that weren't consumed
    }
    // End of recording
    stop_playback();
    return false;
}

bool SessionRecorder::stop_playback() {
    if (state_ != SessionState::PLAYING && state_ != SessionState::IDLE) return false;
    events_.clear();
    play_pos_ = 0;
    state_ = SessionState::IDLE;
    return true;
}
