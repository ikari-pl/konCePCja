#include "avi_recorder.h"
#include <cstring>
#include <algorithm>

#ifdef HAS_LIBJPEG
extern "C" {
#include <jpeglib.h>
}
#endif

AviRecorder g_avi_recorder;

namespace {

// AVI chunk IDs used for index entries (little-endian)
constexpr uint32_t FOURCC_01wb = 0x62773130;  // "01wb" - audio data chunk

constexpr uint32_t AVIF_HASINDEX = 0x00000010;
constexpr uint32_t CPC_FPS = 50;

#ifdef HAS_LIBJPEG
constexpr uint32_t FOURCC_00dc = 0x63643030;  // "00dc" - video data chunk
constexpr uint32_t AVIIF_KEYFRAME = 0x00000010;
#endif

// File offsets for fields that patch_sizes() must update after recording.
// Layout: RIFF(4) + size(4) + "AVI "(4) = 12 bytes
//         LIST(4) + size(4) + "hdrl"(4) = 12 bytes
//         "avih"(4) + size(4) + 56 bytes avih data = 64 bytes
//         LIST(4) + size(4) + "strl"(4) = 12 bytes  (video)
//         "strh"(4) + size(4) + 56 bytes strh data = 64 bytes
//         "strf"(4) + size(4) + 40 bytes strf data = 48 bytes
//         LIST(4) + size(4) + "strl"(4) = 12 bytes  (audio)
//         "strh"(4) + size(4) + 56 bytes strh data = 64 bytes
constexpr long RIFF_SIZE_OFFSET = 4;                 // RIFF file size
constexpr long AVIH_TOTAL_FRAMES_OFFSET = 48;        // avih.dwTotalFrames
constexpr long VIDEO_STRH_LENGTH_OFFSET = 128;       // video strh.dwLength
constexpr long AUDIO_STRH_LENGTH_OFFSET = 252;       // audio strh.dwLength

} // namespace

void AviRecorder::write_le_u16(uint16_t val, FILE* f) {
    fputc(val & 0xff, f);
    fputc((val >> 8) & 0xff, f);
}

void AviRecorder::write_le_u32(uint32_t val, FILE* f) {
    fputc(val & 0xff, f);
    fputc((val >> 8) & 0xff, f);
    fputc((val >> 16) & 0xff, f);
    fputc((val >> 24) & 0xff, f);
}

void AviRecorder::write_fourcc(const char* cc, FILE* f) {
    fwrite(cc, 1, 4, f);
}

AviRecorder::~AviRecorder() {
    if (file_) {
        stop();
    }
}

std::string AviRecorder::start(const std::string& path, int quality,
                               uint32_t sample_rate, uint16_t channels,
                               uint16_t bits_per_sample) {
#ifndef HAS_LIBJPEG
    (void)path; (void)quality; (void)sample_rate; (void)channels; (void)bits_per_sample;
    return "AVI recording requires libjpeg (not found at build time)";
#else
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) {
        return "already recording";
    }

    file_ = fopen(path.c_str(), "wb");
    if (!file_) {
        return std::string("cannot open file: ") + strerror(errno);
    }

    path_ = path;
    quality_ = std::clamp(quality, 1, 100);
    sample_rate_ = sample_rate;
    channels_ = channels;
    bits_per_sample_ = bits_per_sample;
    video_frames_ = 0;
    audio_bytes_ = 0;
    total_bytes_ = 0;
    width_ = 0;
    height_ = 0;
    index_entries_.clear();
    movi_start_ = 0;

    // We defer writing the full headers until the first video frame,
    // because we need to know the frame dimensions. Write a placeholder.
    // Actually, we write headers with placeholder dimensions now and patch later.
    // But for simplicity, we write headers assuming 384x270 (CPC default)
    // and update on first frame if different.
    width_ = 384;
    height_ = 270;
    write_headers();

    return "";
#endif
}

uint32_t AviRecorder::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_) {
        return 0;
    }

    finalize();
    fclose(file_);
    file_ = nullptr;

    uint32_t result = video_frames_;
    video_frames_ = 0;
    audio_bytes_ = 0;
    total_bytes_ = 0;
    path_.clear();
    index_entries_.clear();
    return result;
}

