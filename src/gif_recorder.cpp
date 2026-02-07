#define MSF_GIF_IMPL
#include "msf_gif.h"

#include "gif_recorder.h"
#include <cstdio>
#include <cstring>

bool GifRecorder::begin(int width, int height, int delay) {
    if (recording) abort();
    auto* s = new MsfGifState{};
    if (!msf_gif_begin(s, width, height)) {
        delete s;
        return false;
    }
    state = s;
    delay_cs = delay;
    frames_added = 0;
    recording = true;
    return true;
}

bool GifRecorder::add_frame(const uint8_t* pixels, int pitch) {
    if (!recording || !state) return false;
    auto* s = static_cast<MsfGifState*>(state);
    // msf_gif_frame takes non-const uint8_t* but doesn't modify pixels
    int ok = msf_gif_frame(s, const_cast<uint8_t*>(pixels), delay_cs, 16, pitch);
    if (ok) frames_added++;
    return ok != 0;
}

bool GifRecorder::end(const std::string& path) {
    if (!recording || !state) return false;
    auto* s = static_cast<MsfGifState*>(state);
    MsfGifResult result = msf_gif_end(s);
    delete s;
    state = nullptr;
    recording = false;

    if (!result.data) return false;

    FILE* fp = fopen(path.c_str(), "wb");
    bool ok = false;
    if (fp) {
        ok = fwrite(result.data, result.dataSize, 1, fp) == 1;
        fclose(fp);
    }
    msf_gif_free(result);
    return ok;
}

void GifRecorder::abort() {
    if (state) {
        auto* s = static_cast<MsfGifState*>(state);
        msf_gif_end(s); // frees internal allocations
        delete s;
        state = nullptr;
    }
    recording = false;
    frames_added = 0;
}
