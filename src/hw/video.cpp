/* video.cpp — Gate Array video pixel path. See docs/hardware/video-device.md.
 * Decode formulas + palette taken from the legacy renderer; pixel-exact. */

#include "video.h"

#include <cstddef>
#include <cstring>
#include <new>

#include "asic.h"
#include "crtc.h"  // CrtcCharView — the batch renderer's per-char feed
#include "gate_array.h"

namespace {

// CPC hardware colours 0..31 → 8-bit RGB. Channel levels 0.0/0.5/1.0 →
// 0/128/255. 27 distinct colours; 5 indices duplicate (see gate-array hardware
// ref).
constexpr uint8_t kPalette[32][3] = {
    {128, 128, 128}, {128, 128, 128}, {0, 255, 128}, {255, 255, 128},
    {0, 0, 128},     {255, 0, 128},   {0, 128, 128}, {255, 128, 128},
    {255, 0, 128},   {255, 255, 128}, {255, 255, 0}, {255, 255, 255},
    {255, 0, 0},     {255, 0, 255},   {255, 128, 0}, {255, 128, 255},
    {0, 0, 128},     {0, 255, 128},   {0, 255, 0},   {0, 255, 255},
    {0, 0, 0},       {0, 0, 255},     {0, 128, 0},   {0, 128, 255},
    {128, 0, 128},   {128, 255, 128}, {128, 255, 0}, {128, 255, 255},
    {128, 0, 0},     {128, 0, 255},   {128, 128, 0}, {128, 128, 255},
};

uint8_t bit(uint8_t byte, int n) {
  return static_cast<uint8_t>((byte >> n) & 1);
}

}  // namespace

extern "C" {

uint16_t vid_byte_addr(uint16_t ma, uint8_t ra, uint8_t k) {
  return static_cast<uint16_t>(((ma & 0x3000) << 2) | ((ra & 0x07) << 11) |
                               ((ma & 0x03FF) << 1) | (k & 1));
}

int vid_decode(uint8_t mode, uint8_t b, uint8_t pens_out[8]) {
  // Pen-plane bit significance follows the real Gate Array (see
  // gfx_decode_byte): in mode 1 the LOW plane is bit7-k and the HIGH plane is
  // bit3-k; in mode 0 pixel 0's pen bits are byte bit7→P0, bit3→P1, bit5→P2,
  // bit1→P3 (pixel 1 uses bits 6/2/4/0). This matches the legacy M0Map table.
  // Getting bit3/bit5 (pen bits 1↔2) swapped mis-maps half the pens — e.g. a
  // grey mode-0 fill (0xF3 → pen 13) renders black (pen 11).
  switch (mode) {
    case 0:  // 2 px/byte, 4-bit pen
      pens_out[0] = static_cast<uint8_t>((bit(b, 1) << 3) | (bit(b, 5) << 2) |
                                         (bit(b, 3) << 1) | bit(b, 7));
      pens_out[1] = static_cast<uint8_t>((bit(b, 0) << 3) | (bit(b, 4) << 2) |
                                         (bit(b, 2) << 1) | bit(b, 6));
      return 2;
    case 1:  // 4 px/byte, 2-bit pen (high plane = bit3-k, low plane = bit7-k)
      for (int k = 0; k < 4; ++k)
        pens_out[k] =
            static_cast<uint8_t>((bit(b, 3 - k) << 1) | bit(b, 7 - k));
      return 4;
    case 3:  // Undocumented: 2 px/byte (mode-0 pixel layout) but only pens 0-3
             // — the two low pen planes, i.e. mode 1's colour range at mode 0's
             // width (gate-array.md §"Mode 3"). Same P0/P1 bits as mode 1.
      pens_out[0] = static_cast<uint8_t>((bit(b, 3) << 1) | bit(b, 7));
      pens_out[1] = static_cast<uint8_t>((bit(b, 2) << 1) | bit(b, 6));
      return 2;
    case 2:  // 8 px/byte, 1-bit pen, MSB first
    default:
      for (int k = 0; k < 8; ++k) pens_out[k] = bit(b, 7 - k);
      return 8;
  }
}

int vid_decode_lut(uint8_t mode, uint8_t b, uint8_t pens_out[8]) {
  // Same result as vid_decode(), read from a table built ONCE from it — so it is
  // byte-identical by construction (VideoDecodeLut.MatchesScalar guards the
  // accessor). vid_decode is pure in (mode, byte), so the whole domain (4 modes ×
  // 256 bytes) folds into a ~9 KB table; the Fast-tier batch renderer (Gate B7)
  // reads this instead of re-running the per-pixel bit-scatter, while the
  // faithful per-cycle path keeps calling vid_decode. C++11 guarantees the
  // static-local table is built exactly once, thread-safely.
  struct DecodedByte {
    uint8_t pens[8];
    uint8_t count;
  };
  static const struct Lut {
    DecodedByte e[4][256];
    Lut() {
      for (uint8_t m = 0; m < 4; ++m)
        for (int i = 0; i < 256; ++i) {
          uint8_t pens[8] = {0};
          const int n = vid_decode(m, static_cast<uint8_t>(i), pens);
          std::memcpy(e[m][i].pens, pens, sizeof(pens));
          e[m][i].count = static_cast<uint8_t>(n);
        }
    }
  } lut;
  // mode 0/1/3 are distinct; mode 2 and any mode > 3 share mode-2 decode, exactly
  // mirroring vid_decode's default arm (the 2-bit mode register never exceeds 3).
  const DecodedByte& d = lut.e[mode > 3 ? 2 : mode][b];
  std::memcpy(pens_out, d.pens, sizeof(d.pens));
  return d.count;
}

void vid_hw_rgb(uint8_t colour, uint8_t* r, uint8_t* g, uint8_t* b) {
  const uint8_t* c = kPalette[colour & 0x1F];
  *r = c[0];
  *g = c[1];
  *b = c[2];
}

int vid_render_line(const uint8_t* ram, uint8_t mode, const uint8_t* ink,
                    uint16_t ma_base, uint8_t ra, uint8_t chars, uint8_t* out) {
  int px = 0;
  for (uint8_t ch = 0; ch < chars; ++ch) {
    const uint16_t ma = static_cast<uint16_t>((ma_base + ch) & 0x3FFF);
    for (uint8_t k = 0; k < 2; ++k) {  // 2 bytes per character
      uint8_t pens[8];
      const int n = vid_decode(mode, ram[vid_byte_addr(ma, ra, k)], pens);
      for (int p = 0; p < n; ++p) {
        vid_hw_rgb(ink[pens[p]], &out[px * 3], &out[(px * 3) + 1],
                   &out[(px * 3) + 2]);
        px++;
      }
    }
  }
  return px;
}

int vid_px_per_char(uint8_t mode) {
  // Mode 3 shares mode 0's 160-pixel width (4 px/char); mode 1 is 8, mode 2 16.
  // NOLINTNEXTLINE(readability-avoid-nested-conditional-operator): nested conditional kept intentionally; no clang-tidy auto-fix
  return (mode == 0 || mode == 3) ? 4 : mode == 1 ? 8 : 16;
}

void vid_render_frame(const uint8_t* ram, uint8_t mode, const uint8_t* ink,
                      uint16_t ma_start, uint8_t r1, uint8_t r6, uint8_t r9,
                      uint8_t* fb) {
  const int width = r1 * vid_px_per_char(mode);
  int y = 0;
  for (uint8_t row = 0; row < r6; ++row) {
    const uint16_t ma_base =
        static_cast<uint16_t>((ma_start + (row * r1)) & 0x3FFF);
    for (uint8_t ra = 0; ra <= r9; ++ra) {
      vid_render_line(ram, mode, ink, ma_base, ra, r1,
                      fb + (static_cast<size_t>(y) * width * 3));
      y++;
    }
  }
}

}  // extern "C"