void AviRecorder::capture_video_frame(const uint8_t* pixels, int width, int height, int stride) {
#ifndef HAS_LIBJPEG
    (void)pixels; (void)width; (void)height; (void)stride;
#else
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_ || !pixels) return;

    // Update dimensions if this is first frame with actual data
    if (video_frames_ == 0 && (width != width_ || height != height_)) {
        width_ = width;
        height_ = height;
        // Rewrite headers with correct dimensions
        fseek(file_, 0, SEEK_SET);
        movi_start_ = 0;
        write_headers();
    }

    auto jpeg_data = compress_jpeg(pixels, width, height, stride);
    if (jpeg_data.empty()) return;

    uint32_t chunk_offset = static_cast<uint32_t>(ftell(file_) - movi_start_ - 4);

    // Write video chunk: "00dc" + size + data (+ pad byte if odd size)
    write_le_u32(FOURCC_00dc, file_);
    uint32_t data_size = static_cast<uint32_t>(jpeg_data.size());
    write_le_u32(data_size, file_);
    fwrite(jpeg_data.data(), 1, jpeg_data.size(), file_);
    // AVI chunks must be word-aligned
    if (data_size & 1) {
        fputc(0, file_);
    }

    index_entries_.push_back({FOURCC_00dc, AVIIF_KEYFRAME, chunk_offset, data_size});
    video_frames_++;
    total_bytes_ = static_cast<uint64_t>(ftell(file_));
#endif
}

void AviRecorder::capture_audio_samples(const int16_t* samples, size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_ || !samples || count == 0) return;

    uint32_t byte_count = static_cast<uint32_t>(count * sizeof(int16_t));
    uint32_t chunk_offset = static_cast<uint32_t>(ftell(file_) - movi_start_ - 4);

    // Write audio chunk: "01wb" + size + data (+ pad byte if odd)
    write_le_u32(FOURCC_01wb, file_);
    write_le_u32(byte_count, file_);
    fwrite(samples, 1, byte_count, file_);
    if (byte_count & 1) {
        fputc(0, file_);
    }

    index_entries_.push_back({FOURCC_01wb, 0, chunk_offset, byte_count});
    audio_bytes_ += byte_count;
    total_bytes_ = static_cast<uint64_t>(ftell(file_));
}

bool AviRecorder::is_recording() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return file_ != nullptr;
}

uint32_t AviRecorder::frame_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return video_frames_;
}

uint64_t AviRecorder::bytes_written() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_bytes_;
}

std::string AviRecorder::current_path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return path_;
}

