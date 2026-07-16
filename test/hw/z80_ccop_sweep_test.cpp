/* z80_ccop_sweep_test.cpp — full-ISA instruction-timing sweep of the sub-cycle
 * Z80 + Gate Array WAIT quantiser against the legacy emulator's cc_* golden
 * tables (src/z80.cpp ~line 884). Generalizes the hand-picked anchors in
 * z80_timing_test.cpp to (nearly) every opcode of every prefix group.
 *
 * Table semantics (established from the legacy dispatch code, and validated
 * against the ~40 passing anchors in z80_timing_test.cpp):
 *   - cc_op[op]   TOTAL CPC T-states of an unprefixed opcode
 *                 (z80_execute_instruction: iCycleCount = cc_op[op]).
 *   - cc_cb[op]   cost EXCLUDING the CB prefix fetch; prefix costs
 *                 cc_op[0xCB] = 4, so total = 4 + cc_cb[op]
 *                 (evidence: RLC B anchor = 8 = 4 + cc_cb[0x00]).
 *   - cc_ed[op]   same scheme: total = 4 + cc_ed[op]
 *                 (evidence: SBC HL,BC anchor = 16 = 4 + cc_ed[0x42]).
 *   - cc_xy[op]   same scheme for DD/FD: total = 4 + cc_xy[op]
 *                 (evidence: ADD IX,BC = 16, LD A,(IX+d) = 20).
 *   - cc_xycb[op] DD/FD CB d op: the DD handler first adds cc_xy[0xCB] = 4,
 *                 then the DDCB handler adds cc_xycb[op], so
 *                 total = cc_op[0xDD] + cc_xy[0xCB] + cc_xycb[op]
 *                       = 8 + cc_xycb[op]   (z80.cpp:3667 + :4646).
 *   - cc_ex[op]   EXTRA T-states when a conditional is taken (indexed by the
 *                 unprefixed/xy sub-opcode: DJNZ/JR cc +4, RET cc/CALL cc +8,
 *                 JP cc +0) or when an ED block op repeats one more iteration
 *                 (indexed by the ED sub-opcode: LDIR/LDDR +4, CPIR/CPDR +8,
 *                 INIR/INDR/OTIR/OTDR +4). Applied per re-execution: the
 *                 legacy core rewinds PC by 2 and refetches the prefix+opcode
 *                 each iteration, so an N-iteration block costs
 *                 (N-1) * (4 + cc_ed + io + cc_ex) + (4 + cc_ed + io).
 *   - I/O macros  the table entry holds only the pre-I/O part; the remainder
 *                 is added when the I/O executes ("_" macros). Totals:
 *                   OUT (n),A  = Oa + Oa_ = 8 + 4  = 12
 *                   IN A,(n)   = Ia + Ia_ = 12 + 0 = 12
 *                   OUT (C),r  = 4 + Ox + Ox_ = 4 + 8 + 4  = 16
 *                   IN r,(C)   = 4 + Ix + Ix_ = 4 + 12 + 0 = 16
 *                   OUTI/OUTD  = 4 + Oy + Oy_ = 4 + 12 + 4 = 20
 *                   INI/IND    = 4 + Iy + Iy_ = 4 + 16 + 0 = 20
 *                 i.e. only the OUT-type opcodes carry a deferred +4; the
 *                 tables below are the raw values and io_extra_*() add it.
 *
 * Measurement: program runs on a GA board (real WAIT quantiser) from a poked
 * initial state until HALT; the HALT fetch always starts µs-aligned and costs
 * exactly 4 T, so instruction cost = tstates_at_halt - 4 (same convention as
 * z80_timing_test.cpp — including any alignment padding the instruction forces
 * onto the next fetch, which is precisely what the cc tables encode).
 *
 * Exclusions (each sweep documents its own):
 *   - 0x76 HALT: it is the harness terminator and never runs to a following
 *     fetch; its 4 T cost is implicitly exercised by every case via the -4.
 *   - 0xCB/0xDD/0xED/0xFD as "unprefixed opcodes": they are dispatchers, and
 *     each prefix group has its own dedicated sweep below.
 *   - DD/FD sub-opcodes 0xDD/0xFD/0xED: prefix chains. The legacy core treats
 *     each extra prefix as its own 4 T instruction (cc_xy[0xDD] = 4) and
 *     re-dispatches; there is no single "instruction total" to compare, so
 *     chained prefixes are out of scope for this harness.
 *   - DD/FD 0x76 (prefixed HALT): halts — same terminator problem as 0x76.
 *   - DD/FD 0xCB: covered by the dedicated DDCB/FDCB sweep.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "hw/board.h"
#include "hw/gate_array.h"
#include "hw/z80.h"

namespace {

// ---------------------------------------------------------------------------
// Golden tables — copied VERBATIM from src/z80.cpp (macros materialized:
// Oa=8 Ia=12 Ox=8 Ix=12 Oy=12 Iy=16). Do not "fix" values here; the whole
// point is to compare the hw sim against the legacy emulator's numbers.
// ---------------------------------------------------------------------------

const uint8_t cc_op[256] = {
    4,  12, 8,  8,  4,  4,  8,  4,  4,  12, 8,  8,  4,  4,  8, 4,   // 0x00
    12, 12, 8,  8,  4,  4,  8,  4,  12, 12, 8,  8,  4,  4,  8, 4,   // 0x10
    8,  12, 20, 8,  4,  4,  8,  4,  8,  12, 20, 8,  4,  4,  8, 4,   // 0x20
    8,  12, 16, 8,  12, 12, 12, 4,  8,  12, 16, 8,  4,  4,  8, 4,   // 0x30
    4,  4,  4,  4,  4,  4,  8,  4,  4,  4,  4,  4,  4,  4,  8, 4,   // 0x40
    4,  4,  4,  4,  4,  4,  8,  4,  4,  4,  4,  4,  4,  4,  8, 4,   // 0x50
    4,  4,  4,  4,  4,  4,  8,  4,  4,  4,  4,  4,  4,  4,  8, 4,   // 0x60
    8,  8,  8,  8,  8,  8,  4,  8,  4,  4,  4,  4,  4,  4,  8, 4,   // 0x70
    4,  4,  4,  4,  4,  4,  8,  4,  4,  4,  4,  4,  4,  4,  8, 4,   // 0x80
    4,  4,  4,  4,  4,  4,  8,  4,  4,  4,  4,  4,  4,  4,  8, 4,   // 0x90
    4,  4,  4,  4,  4,  4,  8,  4,  4,  4,  4,  4,  4,  4,  8, 4,   // 0xA0
    4,  4,  4,  4,  4,  4,  8,  4,  4,  4,  4,  4,  4,  4,  8, 4,   // 0xB0
    8,  12, 12, 12, 12, 16, 8,  16, 8,  12, 12, 4,  12, 20, 8, 16,  // 0xC0
    8,  12, 12, 8,  12, 16, 8,  16, 8,  4,  12, 12, 12, 4,  8, 16,  // 0xD0
    8,  12, 12, 24, 12, 16, 8,  16, 8,  4,  12, 4,  12, 4,  8, 16,  // 0xE0
    8,  12, 12, 4,  12, 16, 8,  16, 8,  8,  12, 4,  12, 4,  8, 16,  // 0xF0
};

const uint8_t cc_cb[256] = {
    4, 4, 4, 4, 4, 4, 12, 4, 4, 4, 4, 4, 4, 4, 12, 4,  // 0x00
    4, 4, 4, 4, 4, 4, 12, 4, 4, 4, 4, 4, 4, 4, 12, 4,  // 0x10
    4, 4, 4, 4, 4, 4, 12, 4, 4, 4, 4, 4, 4, 4, 12, 4,  // 0x20
    4, 4, 4, 4, 4, 4, 12, 4, 4, 4, 4, 4, 4, 4, 12, 4,  // 0x30
    4, 4, 4, 4, 4, 4, 8,  4, 4, 4, 4, 4, 4, 4, 8,  4,  // 0x40
    4, 4, 4, 4, 4, 4, 8,  4, 4, 4, 4, 4, 4, 4, 8,  4,  // 0x50
    4, 4, 4, 4, 4, 4, 8,  4, 4, 4, 4, 4, 4, 4, 8,  4,  // 0x60
    4, 4, 4, 4, 4, 4, 8,  4, 4, 4, 4, 4, 4, 4, 8,  4,  // 0x70
    4, 4, 4, 4, 4, 4, 12, 4, 4, 4, 4, 4, 4, 4, 12, 4,  // 0x80
    4, 4, 4, 4, 4, 4, 12, 4, 4, 4, 4, 4, 4, 4, 12, 4,  // 0x90
    4, 4, 4, 4, 4, 4, 12, 4, 4, 4, 4, 4, 4, 4, 12, 4,  // 0xA0
    4, 4, 4, 4, 4, 4, 12, 4, 4, 4, 4, 4, 4, 4, 12, 4,  // 0xB0
    4, 4, 4, 4, 4, 4, 12, 4, 4, 4, 4, 4, 4, 4, 12, 4,  // 0xC0
    4, 4, 4, 4, 4, 4, 12, 4, 4, 4, 4, 4, 4, 4, 12, 4,  // 0xD0
    4, 4, 4, 4, 4, 4, 12, 4, 4, 4, 4, 4, 4, 4, 12, 4,  // 0xE0
    4, 4, 4, 4, 4, 4, 12, 4, 4, 4, 4, 4, 4, 4, 12, 4,  // 0xF0
};

const uint8_t cc_ed[256] = {
    4,  4,  4,  4,  4, 4,  4, 4,  4,  4,  4,  4,  4, 4,  4, 4,   // 0x00
    4,  4,  4,  4,  4, 4,  4, 4,  4,  4,  4,  4,  4, 4,  4, 4,   // 0x10
    4,  4,  4,  4,  4, 4,  4, 4,  4,  4,  4,  4,  4, 4,  4, 4,   // 0x20
    4,  4,  4,  4,  4, 4,  4, 4,  4,  4,  4,  4,  4, 4,  4, 4,   // 0x30
    12, 8,  12, 20, 4, 12, 4, 8,  12, 8,  12, 20, 4, 12, 4, 8,   // 0x40
    12, 8,  12, 20, 4, 12, 4, 8,  12, 8,  12, 20, 4, 12, 4, 8,   // 0x50
    12, 8,  12, 20, 4, 12, 4, 16, 12, 8,  12, 20, 4, 12, 4, 16,  // 0x60
    12, 8,  12, 20, 4, 12, 4, 4,  12, 8,  12, 20, 4, 12, 4, 4,   // 0x70
    4,  4,  4,  4,  4, 4,  4, 4,  4,  4,  4,  4,  4, 4,  4, 4,   // 0x80
    4,  4,  4,  4,  4, 4,  4, 4,  4,  4,  4,  4,  4, 4,  4, 4,   // 0x90
    16, 12, 16, 12, 4, 4,  4, 4,  16, 12, 16, 12, 4, 4,  4, 4,   // 0xA0
    16, 12, 16, 12, 4, 4,  4, 4,  16, 12, 16, 12, 4, 4,  4, 4,   // 0xB0
    4,  4,  4,  4,  4, 4,  4, 4,  4,  4,  4,  4,  4, 4,  4, 4,   // 0xC0
    4,  4,  4,  4,  4, 4,  4, 4,  4,  4,  4,  4,  4, 4,  4, 4,   // 0xD0
    4,  4,  4,  4,  4, 4,  4, 4,  4,  4,  4,  4,  4, 4,  4, 4,   // 0xE0
    4,  4,  4,  4,  4, 4,  4, 4,  4,  4,  4,  4,  4, 4,  4, 4,   // 0xF0
};

const uint8_t cc_xy[256] = {
    4,  12, 8,  8,  4,  4,  8,  4,  4,  12, 8,  8,  4,  4,  8,  4,   // 0x00
    12, 12, 8,  8,  4,  4,  8,  4,  12, 12, 8,  8,  4,  4,  8,  4,   // 0x10
    8,  12, 20, 8,  4,  4,  8,  4,  8,  12, 20, 8,  4,  4,  8,  4,   // 0x20
    8,  12, 16, 8,  20, 20, 20, 4,  8,  12, 16, 8,  4,  4,  8,  4,   // 0x30
    4,  4,  4,  4,  4,  4,  16, 4,  4,  4,  4,  4,  4,  4,  16, 4,   // 0x40
    4,  4,  4,  4,  4,  4,  16, 4,  4,  4,  4,  4,  4,  4,  16, 4,   // 0x50
    4,  4,  4,  4,  4,  4,  16, 4,  4,  4,  4,  4,  4,  4,  16, 4,   // 0x60
    16, 16, 16, 16, 16, 16, 4,  16, 4,  4,  4,  4,  4,  4,  16, 4,   // 0x70
    4,  4,  4,  4,  4,  4,  16, 4,  4,  4,  4,  4,  4,  4,  16, 4,   // 0x80
    4,  4,  4,  4,  4,  4,  16, 4,  4,  4,  4,  4,  4,  4,  16, 4,   // 0x90
    4,  4,  4,  4,  4,  4,  16, 4,  4,  4,  4,  4,  4,  4,  16, 4,   // 0xA0
    4,  4,  4,  4,  4,  4,  16, 4,  4,  4,  4,  4,  4,  4,  16, 4,   // 0xB0
    8,  12, 12, 12, 12, 16, 8,  16, 8,  12, 12, 4,  12, 20, 8,  16,  // 0xC0
    8,  12, 12, 8,  12, 16, 8,  16, 8,  4,  12, 12, 12, 4,  8,  16,  // 0xD0
    8,  12, 12, 24, 12, 16, 8,  16, 8,  4,  12, 4,  12, 4,  8,  16,  // 0xE0
    8,  12, 12, 4,  12, 16, 8,  16, 8,  8,  12, 4,  12, 4,  8,  16,  // 0xF0
};

const uint8_t cc_xycb[256] = {
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,  // 0x00
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,  // 0x10
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,  // 0x20
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,  // 0x30
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,  // 0x40
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,  // 0x50
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,  // 0x60
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,  // 0x70
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,  // 0x80
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,  // 0x90
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,  // 0xA0
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,  // 0xB0
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,  // 0xC0
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,  // 0xD0
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,  // 0xE0
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,  // 0xF0
};

const uint8_t cc_ex[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0x00
    4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0x10
    4, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0,  // 0x20
    4, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0,  // 0x30
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0x40
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0x50
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0x60
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0x70
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0x80
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0x90
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0xA0
    4, 8, 4, 4, 0, 0, 0, 0, 4, 8, 4, 4, 0, 0, 0, 0,  // 0xB0
    8, 0, 0, 0, 8, 0, 0, 0, 8, 0, 0, 0, 8, 0, 0, 0,  // 0xC0
    8, 0, 0, 0, 8, 0, 0, 0, 8, 0, 0, 0, 8, 0, 0, 0,  // 0xD0
    8, 0, 0, 0, 8, 0, 0, 0, 8, 0, 0, 0, 8, 0, 0, 0,  // 0xE0
    8, 0, 0, 0, 8, 0, 0, 0, 8, 0, 0, 0, 8, 0, 0, 0,  // 0xF0
};

// Deferred I/O T-states (the "_" macros): only OUT-type opcodes carry a +4.
int io_extra_base(uint8_t op) {  // unprefixed / DD / FD sweeps
  return op == 0xD3 ? 4 : 0;     // Oa_ = 4; Ia_ = 0
}
int io_extra_ed(uint8_t op) {
  if (op >= 0x40 && op <= 0x7F && (op & 0x07) == 0x01)
    return 4;  // OUT (C),r — Ox_
  if (op == 0xA3 || op == 0xAB || op == 0xB3 || op == 0xBB)
    return 4;  // OUTI/OUTD/OTIR/OTDR — Oy_
  return 0;    // all INs: Ix_ = Iy_ = 0
}

// ---------------------------------------------------------------------------
// Harness (same pattern as z80_timing_test.cpp: RAM + Gate Array + Z80, run
// from a poked state until HALT, cost = tstates - 4).
// ---------------------------------------------------------------------------

struct Ram {
  uint8_t cells[0x10000];
};
void sram_tick(void* self, const Bus* in, Bus* out) {
  Ram* ram = static_cast<Ram*>(self);
  if (in->cpu.mreq && in->cpu.wr)
    ram->cells[in->cpu.addr] = in->cpu.data;
  else if (in->cpu.mreq && in->cpu.rd)
    out->cpu.data = ram->cells[in->cpu.addr];
}
void sno_reset(void*) {}
size_t sram_size(const void*) { return sizeof(Ram); }
void sram_save(const void* s, void* b) { std::memcpy(b, s, sizeof(Ram)); }
void sram_load(void* s, const void* b) { std::memcpy(s, b, sizeof(Ram)); }
Device sram_device(Ram* s) {
  return Device{s,         "ram",     sram_tick, sno_reset,
                sram_size, sram_save, sram_load};
}

// Fixed memory layout shared by all cases.
constexpr uint16_t kProg =
    0x0100;  // program under test (clear of the RST vectors)
constexpr uint16_t kIsland = 0x0200;  // a HALT for jump/call/return targets
constexpr uint16_t kData = 0x8000;    // scratch data area for memory operands
constexpr uint16_t kData2 = 0x9000;   // second data area (DE for block ops)
constexpr uint16_t kStack = 0xFF00;   // SP; [SP] preloaded with kIsland

struct SweepCase {
  std::string name;
  std::vector<uint8_t> prog;  // placed at kProg
  Z80Regs init;
  int expected;  // CPC total T-states from the cc tables
};

Z80Regs default_init() {
  Z80Regs s{};
  s.af = 0x8100;  // A = 0x81: no CPIR match against zeroed RAM, and I/O
                  // addresses formed from A (OUT (n),A) select no device.
  s.bc = 0xA000;
  s.de = kData2;
  s.hl = kData;
  s.ix = kData;
  s.iy = kData;
  s.sp = kStack;
  s.pc = kProg;
  return s;
}

// Seed the shared memory layout for one case.
void seed_ram(Ram* ram, const SweepCase& c) {
  std::memset(ram->cells, 0, sizeof(Ram::cells));
  for (uint16_t v = 0x0000; v <= 0x0038; v += 8)
    ram->cells[v] = 0x76;  // RST targets
  ram->cells[kIsland] = 0x76;
  ram->cells[kStack] = static_cast<uint8_t>(kIsland & 0xFF);  // RET target
  ram->cells[kStack + 1] = static_cast<uint8_t>(kIsland >> 8);
  for (size_t i = 0; i < c.prog.size(); ++i) ram->cells[kProg + i] = c.prog[i];
}

// Run one case on the GA board; returns tstates-at-HALT - 4, or -1 if the
// program never reached HALT (decode hang — itself a failure). Also hands the
// final architectural state and memory back for the dual-mode diff.
int64_t run_case(const SweepCase& c, Z80Regs* out_regs, Ram* ram) {
  seed_ram(ram, c);

  std::vector<uint8_t> zmem(z80_state_size());
  Device zdev = z80_init(zmem.data());
  std::vector<uint8_t> gmem(ga_state_size());
  Device gdev = ga_init(gmem.data());

  Board board;
  board_init(&board);
  board_add(&board, gdev);
  board_add(&board, sram_device(ram));
  board_add(&board, zdev);
  board_reset(&board);
  z80_poke(&zdev, &c.init);

  Z80Regs r{};
  for (int tick = 0; tick < 200000; ++tick) {
    board_tick(&board);
    z80_peek(&zdev, &r);
    if (r.halted) {
      *out_regs = r;
      return static_cast<int64_t>(r.tstates) - 4;
    }
  }
  return -1;
}

// The same case in BATCH mode (z80_batch_step, cpc_grid on): no board, no GA —
// the grid is the driver's closed-form arithmetic, which is exactly what this
// dual-mode diff proves equivalent to the ticked quantiser (plan §5.5, the
// beads-95kn exit gate). I/O reads float 0xFF (the rest bus — no device claims
// the sweep's ports on the per-cycle board either); I/O writes land nowhere.
uint8_t swp_mem_read(void* ctx, uint16_t addr, uint64_t) {
  return static_cast<Ram*>(ctx)->cells[addr];
}
void swp_mem_write(void* ctx, uint16_t addr, uint8_t val, uint64_t) {
  static_cast<Ram*>(ctx)->cells[addr] = val;
}
uint8_t swp_io_read(void*, uint16_t, uint64_t) { return 0xFF; }
void swp_io_write(void*, uint16_t, uint8_t, uint64_t) {}

int64_t run_case_batch(const SweepCase& c, Z80Regs* out_regs, Ram* ram) {
  seed_ram(ram, c);
  std::vector<uint8_t> zmem(z80_state_size());
  Device zdev = z80_init(zmem.data());
  z80_poke(&zdev, &c.init);

  const Z80BatchIO bio{ram, swp_mem_read, swp_mem_write, swp_io_read,
                       swp_io_write};
  Z80Regs r{};
  for (int steps = 0; steps < 50000; ++steps) {
    z80_batch_step(&zdev, &bio, /*irq=*/0, /*vector=*/0xFF, /*grid=*/1);
    z80_peek(&zdev, &r);
    if (r.halted) {
      *out_regs = r;
      return static_cast<int64_t>(r.tstates) - 4;
    }
  }
  return -1;
}

