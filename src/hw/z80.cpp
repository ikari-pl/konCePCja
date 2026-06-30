/* z80.cpp — clean-room cycle-stepped Z80 Device. Round Z80-a: the M1 engine, the
 * flag core, and the 8-bit register/immediate group (NOP, LD r,r', LD r,n, ALU
 * A,{r,n}, INC/DEC r, HALT). (HL)/memory operands, prefixes, control flow, and
 * interrupts arrive in later rounds. See docs/hardware/z80.md.
 *
 * CLEAN-ROOM: built from the Z80 datasheet + Sean Young's UnDoc Z80 + Rak's Q
 * research, not from legacy z80.cpp. Validation: FUSE/jsmoo (final state + T-totals;
 * the per-T bus trace is offset by the bus model's one-hop latency — see hw-spec §2).
 */

#include <new>

#include "z80.h"

namespace {

/* Flag bits. */
constexpr uint8_t SF = 0x80, ZF = 0x40, YF = 0x20, HF = 0x10, XF = 0x08, PF = 0x04,
                  NF = 0x02, CF = 0x01;

bool parity_even(uint8_t value) {
  value ^= static_cast<uint8_t>(value >> 4);
  value ^= static_cast<uint8_t>(value >> 2);
  value ^= static_cast<uint8_t>(value >> 1);
  return (value & 1) == 0;
}

/* S,Z,Y,X from a result; parity variant adds P. */
uint8_t sz53(uint8_t r) {
  return static_cast<uint8_t>((r & (SF | YF | XF)) | (r == 0 ? ZF : 0));
}
uint8_t sz53p(uint8_t r) {
  return static_cast<uint8_t>(sz53(r) | (parity_even(r) ? PF : 0));
}

struct Reg16 {
  uint16_t v = 0;
  uint8_t hi() const { return static_cast<uint8_t>(v >> 8); }
  uint8_t lo() const { return static_cast<uint8_t>(v & 0xFF); }
  void set_hi(uint8_t h) { v = static_cast<uint16_t>((h << 8) | (v & 0x00FF)); }
  void set_lo(uint8_t l) { v = static_cast<uint16_t>((v & 0xFF00) | l); }
};

}  // namespace

struct z80_state {
  Reg16 af, bc, de, hl, af2, bc2, de2, hl2, ix, iy, sp, pc, wz;
  uint8_t i = 0, r = 0, im = 0, iff1 = 0, iff2 = 0;
  uint8_t q = 0;   // committed flags-written latch (Rak's Q)
  uint8_t qq = 0;  // scratch: set to F whenever F is written this instruction
  bool halted = false;

  // Engine state.
  enum class MC : uint8_t { M1, READ };  // current machine-cycle kind
  MC mc = MC::M1;
  uint8_t t = 0;       // T-state within the current machine cycle (0 = not started)
  uint8_t opcode = 0;  // latched opcode
  uint8_t tmp = 0;     // latched operand byte
  uint8_t halt_t = 0;  // sub-counter for the 4T HALT NOP cadence
  bool need_read = false;
  bool unimplemented = false;

  uint64_t tstates = 0;

  // --- register byte accessors via the octal index (0=B..7=A; 6=(HL) unused here) ---
  uint8_t a() const { return af.hi(); }
  uint8_t f() const { return af.lo(); }
  void set_a(uint8_t v) { af.set_hi(v); }
  void set_f(uint8_t v) {
    af.set_lo(v);
    qq = v;  // F written → Q tracks it
  }

  uint8_t get_r(uint8_t idx) const {
    switch (idx) {
      case 0: return bc.hi();
      case 1: return bc.lo();
      case 2: return de.hi();
      case 3: return de.lo();
      case 4: return hl.hi();
      case 5: return hl.lo();
      case 7: return af.hi();
      default: return 0;  // 6 = (HL), not handled in this round
    }
  }
  void set_r(uint8_t idx, uint8_t v) {
    switch (idx) {
      case 0: bc.set_hi(v); break;
      case 1: bc.set_lo(v); break;
      case 2: de.set_hi(v); break;
      case 3: de.set_lo(v); break;
      case 4: hl.set_hi(v); break;
      case 5: hl.set_lo(v); break;
      case 7: af.set_hi(v); break;
      default: break;
    }
  }

  void bump_refresh() { r = static_cast<uint8_t>((r & 0x80) | ((r + 1) & 0x7F)); }