// Write the full AVI header structure.
// Layout:
//   RIFF 'AVI '
//     LIST 'hdrl'
//       avih (main AVI header)
//       LIST 'strl' (video stream)
//         strh (stream header - vids)
//         strf (stream format - BITMAPINFOHEADER)
//       LIST 'strl' (audio stream)
//         strh (stream header - auds)
//         strf (stream format - WAVEFORMATEX)
//     LIST 'movi'
//       ... chunks ...
//     idx1
//       ... index ...
void AviRecorder::write_headers() {
    // RIFF header
    write_fourcc("RIFF", file_);
    write_le_u32(0, file_);  // placeholder for file size
    write_fourcc("AVI ", file_);

    // LIST 'hdrl'
    write_fourcc("LIST", file_);
    write_le_u32(0, file_);  // placeholder for hdrl size
    uint32_t hdrl_start = static_cast<uint32_t>(ftell(file_));
    write_fourcc("hdrl", file_);

    // Main AVI header (avih) - 56 bytes
    write_fourcc("avih", file_);
    write_le_u32(56, file_);  // avih chunk size
    write_le_u32(1000000 / CPC_FPS, file_);  // dwMicroSecPerFrame (20000 for 50fps)
    write_le_u32(0, file_);   // dwMaxBytesPerSec (placeholder)
    write_le_u32(0, file_);   // dwPaddingGranularity
    write_le_u32(AVIF_HASINDEX, file_);  // dwFlags
    write_le_u32(0, file_);   // dwTotalFrames (placeholder)
    write_le_u32(0, file_);   // dwInitialFrames
    write_le_u32(2, file_);   // dwStreams (video + audio)
    write_le_u32(0, file_);   // dwSuggestedBufferSize
    write_le_u32(static_cast<uint32_t>(width_), file_);   // dwWidth
    write_le_u32(static_cast<uint32_t>(height_), file_);  // dwHeight
    write_le_u32(0, file_);   // dwReserved[0]
    write_le_u32(0, file_);   // dwReserved[1]
    write_le_u32(0, file_);   // dwReserved[2]
    write_le_u32(0, file_);   // dwReserved[3]

    // --- Video stream ---
    // LIST 'strl'
    write_fourcc("LIST", file_);
    write_le_u32(0, file_);  // placeholder for strl size
    uint32_t vstrl_start = static_cast<uint32_t>(ftell(file_));
    write_fourcc("strl", file_);

    // strh (stream header) - 56 bytes
    write_fourcc("strh", file_);
    write_le_u32(56, file_);  // strh chunk size
    write_fourcc("vids", file_);  // fccType
    write_fourcc("MJPG", file_);  // fccHandler
    write_le_u32(0, file_);   // dwFlags
    write_le_u16(0, file_);   // wPriority
    write_le_u16(0, file_);   // wLanguage
    write_le_u32(0, file_);   // dwInitialFrames
    write_le_u32(1, file_);   // dwScale
    write_le_u32(CPC_FPS, file_);  // dwRate (fps)
    write_le_u32(0, file_);   // dwStart
    write_le_u32(0, file_);   // dwLength (placeholder - total frames)
    write_le_u32(0, file_);   // dwSuggestedBufferSize
    write_le_u32(0xFFFFFFFF, file_);  // dwQuality (-1 = default)
    write_le_u32(0, file_);   // dwSampleSize
    write_le_u16(0, file_);   // rcFrame.left
    write_le_u16(0, file_);   // rcFrame.top
    write_le_u16(static_cast<uint16_t>(width_), file_);   // rcFrame.right
    write_le_u16(static_cast<uint16_t>(height_), file_);  // rcFrame.bottom

    // strf (stream format) - BITMAPINFOHEADER - 40 bytes
    write_fourcc("strf", file_);
    write_le_u32(40, file_);  // strf chunk size
    write_le_u32(40, file_);  // biSize
    write_le_u32(static_cast<uint32_t>(width_), file_);   // biWidth
    write_le_u32(static_cast<uint32_t>(height_), file_);  // biHeight
    write_le_u16(1, file_);   // biPlanes
    write_le_u16(24, file_);  // biBitCount (MJPEG outputs 24bpp)
    write_fourcc("MJPG", file_);  // biCompression
    write_le_u32(static_cast<uint32_t>(width_ * height_ * 3), file_);  // biSizeImage
    write_le_u32(0, file_);   // biXPelsPerMeter
    write_le_u32(0, file_);   // biYPelsPerMeter
    write_le_u32(0, file_);   // biClrUsed
    write_le_u32(0, file_);   // biClrImportant

    // Patch video strl LIST size
    uint32_t vstrl_end = static_cast<uint32_t>(ftell(file_));
    fseek(file_, static_cast<long>(vstrl_start - 4), SEEK_SET);
    write_le_u32(vstrl_end - vstrl_start, file_);
    fseek(file_, 0, SEEK_END);

    // --- Audio stream ---
    // LIST 'strl'
    write_fourcc("LIST", file_);
    write_le_u32(0, file_);  // placeholder for strl size
    uint32_t astrl_start = static_cast<uint32_t>(ftell(file_));
    write_fourcc("strl", file_);

    // strh (stream header) - 56 bytes
    uint16_t block_align = static_cast<uint16_t>(channels_ * (bits_per_sample_ / 8));
    uint32_t byte_rate = sample_rate_ * block_align;

    write_fourcc("strh", file_);
    write_le_u32(56, file_);  // strh chunk size
    write_fourcc("auds", file_);  // fccType
    write_le_u32(0, file_);   // fccHandler (not used for PCM)
    write_le_u32(0, file_);   // dwFlags
    write_le_u16(0, file_);   // wPriority
    write_le_u16(0, file_);   // wLanguage
    write_le_u32(0, file_);   // dwInitialFrames
    write_le_u32(block_align, file_);  // dwScale
    write_le_u32(byte_rate, file_);    // dwRate
    write_le_u32(0, file_);   // dwStart
    write_le_u32(0, file_);   // dwLength (placeholder - total audio samples)
    write_le_u32(0, file_);   // dwSuggestedBufferSize
    write_le_u32(0xFFFFFFFF, file_);  // dwQuality
    write_le_u32(block_align, file_);  // dwSampleSize
    write_le_u16(0, file_);   // rcFrame.left
    write_le_u16(0, file_);   // rcFrame.top
    write_le_u16(0, file_);   // rcFrame.right
    write_le_u16(0, file_);   // rcFrame.bottom

    // strf (stream format) - WAVEFORMATEX - 18 bytes
    write_fourcc("strf", file_);
    write_le_u32(18, file_);   // strf chunk size
    write_le_u16(1, file_);    // wFormatTag = PCM
    write_le_u16(channels_, file_);
    write_le_u32(sample_rate_, file_);
    write_le_u32(byte_rate, file_);
    write_le_u16(block_align, file_);
    write_le_u16(bits_per_sample_, file_);
    write_le_u16(0, file_);    // cbSize (extra format bytes)

    // Patch audio strl LIST size
    uint32_t astrl_end = static_cast<uint32_t>(ftell(file_));
    fseek(file_, static_cast<long>(astrl_start - 4), SEEK_SET);
    write_le_u32(astrl_end - astrl_start, file_);
    fseek(file_, 0, SEEK_END);

    // Patch hdrl LIST size
    uint32_t hdrl_end = static_cast<uint32_t>(ftell(file_));
    fseek(file_, static_cast<long>(hdrl_start - 4), SEEK_SET);
    write_le_u32(hdrl_end - hdrl_start, file_);
    fseek(file_, 0, SEEK_END);

    // LIST 'movi'
    write_fourcc("LIST", file_);
    write_le_u32(0, file_);  // placeholder for movi size
    movi_start_ = static_cast<uint32_t>(ftell(file_));
    write_fourcc("movi", file_);

    total_bytes_ = static_cast<uint64_t>(ftell(file_));
}