// Field-by-field architectural diff (memcmp would trip on struct padding).
std::string diff_regs(const Z80Regs& a, const Z80Regs& b) {
  std::string d;
  auto cmp = [&](const char* n, uint64_t g, uint64_t e) {
    if (g != e) {
      char buf[64];
      std::snprintf(buf, sizeof buf, " %s=%llX!=%llX", n, (unsigned long long)g,
                    (unsigned long long)e);
      d += buf;
    }
  };
  cmp("AF", a.af, b.af);
  cmp("BC", a.bc, b.bc);
  cmp("DE", a.de, b.de);
  cmp("HL", a.hl, b.hl);
  cmp("AF'", a.af_, b.af_);
  cmp("BC'", a.bc_, b.bc_);
  cmp("DE'", a.de_, b.de_);
  cmp("HL'", a.hl_, b.hl_);
  cmp("IX", a.ix, b.ix);
  cmp("IY", a.iy, b.iy);
  cmp("SP", a.sp, b.sp);
  cmp("PC", a.pc, b.pc);
  cmp("WZ", a.wz, b.wz);
  cmp("I", a.i, b.i);
  cmp("R", a.r, b.r);
  cmp("IM", a.im, b.im);
  cmp("IFF1", a.iff1, b.iff1);
  cmp("IFF2", a.iff2, b.iff2);
  cmp("Q", a.q, b.q);
  cmp("HALT", a.halted, b.halted);
  cmp("T", a.tstates, b.tstates);
  cmp("IC", a.instr_count, b.instr_count);
  return d;
}