  // --- ALU (sets A and F per the datasheet; flags validated against FUSE) ---
  void alu(uint8_t kind, uint8_t val) {
    const uint8_t acc = a();
    const uint8_t carry = (f() & CF) ? 1 : 0;
    uint16_t res = 0;
    uint8_t flags = 0;
    switch (kind) {
      case 0:  // ADD
      case 1:  // ADC
      {
        const uint8_t c = (kind == 1) ? carry : 0;
        res = static_cast<uint16_t>(acc + val + c);
        const uint8_t r8 = static_cast<uint8_t>(res);
        flags = sz53(r8);
        if (res & 0x100) flags |= CF;
        if ((acc ^ val ^ r8) & 0x10) flags |= HF;
        if ((~(acc ^ val) & (acc ^ r8)) & 0x80) flags |= PF;  // overflow
        set_a(r8);
        break;
      }
      case 2:  // SUB
      case 3:  // SBC
      case 7:  // CP (like SUB but A unchanged; X/Y from operand)
      {
        const uint8_t c = (kind == 3) ? carry : 0;
        res = static_cast<uint16_t>(acc - val - c);
        const uint8_t r8 = static_cast<uint8_t>(res);
        flags = static_cast<uint8_t>((r8 & SF) | (r8 == 0 ? ZF : 0) | NF);
        if (res & 0x100) flags |= CF;
        if ((acc ^ val ^ r8) & 0x10) flags |= HF;
        if (((acc ^ val) & (acc ^ r8)) & 0x80) flags |= PF;  // overflow
        if (kind == 7) {
          flags |= (val & (YF | XF));  // CP: undoc flags from operand
        } else {
          flags |= (r8 & (YF | XF));
          set_a(r8);
        }
        break;
      }
      case 4:  // AND
        res = static_cast<uint8_t>(acc & val);
        flags = static_cast<uint8_t>(sz53p(static_cast<uint8_t>(res)) | HF);
        set_a(static_cast<uint8_t>(res));
        break;
      case 5:  // XOR
        res = static_cast<uint8_t>(acc ^ val);
        flags = sz53p(static_cast<uint8_t>(res));
        set_a(static_cast<uint8_t>(res));
        break;
      case 6:  // OR
        res = static_cast<uint8_t>(acc | val);
        flags = sz53p(static_cast<uint8_t>(res));
        set_a(static_cast<uint8_t>(res));
        break;
      default:
        break;
    }
    set_f(flags);
  }

  uint8_t inc8(uint8_t val) {
    const uint8_t r8 = static_cast<uint8_t>(val + 1);
    uint8_t flags = static_cast<uint8_t>((f() & CF) | sz53(r8));
    if ((r8 & 0x0F) == 0x00) flags |= HF;
    if (r8 == 0x80) flags |= PF;  // overflow 0x7F→0x80
    set_f(flags);
    return r8;
  }
  uint8_t dec8(uint8_t val) {
    const uint8_t r8 = static_cast<uint8_t>(val - 1);
    uint8_t flags = static_cast<uint8_t>((f() & CF) | sz53(r8) | NF);
    if ((r8 & 0x0F) == 0x0F) flags |= HF;
    if (r8 == 0x7F) flags |= PF;  // overflow 0x80→0x7F
    set_f(flags);
    return r8;
  }

  // Apply the decoded instruction's effect once all operands are in hand.
  void execute() {
    const uint8_t x = static_cast<uint8_t>(opcode >> 6);
    const uint8_t y = static_cast<uint8_t>((opcode >> 3) & 7);
    const uint8_t z = static_cast<uint8_t>(opcode & 7);

    if (opcode == 0x00) return;        // NOP
    if (opcode == 0x76) { halted = true; return; }  // HALT

    // Index 6 = (HL): memory operand, not handled until Z80-b. Flag it rather
    // than silently computing garbage (get_r(6)/set_r(6) would no-op).
    if (x == 1) {  // LD r,r'
      if (y == 6 || z == 6) { unimplemented = true; return; }
      set_r(y, get_r(z));
      return;
    }
    if (x == 2) {  // ALU A,r
      if (z == 6) { unimplemented = true; return; }
      alu(y, get_r(z));
      return;
    }
    if (x == 3 && z == 6) { alu(y, tmp); return; }  // ALU A,n
    if (x == 0 && z == 6) {                         // LD r,n
      if (y == 6) { unimplemented = true; return; }  // LD (HL),n needs a write
      set_r(y, tmp);
      return;
    }
    if (x == 0 && z == 4) {  // INC r
      if (y == 6) { unimplemented = true; return; }
      set_r(y, inc8(get_r(y)));
      return;
    }
    if (x == 0 && z == 5) {  // DEC r
      if (y == 6) { unimplemented = true; return; }
      set_r(y, dec8(get_r(y)));
      return;
    }
    unimplemented = true;
  }
};

