/* z80_cpp.cpp — clean-room C++ implementation of the z80core.h contract.
 *
 * Round 1: skeleton — state, register file, lifecycle, snapshot/restore, and a
 * cycle-stepped fetch loop wired to the bus. Only NOP and HALT are decoded so
 * far; the full octal-decode instruction set lands in Round 2.
 *
 * CLEAN-ROOM: written from the Zilog Z80 datasheet, Sean Young's "Undocumented
 * Z80 Documented" and the z80.info decode algorithm. The legacy z80.cpp is used
 * ONLY as a black-box oracle through the differential harness — no logic copied.
 */

#include "z80core.h"

namespace {

/* 16-bit register pair with byte accessors. Endian-independent (no unions), so
 * the layout is identical on every target and maps cleanly to an Ada record. */
struct Reg16 {
  uint16_t v = 0;
  uint8_t hi() const { return static_cast<uint8_t>(v >> 8); }
  uint8_t lo() const { return static_cast<uint8_t>(v & 0xFF); }
  void set_hi(uint8_t h) {
    v = static_cast<uint16_t>((static_cast<uint16_t>(h) << 8) | (v & 0x00FF));
  }
  void set_lo(uint8_t l) {
    v = static_cast<uint16_t>((v & 0xFF00) | l);
  }
};

}  // namespace

/* Full definition of the opaque handle from the contract. */
struct z80core_state {
  z80core_bus bus;  // value copy of callbacks + ctx

  Reg16 af, bc, de, hl;      // main register file
  Reg16 af2, bc2, de2, hl2;  // alternate set (AF', BC', DE', HL')
  Reg16 ix, iy, sp, pc, wz;
  uint8_t i = 0, r = 0;
  uint8_t im = 0;
  uint8_t iff1 = 0, iff2 = 0;
  uint8_t q = 0;
  bool halted = false;
  uint64_t tstates = 0;

  // Pending interrupt latches (serviced in Round 3+).
  bool int_pending = false;
  uint8_t int_data = 0xFF;
  bool nmi_pending = false;

  // --- bus access with standard Z80 M-cycle timing ---
  void tick(int t) {
    tstates += static_cast<uint64_t>(t);
    if (bus.tick) bus.tick(bus.ctx, t);
  }
  uint8_t read(uint16_t addr) {
    uint8_t d = bus.read(bus.ctx, addr);
    tick(3);
    return d;
  }
  void write(uint16_t addr, uint8_t val) {
    bus.write(bus.ctx, addr, val);
    tick(3);
  }
  uint8_t in_port(uint16_t port) {
    uint8_t d = bus.in_(bus.ctx, port);
    tick(4);
    return d;
  }
  void out_port(uint16_t port, uint8_t val) {
    bus.out(bus.ctx, port, val);
    tick(4);
  }

  // Bump the 7-bit memory-refresh counter (bit 7 is preserved).
  void bump_r() { r = static_cast<uint8_t>((r & 0x80) | ((r + 1) & 0x7F)); }

  // M1 opcode fetch: 4 T-states, advances PC, refreshes R.
  uint8_t fetch() {
    uint8_t op = bus.read(bus.ctx, pc.v);
    pc.v = static_cast<uint16_t>(pc.v + 1);
    tick(4);
    bump_r();
    return op;
  }
};