// ---------------------------------------------------------------------------
// Every case is asserted against the Bread80-confirmed cc tables. There is
// deliberately NO "known mismatch" escape hatch (beads-v3ig): an earlier
// version let an entry record the DUT's own output as the expected value — a
// self-bless that had historically enshrined 18 wrong I/O timings (IN r,(C)/OUT
// (C),r at 12 T instead of 16 T, OTIR at 28 T instead of 24 T). Those were real
// bugs, since fixed (the GA now drives cpu.wait and MC::IO free-runs its T1).
// Any future divergence from cc must be investigated and fixed — or, if cc
// itself is ever shown wrong against an external hardware oracle, cc corrected
// WITH that citation — never papered over by asserting whatever the sim happens
// to emit.
// ---------------------------------------------------------------------------

void run_all(const std::vector<SweepCase>& cases) {
  auto ram_pc = std::make_unique<Ram>();
  auto ram_batch = std::make_unique<Ram>();
  for (const auto& c : cases) {
    Z80Regs regs_pc{}, regs_batch{};
    const int64_t got = run_case(c, &regs_pc, ram_pc.get());
    ASSERT_GE(got, 0) << c.name << ": never reached HALT (decode or WAIT hang)";
    EXPECT_EQ(got, c.expected)
        << c.name << ": CPC total vs the Bread80-confirmed cc table";

    // Dual-mode: the batch engine must agree with the per-cycle engine on the
    // CPC-grid T-total, the full architectural state, AND memory — the
    // RunTier::Fast equivalence gate (plan §5.5, beads-95kn).
    const int64_t got_batch = run_case_batch(c, &regs_batch, ram_batch.get());
    ASSERT_GE(got_batch, 0) << c.name << ": batch mode never reached HALT";
    EXPECT_EQ(got_batch, got)
        << c.name << ": batch-mode T-total diverges from per-cycle";
    EXPECT_EQ(diff_regs(regs_batch, regs_pc), "")
        << c.name << ": batch-mode architectural state diverges";
    EXPECT_EQ(std::memcmp(ram_batch->cells, ram_pc->cells, sizeof(Ram::cells)),
              0)
        << c.name << ": batch-mode memory diverges";
  }
}

