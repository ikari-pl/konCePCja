#pragma once

#include <string>
#include <cstdint>
#include <cstdio>
#include <mutex>

class WavRecorder {
public:
    ~WavRecorder();

    // Start recording. Returns empty string on success, error message on failure.
    std::string start(const std::string& path, uint32_t sample_rate,
                      uint16_t bits_per_sample, uint16_t channels);

    // Stop recording. Returns bytes of PCM data written.
    uint32_t stop();

    // Write PCM samples to WAV file. Thread-safe.
    void write_samples(const uint8_t* data, uint32_t len);

    bool is_recording() const;
    std::string current_path() const;
    uint32_t bytes_written() const;
    bool has_error() const;

private:
    FILE* file_ = nullptr;
    std::string path_;
    uint32_t data_bytes_ = 0;
    uint32_t sample_rate_ = 0;
    uint16_t bits_per_sample_ = 0;
    uint16_t channels_ = 0;
    bool error_ = false;
    mutable std::mutex mutex_;

    void write_header();
    void finalize_header();
};

// Global instance
extern WavRecorder g_wav_recorder;