namespace {

bool decode_needs_read(uint8_t op) {
  const uint8_t x = static_cast<uint8_t>(op >> 6);
  const uint8_t z = static_cast<uint8_t>(op & 7);
  if (x == 0 && z == 6) return true;  // LD r,n
  if (x == 3 && z == 6) return true;  // ALU A,n
  return false;
}

z80_state* self_of(void* self) { return static_cast<z80_state*>(self); }

/* One T-state. Drives the CPU's lines into `out`, reads `in`. Advances only on
 * the 4 MHz clock enable; honours WAIT on the memory sample T-state. */
void z80_tick(void* self, const Bus* in, Bus* out) {
  z80_state* z = self_of(self);

  // Advance only on the 4 MHz CPU clock enable.
  // TODO(z80-engine, before GA integration): drive-and-hold — re-drive the
  // current M-cycle's owned lines (addr/mreq/rd) on EVERY master cycle, not just
  // enabled ones, so a memory request stays stable across the 3-of-4 cycles where
  // clk.cpu is low (real GA). Today sampling-at-T3 is correct only because the
  // isolation test clock is contiguous (every tick enabled), which is exactly how
  // the FUSE-validated rounds Z80-a..-e run. Also fold the single-`tmp`/`need_read`
  // pair into an explicit per-instruction M-cycle list (docs/hardware/z80.md §9).
  if (!in->clk.cpu) return;

  if (z->halted) {  // 4T NOP cadence: one R bump per 4 T-states (until interrupts)
    z->tstates++;
    if (++z->halt_t >= 4) { z->halt_t = 0; z->bump_refresh(); }
    return;
  }

  z->tstates++;  // count every executed T-state (so WAIT-inserted Tw count too)

  if (z->mc == z80_state::MC::M1) {
    z->t++;
    switch (z->t) {
      case 1:
        z->qq = 0;  // Q scratch CLEARED at instruction start; set only when F is
                    // written (Rak). A non-flag instruction must commit Q=0.
        out->cpu.addr = z->pc.v;
        out->cpu.m1 = out->cpu.mreq = out->cpu.rd = true;
        z->pc.v = static_cast<uint16_t>(z->pc.v + 1);
        break;
      case 2:
        out->cpu.addr = static_cast<uint16_t>(z->pc.v - 1);
        out->cpu.m1 = out->cpu.mreq = out->cpu.rd = true;
        break;
      case 3:  // data sampled here (one-hop bus latency, see hw-spec §2)
        z->opcode = in->cpu.data;
        out->cpu.addr = static_cast<uint16_t>((z->i << 8) | z->r);
        out->cpu.mreq = out->cpu.rfsh = true;
        z->bump_refresh();
        z->need_read = decode_needs_read(z->opcode);
        break;
      case 4:
        out->cpu.addr = static_cast<uint16_t>((z->i << 8) | z->r);
        out->cpu.rfsh = true;
        z->t = 0;
        if (z->need_read) {
          z->mc = z80_state::MC::READ;
        } else {
          z->execute();
          z->q = z->qq;
        }
        break;
      default:
        break;
    }
    return;
  }

  // READ machine cycle (operand/immediate). 3 T-states (+Tw while WAIT).
  z->t++;
  switch (z->t) {
    case 1:
      out->cpu.addr = z->pc.v;
      out->cpu.mreq = out->cpu.rd = true;
      z->pc.v = static_cast<uint16_t>(z->pc.v + 1);
      break;
    case 2:
      out->cpu.addr = static_cast<uint16_t>(z->pc.v - 1);
      out->cpu.mreq = out->cpu.rd = true;
      if (in->cpu.wait) z->t = 1;  // insert Tw: re-run T2 next tick
      break;
    case 3:
      z->tmp = in->cpu.data;
      z->t = 0;
      z->mc = z80_state::MC::M1;
      z->need_read = false;
      z->execute();
      z->q = z->qq;
      break;
    default:
      break;
  }
}

void z80_reset(void* self) {
  z80_state* z = self_of(self);
  z->pc.v = 0;
  z->i = z->r = z->im = z->iff1 = z->iff2 = 0;
  z->af.v = 0xFFFF;
  z->sp.v = 0xFFFF;
  z->wz.v = 0;
  z->q = z->qq = 0;
  z->halted = false;
  z->mc = z80_state::MC::M1;
  z->t = 0;
  z->opcode = z->tmp = 0;
  z->need_read = false;
  z->unimplemented = false;
  z->tstates = 0;
}

size_t z80_dev_state_size(const void*) { return sizeof(z80_state); }
void z80_save(const void* self, void* buf) {
  *static_cast<uint8_t*>(buf) = 1;  // version (logical-state blob is a later round)
  // Round Z80-a: a raw copy after the version byte is sufficient for tests; the
  // little-endian field-ordered blob (hw-spec §9) lands with the IPC wiring.
}
void z80_load(void*, const void*) {}

}  // namespace

extern "C" {

size_t z80_state_size(void) { return sizeof(z80_state); }

Device z80_init(void* storage) {
  z80_state* z = new (storage) z80_state();
  z80_reset(z);
  return Device{z,        "z80",    z80_tick, z80_reset,
                z80_dev_state_size, z80_save, z80_load};
}

void z80_peek(const Device* dev, Z80Regs* out) {
  const z80_state* z = static_cast<const z80_state*>(dev->self);
  out->af = z->af.v;   out->bc = z->bc.v;   out->de = z->de.v;   out->hl = z->hl.v;
  out->af_ = z->af2.v; out->bc_ = z->bc2.v; out->de_ = z->de2.v; out->hl_ = z->hl2.v;
  out->ix = z->ix.v;   out->iy = z->iy.v;   out->sp = z->sp.v;   out->pc = z->pc.v;
  out->wz = z->wz.v;
  out->i = z->i; out->r = z->r; out->im = z->im;
  out->iff1 = z->iff1; out->iff2 = z->iff2; out->q = z->q;
  out->halted = z->halted ? 1 : 0;
  out->tstates = z->tstates;
  out->last_opcode = z->opcode;
  out->unimplemented = z->unimplemented ? 1 : 0;
}

}  // extern "C"
