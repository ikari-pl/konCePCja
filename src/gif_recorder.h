#pragma once

#include <string>
#include <cstdint>

// Animated GIF recorder for frame dumps.
// Uses msf_gif.h internally for LZW-compressed, delta-encoded GIF output.

class GifRecorder {
public:
    // Begin recording frames for an animated GIF.
    // delay_cs: inter-frame delay in centiseconds (default 2 = 50fps CPC timing)
    bool begin(int width, int height, int delay_cs = 2);

    // Add one frame (RGBA8 pixel data, pitch in bytes).
    bool add_frame(const uint8_t* pixels, int pitch);

    // Finish and write the GIF to path. Returns true on success.
    bool end(const std::string& path);

    // Discard without saving.
    void abort();

    bool is_recording() const { return recording; }
    int frame_count() const { return frames_added; }

private:
    void* state = nullptr;  // MsfGifState*, opaque to avoid header leak
    bool recording = false;
    int delay_cs = 2;
    int frames_added = 0;
};