// ---------------------------------------------------------------------------
// Case generation
// ---------------------------------------------------------------------------

std::string hexname(const char* group, std::initializer_list<uint8_t> ops,
                    const char* variant) {
  char buf[64];
  std::string s = group;
  for (uint8_t b : ops) {
    std::snprintf(buf, sizeof buf, " %02X", b);
    s += buf;
  }
  if (variant[0] != '\0') {
    s += " ";
    s += variant;
  }
  return s;
}

// Flag values that make condition y (NZ Z NC C PO PE P M) true / false.
uint8_t cond_f(int y, bool taken) {
  static const uint8_t bit[8] = {0x40, 0x40, 0x01, 0x01,
                                 0x04, 0x04, 0x80, 0x80};
  const bool wants_set = (y & 1) != 0;  // odd conditions test the bit SET
  return (wants_set == taken) ? bit[y] : 0x00;
}

// Does `op` (base opcode space) address memory through HL, i.e. needs a
// displacement byte when DD/FD-prefixed?
bool uses_hl_mem(uint8_t op) {
  if (op == 0x34 || op == 0x35 || op == 0x36) return true;  // INC/DEC/LD (HL),n
  if (op >= 0x70 && op <= 0x77 && op != 0x76) return true;  // LD (HL),r
  if (op >= 0x40 && op <= 0x7F && (op & 0x07) == 0x06 && op != 0x76)
    return true;  // LD r,(HL)
  if (op >= 0x80 && op <= 0xBF && (op & 0x07) == 0x06)
    return true;  // ALU A,(HL)
  return false;
}

