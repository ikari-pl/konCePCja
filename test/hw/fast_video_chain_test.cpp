/* fast_video_chain_test.cpp — F4 (beads-tnal): the CRTC/GA batch engine in
 * LOCKSTEP with the per-cycle board, interrupt delivery exact to the T-state.
 *
 * The fast side is a driver-let: F2's z80_batch_step + F3's memory seam +
 * F4's crtc_advance_chars / ga_batch_* — the prototype of the F5/F6 frame
 * scheduler. The video chain runs BEHIND the CPU (catch-up), never ahead,
 * except while the CPU is halted (when it cannot write).
 *
 * TIME MAPPING (derived from the two-phase bus's one-hop latency; these
 * formulas ARE the contract this test proves):
 *   - the Z80's T-state i executes at master 4i+1; CRTC char k at 16k+1;
 *   - a GA irq change caused by char k's edges is CPU-visible at T-state
 *     4k+1  → edges visible at time B are those with k <= (B-1)/4;
 *   - an I/O write (or INT ack) whose T1 is T-state tau is seen by snooping
 *     devices after char floor(tau/4) → catch up floor(tau/4)+1 chars, apply;
 *   - a non-halted instruction boundary at B samples INT at its ALIGNED M1 T1
 *     (roundup4(B)) — edges landing inside the alignment holds are seen, so
 *     the driver samples at roundup4(B), exactly like the ticked quantiser;
 *   - a HALTED cpu samples INT at every T-state, so the wake burn ends at the
 *     firing edge's 4k+1 exactly.
 *
 * Scenarios (each compared at the PARKED HALT: full Z80 state INCLUDING
 * tstates — the T-state-exact delivery proof — plus all RAM, the CRTC
 * register file, and the GA's software registers; the sync-counter
 * micro-state is proven transitively by the repeated deliveries, since any
 * counter drift shifts a later acceptance and diverges tstates/RAM):
 *   1. HaltFirstIrqLatency — EI; HALT; ISR parks: nails the very first
 *      delivery to the T-state.
 *   2. IsrCadenceEightInterrupts — a counting mainline + a logging ISR: every
 *      acceptance timestamps the mainline's progress into a RAM log; parks
 *      after 8. Distributed delivery-time equality.
 *   3. MidStreamR7AndRearmWrites — the mainline periodically rewrites CRTC R7
 *      (write-triggered VSYNC + resync) and the GA rearm bit mid-frame:
 *      catch-up-then-apply on the video chain, R7-class (plan §4.5).
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "hw/board.h"
#include "hw/crtc.h"
#include "hw/gate_array.h"
#include "hw/memory.h"
#include "hw/z80.h"

namespace {

constexpr uint16_t kProg = 0x8000;
constexpr uint16_t kIsr = 0x0038;

// ---------------------------------------------------------------------------
// Program assembly (small emit helpers over a byte vector).
// ---------------------------------------------------------------------------
struct Asm {
  std::vector<uint8_t> bytes;
  void db(std::initializer_list<uint8_t> bs) {
    bytes.insert(bytes.end(), bs.begin(), bs.end());
  }
  void ld_bc(uint16_t nn) {
    db({0x01, static_cast<uint8_t>(nn & 0xFF), static_cast<uint8_t>(nn >> 8)});
  }
  void out_c_c() { db({0xED, 0x49}); }
  void crtc_reg(uint8_t reg, uint8_t val) {
    ld_bc(static_cast<uint16_t>(0xBC00 | reg));
    out_c_c();
    ld_bc(static_cast<uint16_t>(0xBD00 | val));
    out_c_c();
  }
};

// Shared prologue: ROMs off, the standard CPC frame on the CRTC, IM 1.
void emit_prologue(Asm& a) {
  a.ld_bc(0x7F8C);  // GA fn2: both ROMs off (the RAM ISR at 0x38 must fetch)
  a.out_c_c();
  const uint8_t regs[][2] = {{0, 63}, {1, 40}, {2, 46},  {3, 0x8E}, {4, 38},
                             {5, 0},  {6, 25}, {7, 30},  {9, 7},    {12, 0x30},
                             {13, 0}};
  for (const auto& rv : regs) a.crtc_reg(rv[0], rv[1]);
  a.db({0xED, 0x56});  // IM 1
}

// Scenario 1: EI; HALT. ISR = HALT (acceptance cleared IFF1 → parked).
std::vector<uint8_t> prog_halt_latency() {
  Asm a;
  emit_prologue(a);
  a.db({0xFB, 0x76});  // EI; HALT
  return a.bytes;
}
std::vector<uint8_t> isr_halt_latency() { return {0x76}; }

// Scenarios 2/3 mainline: init the counters, EI, then count forever —
// LD HL,(9000); INC HL; LD (9000),HL; [scenario-3 insert]; JR loop.
std::vector<uint8_t> prog_cadence(bool with_midstream_writes) {
  Asm a;
  emit_prologue(a);
  a.db({0x21, 0x00, 0x00});  // LD HL,0
  a.db({0x22, 0x00, 0x90});  // LD (0x9000),HL  — mainline counter
  a.db({0x22, 0x04, 0x90});  // LD (0x9004),HL  — irq count
  a.db({0x21, 0x00, 0x91});  // LD HL,0x9100
  a.db({0x22, 0x02, 0x90});  // LD (0x9002),HL  — log write pointer
  a.db({0xFB});              // EI
  const size_t loop = a.bytes.size();
  a.db({0x2A, 0x00, 0x90});  // LD HL,(0x9000)
  a.db({0x23});              // INC HL
  a.db({0x22, 0x00, 0x90});  // LD (0x9000),HL
  if (with_midstream_writes) {
    // Every time the counter's low byte hits 200: rewrite R7 (a mid-frame
    // write-triggered VSYNC path) and rearm the GA interrupt (fn2 bit 4).
    // Fires periodically (every 256 iterations) — deterministic on both
    // sides, and each firing exercises catch-up-then-apply again.
    a.db({0x7D});        // LD A,L
    a.db({0xFE, 0xC8});  // CP 200
    const size_t jr_at = a.bytes.size();
    a.db({0x20, 0x00});  // JR NZ, +skip (patched below)
    a.ld_bc(0xBC07);     // select R7
    a.out_c_c();
    a.ld_bc(0xBD04);  // R7 = 4
    a.out_c_c();
    a.ld_bc(0x7F9C);  // GA fn2: rearm (bit4) + both ROMs stay off
    a.out_c_c();
    a.bytes[jr_at + 1] = static_cast<uint8_t>(a.bytes.size() - (jr_at + 2));
  }
  const int8_t back = static_cast<int8_t>(
      -static_cast<int>(a.bytes.size() - loop) - 2);  // JR displacement
  a.db({0x18, static_cast<uint8_t>(back)});           // JR loop
  return a.bytes;
}

// The logging ISR: log the mainline counter, count the interrupt, park after
// 8 (HALT with IFF1 still clear), otherwise EI; RET.
std::vector<uint8_t> isr_cadence() {
  Asm a;
  a.db({0xF5, 0xE5, 0xD5});        // PUSH AF, HL, DE
  a.db({0xED, 0x5B, 0x00, 0x90});  // LD DE,(0x9000)   mainline progress
  a.db({0x2A, 0x02, 0x90});        // LD HL,(0x9002)   log ptr
  a.db({0x73, 0x23, 0x72, 0x23});  // LD (HL),E; INC HL; LD (HL),D; INC HL
  a.db({0x22, 0x02, 0x90});        // LD (0x9002),HL
  a.db({0x2A, 0x04, 0x90});        // LD HL,(0x9004)   irq count
  a.db({0x23});                    // INC HL
  a.db({0x22, 0x04, 0x90});        // LD (0x9004),HL
  a.db({0x7D, 0xFE, 0x08});        // LD A,L; CP 8
  a.db({0x28, 0x05});              // JR Z, park (over the 5 epilogue bytes)
  a.db({0xD1, 0xE1, 0xF1});        // POP DE, HL, AF
  a.db({0xFB, 0xC9});              // EI; RET
  a.db({0x76});                    // park: HALT (IFF1 clear)
  return a.bytes;
}

// ---------------------------------------------------------------------------
// One memory side (the real memory Device, seeded identically per run).
// ---------------------------------------------------------------------------
struct MemSide {
  std::vector<uint8_t> storage = std::vector<uint8_t>(mem_state_size());
  Device dev{};
};

void seed_mem(MemSide& side, const std::vector<uint8_t>& prog,
              const std::vector<uint8_t>& isr) {
  side.dev = mem_init(side.storage.data());
  for (uint32_t a = 0; a < 0x10000; ++a)
    mem_write_ram(&side.dev, static_cast<uint16_t>(a), 0x00);
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

// ---------------------------------------------------------------------------
// Per-cycle reference: Z80 + GA + CRTC + memory on the two-phase bus, run to
// the parked HALT (halted with IFF1 clear).
// ---------------------------------------------------------------------------
struct ChainState {
  Z80Regs z{};
  CrtcRegs crtc{};
  GateArrayRegs ga{};
};

bool run_percycle(const std::vector<uint8_t>& prog,
                  const std::vector<uint8_t>& isr, MemSide& mem,
                  ChainState* out) {
  seed_mem(mem, prog, isr);
  std::vector<uint8_t> zmem(z80_state_size());
  Device zdev = z80_init(zmem.data());
  std::vector<uint8_t> gmem(ga_state_size());
  Device gdev = ga_init(gmem.data());
  std::vector<uint8_t> cmem(crtc_state_size());
  Device cdev = crtc_init(cmem.data());

  Board board;
  board_init(&board);
  board_add(&board, gdev);
  board_add(&board, cdev);
  board_add(&board, mem.dev);
  board_add(&board, zdev);
  board_reset(&board);
  const Z80Regs init = start_regs();
  z80_poke(&zdev, &init);

  for (long tick = 0; tick < 60000000L; ++tick) {
    board_tick(&board);
    z80_peek(&zdev, &out->z);
    if (out->z.halted && out->z.iff1 == 0) {  // parked
      crtc_peek(&cdev, &out->crtc);
      ga_peek(&gdev, &out->ga);
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// The fast driver-let.
// ---------------------------------------------------------------------------
struct FastChain {
  Device* mem;
  Device* crtc;
  Device* ga;
  uint64_t chars_done = 0;

  // Edges visible to the CPU at T-state time B: chars k <= (B-1)/4.
  static uint64_t chars_visible_at(uint64_t tstate) {
    return tstate == 0 ? 0 : (tstate - 1) / 4 + 1;
  }
  // An I/O access with T1/sample at T-state tau lands after char floor(tau/4).
  static uint64_t chars_before_access(uint64_t tau) { return tau / 4 + 1; }

  void advance_chars(uint64_t target) {
    if (target <= chars_done) return;
    const uint32_t n = static_cast<uint32_t>(target - chars_done);
    // Worst legal density is two edges per char (a same-char VSYNC + HSYNC
    // transition), so this can never truncate.
    std::vector<CrtcEdge> edges(static_cast<size_t>(n) * 2 + 4);
    const int total =
        crtc_advance_chars(crtc, n, edges.data(), static_cast<int>(edges.size()));
    ASSERT_LE(total, static_cast<int>(edges.size())) << "edge buffer overflow";
    for (int i = 0; i < total; ++i) {
      switch (edges[i].kind) {
        case CRTC_EDGE_VSYNC_RISE:
          ga_batch_vsync_rise(ga);
          break;
        case CRTC_EDGE_HSYNC_RISE:
          ga_batch_hsync_rise(ga);
          break;
        case CRTC_EDGE_HSYNC_FALL:
          ga_batch_hsync_fall(ga);
          break;
        default:
          break;
      }
    }
    chars_done = target;
  }
};

uint8_t fc_mem_read(void* ctx, uint16_t addr, uint64_t) {
  return mem_fast_read(static_cast<FastChain*>(ctx)->mem, addr);
}
void fc_mem_write(void* ctx, uint16_t addr, uint8_t val, uint64_t) {
  mem_fast_write(static_cast<FastChain*>(ctx)->mem, addr, val);
}
uint8_t fc_io_read(void* ctx, uint16_t port, uint64_t now) {
  FastChain* fc = static_cast<FastChain*>(ctx);
  fc->advance_chars(FastChain::chars_before_access(now));
  uint8_t val = 0;
  if (crtc_fast_io_read(fc->crtc, port, &val)) return val;
  return 0xFF;  // nothing drives — the bus floats
}
void fc_io_write(void* ctx, uint16_t port, uint8_t val, uint64_t now) {
  FastChain* fc = static_cast<FastChain*>(ctx);
  fc->advance_chars(FastChain::chars_before_access(now));
  // Catch-up-then-apply, every decoder on the bus: a write-triggered VSYNC
  // rise (R7 hit) reaches the GA exactly as the level rise would.
  const uint8_t caused = crtc_fast_io_write(fc->crtc, port, val);
  if (caused & (1u << CRTC_EDGE_VSYNC_RISE)) ga_batch_vsync_rise(fc->ga);
  ga_fast_io_write(fc->ga, port, val);
  mem_fast_io_write(fc->mem, port, val);
}
uint8_t fc_int_ack(void* ctx, uint64_t now) {
  FastChain* fc = static_cast<FastChain*>(ctx);
  fc->advance_chars(FastChain::chars_before_access(now));
  ga_batch_int_ack(fc->ga);
  return 0xFF;  // the classic bus floats during the acknowledge
}

bool run_fast(const std::vector<uint8_t>& prog,
              const std::vector<uint8_t>& isr, MemSide& mem, ChainState* out) {
  seed_mem(mem, prog, isr);
  std::vector<uint8_t> zmem(z80_state_size());
  Device zdev = z80_init(zmem.data());
  std::vector<uint8_t> gmem(ga_state_size());
  Device gdev = ga_init(gmem.data());
  std::vector<uint8_t> cmem(crtc_state_size());
  Device cdev = crtc_init(cmem.data());

  FastChain fc{&mem.dev, &cdev, &gdev};
  const Z80BatchIO bio{&fc,        fc_mem_read, fc_mem_write,
                       fc_io_read, fc_io_write, fc_int_ack};
  const Z80Regs init = start_regs();
  z80_poke(&zdev, &init);

  Z80Regs r{};
  for (long guard = 0; guard < 20000000L; ++guard) {
    z80_peek(&zdev, &r);
    if (r.halted) {
      // A halted CPU samples INT every T-state: sync the chain to now, then
      // wait edge-by-edge, burning halt time to each edge's visibility.
      fc.advance_chars(FastChain::chars_visible_at(r.tstates));
      if (r.iff1 == 0) {  // parked — scenario end (IFF1 clear: nothing can
                          // wake it; mirrors the per-cycle stop condition)
        out->z = r;
        crtc_peek(&cdev, &out->crtc);
        ga_peek(&gdev, &out->ga);
        return true;
      }
      if (!ga_irq_asserted(&gdev)) {
        fc.advance_chars(fc.chars_done + 1);  // one more char
        const uint64_t vis = 4 * (fc.chars_done - 1) + 1;
        if (vis > r.tstates)
          z80_batch_halt(&zdev, static_cast<uint32_t>(vis - r.tstates));
        continue;
      }
      // irq visible now: fall through and step (the wake consumes no time).
    }
    // A non-halted boundary samples INT at its ALIGNED M1 T1 — include edges
    // that land inside the alignment holds, exactly like the ticked
    // quantiser.
    const uint64_t b_al = (r.tstates + 3) & ~3ULL;
    fc.advance_chars(FastChain::chars_visible_at(b_al));
    z80_batch_step(&zdev, &bio, ga_irq_asserted(&gdev), 0xFF, /*grid=*/1);
  }
  return false;
}