extern "C" {

z80core_state* z80core_create(const z80core_bus* bus) {
  auto* cpu = new z80core_state();
  cpu->bus = *bus;
  z80core_reset(cpu);
  return cpu;
}

void z80core_destroy(z80core_state* cpu) { delete cpu; }

void z80core_reset(z80core_state* cpu) {
  // Z80 RESET: PC=0, I=0, R=0, IM 0, interrupts disabled. AF and SP are formally
  // undefined after reset; real chips and convention settle on 0xFFFF.
  cpu->pc.v = 0x0000;
  cpu->i = 0;
  cpu->r = 0;
  cpu->im = 0;
  cpu->iff1 = 0;
  cpu->iff2 = 0;
  cpu->af.v = 0xFFFF;
  cpu->sp.v = 0xFFFF;
  cpu->wz.v = 0;
  cpu->q = 0;
  cpu->halted = false;
  cpu->int_pending = false;
  cpu->nmi_pending = false;
}

int z80core_step(z80core_state* cpu) {
  const uint64_t start = cpu->tstates;

  // While halted the CPU runs internal NOPs (refresh continues, PC frozen)
  // until an interrupt wakes it. Interrupt servicing arrives in Round 3.
  if (cpu->halted) {
    cpu->tick(4);
    cpu->bump_r();
    return static_cast<int>(cpu->tstates - start);
  }

  const uint8_t op = cpu->fetch();
  switch (op) {
    case 0x00:  // NOP
      break;
    case 0x76:  // HALT
      cpu->halted = true;
      break;
    default:
      // Round 2 implements the full instruction set via octal x/y/z/p/q decode.
      // Until then, undecoded opcodes behave as NOP so the skeleton runs.
      break;
  }
  return static_cast<int>(cpu->tstates - start);
}

void z80core_irq(z80core_state* cpu, uint8_t bus_value) {
  cpu->int_pending = true;
  cpu->int_data = bus_value;
}

void z80core_nmi(z80core_state* cpu) { cpu->nmi_pending = true; }

void z80core_snapshot(const z80core_state* cpu, z80core_regs* out) {
  out->af = cpu->af.v;
  out->bc = cpu->bc.v;
  out->de = cpu->de.v;
  out->hl = cpu->hl.v;
  out->af_ = cpu->af2.v;
  out->bc_ = cpu->bc2.v;
  out->de_ = cpu->de2.v;
  out->hl_ = cpu->hl2.v;
  out->ix = cpu->ix.v;
  out->iy = cpu->iy.v;
  out->sp = cpu->sp.v;
  out->pc = cpu->pc.v;
  out->wz = cpu->wz.v;
  out->i = cpu->i;
  out->r = cpu->r;
  out->im = cpu->im;
  out->iff1 = cpu->iff1;
  out->iff2 = cpu->iff2;
  out->q = cpu->q;
  out->halted = cpu->halted ? 1 : 0;
  out->tstates = cpu->tstates;
}

void z80core_restore(z80core_state* cpu, const z80core_regs* in) {
  cpu->af.v = in->af;
  cpu->bc.v = in->bc;
  cpu->de.v = in->de;
  cpu->hl.v = in->hl;
  cpu->af2.v = in->af_;
  cpu->bc2.v = in->bc_;
  cpu->de2.v = in->de_;
  cpu->hl2.v = in->hl_;
  cpu->ix.v = in->ix;
  cpu->iy.v = in->iy;
  cpu->sp.v = in->sp;
  cpu->pc.v = in->pc;
  cpu->wz.v = in->wz;
  cpu->i = in->i;
  cpu->r = in->r;
  cpu->im = in->im;
  cpu->iff1 = in->iff1;
  cpu->iff2 = in->iff2;
  cpu->q = in->q;
  cpu->halted = in->halted != 0;
  cpu->tstates = in->tstates;
}

uint16_t z80core_get_reg(const z80core_state* cpu, z80core_reg_id id) {
  switch (id) {
    case Z80CORE_AF: return cpu->af.v;
    case Z80CORE_BC: return cpu->bc.v;
    case Z80CORE_DE: return cpu->de.v;
    case Z80CORE_HL: return cpu->hl.v;
    case Z80CORE_AF_: return cpu->af2.v;
    case Z80CORE_BC_: return cpu->bc2.v;
    case Z80CORE_DE_: return cpu->de2.v;
    case Z80CORE_HL_: return cpu->hl2.v;
    case Z80CORE_IX: return cpu->ix.v;
    case Z80CORE_IY: return cpu->iy.v;
    case Z80CORE_SP: return cpu->sp.v;
    case Z80CORE_PC: return cpu->pc.v;
    case Z80CORE_WZ: return cpu->wz.v;
    case Z80CORE_IR:
      return static_cast<uint16_t>((static_cast<uint16_t>(cpu->i) << 8) | cpu->r);
  }
  return 0;
}

void z80core_set_reg(z80core_state* cpu, z80core_reg_id id, uint16_t value) {
  switch (id) {
    case Z80CORE_AF: cpu->af.v = value; break;
    case Z80CORE_BC: cpu->bc.v = value; break;
    case Z80CORE_DE: cpu->de.v = value; break;
    case Z80CORE_HL: cpu->hl.v = value; break;
    case Z80CORE_AF_: cpu->af2.v = value; break;
    case Z80CORE_BC_: cpu->bc2.v = value; break;
    case Z80CORE_DE_: cpu->de2.v = value; break;
    case Z80CORE_HL_: cpu->hl2.v = value; break;
    case Z80CORE_IX: cpu->ix.v = value; break;
    case Z80CORE_IY: cpu->iy.v = value; break;
    case Z80CORE_SP: cpu->sp.v = value; break;
    case Z80CORE_PC: cpu->pc.v = value; break;
    case Z80CORE_WZ: cpu->wz.v = value; break;
    case Z80CORE_IR:
      cpu->i = static_cast<uint8_t>(value >> 8);
      cpu->r = static_cast<uint8_t>(value & 0xFF);
      break;
  }
}

const char* z80core_impl_name(void) { return "cpp-cleanroom"; }

}  // extern "C"
