#include "ym_recorder.h"
#include <cstdio>
#include <cstring>

namespace {

bool write_be_u16(uint16_t val, FILE* f) {
    return fputc((val >> 8) & 0xff, f) != EOF &&
           fputc(val & 0xff, f) != EOF;
}

bool write_be_u32(uint32_t val, FILE* f) {
    return fputc((val >> 24) & 0xff, f) != EOF &&
           fputc((val >> 16) & 0xff, f) != EOF &&
           fputc((val >> 8) & 0xff, f) != EOF &&
           fputc(val & 0xff, f) != EOF;
}

} // namespace

YmRecorder g_ym_recorder;

YmRecorder::~YmRecorder() {
    if (recording_) {
        stop();
    }
}

std::string YmRecorder::start(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (recording_) {
        return "already recording";
    }

    // Verify the path is writable by opening it briefly
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        return std::string("cannot open file: ") + strerror(errno);
    }
    fclose(f);

    path_ = path;
    frames_.clear();
    recording_ = true;
    error_ = false;
    return "";
}

uint32_t YmRecorder::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!recording_) {
        return 0;
    }

    recording_ = false;
    uint32_t count = static_cast<uint32_t>(frames_.size());

    if (!write_ym5_file()) {
        error_ = true;
    }

    path_.clear();
    frames_.clear();
    return count;
}

void YmRecorder::capture_frame(const uint8_t* regs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!recording_) return;

    std::array<uint8_t, 14> frame;
    for (int i = 0; i < NUM_REGISTERS; i++) {
        frame[i] = regs[i];
    }
    frames_.push_back(frame);
}

bool YmRecorder::is_recording() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return recording_;
}

bool YmRecorder::has_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

uint32_t YmRecorder::frame_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint32_t>(frames_.size());
}

std::string YmRecorder::current_path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return path_;
}

bool YmRecorder::write_ym5_file() {
    FILE* f = fopen(path_.c_str(), "wb");
    if (!f) return false;

    bool ok = true;
    uint32_t num_frames = static_cast<uint32_t>(frames_.size());

    // 1. Magic: "YM5!"
    ok = ok && fwrite("YM5!", 1, 4, f) == 4;

    // 2. Check string: "LeOnArD!"
    ok = ok && fwrite("LeOnArD!", 1, 8, f) == 8;

    // 3. Number of frames (uint32_t BE)
    ok = ok && write_be_u32(num_frames, f);

    // 4. Song attributes (uint32_t BE) - 1 = interleaved
    ok = ok && write_be_u32(1, f);

    // 5. Number of digidrums (uint16_t BE) - 0
    ok = ok && write_be_u16(0, f);

    // 6. Master clock (uint32_t BE) - 1000000 for CPC
    ok = ok && write_be_u32(1000000, f);

    // 7. Player frequency (uint16_t BE) - 50 Hz for PAL CPC
    ok = ok && write_be_u16(50, f);

    // 8. VBL loop frame (uint32_t BE) - 0
    ok = ok && write_be_u32(0, f);

    // 9. Additional data size (uint16_t BE) - 0
    ok = ok && write_be_u16(0, f);

    // 10. Song name (null-terminated)
    const char* song_name = "konCePCja recording";
    size_t name_len = strlen(song_name) + 1;
    ok = ok && fwrite(song_name, 1, name_len, f) == name_len;

    // 11. Author name (null-terminated)
    ok = ok && fputc(0, f) != EOF;

    // 12. Comment (null-terminated)
    ok = ok && fputc(0, f) != EOF;

    // 13. Register data: interleaved format
    // 14 blocks, each block is num_frames bytes (one byte per frame for that register)
    for (int reg = 0; reg < NUM_REGISTERS && ok; reg++) {
        for (uint32_t frame = 0; frame < num_frames && ok; frame++) {
            ok = ok && (fputc(frames_[frame][reg], f) != EOF);
        }
    }

    // 14. End marker: "End!"
    ok = ok && fwrite("End!", 1, 4, f) == 4;

    if (fclose(f) != 0) {
        ok = false;
    }
    return ok;
}
