#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <array>
#include <mutex>

class YmRecorder {
public:
    ~YmRecorder();

    // Start recording. Returns empty string on success, error message on failure.
    std::string start(const std::string& path);

    // Stop recording and write YM5 file. Returns frame count, or 0 if not recording.
    uint32_t stop();

    // Capture one frame of PSG register data (14 registers).
    // Call once per VBL.
    void capture_frame(const uint8_t* regs);

    bool is_recording() const;
    bool has_error() const;
    uint32_t frame_count() const;
    std::string current_path() const;

private:
    static constexpr int NUM_REGISTERS = 14;

    std::string path_;
    std::vector<std::array<uint8_t, 14>> frames_;
    bool recording_ = false;
    bool error_ = false;
    mutable std::mutex mutex_;

    bool write_ym5_file();
};

// Global instance
extern YmRecorder g_ym_recorder;