// ---------------------------------------------------------------------------
// Comparison.
// ---------------------------------------------------------------------------
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
  cmp("WZ", a.wz, b.wz);
  cmp("R", a.r, b.r);
  cmp("IFF1", a.iff1, b.iff1);
  cmp("IFF2", a.iff2, b.iff2);
  cmp("IM", a.im, b.im);
  cmp("T", a.tstates, b.tstates);
  cmp("IC", a.instr_count, b.instr_count);
  return d;
}

void run_lockstep(const std::vector<uint8_t>& prog,
                  const std::vector<uint8_t>& isr) {
  MemSide mem_pc, mem_fast;
  ChainState pc{}, fast{};
  ASSERT_TRUE(run_percycle(prog, isr, mem_pc, &pc))
      << "per-cycle reference never parked";
  ASSERT_TRUE(run_fast(prog, isr, mem_fast, &fast))
      << "fast driver-let never parked";

  // The T-state-exact interrupt-delivery proof: any delivery shift diverges
  // tstates at the parked HALT (scenario 1 directly; 2/3 also through the
  // ISR log in RAM).
  EXPECT_EQ(diff_regs(fast.z, pc.z), "");

  for (uint32_t a = 0; a < 0x10000; ++a) {
    ASSERT_EQ(mem_read_ram(&mem_fast.dev, static_cast<uint16_t>(a)),
              mem_read_ram(&mem_pc.dev, static_cast<uint16_t>(a)))
        << "RAM diverged at " << a;
  }

  // Write-path equality on the video chain's register files. The sync-counter
  // micro-state (sl_count/scanline/hcc) is compared only through the
  // delivery-time equality above: at the park instant the two observation
  // points straddle up to one char of bus latency by construction, so a
  // direct compare would be over-strict — while any REAL counter drift shifts
  // a later delivery and fails tstates/RAM.
  for (int i = 0; i < 18; ++i)
    EXPECT_EQ(fast.crtc.reg[i], pc.crtc.reg[i]) << "CRTC R" << i;
  EXPECT_EQ(fast.crtc.reg_select, pc.crtc.reg_select);
  EXPECT_EQ(fast.ga.pen, pc.ga.pen);
  EXPECT_EQ(fast.ga.mode, pc.ga.mode);
  EXPECT_EQ(fast.ga.req_mode, pc.ga.req_mode);
  EXPECT_EQ(fast.ga.rom_config, pc.ga.rom_config);
  EXPECT_EQ(fast.ga.ram_config, pc.ga.ram_config);
  EXPECT_EQ(fast.ga.irq, pc.ga.irq);
  for (int i = 0; i < 17; ++i)
    EXPECT_EQ(fast.ga.ink[i], pc.ga.ink[i]) << "ink " << i;
}

}  // namespace

