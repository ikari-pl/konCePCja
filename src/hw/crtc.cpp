/* crtc.cpp — the CPC CRTC (6845) character-timing engine, types 0-3.
 * See docs/hardware/crtc-device.md. Rendering lives elsewhere.
 *
 * The CPC shipped with different 6845 variants; software detects and exploits
 * the differences. Modeled here (the program-visible set, mirrored from the
 * reference implementation): per-type register readability, the type-1 status
 * register, the R3 sync-width interpretations, and R8 DISPTMG skew/disable on
 * types 0/3. */

#include "crtc.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <new>

#include "asic.h"

namespace {

struct crtc_state {
  uint8_t reg[18] = {0};
  uint8_t reg_select = 0;
  uint8_t type =
      0;  // 0=HD6845S, 1=UM6845R, 2=MC6845, 3=AMS40489 (hardware strap)
  uint8_t hcc = 0;  // horizontal char counter — 8-BIT, wraps at 256 (quirk §7b)
  uint8_t ra = 0;   // raster within char row — 5-bit counter (0..31 wrap)
  uint8_t vcc = 0;  // char-row counter — 7-bit counter (0..127 wrap)
  uint8_t hsw = 0;  // HSYNC width counter (chars)
  uint8_t vsw = 0;  // VSYNC width counter (scanlines)
  uint8_t vta = 0;  // vertical-total-adjust scanline counter (R5)
  bool in_hsync = false;
  bool in_vsync = false;
  bool in_vta = false;
  bool r7match =
      false;  // "vcc == R7 at the last check" — edge-detects VSYNC starts
  bool dispen = false;
  bool lpen_prev = false;  // previous cycle's LPEN strobe (edge detect)
  uint16_t ma_row = 0;     // char-row base address
  uint16_t ma = 0;         // current memory address

