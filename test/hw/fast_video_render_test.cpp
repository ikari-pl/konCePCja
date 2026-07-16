/* fast_video_render_test.cpp — F5 (beads-y21q): the catch-up video renderer
 * in PIXEL LOCKSTEP with the per-cycle video device.
 *
 * The fast side extends F4's driver-let with a deferred render queue: the
 * CRTC/GA advance stays EAGER (the irq counter must lead the CPU), while
 * cells render lazily — pulled forward only by writes (catch-up-then-apply)
 * and at the end. The uniform apply point (one-hop bus latency, and memory
 * write T1s are always µs-aligned): everything with T1 in µs j applies after
 * char j's CRTC advance and before cell j's render. A write-triggered VSYNC
 * rise (R7 hit) patches the pending char-j view: per-cycle, the beam resets
 * mid-µs before cell j paints, and cell j is vsync-blanked.
 *
 * Framebuffer comparison point: the batch side renders every advanced char
 * (C = (B-1)/4 + 1 at the parked HALT's tstates B); the per-cycle board runs
 * on to master 16*C — the exact tick cell C-1 paints (phase 15 committed) — so both sides
 * have painted the same cells. The parked CPU state is recorded at the park
 * tick itself (the extension only lets the beam finish the last cell).
 *
 * Scenarios (all also assert Z80 regs/tstates + full RAM, and guard against
 * a vacuously-black framebuffer — screenshot-guard doctrine):
 *   1. StaticScreenPixelEqual — mode 1, inks, a filled screen, two frames.
 *   2. MidFrameInkAndModeSplit — a 300 Hz ISR rotates the border ink and
 *      toggles mode 0/1: horizontal raster bands, pixel-exact.
 *   3. MidScanlineInkFlip — the mainline flips pen 0 between two colours
 *      every ~19 µs: SUB-SCANLINE stripes; every cell must take the ink
 *      value of its own microsecond.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "hw/board.h"
#include "hw/crtc.h"
#include "hw/gate_array.h"
#include "hw/memory.h"
#include "hw/video.h"
#include "hw/z80.h"

namespace {

constexpr uint16_t kProg = 0x8000;
constexpr uint16_t kIsr = 0x0038;
constexpr int kFbW = 768, kFbH = 272;
constexpr size_t kFbLen = static_cast<size_t>(kFbW) * kFbH * 3;

// --- program assembly (the F4 emit helpers + a safe back-JR) ---
struct Asm {
  std::vector<uint8_t> bytes;
  void db(std::initializer_list<uint8_t> bs) {
    bytes.insert(bytes.end(), bs.begin(), bs.end());
  }
  void ld_bc(uint16_t nn) {
    db({0x01, static_cast<uint8_t>(nn & 0xFF), static_cast<uint8_t>(nn >> 8)});
  }
  void out_c_c() { db({0xED, 0x49}); }
  void out_c_a() { db({0xED, 0x79}); }
  void crtc_reg(uint8_t reg, uint8_t val) {
    ld_bc(static_cast<uint16_t>(0xBC00 | reg));
    out_c_c();
    ld_bc(static_cast<uint16_t>(0xBD00 | val));
    out_c_c();
  }
  size_t mark() const { return bytes.size(); }
  void jr_back(size_t target) {  // JR to an earlier mark
    const int disp = static_cast<int>(target) -
                     (static_cast<int>(bytes.size()) + 2);
    db({0x18, static_cast<uint8_t>(disp)});
  }
  void jr_nz_back(size_t target) {
    const int disp = static_cast<int>(target) -
                     (static_cast<int>(bytes.size()) + 2);
    db({0x20, static_cast<uint8_t>(disp)});
  }
};

// ROMs off, the standard CPC frame, screen base &C000 (R12=0x30), IM 1, and a
// distinct mode-1 palette through the GA (pens 0-3 + border).
void emit_prologue(Asm& a, uint8_t fn2_mode) {
  a.ld_bc(static_cast<uint16_t>(0x7F00 | fn2_mode));  // fn2: mode + ROMs off
  a.out_c_c();
  const uint8_t regs[][2] = {{0, 63}, {1, 40}, {2, 46},  {3, 0x8E}, {4, 38},
                             {5, 0},  {6, 25}, {7, 30},  {9, 7},    {12, 0x30},
                             {13, 0}};
  for (const auto& rv : regs) a.crtc_reg(rv[0], rv[1]);
  const uint8_t inks[][2] = {
      {0x00, 0x54}, {0x01, 0x4A}, {0x02, 0x55}, {0x03, 0x5C}, {0x10, 0x41}};
  for (const auto& pi : inks) {  // pen select, then ink
    a.ld_bc(static_cast<uint16_t>(0x7F00 | pi[0]));
    a.out_c_c();
    a.ld_bc(static_cast<uint16_t>(0x7F00 | pi[1]));
    a.out_c_c();
  }
  a.db({0xED, 0x56});  // IM 1
}

void emit_fill_screen(Asm& a, uint8_t value) {  // &C000..&FFFF ← value (LDIR)
  a.db({0x21, 0x00, 0xC0});                     // LD HL,&C000
  a.db({0x36, value});                          // LD (HL),value
  a.db({0x11, 0x01, 0xC0});                     // LD DE,&C001
  a.db({0x01, 0xFF, 0x3F});                     // LD BC,&3FFF
  a.db({0xED, 0xB0});                           // LDIR
}

void emit_delay_and_park(Asm& a, uint16_t iters) {  // ~26T per iteration
  a.ld_bc(iters);
  const size_t loop = a.mark();
  a.db({0x0B, 0x78, 0xB1});  // DEC BC; LD A,B; OR C
  a.jr_nz_back(loop);
  a.db({0x76});  // HALT — IFF1 never set: parked
}

std::vector<uint8_t> prog_static_screen() {
  Asm a;
  emit_prologue(a, 0x8D);  // mode 1
  emit_fill_screen(a, 0x1B);
  emit_delay_and_park(a, 6500);  // ~2 frames after the fill
  return a.bytes;
}

std::vector<uint8_t> prog_band_split() {
  Asm a;
  emit_prologue(a, 0x8D);
  emit_fill_screen(a, 0x1B);
  a.db({0x3E, 0x00});        // LD A,0
  a.db({0x32, 0x04, 0x90});  // LD (0x9004),A — irq count
  a.db({0xFB});              // EI
  const size_t spin = a.mark();
  a.jr_back(spin);  // JR $ — interrupt-driven from here
  return a.bytes;
}

// Band-split ISR: rotate the border ink and toggle mode 0/1 per interrupt;
// park after 12 (no register preservation needed — the mainline is a spin).
std::vector<uint8_t> isr_band_split() {
  Asm a;
  a.ld_bc(0x7F00);
  a.db({0x3E, 0x10});        // LD A,0x10 — select the border
  a.out_c_a();
  a.db({0x3A, 0x04, 0x90});  // LD A,(0x9004)
  a.db({0x3C});              // INC A
  a.db({0x32, 0x04, 0x90});  // LD (0x9004),A
  a.db({0xE6, 0x1F});        // AND 0x1F
  a.db({0xF6, 0x40});        // OR 0x40 — GA fn1: set ink
  a.out_c_a();
  a.db({0x3A, 0x04, 0x90});  // LD A,(0x9004)
  a.db({0xE6, 0x01});        // AND 1
  a.db({0xF6, 0x8C});        // OR 0x8C — fn2: mode 0/1, ROMs stay off
  a.out_c_a();
  a.db({0x3A, 0x04, 0x90});  // LD A,(0x9004)
  a.db({0xFE, 0x0C});        // CP 12
  a.db({0x28, 0x02});        // JR Z, park (over EI; RET)
  a.db({0xFB, 0xC9});        // EI; RET
  a.db({0x76});              // park: HALT (IFF1 clear)
  return a.bytes;
}

std::vector<uint8_t> prog_midscanline_flip() {
  Asm a;
  emit_prologue(a, 0x8D);  // mode 1; screen RAM stays 0x00 → pen 0 everywhere
  a.ld_bc(0x7F00);
  a.db({0x3E, 0x00});  // LD A,0 — select pen 0
  a.out_c_a();
  a.db({0x11, 0x40, 0x06});  // LD DE,1600 — ~1.2 frames of flipping
  const size_t loop = a.mark();
  a.db({0x3E, 0x54});  // LD A,0x40|0x14
  a.out_c_a();
  a.db({0x3E, 0x4B});  // LD A,0x40|0x0B
  a.out_c_a();
  a.db({0x1B, 0x7A, 0xB3});  // DEC DE; LD A,D; OR E
  a.jr_nz_back(loop);
  a.db({0x76});  // HALT — parked
  return a.bytes;
}

// --- shared rig pieces ---
struct MemSide {
  std::vector<uint8_t> storage = std::vector<uint8_t>(mem_state_size());
  Device dev{};
};

void seed_mem(MemSide& side, const std::vector<uint8_t>& prog,
              const std::vector<uint8_t>& isr) {
  side.dev = mem_init(side.storage.data());
  for (size_t i = 0; i < prog.size(); ++i)
    mem_write_ram(&side.dev, static_cast<uint16_t>(kProg + i), prog[i]);
  for (size_t i = 0; i < isr.size(); ++i)
    mem_write_ram(&side.dev, static_cast<uint16_t>(kIsr + i), isr[i]);
}

Z80Regs start_regs() {
  Z80Regs regs{};
  regs.pc = kProg;
  regs.sp = 0xBF00;
  return regs;
}

struct RunResult {
  Z80Regs z{};
  std::vector<uint8_t> fb = std::vector<uint8_t>(kFbLen, 0);
};

// Per-cycle reference (Z80+GA+CRTC+mem+VIDEO). Records the CPU at the park
// tick, then extends to master 16*(C-1)+15 so the last batch-rendered cell
// has painted here too.
bool run_percycle(const std::vector<uint8_t>& prog,
                  const std::vector<uint8_t>& isr, MemSide& mem,
                  RunResult* out) {
  seed_mem(mem, prog, isr);
  std::vector<uint8_t> zmem(z80_state_size());
  Device zdev = z80_init(zmem.data());
  std::vector<uint8_t> gmem(ga_state_size());
  Device gdev = ga_init(gmem.data());
  std::vector<uint8_t> cmem(crtc_state_size());
  Device cdev = crtc_init(cmem.data());
  std::vector<uint8_t> vmem(video_state_size());
  Device vdev = video_init(vmem.data());

  Board board;
  board_init(&board);
  board_add(&board, gdev);
  board_add(&board, cdev);
  board_add(&board, mem.dev);
  board_add(&board, vdev);
  board_add(&board, zdev);
  board_reset(&board);
  video_attach(&vdev, &gdev, out->fb.data(), kFbW, kFbH);
  const Z80Regs init = start_regs();
  z80_poke(&zdev, &init);

  bool parked = false;
  long target_m = 0;
  for (long m = 0; m < 80000000L; ++m) {
    board_tick(&board);
    if (!parked) {
      z80_peek(&zdev, &out->z);
      if (out->z.halted && out->z.iff1 == 0) {
        parked = true;  // CPU state recorded HERE; extend for the beam only
        const uint64_t chars = ((out->z.tstates - 1) / 4) + 1;
        // Cell k paints at master 16k+16 (video consumes byte 1 on the tick
        // whose committed clk.phase is 15) — run through 16*chars so the
        // last batch-rendered cell (chars-1) has painted here too.
        target_m = static_cast<long>(16 * chars);
        if (m >= target_m) return true;
      }
    } else if (m >= target_m) {
      return true;
    }
  }
  return false;
}

// --- the fast driver-let, F4's chain + the deferred render queue ---
struct FastChain {
  Device* mem;
  Device* crtc;
  Device* ga;
  Device* video;
  const uint8_t* vram = nullptr;
  uint64_t chars_done = 0;      // eager CRTC+GA position
  uint64_t cells_rendered = 0;  // lazy renderer position
  std::vector<CrtcCharView> pending;  // views [cells_rendered, chars_done)

  static uint64_t chars_visible_at(uint64_t tstate) {
    return tstate == 0 ? 0 : ((tstate - 1) / 4) + 1;
  }
  static uint64_t chars_before_access(uint64_t tau) { return (tau / 4) + 1; }

  void advance_chars(uint64_t target) {
    if (target <= chars_done) return;
    const uint32_t n = static_cast<uint32_t>(target - chars_done);
    const size_t base = pending.size();
    pending.resize(base + n);
    crtc_advance_view(crtc, n, pending.data() + base);
    for (size_t i = base; i < pending.size(); ++i) {
      CrtcCharView& view = pending[i];
      if (view.edges & (1u << CRTC_EDGE_VSYNC_RISE)) ga_batch_vsync_rise(ga);
      if (view.edges & (1u << CRTC_EDGE_HSYNC_RISE)) ga_batch_hsync_rise(ga);
      if (view.edges & (1u << CRTC_EDGE_HSYNC_FALL)) ga_batch_hsync_fall(ga);
      GateArrayRegs g{};
      ga_peek(ga, &g);
      view.mode = g.mode;  // the latch as of THIS char — stamped in order
    }
    chars_done = target;
  }
  void render_below(uint64_t cell_target) {
    if (cell_target > chars_done) cell_target = chars_done;
    if (cell_target <= cells_rendered) return;
    const size_t n = static_cast<size_t>(cell_target - cells_rendered);
    video_batch_cells(video, vram, pending.data(), static_cast<int>(n));
    pending.erase(pending.begin(),
                  pending.begin() + static_cast<long>(n));
    cells_rendered = cell_target;
  }
};

uint8_t fr_mem_read(void* ctx, uint16_t addr, uint64_t) {
  return mem_fast_read(static_cast<FastChain*>(ctx)->mem, addr);
}
void fr_mem_write(void* ctx, uint16_t addr, uint8_t val, uint64_t now) {
  FastChain* fc = static_cast<FastChain*>(ctx);
  // A RAM write lands before cell floor(now/4) displays it (now is the
  // µs-aligned memory T1): render the cells before it, then commit.
  fc->advance_chars(FastChain::chars_before_access(now));
  fc->render_below(now / 4);
  mem_fast_write(fc->mem, addr, val);
}
uint8_t fr_io_read(void* ctx, uint16_t port, uint64_t now) {
  FastChain* fc = static_cast<FastChain*>(ctx);
  fc->advance_chars(FastChain::chars_before_access(now));
  uint8_t val = 0;
  if (crtc_fast_io_read(fc->crtc, port, &val)) return val;
  return 0xFF;
}
void fr_io_write(void* ctx, uint16_t port, uint8_t val, uint64_t now) {
  FastChain* fc = static_cast<FastChain*>(ctx);
  fc->advance_chars(FastChain::chars_before_access(now));
  fc->render_below(now / 4);  // catch-up-then-apply: pre-write cells first
  const uint8_t caused = crtc_fast_io_write(fc->crtc, port, val);
  if (caused & (1u << CRTC_EDGE_VSYNC_RISE)) {
    ga_batch_vsync_rise(fc->ga);
    // Per-cycle, the write-raised VSYNC reaches the video device before cell
    // floor(now/4) paints: patch that pending view's level and edge.
    if (!fc->pending.empty()) {
      CrtcCharView& view = fc->pending.back();
      view.levels |= CRTC_LVL_VSYNC;
      view.edges |= 1u << CRTC_EDGE_VSYNC_RISE;
    }
  }
  ga_fast_io_write(fc->ga, port, val);
  mem_fast_io_write(fc->mem, port, val);
}
uint8_t fr_int_ack(void* ctx, uint64_t now) {
  FastChain* fc = static_cast<FastChain*>(ctx);
  fc->advance_chars(FastChain::chars_before_access(now));
  ga_batch_int_ack(fc->ga);
  return 0xFF;  // the classic bus floats during the acknowledge
}

bool run_fast(const std::vector<uint8_t>& prog,
              const std::vector<uint8_t>& isr, MemSide& mem, RunResult* out) {
  seed_mem(mem, prog, isr);
  std::vector<uint8_t> zmem(z80_state_size());
  Device zdev = z80_init(zmem.data());
  std::vector<uint8_t> gmem(ga_state_size());
  Device gdev = ga_init(gmem.data());
  std::vector<uint8_t> cmem(crtc_state_size());
  Device cdev = crtc_init(cmem.data());
  std::vector<uint8_t> vmem(video_state_size());
  Device vdev = video_init(vmem.data());
  video_attach(&vdev, &gdev, out->fb.data(), kFbW, kFbH);

  FastChain fc{&mem.dev, &cdev, &gdev, &vdev};
  fc.vram = mem_video_ram(&mem.dev);
  const Z80BatchIO bio{&fc,        fr_mem_read, fr_mem_write,
                       fr_io_read, fr_io_write, fr_int_ack};
  const Z80Regs init = start_regs();
  z80_poke(&zdev, &init);

  Z80Regs r{};
  for (long guard = 0; guard < 30000000L; ++guard) {
    z80_peek(&zdev, &r);
    if (r.halted) {
      fc.advance_chars(FastChain::chars_visible_at(r.tstates));
      if (r.iff1 == 0) {  // parked — render everything advanced and finish
        fc.render_below(fc.chars_done);
        out->z = r;
        return true;
      }
      if (!ga_irq_asserted(&gdev)) {
        fc.advance_chars(fc.chars_done + 1);
        const uint64_t vis = (4 * (fc.chars_done - 1)) + 1;
        if (vis > r.tstates)
          z80_batch_halt(&zdev, static_cast<uint32_t>(vis - r.tstates));
        continue;
      }
    }
    const uint64_t b_al = (r.tstates + 3) & ~3ULL;
    fc.advance_chars(FastChain::chars_visible_at(b_al));
    z80_batch_step(&zdev, &bio, ga_irq_asserted(&gdev), 0xFF, /*grid=*/1);
  }
  return false;
}

