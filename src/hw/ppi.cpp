/* ppi.cpp — the CPC PPI 8255 Device. See docs/hardware/ppi-device.md.
 *
 * Port A = AY data bus, Port B = status inputs, Port C = keyboard row + AY
 * control (BDIR/BC1) + cassette. The AY is reached only through this chip (the
 * internal AY bus on Bus.ay); the Z80 never addresses it directly. */

#include "ppi.h"

#include <cstring>
#include <new>

namespace {

struct ppi_state {
  uint8_t portA = 0;    // AY data-bus output latch
  uint8_t portB = 0;    // Port B output latch (rarely used: Port B is input)
  uint8_t portC = 0;    // keyboard row (0..3) + tape (4,5) + AY BC1/BDIR (6,7)
  uint8_t control = 0;  // 8255 direction bits (power-on: all inputs)
  uint8_t jumpers = 0x1E;  // manufacturer id 7 (bits 1..3) + 50 Hz (bit 4)
  bool printer_ready = false;
  bool tape_motor = false;
};

ppi_state* self_of(void* self) { return static_cast<ppi_state*>(self); }

// Direction decodes (1 = input) from the control byte.
bool portA_input(const ppi_state* p) { return (p->control & 0x10) != 0; }
bool portB_input(const ppi_state* p) { return (p->control & 0x02) != 0; }
bool portC_lower_input(const ppi_state* p) { return (p->control & 0x01) != 0; }
bool portC_upper_input(const ppi_state* p) { return (p->control & 0x08) != 0; }

// Port B status read: VSYNC is OR'd in live from the CRTC each read.
uint8_t read_portB(const ppi_state* p, bool vsync, bool rdata) {
  // bit 5 = /EXP: the expansion signal reads 1 unless an expansion device pulls
  // it low (cpcwiki/cpctech 8255 doc — a device signals presence by driving
  // /EXP to 0). No expansion is modelled, so it is always 1. (jumpers holds the
  // id + 50 Hz straps in bits 1..4.)
  return static_cast<uint8_t>((vsync ? 0x01 : 0x00) | (p->jumpers & 0x1E) |
                              0x20 | (p->printer_ready ? 0x00 : 0x40) |
                              (rdata ? 0x80 : 0x00));
}

// Port C read: latch, but input halves return the 8255's defined substitutes.
uint8_t read_portC(const ppi_state* p) {
  uint8_t v = p->portC;
  if (portC_upper_input(p)) {
    v &= 0x0F;
    uint8_t ctl = p->portC & 0xC0;  // AY control bits echo back
    if (ctl == 0xC0) ctl = 0x80;    // "latch" reads back as "write"
    v |= ctl | 0x20;                // cassette-write sense always set
    if (p->tape_motor) v |= 0x10;
  }
  if (portC_lower_input(p)) v |= 0x0F;  // undriven lower half floats high
  return v;
}

// Apply a new Port C latch value: refresh the tape-motor sense (bit 4 — the
// legacy core and the schematics agree: PC4 = motor relay, PC5 = write data)
// when the upper half is an output. (The AY-bus republish happens in ppi_tick
// each cycle.)
void portC_latched(ppi_state* p) {
  if (!portC_upper_input(p)) p->tape_motor = (p->portC & 0x10) != 0;
}

void ppi_read(ppi_state* p, const Bus* in, Bus* out, uint8_t port) {
  switch (port) {
    case 0:  // Port A: AY drove the bus (input) or the last latch (output)
      out->cpu.data = portA_input(p) ? in->ay.da : p->portA;
      break;
    case 1:  // Port B status inputs (VSYNC OR'd in live), or the latch
      out->cpu.data = portB_input(p)
                          ? read_portB(p, in->vid.vsync, in->tape.rdata)
                          : p->portB;
      break;
    case 2:  // Port C
      out->cpu.data = read_portC(p);
      break;
    default:  // control register is write-only; reads float
      break;
  }
}

void ppi_write(ppi_state* p, uint8_t port, uint8_t val) {
  switch (port) {
    case 0:  // Port A latch (data for the AY on the next control code)
      p->portA = val;
      break;
    case 1:  // Port B latch
      p->portB = val;
      break;
    case 2:  // Port C latch + side effects
      p->portC = val;
      portC_latched(p);
      break;
    case 3:
      if (val & 0x80) {  // mode-set: reconfigure directions, clear latches
        p->control = val;
        p->portA = p->portB = p->portC = 0;
      } else {  // bit set/reset on one Port C bit
        const uint8_t bit = (val >> 1) & 0x07;
        if (val & 0x01)
          p->portC |= static_cast<uint8_t>(1u << bit);
        else
          p->portC &= static_cast<uint8_t>(~(1u << bit));
        portC_latched(p);
      }
      break;
    default:
      break;
  }
}

void ppi_tick(void* self, const Bus* __restrict in, Bus* __restrict out) {
  ppi_state* p = self_of(self);

  // Publish the AY-bus outputs every cycle so the PSG sees a stable bus.
  // Port C upper drives BDIR/BC1; the lower nibble selects the keyboard row.
  // EXCEPT while BUSAK is granted: a DMA master (the Plus ASIC sound sequencer)
  // has taken the bus and drives the AY bus in its slot. The PPI yields so the
  // two never contend — the arbitration rule for the AY bus's second master
  // (asic-device.md §1, §4). The CPU is halted during BUSAK, so the PPI's Port
  // C is not changing and nothing is lost by not driving it.
  if (!in->cpu.busak) {
    out->ay.bdir = (p->portC & 0x80) != 0;
    out->ay.bc1 = (p->portC & 0x40) != 0;
    out->ay.kbd_row = static_cast<uint8_t>(p->portC & 0x0F);
    if (!portA_input(p))
      out->ay.da = p->portA;  // Port A output → present data to AY
  }

  // Cassette wires: PC4 = motor relay, PC5 = write data (Port C upper output).
  out->tape.motor = p->tape_motor;
  out->tape.wdata = !portC_upper_input(p) && (p->portC & 0x20) != 0;

  // I/O cycle we own? PPI select is A11 = 0; A9..A8 pick the port.
  if (!in->cpu.iorq) return;
  if (in->cpu.addr & 0x0800) return;  // A11 high → not us
  const uint8_t port = static_cast<uint8_t>((in->cpu.addr >> 8) & 0x03);

  if (in->cpu.rd)
    ppi_read(p, in, out, port);
  else if (in->cpu.wr)
    ppi_write(p, port, in->cpu.data);
}

void ppi_reset(void* self) {
  ppi_state* p = self_of(self);
  const uint8_t jumpers = p->jumpers;  // hardware strap persists across reset
  *p = ppi_state{};
  p->jumpers = jumpers;
  p->control = 0x9B;  // real 8255 RESET: mode 0, ALL ports input (A/B/C in)
}

size_t ppi_dev_state_size(const void* /*unused*/) {
  return sizeof(ppi_state) + 1;
}
void ppi_save(const void* self, void* buf) {
  uint8_t* b = static_cast<uint8_t*>(buf);
  b[0] = 1;
  std::memcpy(b + 1, self, sizeof(ppi_state));
}
void ppi_load(void* self, const void* buf) {
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  if (b[0] == 1) std::memcpy(self, b + 1, sizeof(ppi_state));
}

}  // namespace

