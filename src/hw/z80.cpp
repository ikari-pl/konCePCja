/* z80.cpp — clean-room cycle-stepped Z80 Device.
 *
 * Round Z80-a: M1 engine, flag core, 8-bit register/immediate group.
 * Round Z80-b: an explicit per-instruction M-cycle step machine (M1 / READ /
 *   WRITE / INTERNAL, each driven inline, the next cycle requested by micro()),
 *   and the 8-bit (HL) memory-operand group (LD r,(HL); LD (HL),r; ALU A,(HL);
 *   INC/DEC (HL); LD (HL),n).
 * Not yet: (BC)/(DE)/(nn) loads, 16-bit ops, MEMPTR/WZ updates, control flow,
 *   prefixes, interrupts. See docs/hardware/z80.md.
 *
 * CLEAN-ROOM: built from the Z80 datasheet + Sean Young's UnDoc Z80 + Rak's Q
 * research, not from legacy z80.cpp. Validation: FUSE/jsmoo (final state + T-totals;
 * the per-T bus trace is offset by the bus model's one-hop latency — hw-spec §2).
 *
 * TODO(z80-engine, before GA integration): drive-and-hold — re-drive the current
 * M-cycle's owned lines on EVERY master cycle, not only enabled ones, so a memory
 * request stays stable across the 3-of-4 cycles where clk.cpu is low (real GA).
 * Today drive is inline per enabled T-state, correct only on a contiguous clock —
 * which is exactly how the FUSE-validated isolation rounds run.
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
  uint8_t qq = 0;  // scratch: F whenever F is written this instruction (else 0)
  bool halted = false;

  // Engine: the current machine cycle + a per-instruction step counter. micro()
  // is called at each M-cycle boundary to process the result and request the next.
  enum class MC : uint8_t { M1, READ, WRITE, INTERNAL };
  MC mc = MC::M1;
  uint8_t t = 0;         // T-state within the current machine cycle
  uint16_t mc_addr = 0;  // address for M1/READ/WRITE
  uint8_t mc_wval = 0;   // value for WRITE
  uint8_t mc_ilen = 0;   // length for INTERNAL
  uint8_t step = 0;      // micro-step within the instruction (post-fetch)
  uint8_t opcode = 0;
  uint8_t tmp = 0;       // last byte read
  uint8_t halt_t = 0;    // 4T HALT cadence sub-counter
  bool unimplemented = false;

  uint64_t tstates = 0;

  // --- byte accessors via octal index (0=B..5=L, 7=A; 6=(HL) handled by micro) ---
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
      default: return 0;  // 6=(HL) never reaches here
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
      default: break;  // 6=(HL) never reaches here
    }
  }
  void bump_refresh() { r = static_cast<uint8_t>((r & 0x80) | ((r + 1) & 0x7F)); }

  // --- ALU (flags validated against FUSE) ---
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
      case 7:  // CP
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
    if (r8 == 0x80) flags |= PF;
    set_f(flags);
    return r8;
  }
  uint8_t dec8(uint8_t val) {
    const uint8_t r8 = static_cast<uint8_t>(val - 1);
    uint8_t flags = static_cast<uint8_t>((f() & CF) | sz53(r8) | NF);
    if ((r8 & 0x0F) == 0x0F) flags |= HF;
    if (r8 == 0x7F) flags |= PF;
    set_f(flags);
    return r8;
  }

  // --- M-cycle requests (set up the next cycle; the engine runs it) ---
  void req_read(uint16_t addr) { mc = MC::READ; t = 0; mc_addr = addr; }
  void req_write(uint16_t addr, uint8_t val) {
    mc = MC::WRITE;
    t = 0;
    mc_addr = addr;
    mc_wval = val;
  }
  void req_internal(uint8_t len) { mc = MC::INTERNAL; t = 0; mc_ilen = len; }
  void finish() {
    q = qq;
    mc = MC::M1;
    t = 0;
    step = 0;
  }

  // The instruction step machine. Called at each M-cycle boundary (step 0 = just
  // after M1). Each branch either requests one M-cycle and returns, or finishes.
  void micro() {
    const uint8_t x = static_cast<uint8_t>(opcode >> 6);
    const uint8_t y = static_cast<uint8_t>((opcode >> 3) & 7);
    const uint8_t z = static_cast<uint8_t>(opcode & 7);

    if (opcode == 0x00) { finish(); return; }                 // NOP
    if (opcode == 0x76) { halted = true; finish(); return; }  // HALT

    if (x == 1) {  // LD r,r' / r,(HL) / (HL),r
      if (z == 6) {  // LD r,(HL)
        if (step == 0) { req_read(hl.v); return; }
        set_r(y, tmp);
        finish();
        return;
      }
      if (y == 6) {  // LD (HL),r
        if (step == 0) { req_write(hl.v, get_r(z)); return; }
        finish();
        return;
      }
      set_r(y, get_r(z));  // LD r,r'
      finish();
      return;
    }

    if (x == 2) {  // ALU A,r / A,(HL)
      if (z == 6) {
        if (step == 0) { req_read(hl.v); return; }
        alu(y, tmp);
        finish();
        return;
      }
      alu(y, get_r(z));
      finish();
      return;
    }

    if (x == 3 && z == 6) {  // ALU A,n
      if (step == 0) { req_read(pc.v); pc.v = static_cast<uint16_t>(pc.v + 1); return; }
      alu(y, tmp);
      finish();
      return;
    }

    if (x == 0 && z == 6) {  // LD r,n / LD (HL),n
      if (step == 0) { req_read(pc.v); pc.v = static_cast<uint16_t>(pc.v + 1); return; }
      if (y == 6) {  // LD (HL),n
        if (step == 1) { req_write(hl.v, tmp); return; }
        finish();
        return;
      }
      set_r(y, tmp);  // LD r,n
      finish();
      return;
    }

    if (x == 0 && (z == 4 || z == 5)) {  // INC/DEC r / (HL)
      if (y == 6) {                      // INC/DEC (HL): READ, INTERNAL(1), WRITE
        if (step == 0) { req_read(hl.v); return; }
        if (step == 1) { req_internal(1); return; }
        if (step == 2) {
          const uint8_t nv = (z == 4) ? inc8(tmp) : dec8(tmp);
          req_write(hl.v, nv);
          return;
        }
        finish();
        return;
      }
      set_r(y, (z == 4) ? inc8(get_r(y)) : dec8(get_r(y)));
      finish();
      return;
    }

    unimplemented = true;
    finish();
  }
};

namespace {

z80_state* self_of(void* self) { return static_cast<z80_state*>(self); }

void z80_tick(void* self, const Bus* in, Bus* out) {
  z80_state* z = self_of(self);

  if (!in->clk.cpu) return;  // advance only on the 4 MHz enable (drive-and-hold: TODO)

  if (z->halted) {
    z->tstates++;
    if (++z->halt_t >= 4) { z->halt_t = 0; z->bump_refresh(); }
    return;
  }

  z->tstates++;  // one per executed T-state (WAIT Tw counts for free)
  z->t++;

  switch (z->mc) {
    case z80_state::MC::M1:
      switch (z->t) {
        case 1:
          z->qq = 0;
          z->mc_addr = z->pc.v;
          z->pc.v = static_cast<uint16_t>(z->pc.v + 1);
          out->cpu.addr = z->mc_addr;
          out->cpu.m1 = out->cpu.mreq = out->cpu.rd = true;
          break;
        case 2:
          out->cpu.addr = z->mc_addr;
          out->cpu.m1 = out->cpu.mreq = out->cpu.rd = true;
          break;
        case 3:  // opcode sampled here (one-hop latency, hw-spec §2)
          z->opcode = in->cpu.data;
          z->bump_refresh();
          out->cpu.addr = static_cast<uint16_t>((z->i << 8) | z->r);
          out->cpu.mreq = out->cpu.rfsh = true;
          break;
        case 4:
          out->cpu.addr = static_cast<uint16_t>((z->i << 8) | z->r);
          out->cpu.rfsh = true;
          z->step = 0;
          z->micro();  // begin instruction (requests first M-cycle, or finishes)
          break;
        default:
          break;
      }
      break;

    case z80_state::MC::READ:
      switch (z->t) {
        case 1:
        case 2:
          out->cpu.addr = z->mc_addr;
          out->cpu.mreq = out->cpu.rd = true;
          if (z->t == 2 && in->cpu.wait) z->t = 1;  // insert Tw
          break;
        case 3:
          z->tmp = in->cpu.data;
          z->step++;
          z->micro();
          break;
        default:
          break;
      }
      break;

    case z80_state::MC::WRITE:
      switch (z->t) {
        case 1:
          out->cpu.addr = z->mc_addr;
          out->cpu.data = z->mc_wval;
          out->cpu.mreq = true;
          break;
        case 2:
          out->cpu.addr = z->mc_addr;
          out->cpu.data = z->mc_wval;
          out->cpu.mreq = out->cpu.wr = true;
          if (in->cpu.wait) z->t = 1;  // insert Tw
          break;
        case 3:
          z->step++;
          z->micro();
          break;
        default:
          break;
      }
      break;

    case z80_state::MC::INTERNAL:
      if (z->t >= z->mc_ilen) {
        z->step++;
        z->micro();
      }
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
  z->t = z->step = 0;
  z->opcode = z->tmp = z->halt_t = 0;
  z->unimplemented = false;
  z->tstates = 0;
}

size_t z80_dev_state_size(const void*) { return sizeof(z80_state); }
void z80_save(const void* /*self*/, void* buf) {
  *static_cast<uint8_t*>(buf) = 1;  // version; little-endian field blob lands with IPC wiring
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
