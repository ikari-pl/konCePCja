/* memory.cpp — the CPC memory map Device: 64K RAM + lower/upper ROM overlays.
 * See docs/hardware/memory-device.md. */

#include "memory.h"

#include <cstring>
#include <new>

namespace {

struct mem_state {
  uint8_t ram[0x10000] = {0};       // 64K main RAM
  uint8_t lower_rom[0x4000] = {0};  // firmware (0x0000-0x3FFF when enabled)
  uint8_t upper_rom[0x4000] = {0};  // BASIC/AMSDOS (0xC000-0xFFFF when enabled)
  uint8_t rom_config = 0;           // bit2 = lower-ROM disable, bit3 = upper-ROM disable
  uint8_t ram_config = 0;
};

mem_state* self_of(void* self) { return static_cast<mem_state*>(self); }

bool lower_rom_on(const mem_state* m) { return (m->rom_config & 0x04) == 0; }
bool upper_rom_on(const mem_state* m) { return (m->rom_config & 0x08) == 0; }

uint8_t mem_read(const mem_state* m, uint16_t addr) {
  if (addr < 0x4000 && lower_rom_on(m)) return m->lower_rom[addr];
  if (addr >= 0xC000 && upper_rom_on(m)) return m->upper_rom[addr - 0xC000];
  return m->ram[addr];  // (6128 RAM banking applies here in a later slice)
}

void mem_tick(void* self, const Bus* in, Bus* out) {
  mem_state* m = self_of(self);

  // Independently track the GA's ROM/RAM banking writes (A15=0,A14=1; fn = data>>6).
  if (in->cpu.iorq && in->cpu.wr && (in->cpu.addr & 0xC000) == 0x4000) {
    const uint8_t d = in->cpu.data;
    if ((d >> 6) == 2) m->rom_config = d;
    else if ((d >> 6) == 3) m->ram_config = d;
  }

  // Memory access. Writes always land in RAM (through any ROM overlay).
  if (in->cpu.mreq && in->cpu.rd) {
    out->cpu.data = mem_read(m, in->cpu.addr);
  } else if (in->cpu.mreq && in->cpu.wr) {
    m->ram[in->cpu.addr] = in->cpu.data;
  }
}

void mem_reset(void* self) {
  mem_state* m = self_of(self);
  // ROMs enabled by default; RAM and ROM contents persist (ROM is the firmware).
  m->rom_config = 0;
  m->ram_config = 0;
}

size_t mem_dev_state_size(const void*) { return sizeof(mem_state) + 1; }
void mem_save(const void* self, void* buf) {
  uint8_t* b = static_cast<uint8_t*>(buf);
  b[0] = 1;
  std::memcpy(b + 1, self, sizeof(mem_state));
}
void mem_load(void* self, const void* buf) {
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  if (b[0] == 1) std::memcpy(self, b + 1, sizeof(mem_state));
}

}  // namespace

extern "C" {

size_t mem_state_size(void) { return sizeof(mem_state); }

Device mem_init(void* storage) {
  mem_state* m = new (storage) mem_state();
  mem_reset(m);
  return Device{m,        "memory", mem_tick, mem_reset,
                mem_dev_state_size, mem_save, mem_load};
}

void mem_peek(const Device* dev, MemRegs* out) {
  const mem_state* m = static_cast<const mem_state*>(dev->self);
  out->rom_config = m->rom_config;
  out->ram_config = m->ram_config;
}

void mem_load_lower_rom(const Device* dev, const uint8_t* data, size_t len) {
  mem_state* m = static_cast<mem_state*>(dev->self);
  if (len > sizeof(m->lower_rom)) len = sizeof(m->lower_rom);
  std::memcpy(m->lower_rom, data, len);
}
void mem_load_upper_rom(const Device* dev, const uint8_t* data, size_t len) {
  mem_state* m = static_cast<mem_state*>(dev->self);
  if (len > sizeof(m->upper_rom)) len = sizeof(m->upper_rom);
  std::memcpy(m->upper_rom, data, len);
}
void mem_write_ram(const Device* dev, uint16_t addr, uint8_t val) {
  static_cast<mem_state*>(dev->self)->ram[addr] = val;
}
uint8_t mem_read_ram(const Device* dev, uint16_t addr) {
  return static_cast<const mem_state*>(dev->self)->ram[addr];
}

}  // extern "C"
