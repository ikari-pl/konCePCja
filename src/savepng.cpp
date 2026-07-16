// PNG screenshot writer — SDL_Surface -> PNG file, over libpng.
//
// The surface is first normalised to RGBA32 so every input format (paletted,
// BGR, 15/16-bit, alpha-less) funnels into one 8-bit RGBA encode path — the
// same output the screenshot callers have always produced.
//
// Error handling: libpng reports failures by longjmp'ing out of the call
// stack. encode() below is the only frame that longjmp can cross, and it
// keeps nothing but trivially-destructible locals; every resource (converted
// surface, output stream, png_struct/png_info pair) is owned by an RAII guard
// in the caller's frame, so no path — including mid-encode I/O failure —
// leaks. Write errors are checked per chunk (SDL_WriteIO) and once more on
// close (SDL_CloseIO flushes), so a full disk cannot pass silently.

#include "savepng.h"

#include <SDL3/SDL.h>
#include <png.h>

#include <csetjmp>
#include <memory>
#include <vector>

namespace {

struct SurfaceDeleter {
  void operator()(SDL_Surface* s) const { SDL_DestroySurface(s); }
};
using SurfacePtr = std::unique_ptr<SDL_Surface, SurfaceDeleter>;

// libpng fatal-error hook: record the message, then unwind to the setjmp in
// encode(). png_longjmp never returns.
[[noreturn]] void on_png_error(png_structp png, png_const_charp msg) {
  SDL_SetError("libpng: %s", msg);
  png_longjmp(png, 1);
}

// libpng write hook: forward to the SDL_IOStream and turn a short write into
// a libpng error (SDL has already set the error string).
void on_png_write(png_structp png, png_bytep data, png_size_t length) {
  SDL_IOStream* io = static_cast<SDL_IOStream*>(png_get_io_ptr(png));
  if (SDL_WriteIO(io, data, length) != length)
    png_error(png, "write to output stream failed");
}

// libpng flush hook: nothing to do — SDL_CloseIO flushes, and its result is
// checked by the caller.
void on_png_flush(png_structp /*unused*/) {}

// RAII owner of the png_struct / png_info pair.
class PngWriter {
 public:
  // NOLINTNEXTLINE(modernize-use-equals-default): constructor has a member
  // initializer; = default is not equivalent
  PngWriter()
      : png_(png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr,
                                     on_png_error, nullptr)) {
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
// would be undefined behaviour). Returns false with the error in
// SDL_GetError().
bool encode(png_structp png, png_infop info, SDL_IOStream* io, int width,
            int height, png_bytep* rows) {
  // NOLINTNEXTLINE(modernize-avoid-setjmp-longjmp): libpng's error handling
  // mandates setjmp/longjmp
  if (setjmp(png_jmpbuf(png)) != 0) return false;  // arrived via on_png_error
  png_set_write_fn(png, io, on_png_write, on_png_flush);
  png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGB_ALPHA,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);
  png_write_image(png, rows);
  png_write_end(png, info);
  return true;
}

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

  SDL_IOStream* io = SDL_IOFromFile(file.c_str(), "wb");
  if (io == nullptr) return -1;  // SDL_GetError() set by SDL

  PngWriter const writer;
  // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated
  // (out-param/compound-assign/loop/reference)
  bool encoded = false;
  if (writer.ok()) {
    // One row pointer per scanline into the converted surface (pitch-aware).
    std::vector<png_bytep> rows(static_cast<size_t>(rgba->h));
    png_bytep pixels = static_cast<png_bytep>(rgba->pixels);
    for (int y = 0; y < rgba->h; y++)
      rows[static_cast<size_t>(y)] =
          pixels + (static_cast<size_t>(y) * static_cast<size_t>(rgba->pitch));
    encoded =
        encode(writer.png(), writer.info(), io, rgba->w, rgba->h, rows.data());
  } else {
    SDL_SetError("SDL_SavePNG: libpng writer initialisation failed");
  }

  // Close (and flush) unconditionally; a failed flush means a truncated file.
  const bool closed = SDL_CloseIO(io);
  if (!encoded) return -1;
  if (!closed) return -1;  // SDL_GetError() set by SDL
  return 0;
}