bool is_imm8(uint8_t op) {
  switch (op) {
    case 0x06:
    case 0x0E:
    case 0x16:
    case 0x1E:
    case 0x26:
    case 0x2E:
    case 0x36:
    case 0x3E:
    case 0xC6:
    case 0xCE:
    case 0xD6:
    case 0xDE:
    case 0xE6:
    case 0xEE:
    case 0xF6:
    case 0xFE:
    case 0xD3:
    case 0xDB:
      return true;
    default:
      return false;
  }
}

bool is_imm16_value(uint8_t op) {  // LD rr,nn
  return op == 0x01 || op == 0x11 || op == 0x21 || op == 0x31;
}
bool is_imm16_addr(uint8_t op) {  // (nn) memory ops
  return op == 0x22 || op == 0x2A || op == 0x32 || op == 0x3A;
}

// Sweep the base opcode space: prefix_byte == 0 for unprefixed (vs cc_op),
// 0xDD/0xFD for the index-prefixed sweep (vs cc_xy, +4 prefix fetch).
void gen_base_sweep(std::vector<SweepCase>& out, uint8_t prefix_byte) {
  const bool indexed = prefix_byte != 0;
  const uint8_t* cc = indexed ? cc_xy : cc_op;
  const int pcost = indexed ? 4 : 0;
  const char* group = indexed ? (prefix_byte == 0xDD ? "DD" : "FD") : "op";

  for (int opi = 0; opi < 256; ++opi) {
    const uint8_t op = static_cast<uint8_t>(opi);
    // Exclusions — see the header comment.
    if (op == 0x76) continue;  // HALT (terminator)
    if (op == 0xCB || op == 0xDD || op == 0xED || op == 0xFD)
      continue;  // prefixes

    const int total = pcost + cc[op] + io_extra_base(op);
    auto emit = [&](const char* variant, std::vector<uint8_t> body,
                    Z80Regs init, int expected) {
      SweepCase c;
      c.name = hexname(group, {op}, variant);
      if (indexed) c.prog.push_back(prefix_byte);
      c.prog.insert(c.prog.end(), body.begin(), body.end());
      c.init = init;
      c.expected = expected;
      out.push_back(std::move(c));
    };
    Z80Regs init = default_init();

    if (op == 0x10) {  // DJNZ d — B is the counter (high byte of BC)
      Z80Regs taken = init, skip = init;
      taken.bc = 0x0200;  // B=2: decrements to 1, jumps (d=0 lands on the HALT)
      skip.bc = 0x0100;   // B=1: decrements to 0, falls through
      emit("taken", {op, 0x00, 0x76}, taken, total + cc_ex[op]);
      emit("not-taken", {op, 0x00, 0x76}, skip, total);
    } else if (op == 0x18) {  // JR d — d=0 lands on the HALT either way
      emit("", {op, 0x00, 0x76}, init, total);
    } else if (op == 0x20 || op == 0x28 || op == 0x30 ||
               op == 0x38) {  // JR cc,d
      const int y = ((op >> 3) & 0x07) - 4;
      Z80Regs taken = init, skip = init;
      taken.af = (init.af & 0xFF00) | cond_f(y, true);
      skip.af = (init.af & 0xFF00) | cond_f(y, false);
      emit("taken", {op, 0x00, 0x76}, taken, total + cc_ex[op]);
      emit("not-taken", {op, 0x00, 0x76}, skip, total);
    } else if (op == 0xC3) {  // JP nn → HALT island
      emit("", {op, kIsland & 0xFF, kIsland >> 8}, init, total);
    } else if ((op & 0xC7) ==
               0xC2) {  // JP cc,nn — cc_ex is 0 (10 T either way)
      const int y = (op >> 3) & 0x07;
      Z80Regs taken = init, skip = init;
      taken.af = (init.af & 0xFF00) | cond_f(y, true);
      skip.af = (init.af & 0xFF00) | cond_f(y, false);
      emit("taken", {op, kIsland & 0xFF, kIsland >> 8, 0x76}, taken,
           total + cc_ex[op]);
      emit("not-taken", {op, kIsland & 0xFF, kIsland >> 8, 0x76}, skip, total);
    } else if (op == 0xCD) {  // CALL nn → HALT island
      emit("", {op, kIsland & 0xFF, kIsland >> 8, 0x76}, init, total);
    } else if ((op & 0xC7) == 0xC4) {  // CALL cc,nn
      const int y = (op >> 3) & 0x07;
      Z80Regs taken = init, skip = init;
      taken.af = (init.af & 0xFF00) | cond_f(y, true);
      skip.af = (init.af & 0xFF00) | cond_f(y, false);
      emit("taken", {op, kIsland & 0xFF, kIsland >> 8, 0x76}, taken,
           total + cc_ex[op]);
      emit("not-taken", {op, kIsland & 0xFF, kIsland >> 8, 0x76}, skip, total);
    } else if (op == 0xC9) {  // RET — [SP] preloaded with the HALT island
      emit("", {op}, init, total);
    } else if ((op & 0xC7) == 0xC0) {  // RET cc
      const int y = (op >> 3) & 0x07;
      Z80Regs taken = init, skip = init;
      taken.af = (init.af & 0xFF00) | cond_f(y, true);
      skip.af = (init.af & 0xFF00) | cond_f(y, false);
      emit("taken", {op, 0x76}, taken, total + cc_ex[op]);
      emit("not-taken", {op, 0x76}, skip, total);
    } else if ((op & 0xC7) == 0xC7) {  // RST y*8 — every vector holds a HALT
      emit("", {op}, init, total);
    } else if (op == 0xE9) {  // JP (HL) / JP (IX) / JP (IY)
      if (!indexed)
        init.hl = kIsland;
      else if (prefix_byte == 0xDD)
        init.ix = kIsland;
      else
        init.iy = kIsland;
      emit("", {op}, init, total);
    } else {
      // Straight-line: opcode [+ displacement] [+ operands], then HALT.
      std::vector<uint8_t> body = {op};
      if (indexed && uses_hl_mem(op))
        body.push_back(0x00);  // d = 0 → (Ir+0) = kData
      if (is_imm8(op)) body.push_back(0x5A);
      if (is_imm16_value(op)) {  // harmless values; 0x31 LD SP,nn keeps kStack
        body.push_back(op == 0x31 ? (kStack & 0xFF) : (kData2 & 0xFF));
        body.push_back(op == 0x31 ? (kStack >> 8) : (kData2 >> 8));
      }
      if (is_imm16_addr(op)) {
        body.push_back(kData & 0xFF);
        body.push_back(kData >> 8);
      }
      body.push_back(0x76);
      emit("", body, init, total);
    }
  }
}

