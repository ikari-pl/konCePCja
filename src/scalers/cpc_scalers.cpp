/* Classic 2x pixel-scaling kernels — CLEAN-ROOM implementation.
 *
 * Re-authored here from the PUBLIC published descriptions of each algorithm;
 * no GPL, MAME, SMS-SDL/2xSaI, or Caprice32 source was consulted. Only the
 * module's own interface (cpc_scalers.h) and the public algorithm rules were
 * used:
 *   - Scale2x / EPX          — the public EPX corner rules (Eric Johnston's
 *                              EPX; the identical "Scale2x" rule set, both
 *                              openly documented).
 *   - Advanced Scale2x       — EPX plus the standard "both axes differ" guard
 *                              (the AdvMAME2x refinement) to suppress the
 *                              lone-corner artefact.
 *   - Super Eagle            — the public Eagle corner-detection rule, extended
 *                              with straightforward edge/centre antialiasing.
 *   - TV2x                   — alternate output scanlines dimmed (a scanline
 *                              CRT look); trivially standard.
 *   - Bilinear               — 2x box/bilinear: each source pixel expands to a
 *                              2x2 block averaged toward its right/down
 *                              neighbours.
 *   - Bicubic                — separable Catmull-Rom, evaluated at the t=0.5
 *                              half-sample: (-p0 + 9*p1 + 9*p2 - p3)/16.
 *   - Dot-matrix             — a fixed 2x2 LCD-grid dimming mask.
 *
 * All kernels take 16bpp RGB565 input (srcPitch/dstPitch in BYTES) and write a
 * 2x-scaled RGB565 image. Every read is clamped at the image border, so no
 * kernel reads out of bounds. Pure functions: they touch only the caller's
 * buffers. First-party code, covered by the konCePCja Source License.
 */
#include "scalers/cpc_scalers.h"

#include <algorithm>

namespace {

// --- RGB565 helpers -------------------------------------------------------
// 565 low channel bits: R=0x0800, G=0x0020, B=0x0001 -> 0x0821.
constexpr Uint16 kLow565 = 0x0821;
constexpr Uint16 kHigh565 = 0xF7DE;  // ~kLow565

// Round-to-nearest average of two 565 pixels, done channel-parallel.
inline Uint16 avg2(Uint16 a, Uint16 b) {
  return static_cast<Uint16>(((a & kHigh565) >> 1) + ((b & kHigh565) >> 1) +
                             (a & b & kLow565));
}

inline void unpack565(Uint16 p, int& r, int& g, int& b) {
  r = (p >> 11) & 0x1F;
  g = (p >> 5) & 0x3F;
  b = p & 0x1F;
}

inline Uint16 pack565(int r, int g, int b) {
  r = std::clamp(r, 0, 31);
  g = std::clamp(g, 0, 63);
  b = std::clamp(b, 0, 31);
  return static_cast<Uint16>((r << 11) | (g << 5) | b);
}

// Average of four 565 pixels (per channel, rounded).
inline Uint16 avg4(Uint16 a, Uint16 b, Uint16 c, Uint16 d) {
  int ar, ag, ab, br, bg, bb, cr, cg, cb, dr, dg, db;
  unpack565(a, ar, ag, ab);
  unpack565(b, br, bg, bb);
  unpack565(c, cr, cg, cb);
  unpack565(d, dr, dg, db);
  return pack565((ar + br + cr + dr + 2) >> 2, (ag + bg + cg + dg + 2) >> 2,
                 (ab + bb + cb + db + 2) >> 2);
}

// Dim a 565 pixel by numerator/denominator (integer, per channel).
inline Uint16 dim(Uint16 p, int num, int den) {
  int r, g, b;
  unpack565(p, r, g, b);
  return pack565(r * num / den, g * num / den, b * num / den);
}

// A clamped 2D view over a 16bpp source. pitch is in BYTES.
struct SrcView {
  const Uint16* base;
  int stride;  // in pixels
  int w, h;
  Uint16 at(int x, int y) const {
    x = std::clamp(x, 0, w - 1);
    y = std::clamp(y, 0, h - 1);
    return base[(y * stride) + x];
  }
};

struct DstView {
  Uint16* base;
  int stride;  // in pixels
  Uint16& at(int x, int y) const { return base[(y * stride) + x]; }
};

SrcView src_view(const Uint8* p, Uint32 pitch, int w, int h) {
  return SrcView{reinterpret_cast<const Uint16*>(p),
                 static_cast<int>(pitch / 2), w, h};
}
DstView dst_view(Uint8* p, Uint32 pitch) {
  return DstView{reinterpret_cast<Uint16*>(p), static_cast<int>(pitch / 2)};
}

// EPX / Scale2x corner rule for one output subpixel.
//   center C, plus the two orthogonal edges meeting at this corner.
inline Uint16 epx_corner(Uint16 c, Uint16 edgeA, Uint16 edgeB, Uint16 opp1,
                         Uint16 opp2) {
  // edgeA==edgeB and they differ from the opposing edges -> the edge colour.
  return (edgeA == edgeB && edgeA != opp1 && edgeB != opp2) ? edgeA : c;
}

}  // namespace