TEST(FastVideoChain, HaltFirstIrqLatency) {
  run_lockstep(prog_halt_latency(), isr_halt_latency());
}

TEST(FastVideoChain, IsrCadenceEightInterrupts) {
  run_lockstep(prog_cadence(false), isr_cadence());
}

TEST(FastVideoChain, MidStreamR7AndRearmWrites) {
  run_lockstep(prog_cadence(true), isr_cadence());
}

// The horizon helper: ga_predict_irq_hsyncs must agree with committing the
// same number of edges — dry-run vs the ONE ga_raster_count definition.
TEST(FastVideoChain, IrqPredictionMatchesCommittedEdges) {
  std::vector<uint8_t> gmem(ga_state_size());
  Device gdev = ga_init(gmem.data());
  for (int seed = 0; seed < 80; ++seed) {
    const int predicted = ga_predict_irq_hsyncs(&gdev);
    ASSERT_GT(predicted, 0) << "no irq pending at a fresh edge";
    for (int i = 0; i < predicted - 1; ++i) {
      ga_batch_hsync_fall(&gdev);
      ASSERT_EQ(ga_irq_asserted(&gdev), 0)
          << "irq asserted " << (predicted - 1 - i) << " edges early";
    }
    ga_batch_hsync_fall(&gdev);
    ASSERT_EQ(ga_irq_asserted(&gdev), 1) << "irq not asserted on schedule";
    ga_batch_int_ack(&gdev);  // ack and go again from the mid-count state
  }
}