// --- Live video Device
// ---------------------------------------------------------

namespace {

// The monitor's visible window, in CRTC character columns, and the vertical
// back porch (scanlines from the VSYNC leading edge to the top of the visible
// window). The horizontal beam re-times to each HSYNC, the vertical beam to
// each VSYNC — so moving R2/R7 (or extending DISPEN into overscan)
// shifts/reshapes the picture exactly as on real hardware. Chars are normalized
// to 16 px (mode-2 native), so a mode-0 pixel is physically 4x a mode-2 pixel
// and the border width is mode-independent.
constexpr int kVisChars = 48;
constexpr int kVBackPorch = 36;

struct video_state {
  const Device* gate_array = nullptr;
  const Device* asic = nullptr;  // Plus mode: 12-bit palette + sprites
  uint8_t* fb = nullptr;
  int fb_w = 0, fb_h = 0;
  // Classic batch-render cache (F8 R11) — derived data, reset at every
  // video_batch_cells entry, so it lives HERE, above beam_col: the snapshot
  // format serializes [beam_col, end) only (kVideoLogicalOff) and a cache
  // must never enter it. Valid within one batch call because the inks are
  // call-constant (catch-up-then-apply); keyed on (mode, byte) because the
  // mode latch moves per chain-stamped view.
  uint8_t cc_rgb[4][256][24] = {};  // one display byte = 8 normalized px, 24 B
  uint8_t cc_valid[4][32] = {};     // per-(mode, byte) fill bitmap
  uint8_t cc_border[48] = {};       // one border cell (16 px), built per call
  uint32_t snap_gen = 0xFFFFFFFF;   // asic_vid_gen of the held line snapshot
                                    // (F8 R17); sentinel = no snapshot held
  int beam_col = 0;    // visible char column of the beam (0..kVisChars-1)
  int beam_row = 0;    // visible scanline of the beam (0..fb_h-1)
  uint8_t fetch0 = 0;  // byte 0 of the character, latched off the RAM fetch bus
  bool hsync_prev = false;
  bool vsync_prev = false;
  uint32_t frames = 0;

