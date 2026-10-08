// PNG screenshot writer — packed RGBA32 -> PNG file, over libpng.
//
// SDL_SavePNG (GUI build only) first normalises an SDL_Surface to RGBA32 so
// every input format (paletted, BGR, 15/16-bit, alpha-less) funnels into the
// one 8-bit RGBA encode path below — the same output the screenshot callers
// have always produced. The SDL-free build hands its RGBA32 surface straight
// to save_png_rgba32.
//
// Error handling: libpng reports failures by longjmp'ing out of the call
// stack. encode() below is the only frame that longjmp can cross, and it
// keeps nothing but trivially-destructible locals; every resource (output
// file, png_struct/png_info pair) is owned by an RAII guard in the caller's
// frame, so no path — including mid-encode I/O failure — leaks. Write errors
// are checked per chunk (fwrite) and once more on close (fclose flushes), so
// a full disk cannot pass silently.

#include "savepng.h"

#include <png.h>

#include <csetjmp>
#include <cstdio>
#include <vector>

#ifdef KONCPC_SDL
#include <SDL3/SDL.h>

#include <memory>
#endif

namespace {

// Where the libpng hooks report to: the output file and the first error.
// It must be trivially destructible, as encode()'s setjmp frame points at it.
struct PngSink {
  FILE* file;
  char error[256];
};

// libpng fatal-error hook: record the message, then unwind to the setjmp in
// encode(). png_longjmp never returns.
[[noreturn]] void on_png_error(png_structp png, png_const_charp msg) {
  PngSink* sink = static_cast<PngSink*>(png_get_error_ptr(png));
  if (sink != nullptr && sink->error[0] == '\0')
    std::snprintf(sink->error, sizeof(sink->error), "libpng: %s", msg);
  png_longjmp(png, 1);
}

// libpng write hook: forward to the file and turn a short write into a libpng
// error.
void on_png_write(png_structp png, png_bytep data, png_size_t length) {
  PngSink* sink = static_cast<PngSink*>(png_get_io_ptr(png));
  if (std::fwrite(data, 1, length, sink->file) != length) {
    std::snprintf(sink->error, sizeof(sink->error),
                  "write to output file failed");
    png_error(png, "write to output file failed");
  }
}

// libpng flush hook: nothing to do — fclose flushes, and its result is
// checked by the caller.
void on_png_flush(png_structp /*unused*/) {}

// RAII owner of the png_struct / png_info pair.
class PngWriter {
 public:
  // NOLINTNEXTLINE(modernize-use-equals-default): constructor has a member
  // initializer; = default is not equivalent
  explicit PngWriter(PngSink* sink)
      : png_(png_create_write_struct(PNG_LIBPNG_VER_STRING, sink, on_png_error,
                                     nullptr)) {
    if (png_ != nullptr) info_ = png_create_info_struct(png_);
  }
  ~PngWriter() {
    if (png_ != nullptr)
      png_destroy_write_struct(&png_, info_ != nullptr ? &info_ : nullptr);
  }
  PngWriter(const PngWriter&) = delete;
  PngWriter& operator=(const PngWriter&) = delete;

  bool ok() const { return png_ != nullptr && info_ != nullptr; }
  png_structp png() const { return png_; }
  png_infop info() const { return info_; }

 private:
  png_structp png_ = nullptr;
  png_infop info_ = nullptr;
};

// The libpng call sequence. This frame is the longjmp target, so it must hold
// only trivially-destructible locals (a longjmp that skipped a destructor
// would be undefined behaviour). Returns false with the error in the sink.
bool encode(png_structp png, png_infop info, PngSink* sink, int width,
            int height, png_bytep* rows) {
  // NOLINTNEXTLINE(modernize-avoid-setjmp-longjmp): libpng's error handling
  // mandates setjmp/longjmp
  if (setjmp(png_jmpbuf(png)) != 0) return false;  // arrived via on_png_error
  png_set_write_fn(png, sink, on_png_write, on_png_flush);
  png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGB_ALPHA,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);
  png_write_image(png, rows);
  png_write_end(png, info);
  return true;
}

}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other
// translation units; internal linkage would break the link
int save_png_rgba32(const uint8_t* pixels, int width, int height, int pitch,
                    const std::string& file, std::string& error) {
  if (pixels == nullptr || width <= 0 || height <= 0 || pitch < width * 4) {
    error = "save_png_rgba32: empty or malformed image";
    return -1;
  }

  PngSink sink{std::fopen(file.c_str(), "wb"), {}};
  if (sink.file == nullptr) {
    error = "cannot open " + file + " for writing";
    return -1;
  }

  // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated
  // (out-param/compound-assign/loop/reference)
  bool encoded = false;
  {
    PngWriter const writer(&sink);
    if (writer.ok()) {
      // One row pointer per scanline (pitch-aware). libpng only reads them.
      std::vector<png_bytep> rows(static_cast<size_t>(height));
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): png_write_image
      // takes non-const row pointers but never writes through them
      png_bytep base = const_cast<png_bytep>(pixels);
      for (int y = 0; y < height; y++)
        rows[static_cast<size_t>(y)] =
            base + (static_cast<size_t>(y) * static_cast<size_t>(pitch));
      encoded = encode(writer.png(), writer.info(), &sink, width, height,
                       rows.data());
    } else {
      std::snprintf(sink.error, sizeof(sink.error),
                    "libpng writer initialisation failed");
    }
  }

  // Close (and flush) unconditionally; a failed flush means a truncated file.
  const bool closed = std::fclose(sink.file) == 0;
  if (!encoded) {
    error = sink.error[0] != '\0' ? sink.error : "PNG encode failed";
    return -1;
  }
  if (!closed) {
    error = "closing " + file + " failed";
    return -1;
  }
  return 0;
}

#ifdef KONCPC_SDL
namespace {
struct SurfaceDeleter {
  void operator()(SDL_Surface* s) const { SDL_DestroySurface(s); }
};
using SurfacePtr = std::unique_ptr<SDL_Surface, SurfaceDeleter>;
}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage,readability-non-const-parameter):
// external API consumed by other translation units/tests; internal linkage
// would break the link; pointer written through a cast or passed to a non-const
// callee
int SDL_SavePNG(SDL_Surface* src, const std::string& file) {
  if (src == nullptr) {
    SDL_SetError("SDL_SavePNG: surface must not be null");
    return -1;
  }

  // Normalise to RGBA32: one encode path for every input pixel format.
  SurfacePtr rgba(SDL_ConvertSurface(src, SDL_PIXELFORMAT_RGBA32));
  if (!rgba) return -1;  // SDL_GetError() set by SDL

  std::string error;
  if (save_png_rgba32(static_cast<const uint8_t*>(rgba->pixels), rgba->w,
                      rgba->h, rgba->pitch, file, error) != 0) {
    SDL_SetError("SDL_SavePNG: %s", error.c_str());
    return -1;
  }
  return 0;
}
#endif