extern "C" {

size_t ppi_state_size(void) { return sizeof(ppi_state); }

Device ppi_init(void* storage) {
  // NOLINTNEXTLINE(misc-const-correctness): pointer is stored in Device::self
  // (void*), cannot be const
  ppi_state* p = new (storage) ppi_state();
  return Device{p,        "ppi",   ppi_tick, ppi_reset, ppi_dev_state_size,
                ppi_save, ppi_load};
}

void ppi_peek(const Device* dev, PpiRegs* out) {
  const ppi_state* p = static_cast<const ppi_state*>(dev->self);
  out->portA = p->portA;
  out->portB = p->portB;
  out->portC = p->portC;
  out->control = p->control;
  out->kbd_row = static_cast<uint8_t>(p->portC & 0x0F);
  out->tape_motor = p->tape_motor ? 1 : 0;
}

void ppi_set_jumpers(const Device* dev, uint8_t jumpers) {
  static_cast<ppi_state*>(dev->self)->jumpers = jumpers;
}

void ppi_set_printer_ready(const Device* dev, int ready) {
  static_cast<ppi_state*>(dev->self)->printer_ready = ready != 0;
}

// The published line state as a value — exactly what ppi_tick drives each
// cycle (the AY bus from Port C/A, the cassette wires), derived from latches.
namespace {
void fast_lines_of(const ppi_state* p, PpiAyLines* out) {
  out->bdir = (p->portC & 0x80) ? 1 : 0;
  out->bc1 = (p->portC & 0x40) ? 1 : 0;
  out->kbd_row = static_cast<uint8_t>(p->portC & 0x0F);
  out->da = portA_input(p) ? 0xFF : p->portA;  // undriven bus floats high
  out->tape_motor = p->tape_motor ? 1 : 0;
  out->tape_wdata = (!portC_upper_input(p) && (p->portC & 0x20)) ? 1 : 0;
}
}  // namespace

void ppi_fast_lines(const Device* dev, PpiAyLines* out) {
  fast_lines_of(static_cast<const ppi_state*>(dev->self), out);
}

int ppi_fast_io_write(const Device* dev, uint16_t port, uint8_t val,
                      PpiAyLines* after) {
  ppi_state* p = static_cast<ppi_state*>(dev->self);
  if (port & 0x0800) return 0;  // A11 high → not us
  PpiAyLines before{};
  fast_lines_of(p, &before);
  ppi_write(p, static_cast<uint8_t>((port >> 8) & 0x03), val);
  fast_lines_of(p, after);
  return std::memcmp(&before, after, sizeof(PpiAyLines)) != 0 ? 1 : 0;
}

int ppi_fast_io_read(const Device* dev, uint16_t port, int vsync, int rdata,
                     uint8_t ay_da, uint8_t* out) {
  ppi_state const* p = static_cast<ppi_state*>(dev->self);
  if (port & 0x0800) return 0;
  switch ((port >> 8) & 0x03) {  // ppi_read's arms, with event-fed inputs
    case 0:
      *out = portA_input(p) ? ay_da : p->portA;
      return 1;
    case 1:
      *out = portB_input(p) ? read_portB(p, vsync != 0, rdata != 0) : p->portB;
      return 1;
    case 2:
      *out = read_portC(p);
      return 1;
    default:
      return 0;  // control register is write-only; reads float
  }
}

}  // extern "C"
