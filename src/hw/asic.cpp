/* asic.cpp — the Plus / 6128+ ASIC Device: unlock knock + register-page
 * overlay, sprite/palette/config decode, the split screen, the DMA sound
 * sequencer (bus master), and the raster/DMA interrupts with the IM2 vector.
 * See docs/hardware/asic-device.md. Behavioural oracle: src/asic.cpp (the §1
 * replace-and-delete target — now fully superseded). */

#include "asic.h"

#include <cstddef>
#include <cstring>
#include <new>

namespace {

// The 17-byte unlock knock, written to the CRTC register-select port
// (asic_poke_lock_sequence, asic.cpp). A full match followed by one more byte
// unlocks; the sequence minus its last byte re-locks.
constexpr uint8_t kLockSeq[16] = {0x00, 0x00, 0xFF, 0x77, 0xB3, 0x51,
                                  0xA8, 0xD4, 0x62, 0x39, 0x9C, 0x46,
                                  0x2B, 0x15, 0x8A, 0xCD};
constexpr int kLockLen = 16;

struct asic_state {
  uint8_t plugged = 0;
  uint8_t locked = 1;       // the register page is hidden at cold boot
  uint8_t page_on = 0;      // RMR2 has mapped the register page into &4000-&7FFF
                            // (membank field == 3). The unlock knock only ENABLES
                            // RMR2; the page is not overlaid until an RMR2 write
                            // pages it in — and games page it back OUT (mapping a
                            // low-ROM bank / RAM there) to bulk-copy through &6xxx
                            // without scribbling the ASIC registers.
  int lockpos = 0;          // position in the knock FSM
  bool knock_prev = false;  // previous cycle was a CRTC-select write (edge)
  bool pgwr_prev = false;   // previous cycle was a register-page write (edge)
  uint8_t classic_pen = 0;  // pen latched by the classic Gate Array palette port
                            // — on a Plus those ink writes land in the ASIC's
                            // own palette RAM (the ASIC *is* the Gate Array).

  uint8_t page[0x4000] = {0};  // the register page, raw read-back mirror

  // Decoded config block (&6800-&6805).
  uint8_t pri_line = 0, split_line = 0;
  uint16_t split_addr = 0;
  uint8_t hscroll = 0, vscroll = 0, extend_border = 0, int_vector = 1;

  // Decoded sprites (&4000-&4FFF pixels, &6000-&607F attributes).
  uint8_t sprite_px[16][16][16] = {};  // [id][x][y] -> 4-bit index (0 = clear)
  uint16_t sprite_x[16] = {0};         // 10-bit
  uint16_t sprite_y[16] = {0};         // 9-bit
  uint8_t sprite_mx[16] = {0};         // magnification 0/1/2/4
  uint8_t sprite_my[16] = {0};

  // Decoded 12-bit palette (&6400-&643F): 32 entries, 4 bits per component.
  // pal_set marks entries the Plus has programmed; unset entries fall back to
  // the classic ink colour in the video Device (asic_set_palette semantics).
  uint8_t pal_r[32] = {0}, pal_g[32] = {0}, pal_b[32] = {0};
  uint8_t pal_set[32] = {0};

  // DMA channel registers (&6C00-&6C0F). source/prescaler/enabled are the
  // programmed register view (decoded in increment A); the rest is the
  // sequencer's live per-channel state (increment C).
  uint16_t dma_source[3] = {0};
  uint8_t dma_prescaler[3] = {0};
  uint8_t dma_enabled[3] = {0};
  uint16_t dma_loop_addr[3] = {0};    // REPEAT/LOOP target
  uint16_t dma_loops[3] = {0};        // remaining loop iterations
  uint16_t dma_pause_ticks[3] = {0};  // PAUSE countdown (scanlines * prescaler)
  uint16_t dma_tick_cycles[3] = {0};  // prescaler sub-counter within a PAUSE
  uint8_t dma_int[3] = {0};           // channel raised an INT

  // DMA engine FSM (drives the bus as a master, one burst per scanline).
  uint8_t dma_phase = 0;  // DmaPhase
  uint8_t dma_cur = 0;    // channel being serviced this burst
  uint8_t dma_lo = 0;     // latched low byte of the current instruction
  uint8_t dma_psg_r = 0;  // pending PSG register (LOAD)
  uint8_t dma_psg_v = 0;  // pending PSG value (LOAD)
  bool dma_hsync_prev = false;
  uint8_t dma_prev_hsw = 0;
  bool dma_fired_this_line = false;

