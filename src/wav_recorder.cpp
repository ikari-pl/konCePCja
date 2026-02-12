#include "wav_recorder.h"
#include <cstring>

namespace {
bool write_le_u16(uint16_t val, FILE* f) {
    return fputc(val & 0xff, f) != EOF &&
           fputc((val >> 8) & 0xff, f) != EOF;
}
bool write_le_u32(uint32_t val, FILE* f) {
    return fputc(val & 0xff, f) != EOF &&
           fputc((val >> 8) & 0xff, f) != EOF &&
           fputc((val >> 16) & 0xff, f) != EOF &&
           fputc((val >> 24) & 0xff, f) != EOF;
}
} // namespace

WavRecorder g_wav_recorder;

WavRecorder::~WavRecorder() {
    if (file_) {
        stop();
    }
}

std::string WavRecorder::start(const std::string& path, uint32_t sample_rate,
                               uint16_t bits_per_sample, uint16_t channels) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) {
        return "already recording";
    }

    file_ = fopen(path.c_str(), "wb");
    if (!file_) {
        return std::string("cannot open file: ") + strerror(errno);
    }

    path_ = path;
    data_bytes_ = 0;
    sample_rate_ = sample_rate;
    bits_per_sample_ = bits_per_sample;
    channels_ = channels;
    error_ = false;

    write_header();
    return "";
}

uint32_t WavRecorder::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_) {
        return 0;
    }

    finalize_header();
    fclose(file_);
    file_ = nullptr;

    uint32_t result = data_bytes_;
    data_bytes_ = 0;
    path_.clear();
    return result;
}

void WavRecorder::write_samples(const uint8_t* data, uint32_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_ || len == 0) return;

    size_t written = fwrite(data, 1, len, file_);
    data_bytes_ += static_cast<uint32_t>(written);
    if (written < len) {
        error_ = true;
    }
}

bool WavRecorder::is_recording() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return file_ != nullptr;
}

std::string WavRecorder::current_path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return path_;
}

uint32_t WavRecorder::bytes_written() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return data_bytes_;
}

bool WavRecorder::has_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

// Write the initial 44-byte WAV header with placeholder sizes.
// Sizes will be patched by finalize_header() when recording stops.
void WavRecorder::write_header() {
    uint32_t byte_rate = sample_rate_ * channels_ * (bits_per_sample_ / 8);
    uint16_t block_align = static_cast<uint16_t>(channels_ * (bits_per_sample_ / 8));
    bool ok = true;

    // RIFF header
    ok = ok && fwrite("RIFF", 1, 4, file_) == 4;
    ok = ok && write_le_u32(0, file_);  // file size placeholder, will be patched
    ok = ok && fwrite("WAVE", 1, 4, file_) == 4;

    // fmt sub-chunk
    ok = ok && fwrite("fmt ", 1, 4, file_) == 4;
    ok = ok && write_le_u32(16, file_);           // fmt chunk size
    ok = ok && write_le_u16(1, file_);            // audio format = PCM
    ok = ok && write_le_u16(channels_, file_);
    ok = ok && write_le_u32(sample_rate_, file_);
    ok = ok && write_le_u32(byte_rate, file_);
    ok = ok && write_le_u16(block_align, file_);
    ok = ok && write_le_u16(bits_per_sample_, file_);

    // data sub-chunk
    ok = ok && fwrite("data", 1, 4, file_) == 4;
    ok = ok && write_le_u32(0, file_);  // data size placeholder, will be patched

    if (!ok) error_ = true;
}

// Seek back to the header and patch the file size and data size fields.
void WavRecorder::finalize_header() {
    bool ok = true;

    // Patch data sub-chunk size at offset 40
    ok = ok && fseek(file_, 40, SEEK_SET) == 0;
    ok = ok && write_le_u32(data_bytes_, file_);

    // Patch RIFF chunk size at offset 4: total file size - 8
    uint32_t riff_size = data_bytes_ + 36;  // 44 - 8 = 36 bytes of header after RIFF size
    ok = ok && fseek(file_, 4, SEEK_SET) == 0;
    ok = ok && write_le_u32(riff_size, file_);

    fflush(file_);
    if (!ok) error_ = true;
}
