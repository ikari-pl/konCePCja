/* scalers_test.cpp — the clean-room 2x pixel-scaling kernels (src/scalers).
 *
 * These kernels replaced a GPL-2.0 vendored file with a clean-room
 * re-authoring from the public algorithm descriptions; there were no tests
 * before. Property-based assertions (not exact-rounding oracles): every kernel
 * must double the image, preserve a uniform field where the algorithm says it
 * should, honour the EPX/bilinear rules, and clamp at the border without
 * reading out of bounds. All inputs synthetic.
 */
#include "scalers/cpc_scalers.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

constexpr Uint16 kRed = 0xF800;    // R=31 G=0  B=0
constexpr Uint16 kBlack = 0x0000;  // 0
constexpr Uint16 kGrey = 0x8410;   // R=16 G=16 B=16

int r5(Uint16 p) { return (p >> 11) & 0x1F; }

// A small RGB565 image with a fill and helpers to read the 2x output.
struct Img {
  int w, h;
  std::vector<Uint16> px;
  Img(int w_, int h_, Uint16 fill) : w(w_), h(h_), px((w_ * h_), fill) {}
  Uint8* bytes() { return reinterpret_cast<Uint8*>(px.data()); }
  Uint32 pitch() const { return static_cast<Uint32>(w) * 2; }
  Uint16& at(int x, int y) { return px[y * w + x]; }
};

struct Out {
  int w, h;
  std::vector<Uint16> px;
  Out(int sw, int sh) : w(sw * 2), h(sh * 2), px(sw * 2 * sh * 2, 0xDEAD) {}
  Uint8* bytes() { return reinterpret_cast<Uint8*>(px.data()); }
  Uint32 pitch() const { return static_cast<Uint32>(w) * 2; }
  Uint16 at(int x, int y) const { return px[y * w + x]; }
};

using Filter = void (*)(Uint8*, Uint32, Uint8*, Uint32, int, int);

// Uniform-signature thunks: some header filters take `const Uint8*`, some
// `Uint8*`. Calling through these thunks lets the non-const->const conversion
// happen legitimately at the real call site (no function-pointer casts / UB).
void t_scale2x(Uint8* s, Uint32 sp, Uint8* d, Uint32 dp, int w, int h) {
  filter_scale2x(s, sp, d, dp, w, h);
}
void t_ascale2x(Uint8* s, Uint32 sp, Uint8* d, Uint32 dp, int w, int h) {
  filter_ascale2x(s, sp, d, dp, w, h);
}
void t_supereagle(Uint8* s, Uint32 sp, Uint8* d, Uint32 dp, int w, int h) {
  filter_supereagle(s, sp, d, dp, w, h);
}
void t_tv2x(Uint8* s, Uint32 sp, Uint8* d, Uint32 dp, int w, int h) {
  filter_tv2x(s, sp, d, dp, w, h);
}
void t_bilinear(Uint8* s, Uint32 sp, Uint8* d, Uint32 dp, int w, int h) {
  filter_bilinear(s, sp, d, dp, w, h);
}
void t_bicubic(Uint8* s, Uint32 sp, Uint8* d, Uint32 dp, int w, int h) {
  filter_bicubic(s, sp, d, dp, w, h);
}
void t_dotmatrix(Uint8* s, Uint32 sp, Uint8* d, Uint32 dp, int w, int h) {
  filter_dotmatrix(s, sp, d, dp, w, h);
}

void run(Filter f, Img& in, Out& out) {
  f(in.bytes(), in.pitch(), out.bytes(), out.pitch(), in.w, in.h);
}

const Filter kAllFilters[] = {&t_scale2x,  &t_ascale2x, &t_supereagle, &t_tv2x,
                              &t_bilinear, &t_bicubic,  &t_dotmatrix};

}  // namespace

// Every kernel must fully write the 2x output — no untouched 0xDEAD cells.
TEST(Scalers, FillsEntire2xOutput) {
  for (Filter f : kAllFilters) {
    Img in(8, 6, kGrey);
    Out out(in.w, in.h);
    run(f, in, out);
    for (int y = 0; y < out.h; ++y)
      for (int x = 0; x < out.w; ++x)
        ASSERT_NE(out.at(x, y), 0xDEAD) << "unwritten output cell";
  }
}

// A uniform field stays uniform through the edge-preserving kernels (all
// neighbours equal -> no rule fires). TV2x/dot-matrix intentionally dim, so
// they are excluded here and checked separately.
TEST(Scalers, UniformFieldPreservedBySharpKernels) {
  const Filter sharp[] = {&t_scale2x, &t_ascale2x, &t_supereagle, &t_bilinear,
                          &t_bicubic};
  for (Filter f : sharp) {
    Img in(8, 8, kRed);
    Out out(in.w, in.h);
    run(f, in, out);
    for (int y = 0; y < out.h; ++y)
      for (int x = 0; x < out.w; ++x)
        ASSERT_EQ(out.at(x, y), kRed) << "uniform field not preserved";
  }
}

// Scale2x/EPX: the top-left output subpixel of a pixel takes the edge colour
// when its left and up neighbours agree and differ from the opposite edges.
TEST(Scalers, Scale2xEpxCornerRule) {
  // 3x3, centre black, left & up red, right & down black.
  Img in(3, 3, kBlack);
  in.at(1, 1) = kBlack;
  in.at(0, 1) = kRed;  // left
  in.at(1, 0) = kRed;  // up
  // right (2,1) and down (1,2) stay black.
  Out out(in.w, in.h);
  run(&t_scale2x, in, out);
  // Centre pixel (1,1) expands to out (2..3, 2..3); TL is out(2,2).
  EXPECT_EQ(out.at(2, 2), kRed) << "EPX TL corner should snap to the edge";
  // TR (out(3,2)) — up=red,right=black differ -> stays centre (black).
  EXPECT_EQ(out.at(3, 2), kBlack) << "EPX TR corner should stay centre";
}

// Bilinear: the horizontal-midpoint subpixel lies strictly between the two
// source colours; the source-aligned subpixel is exact.
TEST(Scalers, BilinearMidpointIsBetween) {
  Img in(2, 1, kRed);
  in.at(1, 0) = kBlack;  // red | black horizontally
  Out out(in.w, in.h);
  run(&t_bilinear, in, out);
  EXPECT_EQ(out.at(0, 0), kRed) << "source-aligned subpixel must be exact";
  const int mid = r5(out.at(1, 0));  // TR of the red pixel: avg(red, black)
  EXPECT_GT(mid, 0);
  EXPECT_LT(mid, r5(kRed)) << "midpoint must be between the two colours";
}

// TV2x dims the lower scanline of each pair; the upper stays full.
TEST(Scalers, Tv2xDimsAlternateScanlines) {
  Img in(4, 4, kRed);
  Out out(in.w, in.h);
  run(&t_tv2x, in, out);
  for (int x = 0; x < out.w; ++x) {
    EXPECT_EQ(out.at(x, 0), kRed) << "upper scanline should be full";
    EXPECT_LT(r5(out.at(x, 1)), r5(kRed)) << "lower scanline should be dimmed";
  }
}

// Border safety: a 1x1 image must not read out of bounds; all four output
// cells equal the single source pixel (every neighbour clamps to it).
TEST(Scalers, SinglePixelClampsAtBorder) {
  for (Filter f : kAllFilters) {
    Img in(1, 1, kGrey);
    Out out(in.w, in.h);
    run(f, in, out);
    // At least the top-left (source-aligned, undimmed) cell equals the source.
    EXPECT_EQ(out.at(0, 0), kGrey) << "1x1 top-left must equal source";
  }
}
