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
  enum class MC : uint8_t { M1, READ, WRITE, INTERNAL, IO };
  MC mc = MC::M1;
  uint8_t t = 0;         // T-state within the current machine cycle
  uint16_t mc_addr = 0;  // address/port for M1/READ/WRITE/IO
  uint8_t mc_wval = 0;   // value for WRITE/IO write
  bool mc_io_read = false;  // IO cycle direction (true=IN, false=OUT)
  uint8_t mc_ilen = 0;   // length for INTERNAL
  uint8_t step = 0;      // micro-step within the instruction (post-fetch)
  uint8_t prefix = 0;    // active prefix byte (0xCB/0xED/...) or 0; cleared by finish()
  uint8_t opcode = 0;
  uint8_t tmp = 0;       // last byte read
  uint8_t tmpl = 0;      // low-byte latch (16-bit immediate assembly)
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
  void pcinc() { pc.v = static_cast<uint16_t>(pc.v + 1); }

  // 16-bit register pair by index (0=BC,1=DE,2=HL,3=SP).
  uint16_t get_rp(uint8_t p) const {
    switch (p) {
      case 0: return bc.v;
      case 1: return de.v;
      case 2: return hl.v;
      case 3: return sp.v;
      default: return 0;
    }
  }
  void set_rp(uint8_t p, uint16_t v) {
    switch (p) {
      case 0: bc.v = v; break;
      case 1: de.v = v; break;
      case 2: hl.v = v; break;
      case 3: sp.v = v; break;
      default: break;
    }
  }
  // PUSH/POP register-pair table (0=BC,1=DE,2=HL,3=AF) — AF replaces SP.
  uint16_t get_rp2(uint8_t p) const {
    switch (p) {
      case 0: return bc.v;
      case 1: return de.v;
      case 2: return hl.v;
      case 3: return af.v;
      default: return 0;
    }
  }
  void set_rp2(uint8_t p, uint16_t v) {
    switch (p) {
      case 0: bc.v = v; break;
      case 1: de.v = v; break;
      case 2: hl.v = v; break;
      case 3: af.v = v; break;
      default: break;
    }
  }
  // Branch condition by octal y: NZ,Z,NC,C,PO,PE,P,M.
  bool cond(uint8_t y) const {
    switch (y) {
      case 0: return (f() & ZF) == 0;
      case 1: return (f() & ZF) != 0;
      case 2: return (f() & CF) == 0;
      case 3: return (f() & CF) != 0;
      case 4: return (f() & PF) == 0;
      case 5: return (f() & PF) != 0;
      case 6: return (f() & SF) == 0;
      case 7: return (f() & SF) != 0;
      default: return false;
    }
  }

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

  // --- accumulator rotates (RLCA/RRCA/RLA/RRA): C from the shifted-out bit, H=N=0,
  //     S/Z/P unchanged, X/Y from the result; not the CB rotate flags. ---
  void acc_rot(uint8_t op) {  // op: 0=RLCA 1=RRCA 2=RLA 3=RRA
    const uint8_t av = a();
    const uint8_t oldc = (f() & CF) ? 1 : 0;
    uint8_t carry = 0;
    uint8_t na = 0;
    switch (op) {
      case 0: carry = static_cast<uint8_t>(av >> 7); na = static_cast<uint8_t>((av << 1) | carry); break;
      case 1: carry = static_cast<uint8_t>(av & 1); na = static_cast<uint8_t>((av >> 1) | (carry << 7)); break;
      case 2: carry = static_cast<uint8_t>(av >> 7); na = static_cast<uint8_t>((av << 1) | oldc); break;
      case 3: carry = static_cast<uint8_t>(av & 1); na = static_cast<uint8_t>((av >> 1) | (oldc << 7)); break;
      default: break;
    }
    set_a(na);
    set_f(static_cast<uint8_t>((f() & (SF | ZF | PF)) | (na & (YF | XF)) | (carry ? CF : 0)));
  }
  void daa() {
    const uint8_t av = a();
    uint8_t correction = 0;
    bool carry = (f() & CF) != 0;
    if ((f() & HF) || (av & 0x0F) > 0x09) correction |= 0x06;
    if (carry || av > 0x99) { correction |= 0x60; carry = true; }
    const uint8_t na = (f() & NF) ? static_cast<uint8_t>(av - correction)
                                  : static_cast<uint8_t>(av + correction);
    uint8_t flags = static_cast<uint8_t>(sz53p(na) | (carry ? CF : 0) | (f() & NF));
    flags |= static_cast<uint8_t>((av ^ na) & HF);  // half-carry from the adjust
    set_a(na);
    set_f(flags);
  }
  void cpl() {
    set_a(static_cast<uint8_t>(~a()));
    set_f(static_cast<uint8_t>((f() & (SF | ZF | PF | CF)) | HF | NF | (a() & (YF | XF))));
  }
  // SCF/CCF: NMOS X/Y = ((Q ^ F) | A) & (YF|XF); Q is the prior committed latch.
  uint8_t scf_ccf_xy() const {
    return static_cast<uint8_t>(((q ^ f()) | a()) & (YF | XF));
  }
  void scf() { set_f(static_cast<uint8_t>((f() & (SF | ZF | PF)) | CF | scf_ccf_xy())); }
  void ccf() {
    const uint8_t oldc = static_cast<uint8_t>(f() & CF);
    set_f(static_cast<uint8_t>((f() & (SF | ZF | PF)) | (oldc ? 0 : CF) | (oldc ? HF : 0) | scf_ccf_xy()));
  }
  void add16(uint16_t val) {  // ADD HL,rr — H(bit11), C(bit15), N=0; SZP kept; WZ=HL+1
    wz.v = static_cast<uint16_t>(hl.v + 1);
    const uint32_t res = static_cast<uint32_t>(hl.v) + val;
    const uint16_t r16 = static_cast<uint16_t>(res);
    uint8_t flags = static_cast<uint8_t>((f() & (SF | ZF | PF)) | ((r16 >> 8) & (YF | XF)));
    if ((hl.v ^ val ^ r16) & 0x1000) flags |= HF;
    if (res & 0x10000) flags |= CF;
    hl.v = r16;
    set_f(flags);
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
  void req_io_read(uint16_t port) { mc = MC::IO; t = 0; mc_addr = port; mc_io_read = true; }
  void req_io_write(uint16_t port, uint8_t val) {
    mc = MC::IO;
    t = 0;
    mc_addr = port;
    mc_wval = val;
    mc_io_read = false;
  }
  void finish() {
    q = qq;
    mc = MC::M1;
    t = 0;
    step = 0;
    prefix = 0;
  }

  // Dispatch the instruction step machine for the active prefix.
  void run_micro() {
    if (prefix == 0xCB) micro_cb();
    else if (prefix == 0xED) micro_ed();
    else micro();
  }

  // --- ED-prefix helpers ---
  void adc16(uint16_t val) {
    wz.v = static_cast<uint16_t>(hl.v + 1);
    const uint32_t res = static_cast<uint32_t>(hl.v) + val + ((f() & CF) ? 1u : 0u);
    const uint16_t r16 = static_cast<uint16_t>(res);
    uint8_t flags = static_cast<uint8_t>(((r16 >> 8) & (SF | YF | XF)) | (r16 == 0 ? ZF : 0));
    if ((hl.v ^ val ^ r16) & 0x1000) flags |= HF;
    if (res & 0x10000) flags |= CF;
    if ((~(hl.v ^ val) & (hl.v ^ r16)) & 0x8000) flags |= PF;
    hl.v = r16;
    set_f(flags);
  }
  void sbc16(uint16_t val) {
    wz.v = static_cast<uint16_t>(hl.v + 1);
    const uint32_t res = static_cast<uint32_t>(hl.v) - val - ((f() & CF) ? 1u : 0u);
    const uint16_t r16 = static_cast<uint16_t>(res);
    uint8_t flags = static_cast<uint8_t>(((r16 >> 8) & (SF | YF | XF)) | (r16 == 0 ? ZF : 0) | NF);
    if ((hl.v ^ val ^ r16) & 0x1000) flags |= HF;
    if (res & 0x10000) flags |= CF;
    if (((hl.v ^ val) & (hl.v ^ r16)) & 0x8000) flags |= PF;
    hl.v = r16;
    set_f(flags);
  }
  void neg() {
    const uint8_t old = a();
    const uint16_t res = static_cast<uint16_t>(0 - old);
    const uint8_t r8 = static_cast<uint8_t>(res);
    uint8_t flags = static_cast<uint8_t>((r8 & (SF | YF | XF)) | (r8 == 0 ? ZF : 0) | NF);
    if (res & 0x100) flags |= CF;
    if ((old ^ r8) & 0x10) flags |= HF;
    if ((old & r8) & 0x80) flags |= PF;  // overflow iff old == 0x80
    set_a(r8);
    set_f(flags);
  }
  void ld_a_ir(uint8_t val) {  // LD A,I / LD A,R
    set_a(val);
    uint8_t flags = static_cast<uint8_t>((f() & CF) | sz53(val));
    if (iff2) flags |= PF;  // P/V = IFF2
    set_f(flags);
  }

  // --- block operations (LDI/CPI/INI/OUTI families) ---
  // `inc` = I-variant (++ pointers), else D-variant (--). `repeat` = R-variant
  // (re-execute while the loop condition holds, costing an extra 5T INTERNAL).
  // The undocumented X/Y flags come from a per-family scratch byte `n`.
  void block_ld(bool inc, bool repeat) {  // LDI/LDD/LDIR/LDDR
    if (step == 0) { req_read(hl.v); return; }
    if (step == 1) { req_write(de.v, tmp); return; }
    if (step == 2) {
      const int d = inc ? 1 : -1;
      hl.v = static_cast<uint16_t>(hl.v + d);
      de.v = static_cast<uint16_t>(de.v + d);
      bc.v--;
      const uint8_t n = static_cast<uint8_t>(a() + tmp);  // A + transferred byte
      uint8_t flags = static_cast<uint8_t>(f() & (SF | ZF | CF));
      if (bc.v != 0) flags |= PF;
      if (n & 0x02) flags |= YF;  // bit1 of n → YF
      if (n & 0x08) flags |= XF;  // bit3 of n → XF
      set_f(flags);
      req_internal(2);
      return;
    }
    if (step == 3) {
      if (repeat && bc.v != 0) {
        pc.v = static_cast<uint16_t>(pc.v - 2);  // re-execute the ED-prefixed opcode
        wz.v = static_cast<uint16_t>(pc.v + 1);
        req_internal(5);
        return;
      }
      finish();
      return;
    }
    finish();  // step 4: after the loop's extra internal, re-fetch the opcode
  }
  void block_cp(bool inc, bool repeat) {  // CPI/CPD/CPIR/CPDR
    if (step == 0) { req_read(hl.v); return; }
    if (step == 1) {
      const uint8_t val = tmp;
      const uint8_t res = static_cast<uint8_t>(a() - val);
      const bool hf = ((a() ^ val ^ res) & 0x10) != 0;
      const int d = inc ? 1 : -1;
      hl.v = static_cast<uint16_t>(hl.v + d);
      bc.v--;
      wz.v = static_cast<uint16_t>(wz.v + d);
      const uint8_t n = static_cast<uint8_t>(res - (hf ? 1 : 0));
      uint8_t flags = static_cast<uint8_t>((f() & CF) | (res & SF) | NF);
      if (res == 0) flags |= ZF;
      if (hf) flags |= HF;
      if (bc.v != 0) flags |= PF;
      if (n & 0x02) flags |= YF;
      if (n & 0x08) flags |= XF;
      set_f(flags);
      req_internal(5);
      return;
    }
    if (step == 2) {
      if (repeat && bc.v != 0 && (f() & ZF) == 0) {  // loop until match or BC==0
        pc.v = static_cast<uint16_t>(pc.v - 2);
        wz.v = static_cast<uint16_t>(pc.v + 1);
        req_internal(5);
        return;
      }
      finish();
      return;
    }
    finish();  // step 3: after the loop's extra internal
  }
  void block_in(bool inc, bool repeat) {  // INI/IND/INIR/INDR
    if (step == 0) { req_internal(1); return; }  // 5T M1
    if (step == 1) {
      wz.v = static_cast<uint16_t>(bc.v + (inc ? 1 : -1));
      req_io_read(bc.v);  // IN with the original B in the port
      return;
    }
    if (step == 2) { req_write(hl.v, tmp); return; }  // tmp survives the write
    if (step == 3) {
      const uint8_t val = tmp;
      const int d = inc ? 1 : -1;
      hl.v = static_cast<uint16_t>(hl.v + d);
      bc.set_hi(static_cast<uint8_t>(bc.hi() - 1));  // B-- after the input
      const uint8_t bdec = bc.hi();
      uint8_t flags = sz53(bdec);
      if (val & 0x80) flags |= NF;
      const uint16_t k = static_cast<uint16_t>(val) +
                         (static_cast<uint8_t>(bc.lo() + d) & 0xFF);
      if (k > 0xFF) flags |= static_cast<uint8_t>(HF | CF);
      if (parity_even(static_cast<uint8_t>((k & 7) ^ bdec))) flags |= PF;
      set_f(flags);
      if (repeat && bdec != 0) { pc.v = static_cast<uint16_t>(pc.v - 2); req_internal(5); return; }
      finish();
      return;
    }
    finish();
  }
  void block_out(bool inc, bool repeat) {  // OUTI/OUTD/OTIR/OTDR
    if (step == 0) { req_internal(1); return; }  // 5T M1
    if (step == 1) { req_read(hl.v); return; }
    if (step == 2) {
      bc.set_hi(static_cast<uint8_t>(bc.hi() - 1));  // B-- before the output
      wz.v = static_cast<uint16_t>(bc.v + (inc ? 1 : -1));
      req_io_write(bc.v, tmp);  // OUT with the decremented B
      return;
    }
    if (step == 3) {
      const uint8_t val = tmp;
      const int d = inc ? 1 : -1;
      hl.v = static_cast<uint16_t>(hl.v + d);
      const uint8_t bdec = bc.hi();
      uint8_t flags = sz53(bdec);
      if (val & 0x80) flags |= NF;
      const uint16_t k = static_cast<uint16_t>(val) + hl.lo();  // L after the move
      if (k > 0xFF) flags |= static_cast<uint8_t>(HF | CF);
      if (parity_even(static_cast<uint8_t>((k & 7) ^ bdec))) flags |= PF;
      set_f(flags);
      if (repeat && bdec != 0) { pc.v = static_cast<uint16_t>(pc.v - 2); req_internal(5); return; }
      finish();
      return;
    }
    finish();
  }

  void micro_ed() {
    const uint8_t x = static_cast<uint8_t>(opcode >> 6);
    const uint8_t y = static_cast<uint8_t>((opcode >> 3) & 7);
    const uint8_t z = static_cast<uint8_t>(opcode & 7);
    const uint8_t p = static_cast<uint8_t>(y >> 1);
    const uint8_t qbit = static_cast<uint8_t>(y & 1);

    if (x == 1) {
      switch (z) {
        case 0:  // IN r,(C)  (y=6 → IN (C): flags only). MEMPTR = BC+1
          if (step == 0) { wz.v = static_cast<uint16_t>(bc.v + 1); req_io_read(bc.v); return; }
          set_f(static_cast<uint8_t>((f() & CF) | sz53p(tmp)));  // SZ53P, H=N=0, C kept
          if (y != 6) set_r(y, tmp);
          finish();
          return;
        case 1:  // OUT (C),r  (y=6 → OUT (C),0). MEMPTR = BC+1
          if (step == 0) {
            req_io_write(bc.v, (y == 6) ? 0 : get_r(y));
            wz.v = static_cast<uint16_t>(bc.v + 1);
            return;
          }
          finish();
          return;
        case 3:  // LD (nn),rr / LD rr,(nn) — 20T. WZ as working pointer.
          if (step == 0) { req_read(pc.v); pcinc(); return; }
          if (step == 1) { tmpl = tmp; req_read(pc.v); pcinc(); return; }
          if (step == 2) wz.v = static_cast<uint16_t>((tmp << 8) | tmpl);  // WZ = nn
          if (qbit == 0) {  // LD (nn),rr
            if (step == 2) { req_write(wz.v, static_cast<uint8_t>(get_rp(p) & 0xFF)); wz.v++; return; }
            if (step == 3) { req_write(wz.v, static_cast<uint8_t>(get_rp(p) >> 8)); return; }
            finish();
            return;
          }
          // LD rr,(nn)
          if (step == 2) { req_read(wz.v); wz.v++; return; }
          if (step == 3) { tmpl = tmp; req_read(wz.v); return; }
          set_rp(p, static_cast<uint16_t>((tmp << 8) | tmpl));
          finish();
          return;
        case 2:  // SBC HL,rr (q=0) / ADC HL,rr (q=1) — 15T (4+4+7)
          if (step == 0) { req_internal(7); return; }
          if (qbit == 0) sbc16(get_rp(p)); else adc16(get_rp(p));
          finish();
          return;
        case 4:  // NEG (all 8 encodings are NEG on NMOS)
          neg();
          finish();
          return;
        case 5:  // RETN (y even) / RETI (y odd) — pop PC, IFF1 = IFF2 (14T)
          if (step == 0) { req_read(sp.v); sp.v++; return; }
          if (step == 1) { tmpl = tmp; req_read(sp.v); sp.v++; return; }
          pc.v = wz.v = static_cast<uint16_t>((tmp << 8) | tmpl);
          iff1 = iff2;
          finish();
          return;
        case 6: {  // IM
          static const uint8_t kImTable[8] = {0, 0, 1, 2, 0, 0, 1, 2};
          im = kImTable[y];
          finish();
          return;
        }
        case 7:  // LD I,A / R,A / A,I / A,R / RRD / RLD
          switch (y) {
            case 0:  // LD I,A (9T: + INTERNAL(1))
              if (step == 0) { req_internal(1); return; }
              i = a(); finish(); return;
            case 1:  // LD R,A
              if (step == 0) { req_internal(1); return; }
              r = a(); finish(); return;
            case 2:  // LD A,I
              if (step == 0) { req_internal(1); return; }
              ld_a_ir(i); finish(); return;
            case 3:  // LD A,R
              if (step == 0) { req_internal(1); return; }
              ld_a_ir(r); finish(); return;
            case 4:  // RRD — 18T (4+4+3+4+3)
            case 5:  // RLD
              if (step == 0) { req_read(hl.v); return; }
              if (step == 1) { req_internal(4); return; }
              if (step == 2) {
                const uint8_t old = tmp;
                uint8_t newhl = 0;
                uint8_t newa = 0;
                if (y == 4) {  // RRD
                  newhl = static_cast<uint8_t>((a() << 4) | (old >> 4));
                  newa = static_cast<uint8_t>((a() & 0xF0) | (old & 0x0F));
                } else {  // RLD
                  newhl = static_cast<uint8_t>((old << 4) | (a() & 0x0F));
                  newa = static_cast<uint8_t>((a() & 0xF0) | (old >> 4));
                }
                set_a(newa);
                set_f(static_cast<uint8_t>((f() & CF) | sz53p(newa)));
                wz.v = static_cast<uint16_t>(hl.v + 1);
                req_write(hl.v, newhl);
                return;
              }
              finish();
              return;
            default:
              break;
          }
          break;
        default:
          break;
      }
    }

    if (x == 2 && z < 4 && y >= 4) {  // block ops: I/D variants (y4/5), R variants (y6/7)
      const bool inc = (y == 4 || y == 6);
      const bool repeat = (y >= 6);
      switch (z) {
        case 0: block_ld(inc, repeat); return;
        case 1: block_cp(inc, repeat); return;
        case 2: block_in(inc, repeat); return;
        case 3: block_out(inc, repeat); return;
        default: break;
      }
    }

    unimplemented = true;
    finish();
  }

  // --- CB-prefix helpers ---
  // Rotate/shift (op 0..7: RLC RRC RL RR SLA SRA SLL SRL); sets SZ5H3PNC.
  uint8_t cb_shift(uint8_t op, uint8_t val) {
    const uint8_t oldc = (f() & CF) ? 1 : 0;
    uint8_t carry = 0;
    uint8_t res = 0;
    switch (op) {
      case 0: carry = static_cast<uint8_t>(val >> 7); res = static_cast<uint8_t>((val << 1) | carry); break;
      case 1: carry = static_cast<uint8_t>(val & 1); res = static_cast<uint8_t>((val >> 1) | (carry << 7)); break;
      case 2: carry = static_cast<uint8_t>(val >> 7); res = static_cast<uint8_t>((val << 1) | oldc); break;
      case 3: carry = static_cast<uint8_t>(val & 1); res = static_cast<uint8_t>((val >> 1) | (oldc << 7)); break;
      case 4: carry = static_cast<uint8_t>(val >> 7); res = static_cast<uint8_t>(val << 1); break;
      case 5: carry = static_cast<uint8_t>(val & 1); res = static_cast<uint8_t>((val >> 1) | (val & 0x80)); break;
      case 6: carry = static_cast<uint8_t>(val >> 7); res = static_cast<uint8_t>((val << 1) | 1); break;  // SLL (undoc)
      case 7: carry = static_cast<uint8_t>(val & 1); res = static_cast<uint8_t>(val >> 1); break;
      default: break;
    }
    set_f(static_cast<uint8_t>(sz53p(res) | (carry ? CF : 0)));
    return res;
  }
  // BIT n,src — Z/P from the tested bit, H=1, N=0, S only from bit 7, C kept;
  // undocumented X/Y come from `xy` (the register value, or WZ.hi for (HL)).
  void cb_bit(uint8_t n, uint8_t val, uint8_t xy) {
    const uint8_t bit = static_cast<uint8_t>(val & (1u << n));
    uint8_t flags = static_cast<uint8_t>((f() & CF) | HF | (xy & (YF | XF)));
    if (bit == 0) flags |= static_cast<uint8_t>(ZF | PF);
    if (n == 7 && bit) flags |= SF;
    set_f(flags);
  }

  void micro_cb() {
    const uint8_t x = static_cast<uint8_t>(opcode >> 6);
    const uint8_t y = static_cast<uint8_t>((opcode >> 3) & 7);
    const uint8_t z = static_cast<uint8_t>(opcode & 7);

    if (z != 6) {  // register operand: rotate/BIT/RES/SET in one step (8T total)
      const uint8_t val = get_r(z);
      if (x == 0) set_r(z, cb_shift(y, val));
      else if (x == 1) cb_bit(y, val, val);
      else if (x == 2) set_r(z, static_cast<uint8_t>(val & ~(1u << y)));
      else set_r(z, static_cast<uint8_t>(val | (1u << y)));
      finish();
      return;
    }

    // (HL): READ, INTERNAL(1), then BIT (12T) or write-back (15T).
    if (step == 0) { req_read(hl.v); return; }
    if (step == 1) { req_internal(1); return; }
    if (step == 2) {
      if (x == 1) { cb_bit(y, tmp, wz.hi()); finish(); return; }  // BIT (HL): X/Y from WZ
      uint8_t res = 0;
      if (x == 0) res = cb_shift(y, tmp);
      else if (x == 2) res = static_cast<uint8_t>(tmp & ~(1u << y));
      else res = static_cast<uint8_t>(tmp | (1u << y));
      req_write(hl.v, res);
      return;
    }
    finish();
  }

  // The instruction step machine. Dispatched by the opcode's x field so each
  // family is isolated and decode no longer depends on guard ORDER (important
  // once CB/ED/DD/FD multiply the branches). Each handler requests one M-cycle
  // and returns, or finish()es. Unmatched opcodes set `unimplemented`.
  void micro() {
    const uint8_t x = static_cast<uint8_t>(opcode >> 6);
    const uint8_t y = static_cast<uint8_t>((opcode >> 3) & 7);
    const uint8_t z = static_cast<uint8_t>(opcode & 7);
    switch (x) {
      case 0: micro_x0(y, z); return;
      case 1: micro_x1(y, z); return;
      case 2:  // ALU A,r / A,(HL)
        if (z == 6) {
          if (step == 0) { req_read(hl.v); return; }
          alu(y, tmp);
        } else {
          alu(y, get_r(z));
        }
        finish();
        return;
      case 3:  // control flow, stack, ALU A,n, prefixes
        micro_x3(y, z);
        return;
      default:
        return;
    }
  }

  // x=3: RET cc / POP / JP / CALL / RST / PUSH / EX / DI-EI / ALU A,n.
  // Stack writes push high byte first (to SP-1), then low (to SP-2). Ops with a
  // 5T/6T M1 (PUSH, RST, RET cc, LD SP,HL) model the extra T as a leading
  // INTERNAL — same totals and bus behaviour, no special-casing in the engine.
  void micro_x3(uint8_t y, uint8_t z) {
    const uint8_t p = static_cast<uint8_t>(y >> 1);
    const uint8_t q = static_cast<uint8_t>(y & 1);
    switch (z) {
      case 0:  // RET cc — 5T M1 (decision), then POP pc if taken
        if (step == 0) { req_internal(1); return; }
        if (step == 1) {
          if (!cond(y)) { finish(); return; }       // not taken: 5T
          req_read(sp.v); sp.v++; return;
        }
        if (step == 2) { tmpl = tmp; req_read(sp.v); sp.v++; return; }
        pc.v = wz.v = static_cast<uint16_t>((tmp << 8) | tmpl);
        finish();
        return;
      case 1:
        if (q == 0) {  // POP rp2[p]
          if (step == 0) { req_read(sp.v); sp.v++; return; }
          if (step == 1) { tmpl = tmp; req_read(sp.v); sp.v++; return; }
          set_rp2(p, static_cast<uint16_t>((tmp << 8) | tmpl));
          finish();
          return;
        }
        switch (p) {
          case 0:  // RET
            if (step == 0) { req_read(sp.v); sp.v++; return; }
            if (step == 1) { tmpl = tmp; req_read(sp.v); sp.v++; return; }
            pc.v = wz.v = static_cast<uint16_t>((tmp << 8) | tmpl);
            finish();
            return;
          case 1: {  // EXX
            const uint16_t b = bc.v, d = de.v, h = hl.v;
            bc.v = bc2.v; de.v = de2.v; hl.v = hl2.v;
            bc2.v = b; de2.v = d; hl2.v = h;
            finish();
            return;
          }
          case 2:  // JP (HL) — 4T, no WZ change
            pc.v = hl.v;
            finish();
            return;
          case 3:  // LD SP,HL — 6T (M1 + 2 internal)
            if (step == 0) { req_internal(2); return; }
            sp.v = hl.v;
            finish();
            return;
          default:
            return;
        }
      case 2:  // JP cc,nn — always reads nn, WZ=nn, jumps if cc
        if (step == 0) { req_read(pc.v); pcinc(); return; }
        if (step == 1) { tmpl = tmp; req_read(pc.v); pcinc(); return; }
        wz.v = static_cast<uint16_t>((tmp << 8) | tmpl);
        if (cond(y)) pc.v = wz.v;
        finish();
        return;
      case 3:
        switch (y) {
          case 0:  // JP nn
            if (step == 0) { req_read(pc.v); pcinc(); return; }
            if (step == 1) { tmpl = tmp; req_read(pc.v); pcinc(); return; }
            pc.v = wz.v = static_cast<uint16_t>((tmp << 8) | tmpl);
            finish();
            return;
          case 4:  // EX (SP),HL — 19T. Read the stack word, write HL there, then
                   // load HL from the old word; WZ = new HL.
            if (step == 0) { req_read(sp.v); return; }
            if (step == 1) { tmpl = tmp; req_read(static_cast<uint16_t>(sp.v + 1)); return; }
            if (step == 2) { req_internal(1); return; }
            if (step == 3) { req_write(static_cast<uint16_t>(sp.v + 1), hl.hi()); return; }
            if (step == 4) { req_write(sp.v, hl.lo()); return; }
            if (step == 5) {
              hl.set_lo(tmpl);
              hl.set_hi(tmp);
              wz.v = hl.v;
              req_internal(2);
              return;
            }
            finish();
            return;
          case 5: {  // EX DE,HL
            const uint16_t t2 = de.v;
            de.v = hl.v;
            hl.v = t2;
            finish();
            return;
          }
          case 6:  // DI
            iff1 = iff2 = 0;
            finish();
            return;
          case 7:  // EI
            iff1 = iff2 = 1;
            finish();
            return;
          case 2:  // OUT (n),A — port = (A<<8)|n; MEMPTR low=(n+1)&FF, high=A
            if (step == 0) { req_read(pc.v); pcinc(); return; }
            if (step == 1) {
              const uint16_t port = static_cast<uint16_t>((a() << 8) | tmp);
              wz.set_lo(static_cast<uint8_t>((tmp + 1) & 0xFF));
              wz.set_hi(a());
              req_io_write(port, a());
              return;
            }
            finish();
            return;
          case 3:  // IN A,(n) — port = (A<<8)|n; MEMPTR = port+1
            if (step == 0) { req_read(pc.v); pcinc(); return; }
            if (step == 1) {
              const uint16_t port = static_cast<uint16_t>((a() << 8) | tmp);
              wz.v = static_cast<uint16_t>(port + 1);
              req_io_read(port);
              return;
            }
            set_a(tmp);
            finish();
            return;
          default:  // y=1 is the CB prefix (handled at M1)
            unimplemented = true;
            finish();
            return;
        }
      case 4:  // CALL cc,nn
        if (step == 0) { req_read(pc.v); pcinc(); return; }
        if (step == 1) { tmpl = tmp; req_read(pc.v); pcinc(); return; }
        if (step == 2) {
          wz.v = static_cast<uint16_t>((tmp << 8) | tmpl);
          if (!cond(y)) { finish(); return; }  // not taken: 10T
          req_internal(1);
          return;
        }
        if (step == 3) { sp.v--; req_write(sp.v, pc.hi()); return; }
        if (step == 4) { sp.v--; req_write(sp.v, pc.lo()); return; }
        pc.v = wz.v;
        finish();
        return;
      case 5:
        if (q == 0) {  // PUSH rp2[p] — 5T M1 + 2 writes
          if (step == 0) { req_internal(1); return; }
          if (step == 1) { sp.v--; req_write(sp.v, static_cast<uint8_t>(get_rp2(p) >> 8)); return; }
          if (step == 2) { sp.v--; req_write(sp.v, static_cast<uint8_t>(get_rp2(p) & 0xFF)); return; }
          finish();
          return;
        }
        if (p == 0) {  // CALL nn
          if (step == 0) { req_read(pc.v); pcinc(); return; }
          if (step == 1) { tmpl = tmp; req_read(pc.v); pcinc(); return; }
          if (step == 2) { wz.v = static_cast<uint16_t>((tmp << 8) | tmpl); req_internal(1); return; }
          if (step == 3) { sp.v--; req_write(sp.v, pc.hi()); return; }
          if (step == 4) { sp.v--; req_write(sp.v, pc.lo()); return; }
          pc.v = wz.v;
          finish();
          return;
        }
        unimplemented = true;  // p=1 DD / p=3 FD (index prefix, later)
        finish();
        return;
      case 6:  // ALU A,n
        if (step == 0) { req_read(pc.v); pcinc(); return; }
        alu(y, tmp);
        finish();
        return;
      case 7:  // RST y*8 — 5T M1 + 2 writes
        if (step == 0) { req_internal(1); return; }
        if (step == 1) { sp.v--; req_write(sp.v, pc.hi()); return; }
        if (step == 2) { sp.v--; req_write(sp.v, pc.lo()); return; }
        pc.v = wz.v = static_cast<uint16_t>(y * 8);
        finish();
        return;
      default:
        unimplemented = true;
        finish();
        return;
    }
  }

  void micro_x1(uint8_t y, uint8_t z) {  // LD r,r' / r,(HL) / (HL),r ; HALT
    if (y == 6 && z == 6) { halted = true; finish(); return; }  // HALT
    if (z == 6) {                                               // LD r,(HL)
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
  }

  void micro_x0(uint8_t y, uint8_t z) {
    const uint8_t p = static_cast<uint8_t>(y >> 1);
    const uint8_t q = static_cast<uint8_t>(y & 1);
    switch (z) {
      case 0:  // NOP (y=0); EX AF,AF' (y=1); DJNZ (y=2); JR/JR cc (y>=3)
        if (y == 0) { finish(); return; }
        if (y == 1) {  // EX AF,AF'
          const uint16_t tv = af.v;
          af.v = af2.v;
          af2.v = tv;
          finish();
          return;
        }
        if (y == 2) {  // DJNZ e — 5T M1, read displacement, branch if --B != 0
          if (step == 0) { req_internal(1); return; }
          if (step == 1) { req_read(pc.v); pcinc(); return; }
          if (step == 2) {
            bc.set_hi(static_cast<uint8_t>(bc.hi() - 1));
            if (bc.hi() == 0) { finish(); return; }  // not taken: 8T
            req_internal(5);
            return;
          }
          pc.v = wz.v = static_cast<uint16_t>(pc.v + static_cast<int8_t>(tmp));
          finish();
          return;
        }
        // JR e (y=3) / JR cc,e (y=4..7 → NZ,Z,NC,C)
        if (step == 0) { req_read(pc.v); pcinc(); return; }
        if (step == 1) {
          const bool take = (y == 3) || cond(static_cast<uint8_t>(y - 4));
          if (!take) { finish(); return; }  // not taken: 7T
          req_internal(5);
          return;
        }
        pc.v = wz.v = static_cast<uint16_t>(pc.v + static_cast<int8_t>(tmp));
        finish();
        return;
      case 1:  // LD rr,nn (q=0) / ADD HL,rr (q=1)
        if (q == 0) {
          if (step == 0) { req_read(pc.v); pcinc(); return; }
          if (step == 1) { tmpl = tmp; req_read(pc.v); pcinc(); return; }
          set_rp(p, static_cast<uint16_t>((tmp << 8) | tmpl));
          finish();
          return;
        }
        // ADD HL,rr — INTERNAL(7) then the add (11T total)
        if (step == 0) { req_internal(7); return; }
        add16(get_rp(p));
        finish();
        return;
      case 2:  // LD A,(BC|DE) / LD (BC|DE),A (p<2); (nn) forms (p>=2) later
        if (p < 2) {
          const uint16_t rp = (p == 0) ? bc.v : de.v;
          if (q == 1) {  // LD A,(rp)
            if (step == 0) { req_read(rp); return; }
            set_a(tmp);
            wz.v = static_cast<uint16_t>(rp + 1);  // MEMPTR = rp+1
            finish();
            return;
          }
          if (step == 0) { req_write(rp, a()); return; }  // LD (rp),A
          wz.set_hi(a());
          wz.set_lo(static_cast<uint8_t>((rp & 0xFF) + 1));  // MEMPTR = (A<<8)|(low+1)
          finish();
          return;
        }
        // p>=2: absolute (nn) loads. Read the 16-bit operand, then use WZ as the
        // working address pointer (reads clobber `tmp`, never WZ).
        if (step == 0) { req_read(pc.v); pcinc(); return; }
        if (step == 1) { tmpl = tmp; req_read(pc.v); pcinc(); return; }
        if (step == 2) wz.v = static_cast<uint16_t>((tmp << 8) | tmpl);  // WZ = nn
        if (p == 2 && q == 0) {  // LD (nn),HL
          if (step == 2) { req_write(wz.v, hl.lo()); wz.v++; return; }
          if (step == 3) { req_write(wz.v, hl.hi()); return; }  // WZ already nn+1
          finish();
          return;
        }
        if (p == 2 && q == 1) {  // LD HL,(nn)
          if (step == 2) { req_read(wz.v); wz.v++; return; }
          if (step == 3) { hl.set_lo(tmp); req_read(wz.v); return; }
          hl.set_hi(tmp);
          finish();
          return;
        }
        if (p == 3 && q == 0) {  // LD (nn),A — MEMPTR low=(nn+1)&FF, high=A
          if (step == 2) {
            req_write(wz.v, a());
            wz.set_lo(static_cast<uint8_t>((wz.v & 0xFF) + 1));
            wz.set_hi(a());
            return;
          }
          finish();
          return;
        }
        // p == 3 && q == 1: LD A,(nn)
        if (step == 2) { req_read(wz.v); wz.v++; return; }
        set_a(tmp);
        finish();
        return;
      case 3:  // INC/DEC rr (no flags, INTERNAL(2))
        if (step == 0) { req_internal(2); return; }
        {
          const uint16_t v = get_rp(p);
          set_rp(p, static_cast<uint16_t>(q ? (v - 1) : (v + 1)));
        }
        finish();
        return;
      case 4:  // INC r/(HL)
      case 5:  // DEC r/(HL)
        if (y == 6) {  // INC/DEC (HL): READ, INTERNAL(1), WRITE
          if (step == 0) { req_read(hl.v); return; }
          if (step == 1) { req_internal(1); return; }
          if (step == 2) { req_write(hl.v, (z == 4) ? inc8(tmp) : dec8(tmp)); return; }
          finish();
          return;
        }
        set_r(y, (z == 4) ? inc8(get_r(y)) : dec8(get_r(y)));
        finish();
        return;
      case 6:  // LD r,n / LD (HL),n
        if (step == 0) { req_read(pc.v); pcinc(); return; }
        if (y == 6) {  // LD (HL),n
          if (step == 1) { req_write(hl.v, tmp); return; }
          finish();
          return;
        }
        set_r(y, tmp);  // LD r,n
        finish();
        return;
      case 7:  // RLCA/RRCA/RLA/RRA/DAA/CPL/SCF/CCF
        switch (y) {
          case 0: acc_rot(0); break;
          case 1: acc_rot(1); break;
          case 2: acc_rot(2); break;
          case 3: acc_rot(3); break;
          case 4: daa(); break;
          case 5: cpl(); break;
          case 6: scf(); break;
          case 7: ccf(); break;
          default: break;
        }
        finish();
        return;
      default:
        break;
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
          if (z->prefix == 0 && (z->opcode == 0xCB || z->opcode == 0xED)) {
            z->prefix = z->opcode;  // fetch the prefixed opcode as a second M1
            z->mc = z80_state::MC::M1;
            z->t = 0;
          } else {
            z->step = 0;
            z->run_micro();  // begin instruction (by active prefix)
          }
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
          z->run_micro();
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
          z->run_micro();
          break;
        default:
          break;
      }
      break;

    case z80_state::MC::INTERNAL:
      if (z->t >= z->mc_ilen) {
        z->step++;
        z->run_micro();
      }
      break;

    case z80_state::MC::IO:
      // 4 T-states: T1, T2, an always-present auto-wait Tw, then sample at T4.
      switch (z->t) {
        case 1:
        case 2:
        case 3:
          out->cpu.addr = z->mc_addr;
          out->cpu.iorq = true;
          if (z->mc_io_read) {
            out->cpu.rd = true;
          } else {
            out->cpu.wr = true;
            out->cpu.data = z->mc_wval;
          }
          if (z->t == 3 && in->cpu.wait) z->t = 2;  // honor external wait too
          break;
        case 4:
          if (z->mc_io_read) z->tmp = in->cpu.data;
          z->step++;
          z->run_micro();
          break;
        default:
          break;
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
  z->opcode = z->tmp = z->tmpl = z->halt_t = z->prefix = 0;
  z->mc_addr = z->mc_wval = z->mc_ilen = 0;  // scratch: deterministic for snapshots
  z->mc_io_read = false;
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