// --- Scale2x / EPX --------------------------------------------------------
void filter_scale2x(const Uint8* srcPtr, Uint32 srcPitch, Uint8* dstPtr,
                    Uint32 dstPitch, int width, int height) {
  const SrcView s = src_view(srcPtr, srcPitch, width, height);
  const DstView d = dst_view(dstPtr, dstPitch);
  for (int y = 0; y < height; ++y) {
    const int dy = y * 2;
    for (int x = 0; x < width; ++x) {
      const int dx = x * 2;
      const Uint16 c = s.at(x, y);
      const Uint16 up = s.at(x, y - 1), dn = s.at(x, y + 1);
      const Uint16 lf = s.at(x - 1, y), rt = s.at(x + 1, y);
      // E0=TL E1=TR E2=BL E3=BR
      d.at(dx, dy) = epx_corner(c, lf, up, rt, dn);
      d.at(dx + 1, dy) = epx_corner(c, up, rt, lf, dn);
      d.at(dx, dy + 1) = epx_corner(c, lf, dn, rt, up);
      d.at(dx + 1, dy + 1) = epx_corner(c, dn, rt, up, lf);
    }
  }
}

// --- Advanced Scale2x (EPX + AdvMAME2x "both axes differ" guard) ----------
void filter_ascale2x(Uint8* srcPtr, Uint32 srcPitch, Uint8* dstPtr,
                     Uint32 dstPitch, int width, int height) {
  const SrcView s = src_view(srcPtr, srcPitch, width, height);
  const DstView d = dst_view(dstPtr, dstPitch);
  for (int y = 0; y < height; ++y) {
    const int dy = y * 2;
    for (int x = 0; x < width; ++x) {
      const int dx = x * 2;
      const Uint16 c = s.at(x, y);
      const Uint16 up = s.at(x, y - 1), dn = s.at(x, y + 1);
      const Uint16 lf = s.at(x - 1, y), rt = s.at(x + 1, y);
      Uint16 e0 = c, e1 = c, e2 = c, e3 = c;
      // Only interpolate where the vertical and horizontal neighbours both
      // differ — this drops the isolated single-pixel corner artefact.
      if (up != dn && lf != rt) {
        e0 = epx_corner(c, lf, up, rt, dn);
        e1 = epx_corner(c, up, rt, lf, dn);
        e2 = epx_corner(c, lf, dn, rt, up);
        e3 = epx_corner(c, dn, rt, up, lf);
      }
      d.at(dx, dy) = e0;
      d.at(dx + 1, dy) = e1;
      d.at(dx, dy + 1) = e2;
      d.at(dx + 1, dy + 1) = e3;
    }
  }
}

// --- Super Eagle (public Eagle corner rule + edge/centre antialiasing) ----
namespace {
inline Uint16 eagle_corner(Uint16 c, Uint16 edgeA, Uint16 edgeB, Uint16 diag) {
  if (edgeA == edgeB) {
    if (edgeA == diag) return edgeA;  // solid corner -> take it
    return avg2(edgeA, c);            // partial edge -> antialias
  }
  return c;
}
}  // namespace

void filter_supereagle(Uint8* srcPtr, Uint32 srcPitch, Uint8* dstPtr,
                       Uint32 dstPitch, int width, int height) {
  const SrcView s = src_view(srcPtr, srcPitch, width, height);
  const DstView d = dst_view(dstPtr, dstPitch);
  for (int y = 0; y < height; ++y) {
    const int dy = y * 2;
    for (int x = 0; x < width; ++x) {
      const int dx = x * 2;
      const Uint16 c = s.at(x, y);
      const Uint16 up = s.at(x, y - 1), dn = s.at(x, y + 1);
      const Uint16 lf = s.at(x - 1, y), rt = s.at(x + 1, y);
      const Uint16 ul = s.at(x - 1, y - 1), ur = s.at(x + 1, y - 1);
      const Uint16 dl = s.at(x - 1, y + 1), dr = s.at(x + 1, y + 1);
      d.at(dx, dy) = eagle_corner(c, lf, up, ul);
      d.at(dx + 1, dy) = eagle_corner(c, up, rt, ur);
      d.at(dx, dy + 1) = eagle_corner(c, lf, dn, dl);
      d.at(dx + 1, dy + 1) = eagle_corner(c, dn, rt, dr);
    }
  }
}

// --- TV2x (dimmed alternate scanlines) ------------------------------------
void filter_tv2x(const Uint8* srcPtr, Uint32 srcPitch, Uint8* dstPtr,
                 Uint32 dstPitch, int width, int height) {
  const SrcView s = src_view(srcPtr, srcPitch, width, height);
  const DstView d = dst_view(dstPtr, dstPitch);
  for (int y = 0; y < height; ++y) {
    const int dy = y * 2;
    for (int x = 0; x < width; ++x) {
      const int dx = x * 2;
      const Uint16 c = s.at(x, y);
      const Uint16 rt = s.at(x + 1, y);
      const Uint16 hmix = avg2(c, rt);
      d.at(dx, dy) = c;
      d.at(dx + 1, dy) = hmix;
      // Lower scanline dimmed to ~3/4 for the CRT-line look.
      d.at(dx, dy + 1) = dim(c, 3, 4);
      d.at(dx + 1, dy + 1) = dim(hmix, 3, 4);
    }
  }
}

