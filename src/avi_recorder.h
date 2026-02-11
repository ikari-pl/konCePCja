#pragma once

#include <string>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <mutex>

class AviRecorder {
public:
    ~AviRecorder();

    // Start recording. Returns empty string on success, error message on failure.
    std::string start(const std::string& path, int quality = 85,
                      uint32_t sample_rate = 44100,
                      uint16_t channels = 2,
                      uint16_t bits_per_sample = 16);

    // Stop recording and finalize AVI file. Returns frame count.
    uint32_t stop();

    // Capture a video frame (RGBA pixel data).
    void capture_video_frame(const uint8_t* pixels, int width, int height, int stride);

    // Capture audio samples (interleaved PCM). Thread-safe.
    void capture_audio_samples(const int16_t* samples, size_t count);

    bool is_recording() const;
    uint32_t frame_count() const;
    uint64_t bytes_written() const;
    std::string current_path() const;

private:
    FILE* file_ = nullptr;
    std::string path_;
    int quality_ = 85;

    int width_ = 0;
    int height_ = 0;
    uint32_t video_frames_ = 0;

    uint32_t sample_rate_ = 44100;
    uint16_t channels_ = 2;
    uint16_t bits_per_sample_ = 16;
    uint32_t audio_bytes_ = 0;

    uint32_t movi_start_ = 0;
    uint64_t total_bytes_ = 0;

    struct IndexEntry {
        uint32_t chunk_id;
        uint32_t flags;
        uint32_t offset;
        uint32_t size;
    };
    std::vector<IndexEntry> index_entries_;

    mutable std::mutex mutex_;

    void write_headers();
    void finalize();
    void write_idx1();
    void patch_sizes();

#ifdef HAS_LIBJPEG
    std::vector<uint8_t> compress_jpeg(const uint8_t* rgba, int w, int h, int stride);
#endif

    static void write_le_u16(uint16_t val, FILE* f);
    static void write_le_u32(uint32_t val, FILE* f);
    static void write_fourcc(const char* cc, FILE* f);
};

extern AviRecorder g_avi_recorder;