std::string diff_regs(const Z80Regs& a, const Z80Regs& b) {
  std::string d;
  auto cmp = [&](const char* n, uint64_t g, uint64_t e) {
    if (g != e) {
      char buf[64];
      std::snprintf(buf, sizeof buf, " %s=%llX!=%llX", n,
                    (unsigned long long)g, (unsigned long long)e);
      d += buf;
    }
  };
  cmp("AF", a.af, b.af);
  cmp("BC", a.bc, b.bc);
  cmp("DE", a.de, b.de);
  cmp("HL", a.hl, b.hl);
  cmp("SP", a.sp, b.sp);
  cmp("PC", a.pc, b.pc);
  cmp("R", a.r, b.r);
  cmp("T", a.tstates, b.tstates);
  cmp("IC", a.instr_count, b.instr_count);
  return d;
}

// Distinct RGB triples in a framebuffer — the vacuous-black guard.
size_t distinct_colours(const std::vector<uint8_t>& fb) {
  std::set<uint32_t> seen;
  for (size_t i = 0; i + 2 < fb.size(); i += 3)
    seen.insert(static_cast<uint32_t>(fb[i]) << 16 |
                static_cast<uint32_t>(fb[i + 1]) << 8 | fb[i + 2]);
  return seen.size();
}

void run_lockstep(const std::vector<uint8_t>& prog,
                  const std::vector<uint8_t>& isr, size_t min_colours) {
  MemSide mem_pc, mem_fast;
  RunResult pc{}, fast{};
  ASSERT_TRUE(run_percycle(prog, isr, mem_pc, &pc))
      << "per-cycle reference never parked";
  ASSERT_TRUE(run_fast(prog, isr, mem_fast, &fast))
      << "fast driver-let never parked";

  EXPECT_EQ(diff_regs(fast.z, pc.z), "");
  for (uint32_t a = 0; a < 0x10000; ++a) {
    ASSERT_EQ(mem_read_ram(&mem_fast.dev, static_cast<uint16_t>(a)),
              mem_read_ram(&mem_pc.dev, static_cast<uint16_t>(a)))
        << "RAM diverged at " << a;
  }

  // The pixel-lockstep gate — and the screenshot-guard: a doubly-black pair
  // must not pass.
  ASSERT_GE(distinct_colours(pc.fb), min_colours)
      << "reference framebuffer suspiciously flat — scenario broken";
  int diverged = 0;
  for (size_t i = 0; i < kFbLen && diverged < 8; i += 3) {
    if (fast.fb[i] != pc.fb[i] || fast.fb[i + 1] != pc.fb[i + 1] ||
        fast.fb[i + 2] != pc.fb[i + 2]) {
      const size_t px = i / 3;
      ADD_FAILURE() << "pixel (" << px % kFbW << "," << px / kFbW
                    << ") diverged: fast=" << int(fast.fb[i]) << ","
                    << int(fast.fb[i + 1]) << "," << int(fast.fb[i + 2])
                    << " ref=" << int(pc.fb[i]) << "," << int(pc.fb[i + 1])
                    << "," << int(pc.fb[i + 2]);
      ++diverged;
    }
  }
  EXPECT_EQ(std::memcmp(fast.fb.data(), pc.fb.data(), kFbLen), 0)
      << "framebuffers diverge";
}

}  // namespace

TEST(FastVideoRender, StaticScreenPixelEqual) {
  run_lockstep(prog_static_screen(), {0x76}, 4);
}

TEST(FastVideoRender, MidFrameInkAndModeSplit) {
  run_lockstep(prog_band_split(), isr_band_split(), 5);
}

TEST(FastVideoRender, MidScanlineInkFlip) {
  run_lockstep(prog_midscanline_flip(), {0x76}, 2);
}