  // Interrupts (increment D): the programmable raster interrupt counts frame
  // scanlines and fires at pri_line; the DMA INT flags and the IM2 vector round
  // it out. cpu.irq is wired-OR; the ASIC only ever asserts.
  uint16_t pri_prev_line = 0xFFFF;  // last CRTC frame_line seen — edge-detect so
                                    // the PRI fires once as the line is reached
  bool pri_pending = false;   // a raster/DMA interrupt is awaiting acknowledge
  // Write generation of the per-line video snapshot's inputs (sprite attrs,
  // palette incl. classic-ink snoops, scroll/config) — F8 R17. The video
  // device skips its per-line re-snapshot while this is unchanged (a re-read
  // of unchanged registers is idempotent, so the skip is exact). Monotonic;
  // MUST REMAIN THE LAST MEMBER: version-1 save blobs load as
  // offsetof(asic_state, vid_gen) bytes.
  uint32_t vid_gen = 1;
};

// The DMA engine's cycle-by-cycle state. A burst: request the bus, wait for the
// grant, then per active channel a 4-cycle banked-RAM fetch of one 16-bit
// instruction, execute it, and (for LOAD) a 2-cycle PSG write over the AY bus.
enum DmaPhase : uint8_t {
  DMA_IDLE = 0,
  DMA_WAIT_BUSAK,  // busrq raised, CPU still finishing its M-cycle
  DMA_FETCH0,      // drive addr = source (low byte)
  DMA_FETCH0_W,    // hold — memory is responding
  DMA_FETCH1,      // latch low, drive addr = source+1 (high byte)
  DMA_FETCH1_W,    // hold
  DMA_EXEC,        // latch high, decode + execute
  DMA_PSG_SEL,     // AY bus: latch the register address (bdir & bc1)
  DMA_PSG_WR,      // AY bus: write the value (bdir & !bc1)
  DMA_ADVANCE,     // writeback, then next channel or release the bus
};

asic_state* self_of(void* self) { return static_cast<asic_state*>(self); }

// The register-select knock FSM — the golden master's asic_poke_lock_sequence
// verbatim (the two leading 0x00 in kLockSeq are matched after a non-zero
// primes the FSM, exactly as the hardware expects).
void feed_knock(asic_state* a, uint8_t val) {
  if (a->lockpos == 0) {
    if (val > 0) a->lockpos = 1;
    return;
  }
  if (a->lockpos < kLockLen) {
    if (val == kLockSeq[a->lockpos]) {
      a->lockpos++;
    } else {
      a->lockpos++;
      if (a->lockpos == kLockLen) a->locked = 1;  // matched all-but-last: lock
      a->lockpos = (val == 0) ? 2 : 1;
    }
    return;
  }
  // Full match already reached, and another byte arrived: unlock.
  a->locked = 0;
  a->lockpos = (val == 0) ? 0 : 1;
}

// Magnification field: 2 bits, 0/1/2 pass through, 3 means ×4 (asic.cpp
// decode_magnification).
uint8_t decode_mag(uint8_t val) {
  const uint8_t mag = val & 0x03;
  return mag == 3 ? 4 : mag;
}

// &4000-&4FFF: 16 sprites, 16×16 pixels, 4-bit palette index (0 transparent).
// addr = 010S SSSS YYYY XXXX — id in bits 8-11, y in 4-7, x in 0-3.
void decode_sprite_pixel(asic_state* a, uint16_t addr, uint8_t val) {
  const int id = (addr & 0x0F00) >> 8;
  const int y = (addr & 0x00F0) >> 4;
  const int x = addr & 0x000F;
  a->sprite_px[id][x][y] = val & 0x0F;
}

// &6000-&607F: sprite attributes, 8 bytes each. type 0/1 = X lo/hi (10-bit),
// 2/3 = Y lo/hi (9-bit), 4 = per-axis magnification (asic.cpp).
void decode_sprite_attr(asic_state* a, uint16_t addr, uint8_t val) {
  const int id = (addr - 0x6000) >> 3;
  if (id >= 16) return;
  switch (addr & 0x07) {
    case 0:
      a->sprite_x[id] = static_cast<uint16_t>((a->sprite_x[id] & 0xFF00) | val);
      break;
    case 1:
      a->sprite_x[id] = static_cast<uint16_t>((a->sprite_x[id] & 0x00FF) |
                                              ((val & 0x03) << 8));
      break;
    case 2:
      a->sprite_y[id] = static_cast<uint16_t>((a->sprite_y[id] & 0xFF00) | val);
      break;
    case 3:
      a->sprite_y[id] = static_cast<uint16_t>((a->sprite_y[id] & 0x00FF) |
                                              ((val & 0x01) << 8));
      break;
    case 4:
      a->sprite_mx[id] = decode_mag(val >> 2);
      a->sprite_my[id] = decode_mag(val);
      break;
    default:
      break;
  }
}

// &6400-&643F: 32 palette entries, 2 bytes each. Even byte = RRRR_BBBB, odd
// byte = 0000_GGGG (asic.cpp asic_register_page_write / asic_set_palette).
void decode_palette(asic_state* a, uint16_t addr, uint8_t val) {
  const int idx = (addr & 0x3F) >> 1;
  if (addr & 1) {
    a->pal_g[idx] = val & 0x0F;
  } else {
    a->pal_r[idx] = (val & 0xF0) >> 4;
    a->pal_b[idx] = val & 0x0F;
  }
  a->pal_set[idx] = 1;
}

// The 32 classic CPC hardware colours as the ASIC's own 4-bit-per-channel
// palette entries. The CPC's channel levels (0 / 0.5 / 1.0 → 0 / 128 / 255) map
// to 4-bit 0 / 8 / 15 (video re-expands ×17). Mirrors video.cpp kPalette — the
// shared gate-array colour set.
constexpr uint8_t kClassic4[32][3] = {
    {8, 8, 8},   {8, 8, 8},   {0, 15, 8},  {15, 15, 8}, {0, 0, 8},   {15, 0, 8},
    {0, 8, 8},   {15, 8, 8},  {15, 0, 8},  {15, 15, 8}, {15, 15, 0}, {15, 15, 15},
    {15, 0, 0},  {15, 0, 15}, {15, 8, 0},  {15, 8, 15}, {0, 0, 8},   {0, 15, 8},
    {0, 15, 0},  {0, 15, 15}, {0, 0, 0},   {0, 0, 15},  {0, 8, 0},   {0, 8, 15},
    {8, 0, 8},   {8, 15, 8},  {8, 15, 0},  {8, 15, 15}, {8, 0, 0},   {8, 0, 15},
    {8, 8, 0},   {8, 8, 15},
};

// Snoop the classic Gate Array palette port (I/O A15=0/A14=1). On a Plus the
// ASIC contains the Gate Array: a classic ink write lands in the SAME palette
// RAM the &6400 12-bit writes do — one store, last-writer-wins, independent of
// the register-page lock (the lock only hides the page from the CPU). Games mix
// both paths (Burnin' Rubber fades out via &6400 then repaints via classic
// inks); keeping two gated stores leaves the repaint invisible — black screen.
void snoop_classic_palette(asic_state* a, uint16_t addr, uint8_t data) {
  if ((addr & 0xC000) != 0x4000) return;  // Gate Array port select
  switch (data >> 6) {
    case 0:  // pen select: 0..15, or the border (bit 4)
      a->classic_pen = (data & 0x10) ? 16 : (data & 0x0F);
      break;
    case 1: {  // set ink → translate the classic colour into this palette entry
      a->vid_gen++;
      const uint8_t hw = data & 0x1F;
      a->pal_r[a->classic_pen] = kClassic4[hw][0];
      a->pal_g[a->classic_pen] = kClassic4[hw][1];
      a->pal_b[a->classic_pen] = kClassic4[hw][2];
      a->pal_set[a->classic_pen] = 1;
      break;
    }
    default:
      break;  // mode / ROM / RAM selects don't touch the palette
  }
}

// &6800-&6805: the raster/split/scroll/vector config block.
void decode_config(asic_state* a, uint16_t addr, uint8_t val) {
  switch (addr) {
    case 0x6800:
      a->pri_line = val;
      break;
    case 0x6801:
      a->split_line = val;
      break;
    case 0x6802:
      a->split_addr =
          static_cast<uint16_t>((a->split_addr & 0x00FF) | (val << 8));
      break;
    case 0x6803:
      a->split_addr = static_cast<uint16_t>((a->split_addr & 0x3F00) | val);
      break;
    case 0x6804:
      a->hscroll = val & 0x0F;
      a->vscroll = (val >> 4) & 0x07;
      a->extend_border = (val >> 7) & 1;
      break;
    case 0x6805:
      a->int_vector = val & 0xF8;
      break;
    default:
      break;
  }
}

// &6C00-&6C0F: DMA. &6C0F is the control/status register (per-channel enable);
// &6C00+4c holds channel c's source (word-aligned) lo/hi + prescaler
// (asic.cpp).
void decode_dma(asic_state* a, uint16_t addr, uint8_t val) {
  if (addr == 0x6C0F) {
    for (int c = 0; c < 3; ++c) a->dma_enabled[c] = (val >> c) & 1;
    return;
  }
  const int c = (addr & 0x0C) >> 2;
  if (c >= 3) return;
  switch (addr & 0x03) {
    case 0:  // low byte, word-aligned (bit 0 ignored)
      a->dma_source[c] =
          static_cast<uint16_t>((a->dma_source[c] & 0xFF00) | (val & 0xFE));
      break;
    case 1:  // high byte
      a->dma_source[c] =
          static_cast<uint16_t>((a->dma_source[c] & 0x00FF) | (val << 8));
      break;
    case 2:
      a->dma_prescaler[c] = val;
      break;
    default:
      break;
  }
}

// A register-page write: mirror the raw byte for read-back, then dispatch to
// the region decoder. `addr` is the raw &4000-&7FFF address.
void decode_write(asic_state* a, uint16_t addr, uint8_t val) {
  a->page[addr & 0x3FFF] = val;
  if (addr >= 0x4000 && addr < 0x5000) {
    decode_sprite_pixel(a, addr, val);  // pixels are read live, not snapshot
  } else if (addr >= 0x6000 && addr < 0x6080) {
    a->vid_gen++;
    decode_sprite_attr(a, addr, val);
  } else if (addr >= 0x6400 && addr < 0x6440) {
    a->vid_gen++;
    decode_palette(a, addr, val);
  } else if (addr >= 0x6800 && addr < 0x6806) {
    a->vid_gen++;
    decode_config(a, addr, val);
  } else if (addr >= 0x6C00 && addr <= 0x6C0F) {
    decode_dma(a, addr, val);  // DMA state is not a snapshot input
  }
}

// Execute one decoded 16-bit sequencer instruction on channel `c`. Returns true
// if a PSG LOAD is pending (dma_psg_r/dma_psg_v set). Opcode = bits 12-14; the
// non-LOAD sub-ops (PAUSE/REPEAT and LOOP/INT/STOP) are OR-combinable.
bool dma_exec(asic_state* a, int c, uint16_t instr) {
  const int opcode = (instr & 0x7000) >> 12;
  if (opcode == 0) {  // LOAD R, DD → write DD to PSG register R
    a->dma_psg_r = static_cast<uint8_t>((instr & 0x0F00) >> 8);
    a->dma_psg_v = static_cast<uint8_t>(instr & 0x00FF);
    return true;
  }
  if (opcode & 0x1) {  // PAUSE N (× prescaler+1 scanlines)
    a->dma_pause_ticks[c] = instr & 0x0FFF;
    a->dma_tick_cycles[c] = 0;
  }
  if (opcode & 0x2) {  // REPEAT NNN — remember the NEXT instruction as the loop
    a->dma_loops[c] = instr & 0x0FFF;  // target (source is advanced past this
    a->dma_loop_addr[c] =              // REPEAT in DMA_EXEC before we run).
        static_cast<uint16_t>(a->dma_source[c] + 2);
  }
  if (opcode & 0x4) {
    // LOOP: jump back and DECREMENT while iterations remain — so the block runs
    // NNN+1 times (cpcwiki Plus DMA). NB the src/asic.cpp oracle omits the
    // decrement (a bug it flags near issue #40); we implement the real
    // hardware.
    if ((instr & 0x0001) && a->dma_loops[c] > 0) {
      a->dma_source[c] = a->dma_loop_addr[c];
      a->dma_loops[c]--;
    }
    if (instr & 0x0010) a->dma_int[c] = 1;      // INT: raise the channel flag
    if (instr & 0x0020) a->dma_enabled[c] = 0;  // STOP
  }
  return false;
}

// Mirror a channel's advanced source address + the composite DCSR back into the
// register page, so a CPU read of &6C00-&6C0F sees the live values (the
// oracle's write_raw). Offsets are within the &4000 page (&6C00 → 0x2C00).
void dma_writeback(asic_state* a, int c) {
  const int base = 0x2C00 + (c << 2);
  a->page[base] = static_cast<uint8_t>(a->dma_source[c] & 0xFF);
  a->page[base + 1] = static_cast<uint8_t>((a->dma_source[c] >> 8) & 0xFF);
  uint8_t dcsr = 0;
  for (int i = 0; i < 3; ++i) {
    if (a->dma_enabled[i]) dcsr |= static_cast<uint8_t>(0x1 << i);
    if (a->dma_int[i]) dcsr |= static_cast<uint8_t>(0x40 >> i);
  }
  if (a->pri_pending) dcsr |= 0x80;  // bit 7 = raster interrupt (cpcwiki)
  a->page[0x2C0F] = dcsr;
}

// Advance dma_cur to the next channel that needs a fetch this burst (enabled
// and not paused). Returns false when the burst is done.
bool dma_seek(asic_state* a) {
  while (a->dma_cur < 3) {
    if (a->dma_enabled[a->dma_cur] && a->dma_pause_ticks[a->dma_cur] == 0)
      return true;
    a->dma_cur++;
  }
  return false;
}

// The DMA sound engine. Runs regardless of the register-page lock (the lock
// only hides the page from the CPU, not the sequencer). Returns true while the
// ASIC is bus-mastering (BUSAK granted, CPU tristated) — the caller then skips
// the register-page overlay, since the bus activity it would see is the ASIC's
// own.
bool dma_service(asic_state* a, const Bus* in, Bus* out) {
  const bool hsync = in->vid.hsync;
  const bool hsync_rise = hsync && !a->dma_hsync_prev;
  a->dma_hsync_prev = hsync;
  if (hsync_rise) a->dma_fired_this_line = false;
  // Track hsw continuously (also while bus-mastering): the CRTC freezes the
  // counter at its final value when HSYNC ends — an HSYNC width of 3 parks
  // hsw at 3 for the rest of the line — so only the 2->3 EDGE is a once-
  // per-line event; a level check would re-fire DMA on every char clock.
  bool hsw3_edge = false;
  if (in->clk.crtc) {
    hsw3_edge = in->vid.hsw == 3 && a->dma_prev_hsw != 3;
    a->dma_prev_hsw = in->vid.hsw;
  }

  if (a->dma_phase == DMA_IDLE) {
    // Legacy oracle: asic_dma_cycle() at CRTC.hsw_count==3 (crtc.cpp) when the
    // CRTC char clock is on the bus; isolated ASIC unit tests (no CRTC device)
    // keep the HSYNC-rise trigger from the pre-parity engine.
    const bool trigger = in->clk.crtc ? hsw3_edge : hsync_rise;
    if (!trigger || a->dma_fired_this_line) return false;
    a->dma_fired_this_line = true;
    // Once per scanline: paused channels count down (no bus needed); if any
    // channel is ready to fetch, start a bus burst for it.
    bool need_bus = false;
    for (int c = 0; c < 3; ++c) {
      if (!a->dma_enabled[c]) continue;
      if (a->dma_pause_ticks[c] > 0) {  // PAUSE ongoing (oracle branch 2)
        if (a->dma_tick_cycles[c] < a->dma_prescaler[c]) {
          a->dma_tick_cycles[c]++;
        } else {
          a->dma_tick_cycles[c] = 0;
          a->dma_pause_ticks[c]--;
        }
      } else {
        need_bus = true;
      }
    }
    if (!need_bus) return false;
    a->dma_cur = 0;
    a->dma_phase = DMA_WAIT_BUSAK;
  }

  out->cpu.busrq = true;  // hold the request through the whole burst

  if (a->dma_phase == DMA_WAIT_BUSAK) {
    if (!in->cpu.busak) return false;  // CPU still master → overlay stays live
    a->dma_phase = dma_seek(a) ? DMA_FETCH0 : DMA_IDLE;
    return a->dma_phase != DMA_IDLE;  // a dead cycle after the grant
  }

  const uint16_t src = a->dma_source[a->dma_cur];
  switch (a->dma_phase) {
    case DMA_FETCH0:
    case DMA_FETCH0_W:
      out->cpu.addr = src;
      out->cpu.mreq = out->cpu.rd = true;
      a->dma_phase = (a->dma_phase == DMA_FETCH0) ? DMA_FETCH0_W : DMA_FETCH1;
      break;
    case DMA_FETCH1:
      a->dma_lo = in->cpu.data;  // RAM[source], banked by the memory Device
      out->cpu.addr = static_cast<uint16_t>(src + 1);
      out->cpu.mreq = out->cpu.rd = true;
      a->dma_phase = DMA_FETCH1_W;
      break;
    case DMA_FETCH1_W:
      out->cpu.addr = static_cast<uint16_t>(src + 1);
      out->cpu.mreq = out->cpu.rd = true;
      a->dma_phase = DMA_EXEC;
      break;
    case DMA_EXEC: {
      const uint16_t instr =
          static_cast<uint16_t>(a->dma_lo | (in->cpu.data << 8));
      a->dma_source[a->dma_cur] =
          static_cast<uint16_t>(a->dma_source[a->dma_cur] + 2);
      const bool load = dma_exec(a, a->dma_cur, instr);
      dma_writeback(a, a->dma_cur);
      a->dma_phase = load ? DMA_PSG_SEL : DMA_ADVANCE;
      break;
    }
    case DMA_PSG_SEL:  // AY bus: latch the register address (PPI yields, §C-1a)
      out->ay.bdir = true;
      out->ay.bc1 = true;
      out->ay.da = a->dma_psg_r;
      a->dma_phase = DMA_PSG_WR;
      break;
    case DMA_PSG_WR:  // AY bus: write the value
      out->ay.bdir = true;
      out->ay.bc1 = false;
      out->ay.da = a->dma_psg_v;
      a->dma_phase = DMA_ADVANCE;
      break;
    case DMA_ADVANCE:
      a->dma_cur++;
      a->dma_phase = dma_seek(a) ? DMA_FETCH0 : DMA_IDLE;
      break;
    default:
      break;
  }
  return true;  // bus-mastering
}

// Interrupts. The programmable raster interrupt fires when the CRTC's frame
// scanline (VidBus.frame_line) reaches pri_line — the SAME frame-top reference
// the Plus split counts off, exactly as the legacy shares one CRTC.sl_count for
// both. The DMA INT flags fire too. On the CPU's interrupt-acknowledge cycle
// (m1+iorq) the ASIC drives the IM2 vector on the data bus and clears its
// sources. Runs every cycle, independent of the register-page lock and the DMA.
void asic_irq(asic_state* a, const Bus* in, Bus* out) {
  const uint16_t line = in->vid.frame_line;
  if (a->pri_line != 0 && line == a->pri_line && a->pri_prev_line != line)
    a->pri_pending = true;  // fire once as the target line is reached
  a->pri_prev_line = line;

  // Reflect the raster-int flag in DCSR bit 7 (&6C0F) so a CPU read sees it live,
  // even with no DMA activity to trigger dma_writeback (cpcwiki: bit 7 = RasterInt).
  a->page[0x2C0F] = static_cast<uint8_t>((a->page[0x2C0F] & 0x7F) |
                                         (a->pri_pending ? 0x80 : 0));

  bool dma_int = false;
  for (unsigned char const c : a->dma_int)
    if (c) dma_int = true;

  if (in->cpu.m1 && in->cpu.iorq) {  // interrupt acknowledge
    if (a->pri_pending || dma_int) {
      // IM2 vector = (programmed vector & 0xF8) | a 3-bit source code, chosen by
      // priority (cpcwiki ASIC): raster (6, highest) > DMA ch2 (0) > ch1 (2) >
      // ch0 (4, lowest). int_vector already holds the & 0xF8 part. Real hardware
      // has the "Plus Vectored Interrupt Bug" (a raster ack occasionally drives
      // 4 instead of 6); we implement the intended behaviour, not the bug.
      uint8_t src = 0;
      if (a->dma_int[0]) src = 4;
      if (a->dma_int[1]) src = 2;
      if (a->dma_int[2]) src = 0;
      if (a->pri_pending) src = 6;
      out->cpu.data = static_cast<uint8_t>(a->int_vector | src);  // IM2 vector
    }
    a->pri_pending = false;
    for (unsigned char& c : a->dma_int) c = 0;
    return;  // the sources are cleared this cycle
  }

  if (a->pri_pending || dma_int) out->cpu.irq = true;  // wired-OR assert
}

void asic_tick(void* self, const Bus* __restrict in, Bus* __restrict out) {
  asic_state* a = self_of(self);
  if (!a->plugged) return;

  // Snoop the unlock knock: writes to the CRTC register-select decode (A14
  // low, A9..A8 = 0 — the same decode the CRTC Device uses).
  const bool knock = in->cpu.iorq && !in->cpu.m1 && in->cpu.wr &&
                     !(in->cpu.addr & 0x4000) && ((in->cpu.addr >> 8) & 3) == 0;
  if (knock && !a->knock_prev) feed_knock(a, in->cpu.data);
  a->knock_prev = knock;

  // Classic Gate Array ink writes share the ASIC's one palette RAM — always
  // live, lock or no lock (see snoop_classic_palette).
  if (in->cpu.iorq && in->cpu.wr)
    snoop_classic_palette(a, in->cpu.addr, in->cpu.data);

  // RMR2 (6128+ secondary ROM/register-page map): a Gate-Array fn-2 write
  // (A15=0, A14=1, data 10xxxxxx) with bit5 set, honoured only while unlocked.
  // Its membank field (bits 4-3) selects what sits at &4000-&7FFF: value 3 maps
  // the ASIC register page in (page_on), any other value maps a low-ROM bank /
  // RAM there instead (page off). The knock alone must NOT overlay the page —
  // the CPU still has to page it in — else a game's bulk copy through &6xxx while
  // the page is OFF would scribble the ASIC registers (enabling DMA, derailing).
  if (!a->locked && in->cpu.iorq && in->cpu.wr &&
      (in->cpu.addr & 0xC000) == 0x4000 && (in->cpu.data >> 6) == 2 &&
      (in->cpu.data & 0x20))
    a->page_on = (((in->cpu.data >> 3) & 3) == 3) ? 1 : 0;

  // Raster/DMA interrupts + the IM2 vector — independent of the lock and the
  // DMA burst (an ack can land any cycle; the two use disjoint bus lines).
  asic_irq(a, in, out);

  // The DMA sound engine runs whether or not the page is locked. While it holds
  // the bus (BUSAK granted), the ASIC is the master and the overlay is skipped.
  if (dma_service(a, in, out)) return;

  // The register page is invisible unless it is both unlocked AND paged in by
  // RMR2 (page_on). Either condition off → RAM/low-ROM shows through at &4000.
  if (a->locked || !a->page_on) return;

  // The register page overlays RAM at &4000-&7FFF while unlocked: reads answer
  // from page[] under /RAMDIS; writes land in the ASIC (and the memory Device's
  // one-tick latch vetoes the internal RAM write, memory-device.md §4b).
  const bool in_page = in->cpu.addr >= 0x4000 && in->cpu.addr < 0x8000;
  if (in->cpu.mreq && in->cpu.rd && !in->cpu.rfsh && in_page) {
    out->cpu.ramdis = true;
    out->cpu.data = a->page[in->cpu.addr & 0x3FFF];
  }
  const bool pgwr = in->cpu.mreq && in->cpu.wr && in_page;
  if (pgwr) out->cpu.ramdis = true;  // veto the internal RAM write
  if (pgwr && !a->pgwr_prev) decode_write(a, in->cpu.addr, in->cpu.data);
  a->pgwr_prev = pgwr;
}

void asic_dev_reset(void* self) {
  asic_state* a = self_of(self);
  // A cold boot re-locks the ASIC; the register page contents and plugged
  // state persist (the ASIC is soldered in, not reset-cleared).
  a->locked = 1;
  a->page_on = 0;  // the register page is not mapped until RMR2 pages it in
  a->lockpos = 0;
  a->knock_prev = false;
  a->pgwr_prev = false;
  // The DMA engine returns to idle; the channel registers persist (soldered-in,
  // like the register page).
  a->dma_phase = DMA_IDLE;
  a->dma_hsync_prev = false;
  a->pri_prev_line = 0xFFFF;
  a->pri_pending = false;
}

size_t asic_dev_state_size(const void* /*unused*/) {
  return sizeof(asic_state) + 1;
}
void asic_save(const void* self, void* buf) {
  uint8_t* b = static_cast<uint8_t*>(buf);
  b[0] = 2;  // v2 appends vid_gen to the struct (v1 blobs still load)
  std::memcpy(b + 1, self, sizeof(asic_state));
  // vid_gen is a session-local cache key, not machine state: zero it in the
  // blob so saves stay canonical (byte-identical for identical hardware
  // state); every load re-keys it below.
  std::memset(b + 1 + offsetof(asic_state, vid_gen), 0, sizeof(uint32_t));
}
void asic_load(void* self, const void* buf) {
  asic_state* a = self_of(self);
  const uint32_t live_gen = a->vid_gen;
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  if (b[0] == 2) {
    std::memcpy(self, b + 1, sizeof(asic_state));
  } else if (b[0] == 1) {
    // v1 predates vid_gen (the last member) — its blob holds exactly the
    // bytes before it.
    std::memcpy(self, b + 1, offsetof(asic_state, vid_gen));
  }
  // Any load moves the snapshot inputs out from under a cached generation —
  // re-key past every generation this process has seen.
  a->vid_gen = live_gen + 1;
}

}  // namespace