void gen_cb_sweep(std::vector<SweepCase>& out) {
  for (int opi = 0; opi < 256; ++opi) {
    const uint8_t op = static_cast<uint8_t>(opi);
    SweepCase c;
    c.name = hexname("CB", {op}, "");
    c.prog = {0xCB, op, 0x76};  // (HL) forms hit kData
    c.init = default_init();
    c.expected = 4 + cc_cb[op];
    out.push_back(std::move(c));
  }
}

void gen_ed_sweep(std::vector<SweepCase>& out) {
  for (int opi = 0; opi < 256; ++opi) {
    const uint8_t op = static_cast<uint8_t>(opi);
    const int total = 4 + cc_ed[op] + io_extra_ed(op);
    Z80Regs init = default_init();

    const bool is_block_repeat = op == 0xB0 || op == 0xB1 || op == 0xB2 ||
                                 op == 0xB3 || op == 0xB8 || op == 0xB9 ||
                                 op == 0xBA || op == 0xBB;
    if (is_block_repeat) {
      // Two variants: count=1 (no repeat: cc_ed) and count=2 (one repeat:
      // the repeating iteration re-fetches ED+op, so it costs a full
      // (4 + cc_ed + io) plus the cc_ex repeat penalty).
      const bool io_block =
          op == 0xB2 || op == 0xB3 || op == 0xBA || op == 0xBB;
      for (int count = 1; count <= 2; ++count) {
        SweepCase c;
        c.name = hexname("ED", {op}, count == 1 ? "count=1" : "count=2");
        c.prog = {0xED, op, 0x76};
        c.init = init;
        if (io_block)
          c.init.bc =
              static_cast<uint16_t>((count << 8) | 0x50);  // B = counter
        else
          c.init.bc = static_cast<uint16_t>(count);  // BC = counter
        c.expected = (count - 1) * (total + cc_ex[op]) + total;
        out.push_back(std::move(c));
      }
      continue;
    }

    SweepCase c;
    c.name = hexname("ED", {op}, "");
    c.init = init;
    c.expected = total;
    if ((op & 0xC7) == 0x43) {  // LD (nn),rr / LD rr,(nn)
      c.prog = {0xED, op, kData & 0xFF, kData >> 8, 0x76};
    } else if ((op & 0xC7) ==
               0x45) {  // RETN/RETI family — pops the HALT island
      c.prog = {0xED, op};
    } else {
      // Everything else is straight-line, including the single-shot block ops
      // (A0-A3/A8-AB), IN r,(C) / OUT (C),r (no device claims the port; reads
      // float 0xFF), IM x, LD I/R,A / A,I/R, RRD/RLD, NEG, and every invalid
      // ED (a two-fetch 8 T no-op in both implementations).
      c.prog = {0xED, op, 0x76};
    }
    out.push_back(std::move(c));
  }
}