  // Plus mode, refreshed once per scanline (plus_refresh_line) so mid-frame
  // register changes take effect on the next line — the raster-trick path.
  bool plus_active = false;
  int disp_char = 0;          // active char index within the current line
  int first_active_row = -1;  // beam_row of the frame's first active line
  uint16_t spr_x[16] = {0}, spr_y[16] = {0};
  uint8_t spr_mx[16] = {0}, spr_my[16] = {0};
  uint8_t pal_r[32] = {0}, pal_g[32] = {0}, pal_b[32] = {0}, pal_set[32] = {0};
  uint8_t hscroll = 0;  // horizontal soft scroll (pixels)
  uint8_t prev_pen[16] = {
      0};  // previous cell's background pens (hscroll carry)

  // Per-line sprite cull (F8): the sprite snapshot above is line-constant by
  // contract, so the Y-overlap test hoists out of render_cell_plus into
  // plus_refresh_line — the per-cell path only X-filters these candidates.
  // Rebuilt every HSYNC fall; ascending sprite order keeps sprite-0 priority.
  int16_t lc_sx[16] = {0};    // candidate left edge (10-bit X fits)
  int16_t lc_xend[16] = {0};  // candidate right edge: sx + 16·mx
  int8_t lc_id[16] = {0};     // sprite index
  int8_t lc_row[16] = {0};    // sprite pixel row at this scanline
  int8_t lc_shift[16] = {0};  // log2(mx) — the ×mx X shift
  int8_t lc_n = 0;
  // Programmed-entry RGB, expanded once per line (F8): a pal_set entry's
  // colour is a pure function of the line snapshot (never the GA inks), so
  // the ×17 expansion hoists out of the pixel loop. Unset entries keep the
  // per-cell classic-ink fallback (the inks may move between cells).
  uint8_t line_rgb[32][3] = {};
};

video_state* vself(void* self) { return static_cast<video_state*>(self); }

// Refresh the per-scanline Plus snapshot (sprite attributes + 12-bit palette).
// Reading once per line means mid-frame register changes land on the next line
// — the pin-level analog of a raster split.
void plus_refresh_line(video_state* v) {
  const bool was_plus = v->plus_active;
  v->plus_active = false;
  if (!v->asic || !asic_vid_active(v->asic)) {
    // Deactivation edge: hand the (mode, byte) cache back to the classic
    // batch path clean — its per-call reset only runs at call entry — and
    // drop the held snapshot (reactivation must re-read).
    if (was_plus) {
      std::memset(v->cc_valid, 0, sizeof(v->cc_valid));
      v->snap_gen = 0xFFFFFFFF;
    }
    return;
  }
  v->plus_active = true;
  // The register half of the refresh is a pure function of the ASIC's
  // snapshot inputs: an unchanged generation ⟹ byte-identical registers
  // (asic.h), so the re-read is idempotent — skip it, KEEPING the
  // pal_set-pure byte cache (its entries are functions of this same
  // unchanged snapshot). Exact in both execution shapes (each runs this at
  // every HSYNC fall). The sprite Y-cull below is NOT skippable — it also
  // depends on the line (beam_row) — and runs every refresh.
  const uint32_t gen = asic_vid_gen(v->asic);
  if (!was_plus || gen != v->snap_gen) {
    v->snap_gen = gen;
    // New snapshot: every cached pal_set-pure byte expansion (F8 R12) is a
    // function of the registers refreshed below — reset it with them, in
    // BOTH execution shapes.
    std::memset(v->cc_valid, 0, sizeof(v->cc_valid));
    asic_vid_scroll(v->asic, &v->hscroll, nullptr);  // vscroll: the CRTC's job
    for (int i = 0; i < 16; ++i)
      asic_vid_sprite_attr(v->asic, i, &v->spr_x[i], &v->spr_y[i],
                           &v->spr_mx[i], &v->spr_my[i]);
    for (int e = 0; e < 32; ++e) {
      const uint16_t rgb = asic_vid_palette(v->asic, e);
      v->pal_r[e] = static_cast<uint8_t>((rgb >> 8) & 0x0F);
      v->pal_g[e] = static_cast<uint8_t>((rgb >> 4) & 0x0F);
      v->pal_b[e] = static_cast<uint8_t>(rgb & 0x0F);
      v->pal_set[e] = static_cast<uint8_t>(asic_vid_palette_set(v->asic, e));
      if (v->pal_set[e]) {  // ×17 expansion — plus_index_rgb's set branch
        v->line_rgb[e][0] = static_cast<uint8_t>(v->pal_r[e] * 17);
        v->line_rgb[e][1] = static_cast<uint8_t>(v->pal_g[e] * 17);
        v->line_rgb[e][2] = static_cast<uint8_t>(v->pal_b[e] * 17);
      }
    }
  }
  // The line's sprite Y-cull, hoisted from render_cell_plus (the register
  // snapshot cannot move until the next refresh) — rebuilt EVERY line: py
  // moves with beam_row. px_y is line-constant: when first_active_row is
  // established mid-line it lands on THIS beam_row, and
  // beam_row - first_active_row is 0 either way.
  const int py =
      (v->first_active_row < 0) ? 0 : v->beam_row - v->first_active_row;
  v->lc_n = 0;
  for (int i = 0; i < 16; ++i) {
    const int mx = v->spr_mx[i], my = v->spr_my[i];
    if (mx == 0 || my == 0) continue;                // disabled
    const int sy = v->spr_y[i];
    if (py < sy || py >= sy + (16 * my)) continue;  // scanline outside sprite
    const int sx = v->spr_x[i];
    v->lc_sx[v->lc_n] = static_cast<int16_t>(sx);
    v->lc_xend[v->lc_n] = static_cast<int16_t>(sx + (16 * mx));
    v->lc_id[v->lc_n] = static_cast<int8_t>(i);
    v->lc_row[v->lc_n] = static_cast<int8_t>((py - sy) / my);
    v->lc_shift[v->lc_n] =
        // NOLINTNEXTLINE(readability-avoid-nested-conditional-operator): nested conditional kept intentionally; no clang-tidy auto-fix
        static_cast<int8_t>((mx == 4) ? 2 : (mx == 2 ? 1 : 0));
    v->lc_n++;
  }
}

// Map a final palette index (0..31) to RGB. A programmed 12-bit entry expands
// its 4-bit components to 8-bit (×17: 0x0→0, 0xF→255); an unprogrammed screen/
// border entry (≤16) falls through to the classic ink colour underneath; an
// unprogrammed sprite entry is black.
void plus_index_rgb(const video_state* v, const GateArrayRegs* g, int index,
                    uint8_t* r, uint8_t* gg, uint8_t* b) {
  if (v->pal_set[index]) {
    *r = static_cast<uint8_t>(v->pal_r[index] * 17);
    *gg = static_cast<uint8_t>(v->pal_g[index] * 17);
    *b = static_cast<uint8_t>(v->pal_b[index] * 17);
  } else if (index <= 16) {
    vid_hw_rgb(g->ink[index], r, gg, b);
  } else {
    *r = *gg = *b = 0;
  }
}

// Expand a character cell's two display bytes into 16 background pen indices
// (one per mode-2 pixel column; each byte spans 8 columns).
void plus_decode_cell(uint8_t mode, uint8_t byte0, uint8_t byte1,
                      uint8_t cell_pen[16]) {
  const uint8_t bytes[2] = {byte0, byte1};
  int filled = 0;
  for (unsigned char const byte : bytes) {
    uint8_t pens[8];
    const int n = vid_decode(mode, byte, pens);
    const int pw = 8 / (n ? n : 1);
    for (int p = 0; p < n; ++p)
      for (int q = 0; q < pw && filled < 16; ++q) cell_pen[filled++] = pens[p];
  }
  while (filled < 16) cell_pen[filled++] = 0;
}

// F8: the per-(mode, byte) pen tables, built once FROM vid_decode and
// plus_decode_cell — the decode rules keep their single definition; this is
// only their memoization (the F8 profiles put the per-bit extraction and the
// per-cell expansion at ~3% of the frame each).
struct VidDecodeLut {
  uint8_t pens[4][256][8] = {};
  uint8_t count[4] = {};
  uint8_t cols[4][256][8] = {};  // Plus: byte → its 8 mode-2 background cols
  VidDecodeLut() {
    for (int m = 0; m < 4; ++m)
      for (int b = 0; b < 256; ++b) {
        count[m] = static_cast<uint8_t>(vid_decode(
            static_cast<uint8_t>(m), static_cast<uint8_t>(b), pens[m][b]));
        uint8_t cell[16];
        plus_decode_cell(static_cast<uint8_t>(m), static_cast<uint8_t>(b), 0,
                         cell);  // a byte's expansion is byte0's half
        std::memcpy(cols[m][b], cell, 8);
      }
  }
};

const VidDecodeLut& vid_lut() {
  static const VidDecodeLut lut;  // magic-static: built on first render
  return lut;
}

// Plus-mode character cell: border, or active display with per-pixel 12-bit
// palette lookup and sprite compositing — the ONE definition both execution
// shapes share (`mode` and `dispen` are parameters because the per-cycle
// caller reads the live GA latch and bus while the batch caller reads the
// chain-stamped view — the latch moves at HSYNCs INSIDE a batch run, so a
// run-peeked mode goes stale: beads-agha).
//
// Pixels write unchecked off one row base — the same caller contract as
// render_cell_classic below (only `visible` cells reach here).
// Paint ONE display byte through the Plus line snapshot (8 normalized px =
// 24 bytes). Returns true when every pixel resolved through a programmed
// (pal_set) entry — then the bytes are a pure function of (mode, byte) and
// the line snapshot, cacheable until the next plus_refresh_line in EITHER
// execution shape. Unset entries read the live GA inks, which move per cell
// in the per-cycle shape — those paints are never cached.
bool paint_byte_plus(video_state* v, const GateArrayRegs* g, uint8_t m,
                     uint8_t byte, uint8_t* px) {
  const uint8_t* pens = vid_lut().cols[m][byte];
  bool pure = true;
  for (int p = 0; p < 8; ++p) {
    const uint8_t index = pens[p];
    if (v->pal_set[index]) {
      px[0] = v->line_rgb[index][0];
      px[1] = v->line_rgb[index][1];
      px[2] = v->line_rgb[index][2];
    } else {
      pure = false;
      plus_index_rgb(v, g, index, &px[0], &px[1], &px[2]);
    }
    px += 3;
  }
  return pure;
}

void render_cell_plus(video_state* v, const GateArrayRegs* g, uint8_t mode,
                      bool dispen, uint8_t byte1, int x0, int char_w) {
  if (dispen && v->first_active_row < 0)
    v->first_active_row = v->beam_row;  // sprite Y origin: first active line
  uint8_t* px =
      v->fb + (((static_cast<size_t>(v->beam_row) * v->fb_w) + x0) * 3);
  if (!dispen) {  // border — palette entry 16 (no sprites over border)
    uint8_t r, gg, b;
    if (v->pal_set[16]) {  // line-expanded (the common programmed case)
      r = v->line_rgb[16][0];
      gg = v->line_rgb[16][1];
      b = v->line_rgb[16][2];
    } else {
      plus_index_rgb(v, g, 16, &r, &gg, &b);
    }
    for (int dx = 0; dx < char_w; ++dx) {
      px[0] = r;
      px[1] = gg;
      px[2] = b;
      px += 3;
    }
    return;
  }
  const VidDecodeLut& lut = vid_lut();
  const uint8_t m = mode & 3;  // the GA latch is 2 bits wide
  const int px_xbase = v->disp_char * 16;  // active-relative X of the cell
  const int hs = v->hscroll;  // horizontal soft scroll (background only)

  // Sprite cull, X half — the Y half ran once per line in plus_refresh_line
  // (the snapshot is line-constant, so this is byte-identical to the old
  // per-cell 16-sprite scan). Ascending order preserves sprite-0-wins.
  int cand[16];
  int ncand = 0;
  const int cell_x1 = px_xbase + char_w;
  for (int c = 0; c < v->lc_n; ++c) {
    if (cell_x1 <= v->lc_sx[c] || px_xbase >= v->lc_xend[c]) continue;
    cand[ncand++] = c;
  }

  // Fast path (F8 R12): no sprite overlaps this cell and no soft scroll —
  // the 16 pixels are two straight byte paints. pal_set-pure halves cache
  // per line (plus_refresh_line resets cc_valid with every new snapshot);
  // impure halves (live-ink fallback) repaint every cell. hscroll is
  // line-constant, so with hs == 0 prev_pen is never read before the next
  // HSYNC-fall reset — its carry update is skipped.
  if (hs == 0 && ncand == 0 && char_w == 16) {
    const uint8_t bytes2[2] = {v->fetch0, byte1};
    for (int k = 0; k < 2; ++k) {
      const uint8_t byte = bytes2[k];
      uint8_t* slot = v->cc_rgb[m][byte];
      const uint8_t bit = static_cast<uint8_t>(1u << (byte & 7));
      if ((v->cc_valid[m][byte >> 3] & bit) == 0) {
        if (paint_byte_plus(v, g, m, byte, slot))
          v->cc_valid[m][byte >> 3] |= bit;
      }
      std::memcpy(px + (static_cast<size_t>(k) * 24), slot, 24);
    }
    v->disp_char++;
    return;
  }

  uint8_t cell_pen[16];
  std::memcpy(cell_pen, lut.cols[m][v->fetch0], 8);
  std::memcpy(cell_pen + 8, lut.cols[m][byte1], 8);

  // Palette-index → RGB, resolved lazily once per distinct index per cell
  // (pal_* is a per-line snapshot and the inks are slice-constant, so the
  // mapping cannot move inside a cell).
  uint32_t rgb_have = 0;
  uint8_t rgb_r[32], rgb_g[32], rgb_b[32];
  for (int lx = 0; lx < char_w; ++lx) {
    // hscroll shifts the BACKGROUND right by hs pixels, pulling from the
    // previous cell at the left edge; the sprite layer is not scrolled.
    const int src = lx - hs;
    int index = (src >= 0) ? cell_pen[src < 16 ? src : 15]
                           : v->prev_pen[(16 + src) & 0x0F];
    for (int cc = 0; cc < ncand; ++cc) {
      // The per-pixel X test stays (the cell overlaps the sprite, but this
      // pixel may fall outside it); first opaque candidate wins (sprite 0
      // keeps the highest priority).
      const int c = cand[cc];
      const int dx = (px_xbase + lx) - v->lc_sx[c];
      if (dx < 0) continue;
      const int col = dx >> v->lc_shift[c];
      if (col >= 16) continue;
      const uint8_t s =
          asic_vid_sprite_pixel(v->asic, v->lc_id[c], col, v->lc_row[c]);
      if (s) {
        index = 16 + s;
        break;
      }
    }
    if (v->pal_set[index]) {  // line-expanded (the common programmed case)
      px[0] = v->line_rgb[index][0];
      px[1] = v->line_rgb[index][1];
      px[2] = v->line_rgb[index][2];
      px += 3;
      continue;
    }
    if (((rgb_have >> index) & 1u) == 0) {
      plus_index_rgb(v, g, index, &rgb_r[index], &rgb_g[index], &rgb_b[index]);
      rgb_have |= 1u << static_cast<unsigned>(index);
    }
    px[0] = rgb_r[index];
    px[1] = rgb_g[index];
    px[2] = rgb_b[index];
    px += 3;
  }
  for (int k = 0; k < 16; ++k) v->prev_pen[k] = cell_pen[k];  // hscroll carry
  v->disp_char++;
}

// The classic (non-Plus) cell paint — the ONE definition both execution
// shapes share: active display (two decoded display bytes through the inks)
// or the border pen. `mode` is a parameter because the two callers source it
// differently (per-cycle: the GA's live latch; batch: the chain-stamped view).
//
// Pixels write unchecked off one row base: both callers only paint `visible`
// cells, which pins beam_row ∈ [0, fb_h) and [x0, x0 + char_w] ⊆ [0, fb_w]
// (x0 = beam_col·char_w with beam_col < kVisChars and char_w = fb_w/kVisChars).
// The F8 profile showed fill_run's per-pixel row multiply + bounds pair as the
// single hottest instruction stream of the whole tier.
// Paint ONE display byte through the inks: n decoded pens, each pw pixels
// wide, RGB written left to right — the single pixel-layout definition. Both
// render_cell_classic (straight to the framebuffer) and the batch renderer's
// per-call cache fill (into a 24-byte slot) run this exact code, so the
// cached path is byte-identical by construction.
void paint_byte_classic(const GateArrayRegs* g, uint8_t m, uint8_t byte,
                        int pw, uint8_t* px) {
  const VidDecodeLut& lut = vid_lut();
  const int n = lut.count[m];
  const uint8_t* pens = lut.pens[m][byte];
  for (int p = 0; p < n; ++p) {
    uint8_t r, gg, b;
    vid_hw_rgb(g->ink[pens[p]], &r, &gg, &b);
    for (int w = 0; w < pw; ++w) {
      px[0] = r;
      px[1] = gg;
      px[2] = b;
      px += 3;
    }
  }
}

void render_cell_classic(video_state* v, const GateArrayRegs* g, uint8_t mode,
                         bool dispen, uint8_t byte0, uint8_t byte1, int x0,
                         int char_w) {
  uint8_t* px =
      v->fb + (((static_cast<size_t>(v->beam_row) * v->fb_w) + x0) * 3);
  if (dispen) {  // active display: decode the two fetched bytes
    const VidDecodeLut& lut = vid_lut();
    const uint8_t m = mode & 3;  // the GA latch is 2 bits wide
    const int n = lut.count[m];
    const int pw = (char_w / 2) / (n ? n : 1);  // pixel width within a byte
    paint_byte_classic(g, m, byte0, pw, px);
    paint_byte_classic(g, m, byte1, pw, px + (static_cast<size_t>(n) * pw * 3));
  } else {  // visible but not active → the border (pen 16)
    uint8_t r, gg, b;
    vid_hw_rgb(g->ink[16], &r, &gg, &b);
    for (int w = 0; w < char_w; ++w) {
      px[0] = r;
      px[1] = gg;
      px[2] = b;
      px += 3;
    }
  }
}

// Paint the current character cell: the two display bytes fetched off the RAM
// bus (active display), or the border pen (visible but DISPEN low).
void render_char(video_state* v, const Bus* in, uint8_t byte1) {
  const int char_w = v->fb_w / kVisChars;  // 16 for a 768-wide canvas
  const bool visible = !in->vid.hsync && !in->vid.vsync && v->beam_col >= 0 &&
                       v->beam_col < kVisChars && v->beam_row >= 0 &&
                       v->beam_row < v->fb_h;
  if (!visible) return;
  GateArrayRegs g{};
  ga_peek(v->gate_array, &g);
  const int x0 = v->beam_col * char_w;
  if (v->plus_active) {  // Plus: 12-bit palette + per-pixel sprite compositing
    render_cell_plus(v, &g, g.mode, in->vid.dispen, byte1, x0, char_w);
    return;
  }
  render_cell_classic(v, &g, g.mode, in->vid.dispen, v->fetch0, byte1, x0,
                      char_w);
}

void video_tick(void* self, const Bus* __restrict in, Bus* __restrict out) {
  (void)out;
  video_state* v = vself(self);
  if (!v->gate_array || !v->fb) return;

  const bool hsync = in->vid.hsync, vsync = in->vid.vsync;
  const bool hs_rise = hsync && !v->hsync_prev;
  const bool hs_fall = !hsync && v->hsync_prev;
  const bool vs_rise = vsync && !v->vsync_prev;
  v->hsync_prev = hsync;
  v->vsync_prev = vsync;

  if (vs_rise) {
    v->frames++;
    v->beam_row = -kVBackPorch;
    v->first_active_row = -1;  // sprite Y origin re-established each frame
  }  // new frame
  else if (hs_rise) {
    v->beam_row++;
  }  // new scanline
  if (hs_fall) {
    v->beam_col = 0;   // retrace → left edge
    v->disp_char = 0;  // active-char index restarts each line
    for (unsigned char& k : v->prev_pen) k = 0;  // hscroll carry resets
    plus_refresh_line(v);  // latch this line's sprites + palette + hscroll
  }

  // The GA fetches the character's two display bytes on the RAM bus during the
  // video slots (drive 12 → data on the bus with phase 13; drive 14 → phase
  // 15). Latch byte 0, render the whole cell when byte 1 lands, then advance
  // the beam.
  if (in->clk.phase == 13) v->fetch0 = in->ram.data;
  if (in->clk.phase == 15) {
    render_char(v, in, in->ram.data);
    v->beam_col++;
  }
}

void video_reset(void* self) {
  video_state* v = vself(self);
  v->beam_col = 0;
  v->beam_row = 0;
  v->hsync_prev = v->vsync_prev = false;
  v->frames = 0;
  v->plus_active = false;
  v->disp_char = 0;
  v->first_active_row = -1;
  // fb / device pointers persist (set by video_attach / video_attach_asic).
}

// video_state leads with the caller-owned wiring (gate_array/asic/fb pointers +
// fb dims, set by video_attach*); everything from beam_col onward is logical
// beam/plus state. Serialize that tail only — pointer-free by construction, and
// future logical fields are picked up automatically as long as they're declared
// after the wiring block (invariant asserted in the round-trip test).
constexpr size_t kVideoLogicalOff = offsetof(video_state, beam_col);
constexpr size_t kVideoLogicalLen = sizeof(video_state) - kVideoLogicalOff;
size_t video_dev_state_size(const void* /*unused*/) {
  return kVideoLogicalLen + 1;
}
void video_save(const void* self, void* buf) {
  const video_state* v = static_cast<const video_state*>(self);
  uint8_t* b = static_cast<uint8_t*>(buf);
  b[0] = 1;
  std::memcpy(b + 1, &v->beam_col, kVideoLogicalLen);
}
void video_load(void* self, const void* buf) {
  video_state* v = static_cast<video_state*>(self);
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  if (b[0] != 1) return;
  std::memcpy(&v->beam_col, b + 1, kVideoLogicalLen);  // wiring untouched
}

}  // namespace