void AviRecorder::finalize() {
    // Pad movi list to word boundary
    long movi_end = ftell(file_);

    // Patch movi LIST size
    uint32_t movi_size = static_cast<uint32_t>(movi_end) - movi_start_;
    fseek(file_, static_cast<long>(movi_start_ - 4), SEEK_SET);
    write_le_u32(movi_size, file_);
    fseek(file_, movi_end, SEEK_SET);

    // Write idx1 index
    write_idx1();

    // Patch sizes in headers
    patch_sizes();

    fflush(file_);
}

void AviRecorder::write_idx1() {
    write_fourcc("idx1", file_);
    uint32_t idx_size = static_cast<uint32_t>(index_entries_.size() * 16);
    write_le_u32(idx_size, file_);

    for (const auto& entry : index_entries_) {
        write_le_u32(entry.chunk_id, file_);
        write_le_u32(entry.flags, file_);
        write_le_u32(entry.offset, file_);
        write_le_u32(entry.size, file_);
    }
}

void AviRecorder::patch_sizes() {
    long file_end = ftell(file_);

    // Patch RIFF file size (total file size minus 8 bytes for RIFF header)
    uint32_t riff_size = static_cast<uint32_t>(file_end - 8);
    fseek(file_, RIFF_SIZE_OFFSET, SEEK_SET);
    write_le_u32(riff_size, file_);

    // Patch avih.dwTotalFrames
    fseek(file_, AVIH_TOTAL_FRAMES_OFFSET, SEEK_SET);
    write_le_u32(video_frames_, file_);

    // Patch video strh.dwLength (total video frames)
    fseek(file_, VIDEO_STRH_LENGTH_OFFSET, SEEK_SET);
    write_le_u32(video_frames_, file_);

    // Patch audio strh.dwLength (total audio samples)
    uint16_t block_align = static_cast<uint16_t>(channels_ * (bits_per_sample_ / 8));
    uint32_t audio_samples = (block_align > 0) ? (audio_bytes_ / block_align) : 0;
    fseek(file_, AUDIO_STRH_LENGTH_OFFSET, SEEK_SET);
    write_le_u32(audio_samples, file_);

    fseek(file_, file_end, SEEK_SET);
}

#ifdef HAS_LIBJPEG
// Suppress old-style-cast warnings from libjpeg macros (jpeg_create_compress, etc.)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"

std::vector<uint8_t> AviRecorder::compress_jpeg(const uint8_t* rgba, int w, int h, int stride) {
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    unsigned char* out_buf = nullptr;
    unsigned long out_size = 0;
    jpeg_mem_dest(&cinfo, &out_buf, &out_size);

    cinfo.image_width = static_cast<JDIMENSION>(w);
    cinfo.image_height = static_cast<JDIMENSION>(h);
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality_, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    // Convert RGBA rows to RGB for JPEG compression
    std::vector<uint8_t> rgb_row(static_cast<size_t>(w * 3));

    while (cinfo.next_scanline < cinfo.image_height) {
        const uint8_t* src = rgba + cinfo.next_scanline * stride;
        for (int x = 0; x < w; x++) {
            rgb_row[static_cast<size_t>(x * 3 + 0)] = src[x * 4 + 0];  // R
            rgb_row[static_cast<size_t>(x * 3 + 1)] = src[x * 4 + 1];  // G
            rgb_row[static_cast<size_t>(x * 3 + 2)] = src[x * 4 + 2];  // B
        }
        JSAMPROW row_ptr = rgb_row.data();
        jpeg_write_scanlines(&cinfo, &row_ptr, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    std::vector<uint8_t> result(out_buf, out_buf + out_size);
    free(out_buf);  // NOLINT - libjpeg allocates with malloc
    return result;
}

#pragma GCC diagnostic pop
#endif