void gen_xycb_sweep(std::vector<SweepCase>& out, uint8_t prefix_byte) {
  for (int opi = 0; opi < 256; ++opi) {
    const uint8_t op = static_cast<uint8_t>(opi);
    SweepCase c;
    c.name = hexname(prefix_byte == 0xDD ? "DDCB" : "FDCB", {op}, "");
    c.prog = {prefix_byte, 0xCB, 0x00, op, 0x76};  // d = 0 → kData
    c.init = default_init();
    // Total = cc_op[DD] (4) + cc_xy[CB] (4) + cc_xycb[op] — see header.
    c.expected = 8 + cc_xycb[op];
    out.push_back(std::move(c));
  }
}

}  // namespace

TEST(Z80CcOpSweep, Unprefixed) {
  std::vector<SweepCase> cases;
  gen_base_sweep(cases, 0x00);
  EXPECT_EQ(cases.size(), 280u);  // 251 opcodes, 29 conditionals doubled
  run_all(cases);
}

TEST(Z80CcOpSweep, CBPrefixed) {
  std::vector<SweepCase> cases;
  gen_cb_sweep(cases);
  EXPECT_EQ(cases.size(), 256u);
  run_all(cases);
}

TEST(Z80CcOpSweep, EDPrefixed) {
  std::vector<SweepCase> cases;
  gen_ed_sweep(cases);
  EXPECT_EQ(cases.size(), 264u);  // 256 opcodes, 8 block-repeats doubled
  run_all(cases);
}

TEST(Z80CcOpSweep, DDPrefixed) {
  std::vector<SweepCase> cases;
  gen_base_sweep(cases, 0xDD);
  EXPECT_EQ(cases.size(), 280u);
  run_all(cases);
}

TEST(Z80CcOpSweep, FDPrefixed) {
  std::vector<SweepCase> cases;
  gen_base_sweep(cases, 0xFD);
  EXPECT_EQ(cases.size(), 280u);
  run_all(cases);
}

TEST(Z80CcOpSweep, DDCBPrefixed) {
  std::vector<SweepCase> cases;
  gen_xycb_sweep(cases, 0xDD);
  EXPECT_EQ(cases.size(), 256u);
  run_all(cases);
}

TEST(Z80CcOpSweep, FDCBPrefixed) {
  std::vector<SweepCase> cases;
  gen_xycb_sweep(cases, 0xFD);
  EXPECT_EQ(cases.size(), 256u);
  run_all(cases);
}