  // Plus split screen (Amstrad Plus / ASIC "type-4"). The ASIC decodes
  // split_line/split_addr from &6800-&6803; the CRTC reads them via an attach
  // reference and, at the frame scanline == split_line, swaps the display base
  // to split_addr for the rest of the frame (crtc.cpp:1342). Snapshotted per
  // frame so the split is stable within a frame.
  const Device* asic = nullptr;
  uint16_t scanline = 0;         // frame scanline counter (reset at frame top)
  bool plus_split = false;       // ASIC plugged and split_line != 0 this frame
  uint16_t split_sl_snap = 0;    // latched split_line
  uint16_t split_addr_snap = 0;  // latched split_addr
};

// Per-register write masks — the 6845's registers are only as wide as the
// datasheet says; unused high bits are not stored (R4/R6/R7 are 7-bit,
// R5/R9/R11 5-bit, R10/R14 cursor 7/6-bit, R12 6-bit). Writing 0xFF to R4
// latches 0x7F.
constexpr uint8_t kRegWMask[16] = {
    0xFF, 0xFF, 0xFF, 0xFF,  // R0-R3
    0x7F, 0x1F, 0x7F, 0x7F,  // R4-R7
    0xFF, 0x1F, 0x7F, 0x1F,  // R8-R11
    0x3F, 0xFF, 0x3F, 0xFF,  // R12-R15
};

crtc_state* self_of(void* self) { return static_cast<crtc_state*>(self); }

// --- Per-type behaviour helpers (docs/hardware/crtc-device.md §5)
// -------------

// HSYNC width from R3 bits 3..0. Width 0: types 2/3 treat it as 16; on types
// 0/1 it means NO HSYNC at all (and therefore no GA raster interrupts).
uint8_t hsync_width(const crtc_state* c) {
  uint8_t w = static_cast<uint8_t>(c->reg[3] & 0x0F);
  if (w == 0 && (c->type == 2 || c->type == 3)) w = 16;
  return w;
}

// VSYNC width: fixed 16 scanlines on types 1/2 (R3 bits 7..4 ignored); from R3
// bits 7..4 on types 0/3, where 0 means 16.
uint8_t vsync_width(const crtc_state* c) {
  if (c->type == 1 || c->type == 2) return 16;
  const uint8_t w = static_cast<uint8_t>(c->reg[3] >> 4);
  return w ? w : 16;
}

// R8 DISPTMG skew (types 0/3 only): bits 5..4 delay the display window by 0..2
// chars; value 3 disables the display entirely. Types 1/2 ignore R8 here.
// Returns the shift, or 0xFF for display-off. Plus extend_border adds one char.
uint8_t disp_skew(const crtc_state* c) {
  if (c->type == 1 || c->type == 2) return 0;
  const uint8_t s = static_cast<uint8_t>((c->reg[8] >> 4) & 3);
  if (s == 3) return 0xFF;
  uint8_t skew = s;
  if (c->asic && asic_vid_active(c->asic) && asic_vid_extend_border(c->asic))
    skew++;
  return skew;
}

// Register read (&BFxx on all types; &BExx too on type 3). Readable windows:
// types 0/3: R12-R17; types 1/2: R14-R17 (R12/13 write-only); type 1
// additionally answers 0xFF for R31. Everything else reads 0.
uint8_t read_reg(const crtc_state* c) {
  const uint8_t reg = c->reg_select;
  const uint8_t lo = (c->type == 1 || c->type == 2) ? 14 : 12;
  if (reg >= lo && reg <= 17) return c->reg[reg];
  if (c->type == 1 && reg == 31) return 0xFF;
  return 0;
}

// Type 1 (UM6845R) status register at &BExx: bit 5 = vertical blanking (the row
// counter has left the displayed area). Bit 6 (LPEN strobe) not modeled.
uint8_t status_reg(const crtc_state* c) {
  return (c->vcc >= c->reg[6]) ? 0x20 : 0x00;
}

// Evaluate the "vcc == R7" VSYNC trigger with edge semantics (mirrors the
// legacy r7match): VSYNC starts when the equality BECOMES true — at a row
// change or at an R7 write — and never retriggers while a VSYNC is already
// running.
void check_vsync_start(crtc_state* c) {
  const bool match = (c->vcc == c->reg[7]);
  if (match && !c->r7match && !c->in_vsync) {
    c->in_vsync = true;
    c->vsw = 0;
  }
  c->r7match = match;
}

// Latch the Plus split registers from the ASIC once per frame (a no-op on
// models 0-2, where no ASIC is attached or it is unplugged).
void snapshot_split(crtc_state* c) {
  c->plus_split = false;
  if (!c->asic || !asic_vid_active(c->asic)) return;
  uint8_t line = 0;
  uint16_t addr = 0;
  asic_vid_split(c->asic, &line, &addr);
  if (line == 0) return;
  c->plus_split = true;
  c->split_sl_snap = line;
  c->split_addr_snap = addr;
}

// Plus vertical soft scroll (&6804 bits 4-6): the displayed scanline fetches
// from vscroll lines further down, wrapping into the next character row past R9
// (asic.cpp prerender_*_plus). Adjusts only the (ma, ra) driven for the fetch —
// the beam position, HSYNC/VSYNC timing and sprites are untouched.
void apply_vscroll(const crtc_state* c, uint16_t* ma, uint8_t* ra) {
  if (!c->asic || !asic_vid_active(c->asic)) return;
  uint8_t vscroll = 0;
  asic_vid_scroll(c->asic, nullptr, &vscroll);
  if (vscroll == 0) return;
  const int ra_eff = *ra + vscroll;
  if (ra_eff <= c->reg[9]) {
    *ra = static_cast<uint8_t>(ra_eff);
  } else {  // spilled past the char row → the next row's data
    *ra = static_cast<uint8_t>(ra_eff - (c->reg[9] + 1));
    *ma = static_cast<uint16_t>((*ma + c->reg[1]) & 0x3FFF);
  }
}

void crtc_newframe(crtc_state* c) {
  c->vcc = 0;
  c->ra = 0;
  c->in_vta = false;
  c->vta = 0;
  c->scanline = 0;
  c->ma_row = static_cast<uint16_t>(((c->reg[12] << 8) | c->reg[13]) & 0x3FFF);
  snapshot_split(c);
  check_vsync_start(c);  // R7 == 0 edge case
}

// Advance to the next char row. Returns true if this restarted the frame (so
// the caller must not then override the freshly-reloaded base).
bool crtc_newrow(crtc_state* c) {
  c->ma_row =
      static_cast<uint16_t>((c->ma_row + c->reg[1]) & 0x3FFF);  // += R1 per row
  if (c->vcc == c->reg[4]) {  // end of vertical total
    if (c->reg[5] != 0) {
      c->in_vta = true;
      c->vta = 0;
      return false;
    }  // run R5 adjust scanlines
    crtc_newframe(c);
    return true;
  }
  c->vcc = static_cast<uint8_t>((c->vcc + 1) & 0x7F);  // 7-bit row counter
  check_vsync_start(c);                                // VSYNC starts at row R7
  return false;
}

void crtc_newscanline(crtc_state* c) {
  // Plus split: newscanline sets up the NEXT scanline, so advance the frame
  // counter first, then fire the swap when it reaches split_line — that
  // scanline then renders from split_addr (the legacy addr==split_addr at
  // sl_count == split_sl, crtc.cpp:1342).
  c->scanline = static_cast<uint16_t>(c->scanline + 1);
  const bool split_here = c->plus_split && c->scanline == c->split_sl_snap;

  if (c->in_vsync) {  // VSYNC width is measured in scanlines (per-type,
                      // §helpers)
    if (++c->vsw >= vsync_width(c)) c->in_vsync = false;
  }
  if (c->in_vta) {  // vertical total adjust: R5 extra scanlines, then restart
    if (++c->vta >= c->reg[5]) crtc_newframe(c);
    return;
  }
  bool restarted = false;
  if (c->ra == c->reg[9]) {  // last scanline of the char row
    c->ra = 0;
    restarted = crtc_newrow(c);
  } else {
    c->ra = static_cast<uint8_t>((c->ra + 1) & 0x1F);  // 5-bit raster counter:
    // writing R9 below the current RA makes it run to 31 and wrap before
    // matching.
  }
  // Type 1 (UM6845R) quirk: while the FIRST char row is displayed (vcc == 0),
  // the start address is re-read from R12/R13 at every scanline — mid-row-0
  // writes shift the rest of the row. Other types latch it once, at frame
  // start.
  if (c->type == 1 && c->vcc == 0)
    c->ma_row =
        static_cast<uint16_t>(((c->reg[12] << 8) | c->reg[13]) & 0x3FFF);
  // The split base wins over the per-row += R1 (and the type-1 reload), but not
  // over a genuine frame restart, which has already reloaded R12/R13.
  if (split_here && !restarted) c->ma_row = c->split_addr_snap;
}

// One 1 MHz character cycle.
void crtc_char(crtc_state* c) {
  const uint8_t hwidth = hsync_width(c);  // 0 on types 0/1 → HSYNC never starts
  if (c->hcc == c->reg[2] && hwidth != 0) {  // HSYNC starts at char R2
    c->in_hsync = true;
    c->hsw = 0;
  } else if (c->in_hsync) {
    if (++c->hsw >= hwidth) c->in_hsync = false;
  }

  c->ma = static_cast<uint16_t>((c->ma_row + c->hcc) & 0x3FFF);
  // Display window, shifted right by the R8 DISPTMG skew (types 0/3); skew 3
  // blanks the display entirely. MA is NOT skewed — only the enable is delayed.
  const uint8_t skew = disp_skew(c);
  c->dispen = (skew != 0xFF) && (c->hcc >= skew) &&
              (c->hcc < static_cast<int>(skew) + c->reg[1]) &&
              (c->vcc < c->reg[6]);

  if (c->hcc == c->reg[0]) {  // end of scanline (equality, not >=!)
    c->hcc = 0;
    crtc_newscanline(c);
  } else {
    // 8-bit counter: writing R0 BELOW the current count makes the line run to
    // the 256 wrap and around to R0 — the mid-line stretch demos use for
    // rupture.
    c->hcc = static_cast<uint8_t>(c->hcc + 1);
  }
}

// The register-WRITE decode — the ONE definition both execution shapes share:
// crtc_tick snoops it off the bus, the Fast tier applies it per OUT event
// (crtc_fast_io_write). A14 LOW selects the CRTC; A9..A8 pick the function
// (0 = &BCxx select, 1 = &BDxx write). Returns a CrtcEdgeKind bitmask of sync
// edges the write itself caused (an R7 hit can start VSYNC immediately).
uint8_t crtc_io_write_decode(crtc_state* c, uint16_t addr, uint8_t data) {
  if (addr & 0x4000) return 0;
  const uint8_t fn = static_cast<uint8_t>((addr >> 8) & 3);
  if (fn == 0) {
    c->reg_select = static_cast<uint8_t>(data & 0x1F);
  } else if (fn == 1 && c->reg_select < 16) {
    c->reg[c->reg_select] =
        static_cast<uint8_t>(data & kRegWMask[c->reg_select]);
    // Writing R7 equal to the CURRENT row starts VSYNC immediately (edge
    // semantics via r7match — no retrigger while the equality holds or a
    // VSYNC is already running). Mirrors the legacy match_line_count().
    if (c->reg_select == 7) {
      const bool was = c->in_vsync;
      check_vsync_start(c);
      if (c->in_vsync && !was) return 1u << CRTC_EDGE_VSYNC_RISE;
    }
  }
  // R16/R17 are the light-pen latches — read-only on every type; writes to
  // the &BE/&BF functions are ignored.
  return 0;
}

// The register-READ decode, shared the same way. Returns whether the CRTC
// drives the bus (the type-dependent readable windows), filling *data.
bool crtc_io_read_decode(const crtc_state* c, uint16_t addr, uint8_t* data) {
  if (addr & 0x4000) return false;
  const uint8_t fn = static_cast<uint8_t>((addr >> 8) & 3);
  if (fn == 3 || (fn == 2 && c->type == 3)) {
    *data = read_reg(c);
    return true;
  }
  if (fn == 2 && c->type == 1) {
    *data = status_reg(c);
    return true;
  }
  // fn 2 on types 0/2: no function — nothing drives, the bus floats (0xFF).
  return false;
}

void crtc_tick(void* self, const Bus* __restrict in, Bus* __restrict out) {
  crtc_state* c = self_of(self);

  if (in->cpu.iorq) {
    if (in->cpu.wr) {
      crtc_io_write_decode(c, in->cpu.addr, in->cpu.data);
    } else if (in->cpu.rd) {
      uint8_t data = 0;
      if (crtc_io_read_decode(c, in->cpu.addr, &data)) out->cpu.data = data;
    }
  }

  if (in->clk.crtc) crtc_char(c);  // advance one character per 1 MHz tick

  // Light-pen (light gun) strobe: latch the current refresh address into
  // R16/R17 on the LPEN rising edge — 6845-faithful, all types carry R16/R17
  // (light-gun-device.md §2). Gated on the strobe, which rests LOW, so an
  // unplugged machine never runs this and the CKSUMs are unchanged.
  if (in->pen.strobe && !c->lpen_prev) {
    c->reg[16] = static_cast<uint8_t>((c->ma >> 8) & 0x3F);
    c->reg[17] = static_cast<uint8_t>(c->ma & 0xFF);
  }
  c->lpen_prev = in->pen.strobe;

  out->vid.hsync = c->in_hsync;
  out->vid.hsw = c->hsw;
  out->vid.vsync = c->in_vsync;
  out->vid.dispen = c->dispen;
  uint16_t ma = c->ma;
  uint8_t ra = c->ra;
  apply_vscroll(c, &ma, &ra);  // Plus vertical soft scroll (fetch only)
  out->vid.ma = ma;
  out->vid.ra = ra;
  out->vid.frame_line =
      c->scanline;  // shared raster ref: Plus split + ASIC PRI
}

void crtc_reset(void* self) {
  crtc_state* c = self_of(self);
  std::memset(c->reg, 0, sizeof(c->reg));
  c->reg_select = 0;
  // c->type persists: which 6845 variant is soldered in is a hardware strap.
  c->hcc = c->vcc = c->ra = 0;
  c->hsw = c->vsw = c->vta = 0;
  c->in_hsync = c->in_vsync = c->in_vta = c->dispen = false;
  c->lpen_prev = false;
  c->r7match = false;
  c->ma_row = c->ma = 0;
  c->scanline = 0;
  c->plus_split = false;
  c->split_sl_snap = c->split_addr_snap = 0;
  // c->asic persists: the ASIC attachment is board wiring, not machine state.
}

size_t crtc_dev_state_size(const void* /*unused*/) {
  return sizeof(crtc_state) + 1;
}
void crtc_save(const void* self, void* buf) {
  uint8_t* b = static_cast<uint8_t*>(buf);
  b[0] = 1;  // format version
  std::memcpy(b + 1, self, sizeof(crtc_state));
  // The ASIC attachment is board wiring, not machine state — zero its pointer
  // in the blob so the serialised bytes are deterministic (load keeps the live
  // one).
  std::memset(b + 1 + offsetof(crtc_state, asic), 0, sizeof(const Device*));
}
void crtc_load(void* self, const void* buf) {
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  if (b[0] != 1) return;
  // Preserve the live ASIC attachment (board wiring) across a state load — the
  // serialized pointer value is meaningless in a fresh process.
  crtc_state* c = self_of(self);
  const Device* keep = c->asic;
  std::memcpy(self, b + 1, sizeof(crtc_state));
  c->asic = keep;
}

}  // namespace

