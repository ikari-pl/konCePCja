/* mf2.cpp — the Multiface II Device. See docs/hardware/multiface-device.md.
 *
 * The pin-level version of what the golden master does with pokes scattered
 * through kon_cpc_ja.cpp: the cartridge WATCHES the bus. It snoops hardware
 * writes into its RAM's fixed shadow cells, pages its ROM/RAM over the
 * bottom 16K with /ROMDIS + /RAMDIS, and rides the NMI for the STOP button.
 */

#include "mf2.h"

#include <cstring>
#include <new>

namespace {

// The golden master's shadow cells, as offsets into the 8K RAM (its 16K
// pbMF2ROM block put the RAM at +0x2000; subtract that).
constexpr uint16_t kCellPen = 0x3FCF - 0x2000;
constexpr uint16_t kCellInk = 0x3FEF - 0x2000;
constexpr uint16_t kCellGaCfg = 0x3FFF - 0x2000;
constexpr uint16_t kCellRamCfg = 0x37FF - 0x2000;
constexpr uint16_t kCellCrtcSel = 0x3CFF - 0x2000;
constexpr uint16_t kCellCrtcReg = 0x3DB0 - 0x2000;  // | (reg & 0x0F)
constexpr uint16_t kCellRomSel = 0x3AAC - 0x2000;

// STOP holds /NMI long enough for the CPU's edge detector to see it across
// instruction boundaries (a few T-states; 16 master cycles = 1 µs).
constexpr uint8_t kNmiHold = 16;

struct mf2_state {
  uint8_t ram[0x2000] = {0};  // the freeze workspace — SERIALIZED state
  uint8_t plugged = 0;
  uint8_t active = 0;     // ROM/RAM paged over 0x0000-0x3FFF
  uint8_t invisible = 0;  // &FEE8 dead until reset (anti-detection)
  uint8_t frozen = 0;     // a STOP session is in progress (spec §3)
  uint8_t nmi_hold = 0;   // remaining cycles to drive /NMI
  bool io_prev = false;   // previous cycle was an I/O write access (edge)