// --- Bilinear (2x2 block averaged toward right/down neighbours) -----------
void filter_bilinear(const Uint8* srcPtr, Uint32 srcPitch, Uint8* dstPtr,
                     Uint32 dstPitch, int width, int height) {
  const SrcView s = src_view(srcPtr, srcPitch, width, height);
  const DstView d = dst_view(dstPtr, dstPitch);
  for (int y = 0; y < height; ++y) {
    const int dy = y * 2;
    for (int x = 0; x < width; ++x) {
      const int dx = x * 2;
      const Uint16 c = s.at(x, y);
      const Uint16 rt = s.at(x + 1, y);
      const Uint16 dn = s.at(x, y + 1);
      const Uint16 dg = s.at(x + 1, y + 1);
      d.at(dx, dy) = c;
      d.at(dx + 1, dy) = avg2(c, rt);
      d.at(dx, dy + 1) = avg2(c, dn);
      d.at(dx + 1, dy + 1) = avg4(c, rt, dn, dg);
    }
  }
}

// --- Bicubic (separable Catmull-Rom at the half sample) -------------------
namespace {
// Catmull-Rom at t=0.5 between p1,p2 (with outer taps p0,p3), per channel.
inline int cr_half(int p0, int p1, int p2, int p3) {
  return (-p0 + (9 * p1) + (9 * p2) - p3 + 8) >> 4;
}
inline Uint16 cr_half_px(Uint16 a, Uint16 b, Uint16 c, Uint16 e) {
  int ar, ag, ab, br, bg, bb, cr, cg, cb, er, eg, eb;
  unpack565(a, ar, ag, ab);
  unpack565(b, br, bg, bb);
  unpack565(c, cr, cg, cb);
  unpack565(e, er, eg, eb);
  return pack565(cr_half(ar, br, cr, er), cr_half(ag, bg, cg, eg),
                 cr_half(ab, bb, cb, eb));
}
}  // namespace

void filter_bicubic(Uint8* srcPtr, Uint32 srcPitch, Uint8* dstPtr,
                    Uint32 dstPitch, int width, int height) {
  const SrcView s = src_view(srcPtr, srcPitch, width, height);
  const DstView d = dst_view(dstPtr, dstPitch);
  // Even output columns/rows carry source samples; odd ones the half-samples.
  for (int y = 0; y < height; ++y) {
    const int dy = y * 2;
    for (int x = 0; x < width; ++x) {
      const int dx = x * 2;
      const Uint16 c = s.at(x, y);
      // Horizontal half-sample between x and x+1.
      const Uint16 hx =
          cr_half_px(s.at(x - 1, y), c, s.at(x + 1, y), s.at(x + 2, y));
      // Vertical half-sample between y and y+1.
      const Uint16 vy =
          cr_half_px(s.at(x, y - 1), c, s.at(x, y + 1), s.at(x, y + 2));
      // Diagonal half-sample: cubic across the four vertical half-samples of
      // columns x-1..x+2 (separable order gives the same result at t=0.5).
      const Uint16 vy_m1 = cr_half_px(s.at(x - 1, y - 1), s.at(x - 1, y),
                                      s.at(x - 1, y + 1), s.at(x - 1, y + 2));
      const Uint16 vy_p1 = cr_half_px(s.at(x + 1, y - 1), s.at(x + 1, y),
                                      s.at(x + 1, y + 1), s.at(x + 1, y + 2));
      const Uint16 vy_p2 = cr_half_px(s.at(x + 2, y - 1), s.at(x + 2, y),
                                      s.at(x + 2, y + 1), s.at(x + 2, y + 2));
      const Uint16 dgn = cr_half_px(vy_m1, vy, vy_p1, vy_p2);
      d.at(dx, dy) = c;
      d.at(dx + 1, dy) = hx;
      d.at(dx, dy + 1) = vy;
      d.at(dx + 1, dy + 1) = dgn;
    }
  }
}

// --- Dot-matrix (fixed 2x2 LCD-grid dimming mask) -------------------------
void filter_dotmatrix(const Uint8* srcPtr, Uint32 srcPitch, Uint8* dstPtr,
                      Uint32 dstPitch, int width, int height) {
  const SrcView s = src_view(srcPtr, srcPitch, width, height);
  const DstView d = dst_view(dstPtr, dstPitch);
  // Per-subpixel dimming: full, light, light, darker — the LCD "dot" look.
  for (int y = 0; y < height; ++y) {
    const int dy = y * 2;
    for (int x = 0; x < width; ++x) {
      const int dx = x * 2;
      const Uint16 c = s.at(x, y);
      d.at(dx, dy) = c;
      d.at(dx + 1, dy) = dim(c, 7, 8);
      d.at(dx, dy + 1) = dim(c, 7, 8);
      d.at(dx + 1, dy + 1) = dim(c, 3, 4);
    }
  }
}