extern "C" {

size_t crtc_state_size(void) { return sizeof(crtc_state); }

Device crtc_init(void* storage) {
  crtc_state* c = new (storage) crtc_state();
  crtc_reset(c);
  return Device{c,         "crtc",   crtc_tick, crtc_reset, crtc_dev_state_size,
                crtc_save, crtc_load};
}

void crtc_peek(const Device* dev, CrtcRegs* out) {
  const crtc_state* c = static_cast<const crtc_state*>(dev->self);
  out->hcc = c->hcc;
  out->ra = c->ra;
  out->vcc = c->vcc;
  out->ma = c->ma;
  out->hsync = c->in_hsync ? 1 : 0;
  out->vsync = c->in_vsync ? 1 : 0;
  out->dispen = c->dispen ? 1 : 0;
  out->reg_select = c->reg_select;
  out->type = c->type;
  out->scanline = c->scanline;
  out->hsw = c->hsw;
  out->vsw = c->vsw;
  out->vta = c->vta;
  std::memcpy(out->reg, c->reg, sizeof(out->reg));
}

void crtc_attach_asic(const Device* dev, const Device* asic) {
  static_cast<crtc_state*>(dev->self)->asic = asic;
}

void crtc_set_type(const Device* dev, uint8_t type) {
  static_cast<crtc_state*>(dev->self)->type = static_cast<uint8_t>(type & 3);
}

void crtc_poke_reg(const Device* dev, uint8_t idx, uint8_t val) {
  crtc_state* c = static_cast<crtc_state*>(dev->self);
  if (idx < 16)
    c->reg[idx] = static_cast<uint8_t>(val & kRegWMask[idx]);
  else if (idx < 18)
    c->reg[idx] = val;  // light-pen latches (test setup only)
}

void crtc_restore_v3(const Device* dev, uint16_t ma, uint16_t scanline,
                     uint8_t hcc, uint8_t vcc, uint8_t ra, uint8_t hsw,
                     uint8_t vsw, uint8_t flags) {
  crtc_state* c = static_cast<crtc_state*>(dev->self);
  c->ma = ma;
  c->scanline = scanline;
  c->hcc = hcc;
  c->vcc = static_cast<uint8_t>(vcc & 127);
  c->ra = static_cast<uint8_t>(ra & 31);
  c->hsw = static_cast<uint8_t>(hsw & 15);
  c->vsw = static_cast<uint8_t>(vsw & 15);
  c->in_vsync = (flags & 1) != 0;
  c->in_hsync = (flags & 2) != 0;
  c->in_vta = (flags & 0x80) != 0;
}

int crtc_advance_chars(const Device* dev, uint32_t chars, CrtcEdge* out,
                       int max_out) {
  crtc_state* c = static_cast<crtc_state*>(dev->self);
  // The same crtc_char step the per-cycle tick runs, minus the bus — a tight
  // loop first (plan §7: closed forms only if profiling demands them). Edges
  // are level transitions across each char.
  int produced = 0;
  for (uint32_t i = 0; i < chars; ++i) {
    const bool hs = c->in_hsync;
    const bool vs = c->in_vsync;
    crtc_char(c);
    // VSYNC before HSYNC within a char: both edges reach the GA in the same
    // per-cycle tick, whose code arms the VSYNC resync BEFORE the raster
    // count runs — appliers consuming this stream in order must match that.
    if (c->in_vsync != vs) {
      if (produced < max_out)
        out[produced] = CrtcEdge{
            i, static_cast<uint8_t>(c->in_vsync ? CRTC_EDGE_VSYNC_RISE
                                                : CRTC_EDGE_VSYNC_FALL)};
      ++produced;
    }
    if (c->in_hsync != hs) {
      if (produced < max_out)
        out[produced] = CrtcEdge{
            i, static_cast<uint8_t>(c->in_hsync ? CRTC_EDGE_HSYNC_RISE
                                                : CRTC_EDGE_HSYNC_FALL)};
      ++produced;
    }
  }
  return produced;
}

int crtc_advance_view(const Device* dev, uint32_t chars, CrtcCharView* out) {
  crtc_state* c = static_cast<crtc_state*>(dev->self);
  int edges = 0;
  uint32_t i = 0;
  while (i < chars) {
    // Closed-form span (F8 R14): chars strictly BEFORE the next state-machine
    // event — the HSYNC-rise char (enters with HCC == R2; the rise is not
    // !in_hsync-gated, but the span never reaches it) and the line-end char
    // (HCC == R0 runs crtc_newscanline) both take the per-char step below, as
    // does the whole in-HSYNC stretch (hsw counts per char) and the HCC > R0
    // wrap-around stretch. Inside the span every per-char quantity is affine
    // in HCC: MA = ma_row + HCC, RA/scanline/VSYNC constant, DISPEN a window
    // compare, no edges. Registers and ASIC state are frozen for the whole
    // call (catch-up-then-apply), so the vscroll adjustment collapses to one
    // span-constant (ra', +R1-or-0) pair. Byte-identical to the per-char step
    // by construction — Crtc.AdvanceViewMatchesPerCycleTickCharForChar guards.
    if (!c->in_hsync && c->hcc < c->reg[0]) {
      uint32_t n = static_cast<uint32_t>(c->reg[0] - c->hcc);
      if (hsync_width(c) != 0 && c->hcc <= c->reg[2] &&
          c->reg[2] <= c->reg[0]) {
        const uint32_t n_hs = static_cast<uint32_t>(c->reg[2] - c->hcc);
        n = std::min(n_hs, n);
      }
      n = std::min(n, chars - i);
      if (n != 0) {
        const uint8_t skew = disp_skew(c);
        const bool row_active = (skew != 0xFF) && (c->vcc < c->reg[6]);
        const int w0 = skew;
        const int w1 = static_cast<int>(skew) + c->reg[1];
        // apply_vscroll, hoisted: ra is span-constant, so the adjusted ra and
        // the +R1 row spill are too.
        uint16_t ma_bump = 0;
        uint8_t ra_out = c->ra;
        if (c->asic && asic_vid_active(c->asic)) {
          uint8_t vscroll = 0;
          asic_vid_scroll(c->asic, nullptr, &vscroll);
          if (vscroll != 0) {
            const int ra_eff = c->ra + vscroll;
            if (ra_eff <= c->reg[9]) {
              ra_out = static_cast<uint8_t>(ra_eff);
            } else {
              ra_out = static_cast<uint8_t>(ra_eff - (c->reg[9] + 1));
              ma_bump = c->reg[1];
            }
          }
        }
        const uint8_t base_levels =
            static_cast<uint8_t>(c->in_vsync ? CRTC_LVL_VSYNC : 0);
        const uint16_t line = c->scanline;
        const int hcc0 = c->hcc;
        for (uint32_t k = 0; k < n; ++k) {
          const int hcc = hcc0 + static_cast<int>(k);
          CrtcCharView& view = out[i + k];
          view.ma = static_cast<uint16_t>((c->ma_row + hcc + ma_bump) & 0x3FFF);
          view.ra = ra_out;
          view.levels = static_cast<uint8_t>(
              base_levels |
              ((row_active && hcc >= w0 && hcc < w1) ? CRTC_LVL_DISPEN : 0));
          view.edges = 0;
          view.mode = 0;  // the chain stamps the GA's latched mode here
          view.frame_line = line;
        }
        // Materialize the state the per-char steps would leave behind.
        const int last = hcc0 + static_cast<int>(n) - 1;
        c->ma = static_cast<uint16_t>((c->ma_row + last) & 0x3FFF);
        c->dispen = row_active && last >= w0 && last < w1;
        c->hcc = static_cast<uint8_t>(hcc0 + static_cast<int>(n));
        i += n;
        continue;
      }
    }
    const bool hs = c->in_hsync;
    const bool vs = c->in_vsync;
    crtc_char(c);
    CrtcCharView& view = out[i];
    uint16_t ma = c->ma;
    uint8_t ra = c->ra;
    apply_vscroll(c, &ma, &ra);  // the bus carries the vscroll-adjusted fetch
    view.ma = ma;
    view.ra = ra;
    view.levels = static_cast<uint8_t>((c->in_hsync ? CRTC_LVL_HSYNC : 0) |
                                       (c->in_vsync ? CRTC_LVL_VSYNC : 0) |
                                       (c->dispen ? CRTC_LVL_DISPEN : 0));
    uint8_t edge_mask = 0;
    if (c->in_vsync != vs)
      edge_mask |= static_cast<uint8_t>(
          1u << (c->in_vsync ? CRTC_EDGE_VSYNC_RISE : CRTC_EDGE_VSYNC_FALL));
    if (c->in_hsync != hs)
      edge_mask |= static_cast<uint8_t>(
          1u << (c->in_hsync ? CRTC_EDGE_HSYNC_RISE : CRTC_EDGE_HSYNC_FALL));
    view.edges = edge_mask;
    view.mode = 0;  // the chain stamps the GA's latched mode here
    view.frame_line = c->scanline;
    if (edge_mask) ++edges;
    ++i;
  }
  return edges;
}

uint32_t crtc_irq_horizon_chars(const Device* dev) {
  const crtc_state* c = static_cast<const crtc_state*>(dev->self);
  const uint8_t r0 = c->reg[0];
  const uint8_t r2 = c->reg[2];
  // Mid-line stretch (R0 written below the running HCC): the line runs to the
  // 256 wrap — geometry is per-char until it resolves.
  if (c->hcc > r0) return 1;
  // The line-end char (HCC == R0 runs crtc_newscanline): frame_line advances
  // (ASIC PRI edge-detect reference) and VSYNC can rise (frame cut) — always a
  // stop point. Inclusive: advancing h chars from HCC executes the chars
  // entering at HCC .. HCC+h-1, so h = R0 - HCC + 1 includes the R0 char.
  uint32_t h = static_cast<uint32_t>(r0 - c->hcc) + 1;
  // The HSYNC-fall char clocks the Gate Array's R52 raster counter. If a line
  // end comes first, stop there and recompute: HSYNC can legally span the wrap.
  const uint8_t width = hsync_width(c);
  if (c->in_hsync && width != 0) {
    const uint32_t h_fall =
        c->hsw >= width ? 1u : static_cast<uint32_t>(width - c->hsw);
    h = std::min(h_fall, h);
  } else if (c->hcc <= r2 && r2 <= r0 && width != 0) {
    const uint32_t h_fall = static_cast<uint32_t>(r2 - c->hcc) + 1u + width;
    h = std::min(h_fall, h);
  }
  return h;
}

uint8_t crtc_fast_io_write(const Device* dev, uint16_t port, uint8_t val) {
  return crtc_io_write_decode(static_cast<crtc_state*>(dev->self), port, val);
}

int crtc_fast_io_read(const Device* dev, uint16_t port, uint8_t* out) {
  return crtc_io_read_decode(static_cast<const crtc_state*>(dev->self), port,
                             out)
             ? 1
             : 0;
}

void crtc_fast_lpen_strobe(const Device* dev, bool level) {
  crtc_state* c = static_cast<crtc_state*>(dev->self);
  // Mirrors crtc_tick's pen.strobe handler: latch ma on the rising edge, then
  // track the level for the next edge-detect. The per-cycle path reads
  // in->pen.strobe AFTER crtc_char advances ma; the caller advances the CRTC
  // to this char before calling, so c->ma is the same post-advance value.
  if (level && !c->lpen_prev) {
    c->reg[16] = static_cast<uint8_t>((c->ma >> 8) & 0x3F);
    c->reg[17] = static_cast<uint8_t>(c->ma & 0xFF);
  }
  c->lpen_prev = level;
}

}  // extern "C"