extern "C" {

size_t asic_state_size(void) { return sizeof(asic_state); }

Device asic_init(void* storage) {
  // NOLINTNEXTLINE(misc-const-correctness): pointer is stored in Device::self (void*), cannot be const
  asic_state *a = new (storage) asic_state();
  return Device{
      a,         "asic",   asic_tick, asic_dev_reset, asic_dev_state_size,
      asic_save, asic_load};
}

void asic_peek(const Device* dev, AsicRegs* out) {
  const asic_state* a = static_cast<const asic_state*>(dev->self);
  out->plugged = a->plugged;
  out->locked = a->locked;
  out->page_on = a->page_on;
  out->pri_line = a->pri_line;
  out->split_line = a->split_line;
  out->split_addr = a->split_addr;
  out->hscroll = a->hscroll;
  out->vscroll = a->vscroll;
  out->extend_border = a->extend_border;
  out->int_vector = a->int_vector;
}

void asic_set_plugged(const Device* dev, int on) {
  static_cast<asic_state*>(dev->self)->plugged = on ? 1 : 0;
}

// --- Intra-chip video interface (read by the video / CRTC Devices) ---

int asic_vid_active(const Device* dev) {
  return static_cast<const asic_state*>(dev->self)->plugged;
}

// Intra-chip: the memory device reads this to gate the RMR2 cartridge low-ROM
// bank remap (an ASIC feature, live only once the register page is unlocked).
int asic_unlocked(const Device* dev) {
  const asic_state* a = static_cast<const asic_state*>(dev->self);
  return a->plugged && !a->locked;
}

void asic_vid_split(const Device* dev, uint8_t* line, uint16_t* addr) {
  const asic_state* a = static_cast<const asic_state*>(dev->self);
  if (line) *line = a->split_line;
  if (addr) *addr = static_cast<uint16_t>(a->split_addr & 0x3FFF);
}

int asic_vid_pri_active(const Device* dev) {
  const asic_state* a = static_cast<const asic_state*>(dev->self);
  return a->plugged && a->pri_line != 0;
}

void asic_vid_scroll(const Device* dev, uint8_t* hscroll, uint8_t* vscroll) {
  const asic_state* a = static_cast<const asic_state*>(dev->self);
  if (hscroll) *hscroll = a->hscroll;
  if (vscroll) *vscroll = a->vscroll;
}

int asic_vid_extend_border(const Device* dev) {
  return static_cast<const asic_state*>(dev->self)->extend_border;
}

uint32_t asic_vid_gen(const Device* dev) {
  return static_cast<const asic_state*>(dev->self)->vid_gen;
}

uint16_t asic_vid_palette(const Device* dev, int index) {
  const asic_state* a = static_cast<const asic_state*>(dev->self);
  const int i = index & 0x1F;
  return static_cast<uint16_t>((a->pal_r[i] << 8) | (a->pal_g[i] << 4) |
                               a->pal_b[i]);
}

int asic_vid_palette_set(const Device* dev, int index) {
  return static_cast<const asic_state*>(dev->self)->pal_set[index & 0x1F];
}

uint8_t asic_vid_sprite_pixel(const Device* dev, int id, int x, int y) {
  const asic_state* a = static_cast<const asic_state*>(dev->self);
  return a->sprite_px[id & 0x0F][x & 0x0F][y & 0x0F];
}

void asic_vid_sprite_attr(const Device* dev, int id, uint16_t* x, uint16_t* y,
                          uint8_t* mag_x, uint8_t* mag_y) {
  const asic_state* a = static_cast<const asic_state*>(dev->self);
  const int i = id & 0x0F;
  if (x) *x = a->sprite_x[i];
  if (y) *y = a->sprite_y[i];
  if (mag_x) *mag_x = a->sprite_mx[i];
  if (mag_y) *mag_y = a->sprite_my[i];
}

int asic_lock_pos(const Device* dev) {
  return static_cast<const asic_state*>(dev->self)->lockpos;
}

void asic_dma_debug(const Device* dev, int ch, uint16_t* loop_addr,
                    uint16_t* loops, uint16_t* pause_ticks,
                    uint16_t* tick_cycles, uint8_t* irq) {
  const asic_state* a = static_cast<const asic_state*>(dev->self);
  const int c = ch % 3;
  if (loop_addr) *loop_addr = a->dma_loop_addr[c];
  if (loops) *loops = a->dma_loops[c];
  if (pause_ticks) *pause_ticks = a->dma_pause_ticks[c];
  if (tick_cycles) *tick_cycles = a->dma_tick_cycles[c];
  if (irq) *irq = a->dma_int[c];
}

uint8_t asic_page_peek(const Device* dev, uint16_t offset) {
  return static_cast<const asic_state*>(dev->self)->page[offset & 0x3FFF];
}

void asic_dma_regs(const Device* dev, int ch, uint16_t* source,
                   uint8_t* prescaler, uint8_t* enabled) {
  const asic_state* a = static_cast<const asic_state*>(dev->self);
  const int c = ch % 3;
  if (source) *source = a->dma_source[c];
  if (prescaler) *prescaler = a->dma_prescaler[c];
  if (enabled) *enabled = a->dma_enabled[c];
}

void asic_fast_io_write(const Device* dev, uint16_t port, uint8_t val) {
  asic_state* a = static_cast<asic_state*>(dev->self);
  if (!a->plugged) return;
  // The knock: CRTC register-select decode, one FSM feed per access (the
  // per-cycle edge detector collapses a held strobe the same way).
  if (!(port & 0x4000) && ((port >> 8) & 3) == 0) feed_knock(a, val);
  snoop_classic_palette(a, port, val);
  if (!a->locked && (port & 0xC000) == 0x4000 && (val >> 6) == 2 &&
      (val & 0x20))
    a->page_on = (((val >> 3) & 3) == 3) ? 1 : 0;
}

int asic_fast_mem_read(const Device* dev, uint16_t addr, uint8_t* out) {
  const asic_state* a = static_cast<const asic_state*>(dev->self);
  if (!a->plugged || a->locked || !a->page_on) return 0;
  if (addr < 0x4000 || addr >= 0x8000) return 0;
  *out = a->page[addr & 0x3FFF];
  return 1;
}

int asic_fast_mem_write(const Device* dev, uint16_t addr, uint8_t val) {
  asic_state* a = static_cast<asic_state*>(dev->self);
  if (!a->plugged || a->locked || !a->page_on) return 0;
  if (addr < 0x4000 || addr >= 0x8000) return 0;
  decode_write(a, addr, val);  // once per access — the pgwr_prev edge's twin
  return 1;                    // the RAM underneath is vetoed
}

int asic_page_armed(const Device* dev) {
  const asic_state* a = static_cast<const asic_state*>(dev->self);
  return (a->plugged && !a->locked && a->page_on) ? 1 : 0;
}

void asic_batch_frame_line(const Device* dev, uint16_t line) {
  asic_state* a = static_cast<asic_state*>(dev->self);
  if (a->pri_line != 0 && line == a->pri_line && a->pri_prev_line != line)
    a->pri_pending = true;  // fire once as the target line is reached
  a->pri_prev_line = line;
  a->page[0x2C0F] = static_cast<uint8_t>((a->page[0x2C0F] & 0x7F) |
                                         (a->pri_pending ? 0x80 : 0));
}

int asic_irq_asserted(const Device* dev) {
  const asic_state* a = static_cast<const asic_state*>(dev->self);
  if (!a->plugged) return 0;
  if (a->pri_pending) return 1;
  for (unsigned char const c : a->dma_int)
    if (c) return 1;
  return 0;
}

uint8_t asic_batch_int_ack(const Device* dev) {
  asic_state* a = static_cast<asic_state*>(dev->self);
  // asic_irq's m1+iorq arm: the IM2 vector by priority, then clear.
  uint8_t vector = 0xFF;  // nothing pending → the bus floats
  bool dma_int = false;
  for (unsigned char const c : a->dma_int)
    if (c) dma_int = true;
  if (a->pri_pending || dma_int) {
    uint8_t src = 0;
    if (a->dma_int[0]) src = 4;
    if (a->dma_int[1]) src = 2;
    if (a->dma_int[2]) src = 0;
    if (a->pri_pending) src = 6;
    vector = static_cast<uint8_t>(a->int_vector | src);
  }
  a->pri_pending = false;
  for (unsigned char& c : a->dma_int) c = 0;
  a->page[0x2C0F] &= 0x7F;  // DCSR raster-int mirror follows the clear
  return vector;
}

int asic_dma_active(const Device* dev) {
  const asic_state* a = static_cast<const asic_state*>(dev->self);
  if (!a->plugged) return 0;
  return (a->dma_enabled[0] | a->dma_enabled[1] | a->dma_enabled[2]) != 0;
}

void asic_batch_set_sync(const Device* dev, int hsync, uint16_t frame_line) {
  asic_state* a = static_cast<asic_state*>(dev->self);
  a->dma_hsync_prev = hsync != 0;
  // Outside HSYNC the CRTC's hsw counter sits frozen at its final value;
  // seed prev at 3 so the handover can't synthesize a spurious 2->3 edge.
  a->dma_prev_hsw = hsync != 0 ? 0 : 3;
  a->dma_fired_this_line = false;
  a->pri_prev_line = frame_line;
  a->knock_prev = false;  // no strobes on the synthesized resting bus
  a->pgwr_prev = false;
}

}  // extern "C"