  // Live wiring (never serialized): MUST stay the LAST members.
  const uint8_t* rom = nullptr;  // caller-owned 8K ROM
  size_t rom_len = 0;
};

mf2_state* self_of(void* self) { return static_cast<mf2_state*>(self); }

// Shadow one committed I/O write into the RAM cells the MF2 ROM expects
// (spec §5): the same partial decodes the GA / CRTC / ROM-select latch use.
void snoop_io(mf2_state* f, uint16_t addr, uint8_t data) {
  if ((addr & 0xC000) == 0x4000) {  // Gate Array (A15 low, A14 high)
    switch (data >> 6) {
      case 0:
        f->ram[kCellPen] = data;
        break;
      case 1:
        f->ram[kCellInk] = data;
        break;
      case 2:
        f->ram[kCellGaCfg] = data;
        break;
      default:
        break;  // function 3 = the PAL's business, shadowed below
    }
  }
  if ((addr & 0x8000) == 0 && (data & 0xC0) == 0xC0)  // PAL RAM banking
    f->ram[kCellRamCfg] = data;
  if ((addr & 0x4000) == 0) {  // CRTC (A14 low); A9-A8 pick the function
    const uint8_t fn = (addr >> 8) & 3;
    if (fn == 0)
      f->ram[kCellCrtcSel] = data;
    else if (fn == 1)
      f->ram[kCellCrtcReg | (f->ram[kCellCrtcSel] & 0x0F)] = data;
  }
  if ((addr & 0x2000) == 0)  // upper-ROM select (A13 low)
    f->ram[kCellRomSel] = data;
}

void mf2_tick(void* self, const Bus* __restrict in, Bus* __restrict out) {
  mf2_state* f = self_of(self);
  if (!f->plugged) return;  // nothing on the port: no decode, no snoop

  if (in->cpu.reset) {  // a machine reset un-hides the cartridge (spec §4)
    f->invisible = 0;
    f->active = 0;
    f->frozen = 0;
  }

  if (f->nmi_hold > 0) {  // the STOP button's /NMI pulse
    out->cpu.nmi = true;  // wired-OR
    f->nmi_hold--;
  }

  // --- I/O writes: paging ports + the hardware shadow (edge per access) ---
  const bool io_wr = in->cpu.iorq && !in->cpu.m1 && in->cpu.wr;
  const bool edge = io_wr && !f->io_prev;
  f->io_prev = io_wr;
  if (edge) {
    if (in->cpu.addr == 0xFEE8) {
      if (!f->invisible) f->active = 1;
    } else if (in->cpu.addr == 0xFEEA) {
      f->active = 0;
      if (f->frozen) {  // the freeze session's exit path hides the MF2
        f->frozen = 0;
        f->invisible = 1;
      }
    }
    snoop_io(f, in->cpu.addr, in->cpu.data);
  }

  // --- The memory overlay while paged in (spec §2) ---
  if (!f->active) return;
  if (in->cpu.mreq && in->cpu.rd && !in->cpu.rfsh && in->cpu.addr < 0x4000) {
    out->cpu.romdis = true;  // silence the firmware ROM decode
    if (in->cpu.addr < 0x2000) {
      out->cpu.data = (f->rom != nullptr && in->cpu.addr < f->rom_len)
                          ? f->rom[in->cpu.addr]
                          : 0xFF;
    } else {
      out->cpu.ramdis = true;  // this 8K reads from the MF2 RAM
      out->cpu.data = f->ram[in->cpu.addr - 0x2000];
    }
  }
  if (in->cpu.mreq && in->cpu.wr && in->cpu.addr >= 0x2000 &&
      in->cpu.addr < 0x4000) {
    out->cpu.ramdis = true;  // veto the internal RAM's write latch (§2)
    f->ram[in->cpu.addr - 0x2000] = in->cpu.data;
  }
}

void mf2_dev_reset(void* self) {
  mf2_state* f = self_of(self);
  // A cold boot: overlay off, visible again. The freeze RAM and the plugged
  // state persist — the cartridge keeps its memory across a reset.
  f->active = 0;
  f->invisible = 0;
  f->frozen = 0;
  f->nmi_hold = 0;
  f->io_prev = false;
}

// Serialize everything BEFORE the live wiring (the RAM is the freeze state).
constexpr size_t kSaveBytes = offsetof(mf2_state, rom);

size_t mf2_dev_state_size(const void* /*unused*/) { return kSaveBytes + 1; }
void mf2_save(const void* self, void* buf) {
  uint8_t* b = static_cast<uint8_t*>(buf);
  b[0] = 1;
  std::memcpy(b + 1, self, kSaveBytes);
}
void mf2_load(void* self, const void* buf) {
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  if (b[0] == 1) std::memcpy(self, b + 1, kSaveBytes);
}

}  // namespace

extern "C" {

size_t mf2_state_size(void) { return sizeof(mf2_state); }

Device mf2_init(void* storage) {
  // NOLINTNEXTLINE(misc-const-correctness): pointer is stored in Device::self
  // (void*), cannot be const
  mf2_state* f = new (storage) mf2_state();
  return Device{f,        "mf2",   mf2_tick, mf2_dev_reset, mf2_dev_state_size,
                mf2_save, mf2_load};
}

void mf2_peek(const Device* dev, Mf2Regs* out) {
  const mf2_state* f = static_cast<const mf2_state*>(dev->self);
  out->plugged = f->plugged;
  out->active = f->active;
  out->invisible = f->invisible;
}

void mf2_attach_rom(const Device* dev, const uint8_t* rom8k, size_t len) {
  mf2_state* f = static_cast<mf2_state*>(dev->self);
  f->rom = rom8k;
  f->rom_len = len < 0x2000 ? len : 0x2000;
}

void mf2_set_plugged(const Device* dev, int on) {
  static_cast<mf2_state*>(dev->self)->plugged = on ? 1 : 0;
}

void mf2_stop(const Device* dev) {
  mf2_state* f = static_cast<mf2_state*>(dev->self);
  if (!f->plugged || f->active) return;  // the golden master ignores re-STOP
  f->active = 1;  // the hardware pages itself in for the &0066 vector fetch
  f->frozen = 1;
  f->nmi_hold = kNmiHold;
}

uint8_t mf2_ram_peek(const Device* dev, uint16_t offset) {
  const mf2_state* f = static_cast<const mf2_state*>(dev->self);
  return f->ram[offset & 0x1FFF];
}

}  // extern "C"