extern "C" {

size_t video_state_size(void) { return sizeof(video_state); }

Device video_init(void* storage) {
  // NOLINTNEXTLINE(misc-const-correctness): pointer is stored in Device::self (void*), cannot be const
  video_state *v = new (storage) video_state();
  return Device{
      v,          "video",   video_tick, video_reset, video_dev_state_size,
      video_save, video_load};
}

void video_attach(const Device* vid, const Device* gate_array, uint8_t* fb,
                  int w, int h) {
  video_state* v = static_cast<video_state*>(vid->self);
  v->gate_array = gate_array;
  v->fb = fb;
  v->fb_w = w;
  v->fb_h = h;
}

void video_attach_asic(const Device* vid, const Device* asic) {
  static_cast<video_state*>(vid->self)->asic = asic;
}

void video_peek(const Device* vid, VideoRegs* out) {
  const video_state* v = static_cast<const video_state*>(vid->self);
  out->mode = 0;
  out->frames = v->frames;
  out->cur_row = v->beam_row;
}

void video_batch_cells(const Device* vid, const uint8_t* ram,
                       const CrtcCharView* views, int count) {
  video_state* v = vself(vid->self);
  if (!v->gate_array || !v->fb || count <= 0) return;
  // Inks are constant across the run — any write catches the renderer up
  // first (catch-up-then-apply), so one peek covers every cell. The screen
  // MODE is not: its latch moves at HSYNCs inside the run, which is why each
  // view carries the chain-stamped value.
  GateArrayRegs g{};
  ga_peek(v->gate_array, &g);
  const int char_w = v->fb_w / kVisChars;
  // Classic-cell cache (F8 R11): with the inks call-constant, (mode, byte) →
  // 24-byte RGB is pure for this whole run — the fill runs paint_byte_classic
  // (the one pixel-layout definition), so hits are byte-identical to the
  // uncached paint. Geometry-gated to the native 16-px cell; other canvas
  // widths (none ship) fall back to the direct painter.
  const bool cc_on = !v->plus_active && char_w == 16;
  if (cc_on) {
    std::memset(v->cc_valid, 0, sizeof(v->cc_valid));
    uint8_t r, gg, b;
    vid_hw_rgb(g.ink[16], &r, &gg, &b);
    for (int w = 0; w < 16; ++w) {
      v->cc_border[(w * 3) + 0] = r;
      v->cc_border[(w * 3) + 1] = gg;
      v->cc_border[(w * 3) + 2] = b;
    }
  }
  for (int i = 0; i < count; ++i) {
    const CrtcCharView& view = views[i];
    if (view.edges) {  // rare (≤3 chars per scanline carry an edge)
      // Beam movement — video_tick's edge rules verbatim (VSYNC wins).
      if (view.edges & (1u << CRTC_EDGE_VSYNC_RISE)) {
        v->frames++;
        v->beam_row = -kVBackPorch;
        v->first_active_row = -1;
      } else if (view.edges & (1u << CRTC_EDGE_HSYNC_RISE)) {
        v->beam_row++;
      }
      if (view.edges & (1u << CRTC_EDGE_HSYNC_FALL)) {
        v->beam_col = 0;   // retrace → left edge
        v->disp_char = 0;  // active-char index restarts each line
        for (unsigned char& k : v->prev_pen) k = 0;  // hscroll carry reset
        plus_refresh_line(v);  // latch this line's sprites + palette + hscroll
                               // — catch-up-then-apply keeps this snapshot
                               // exactly as per-cycle: a write forces the
                               // pending cells (this hs_fall among them)
                               // rendered first
      }
    }

    const bool visible = !(view.levels & CRTC_LVL_HSYNC) &&
                         !(view.levels & CRTC_LVL_VSYNC) && v->beam_col >= 0 &&
                         v->beam_col < kVisChars && v->beam_row >= 0 &&
                         v->beam_row < v->fb_h;
    if (visible) {
      // The hardware fetches every microsecond regardless, but the byte-0
      // latch is only OBSERVABLE at a drain boundary — the assignment after
      // this loop reproduces the final latch, so per-view fetches (address
      // swizzle + load) run only for cells whose paint consumes the bytes:
      // the active-display ones.
      const bool active = (view.levels & CRTC_LVL_DISPEN) != 0;
      uint8_t byte0 = 0, byte1 = 0;
      if (active) {
        byte0 = ram[vid_byte_addr(view.ma, view.ra, 0)];
        byte1 = ram[vid_byte_addr(view.ma, view.ra, 1)];
      }
      if (v->plus_active) {
        if (active) v->fetch0 = byte0;  // render_cell_plus reads the latch
        render_cell_plus(v, &g, view.mode, active, byte1,
                         v->beam_col * char_w, char_w);
      } else if (cc_on) {
        uint8_t* px = v->fb + (((static_cast<size_t>(v->beam_row) * v->fb_w) +
                                (static_cast<size_t>(v->beam_col) * 16)) *
                               3);
        if (active) {
          const uint8_t m = view.mode & 3;
          const uint8_t bytes[2] = {byte0, byte1};
          for (int k = 0; k < 2; ++k) {
            const uint8_t byte = bytes[k];
            uint8_t* slot = v->cc_rgb[m][byte];
            const uint8_t bit = static_cast<uint8_t>(1u << (byte & 7));
            if ((v->cc_valid[m][byte >> 3] & bit) == 0) {
              paint_byte_classic(&g, m, byte, 8 / vid_lut().count[m], slot);
              v->cc_valid[m][byte >> 3] |= bit;
            }
            std::memcpy(px + (static_cast<size_t>(k) * 24), slot, 24);
          }
        } else {
          std::memcpy(px, v->cc_border, 48);
        }
      } else {
        render_cell_classic(v, &g, view.mode, active, byte0, byte1,
                            v->beam_col * char_w, char_w);
      }
    }
    v->beam_col++;
  }
  // The byte-0 latch at the batch edge — what a per-cycle run's every-µs
  // fetch leaves in the device. Only this final value is observable (the
  // next drain starts from the next view), so it stands in for the per-view
  // stores elided above.
  v->fetch0 = ram[vid_byte_addr(views[count - 1].ma, views[count - 1].ra, 0)];
}

void video_batch_set_sync(const Device* vid, int hsync, int vsync) {
  video_state* v = vself(vid->self);
  v->hsync_prev = hsync != 0;
  v->vsync_prev = vsync != 0;
}

}  // extern "C"
