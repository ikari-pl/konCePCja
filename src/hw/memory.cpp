/* memory.cpp — the CPC memory map Device: 64K RAM + lower/upper ROM overlays.
 * See docs/hardware/memory-device.md. */

#include "memory.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <new>

#include "asic.h"  // asic_unlocked() — gates the RMR2 cartridge low-ROM remap

namespace {

struct mem_state {
  uint8_t ram[0x10000] = {
      0};  // 64K base RAM (pages 0-3; also all video fetches)
  uint8_t lower_rom[0x4000] = {0};  // firmware (0x0000-0x3FFF when enabled)
  uint8_t upper_rom[0x4000] = {0};  // BASIC/AMSDOS (0xC000-0xFFFF when enabled)
  uint8_t rom_config = 0;  // bit2 = lower-ROM disable, bit3 = upper-ROM disable
  uint8_t ram_config = 0;  // PAL banking latch: 11 bbb ccc
  uint8_t ram_ext = 0;     // Yarek extended bank bits (inverted A13..A11)
  uint8_t rom_select = 0;  // upper-ROM number (A13-low I/O write, &DFxx)
  // One-tick write latch (docs §4b): RAM commits a CPU write on the NEXT
  // master cycle, so a same-access /RAMDIS (settling one tick behind the
  // strobes, like every expansion line) can still veto it — the golden
  // master's exclusive-write behaviour for the Multiface's paged RAM.
  uint16_t wr_addr = 0;
  uint8_t wr_val = 0;
  uint8_t wr_armed = 0;
  uint8_t wr_prev = 0;           // write strobes seen last tick (edge detector)
  uint8_t* expansion = nullptr;  // caller-owned expansion RAM (64K banks)
  size_t expansion_len = 0;      // bytes attached (multiple of 64K)
  const uint8_t* roms[256] = {};  // caller-owned 16K expansion ROM images

  // Plus (6128+) cartridge banking. When `cart` is set (model 3), the lower and
  // upper ROM windows read from the 16K cartridge banks instead of lower/upper_rom
  // + roms[]. RMR2 (an ASIC mode-write) picks the low bank; ROM-select the high.
  const uint8_t* cart = nullptr;  // caller-owned CPR image: cart_banks x 16K
  int cart_banks = 0;             // nonzero => Plus cartridge mode
  uint8_t cart_lower = 0;         // low-ROM cartridge page (RMR2 & 7); boots at 0 = OS
  // RMR2 membank field (bits 4-3): which 16K CPU slot the low-ROM page maps into
  // — 0 = &0000-&3FFF, 1 = &4000-&7FFF, 2 = &8000-&BFFF (value 3 = register page,
  // which pages the low ROM back to slot 0 and hands &4000 to the ASIC). The
  // legacy Gate Array applies the low ROM at `lower_ROM_bank`, NOT always at slot
  // 0 (kon_cpc_ja.cpp ga_memory_manager). Burnin' Rubber's RAM-LAM restart uses
  // membank 2 to park the cartridge at &8000, leaving &0000-&3FFF as RAM.
  uint8_t cart_lower_slot = 0;
  uint8_t cart_upper = 1;         // high-ROM bank (ROM-select map); boots = BASIC
  const Device* asic = nullptr;   // read asic_unlocked() to gate RMR2

  // Fast-seam bank tables (memory.h §batch): CPU 16K slot → read source /
  // write target, rebuilt lazily after any banking-relevant change. A derived
  // CACHE of the decode above, not logical state — zeroed in the save blob
  // and re-marked dirty on load (host pointers would break blob determinism).
  const uint8_t* rd_bank[4] = {};
  uint8_t* wr_bank[4] = {};
  uint8_t fast_dirty = 1;
};

mem_state* self_of(void* self) { return static_cast<mem_state*>(self); }

bool lower_rom_on(const mem_state* m) { return (m->rom_config & 0x04) == 0; }
bool upper_rom_on(const mem_state* m) { return (m->rom_config & 0x08) == 0; }

// Does the (enabled) lower ROM overlay this address? On a classic machine that
// is always &0000-&3FFF. On a Plus cartridge the RMR2 membank field relocates
// the low-ROM page to slot 0/1/2 (&0000/&4000/&8000), so ONLY that slot is
// overlaid — the other low slots show RAM (kon_cpc_ja.cpp ga_memory_manager).
bool lower_rom_serves(const mem_state* m, uint16_t addr) {
  if (!lower_rom_on(m)) return false;
  if (m->cart) return (addr >> 14) == m->cart_lower_slot;  // slots 0..2 only
  return addr < 0x4000;
}

// Plus ROM-select -> upper-ROM cartridge bank (mirrors the legacy 6128+ map):
// BASIC (bank 1) by default; AMSDOS (7) -> bank 3; ROM >= 128 -> its low 5 bits.
uint8_t plus_upper_page(uint8_t sel) {
  if (sel == 7) return 3;
  if (sel >= 128) return sel & 31;
  return 1;
}

// The 6128 PAL's eight configurations: CPU slot (addr>>14) → physical 16K page.
// Pages 0-3 = base 64K; 4-7 = the selected expansion bank's four pages.
// (See docs/hardware/memory-device.md §2b — mirrors the legacy
// ga_init_banking.)
constexpr uint8_t kBankTable[8][4] = {
    {0, 1, 2, 3}, {0, 1, 2, 7}, {4, 5, 6, 7}, {0, 3, 2, 7},
    {0, 4, 2, 3}, {0, 5, 2, 3}, {0, 6, 2, 3}, {0, 7, 2, 3},
};

// Resolve a CPU address to its physical RAM byte under the active banking.
uint8_t* banked_ptr(mem_state* m, uint16_t addr) {
  const uint16_t offs = addr & 0x3FFF;
  // Banking is inert without an expansion (the PAL config is forced to 0).
  const uint8_t cfg =
      m->expansion ? static_cast<uint8_t>(m->ram_config & 7) : 0;
  const uint8_t page = kBankTable[cfg][addr >> 14];
  if (page < 4) return &m->ram[(static_cast<size_t>(page) << 14) | offs];

  // Expansion page: dk'tronics bank bits 3-5; Yarek adds inverted-address bits
  // above them for expansions larger than 512K. Out-of-range bank → bank 0.
  size_t bank = (m->ram_config >> 3) & 7;
  if (m->expansion_len > (512u * 1024))
    bank |= static_cast<size_t>(m->ram_ext) << 3;
  if ((bank + 1) * 0x10000 > m->expansion_len) bank = 0;
  return &m->expansion[(bank * 0x10000) |
                       (static_cast<size_t>(page - 4) << 14) | offs];
}

uint8_t mem_read(mem_state* m, uint16_t addr) {
  if (lower_rom_serves(m, addr)) {
    if (m->cart) {  // Plus: low ROM = the RMR2-selected cartridge page, at the
                    // RMR2-selected 16K slot (cart_lower_slot).
      const int bank = m->cart_lower < m->cart_banks ? m->cart_lower : 0;
      return m->cart[(bank * 0x4000) + (addr & 0x3FFF)];
    }
    return m->lower_rom[addr];
  }
  if (addr >= 0xC000 && upper_rom_on(m)) {
    if (m->cart) {  // Plus: high ROM = the ROM-select-mapped cartridge bank
      const int bank = m->cart_upper < m->cart_banks ? m->cart_upper : 0;
      return m->cart[(bank * 0x4000) + (addr - 0xC000)];
    }
    // Multi-ROM select: the latched number picks the expansion ROM; an
    // unpopulated number falls back to the onboard BASIC (no board answers).
    const uint8_t* rom = m->roms[m->rom_select];
    return rom ? rom[addr - 0xC000] : m->upper_rom[addr - 0xC000];
  }
  return *banked_ptr(m,
                     addr);  // ROM overlays win over whatever page is banked in
}

// The banking I/O-WRITE decode — the ONE definition both execution shapes
// share: mem_tick snoops it off the bus every asserted iorq/wr tick (the
// latches are idempotent under the held strobes), the Fast tier applies it
// once per OUT event via mem_fast_io_write. Any latch change invalidates the
// fast-seam bank tables.
void mem_io_write_decode(mem_state* m, uint16_t addr, uint8_t d) {
  // ROM enables / screen mode: the GA's mode register (A15=0, A14=1, fn 2).
  // On a Plus with the register page unlocked, a mode-write with bit5 set is
  // instead RMR2 — it remaps the low-ROM cartridge bank, leaving rom_config.
  if ((addr & 0xC000) == 0x4000 && (d >> 6) == 2) {
    if (m->cart && (d & 0x20) && m->asic && asic_unlocked(m->asic)) {
      // RMR2: bits 0-2 pick the cartridge page; bits 4-3 (membank) pick the
      // 16K slot it maps into. membank 3 = register page (ASIC overlay at
      // &4000): the low ROM falls back to slot 0, mirroring the legacy
      // `if (membank==3) membank=0` (kon_cpc_ja.cpp case 2).
      m->cart_lower = d & 7;
      const uint8_t membank = (d >> 3) & 3;
      m->cart_lower_slot = (membank == 3) ? 0 : membank;
    } else {
      m->rom_config = d;
    }
    m->fast_dirty = 1;
  }
  // RAM banking: the 6128 PAL 16L8 — decodes only A15 low + data 11xxxxxx (it
  // answers &7Fxx but also e.g. &3Fxx). The Yarek extended bank bits ride on
  // the INVERTED address bits A13..A11 of this very write (&7Fxx → 111 → ext
  // 0, so standard software always lands in the first 512K).
  if ((addr & 0x8000) == 0 && (d & 0xC0) == 0xC0) {
    m->ram_config = d;
    m->ram_ext = static_cast<uint8_t>((~(addr >> 11)) & 7);
    m->fast_dirty = 1;
  }
  // Upper-ROM select: board logic latches any write with A13 low (&DFxx
  // conventionally); the data byte is the ROM number. On a Plus it also maps
  // to the high-ROM cartridge bank.
  if ((addr & 0x2000) == 0) {
    m->rom_select = d;
    if (m->cart) m->cart_upper = plus_upper_page(d);
    m->fast_dirty = 1;
  }
}

// Rebuild the fast-seam bank tables from the live latches, THROUGH the same
// resolvers the per-cycle path reads with (banked_ptr, lower_rom_serves /
// upper_rom_on, the cart bank clamps of mem_read) — one banking truth. Every
// 16K page is contiguous in its backing store, so slot base + (addr & 0x3FFF)
// is exact.
void fast_rebuild(mem_state* m) {
  for (int slot = 0; slot < 4; ++slot) {
    uint8_t* ram = banked_ptr(m, static_cast<uint16_t>(slot << 14));
    m->wr_bank[slot] = ram;  // writes always land in banked RAM, never ROM
    m->rd_bank[slot] = ram;
  }
  if (lower_rom_on(m)) {
    const int slot = m->cart ? m->cart_lower_slot : 0;
    if (m->cart) {
      const int bank = m->cart_lower < m->cart_banks ? m->cart_lower : 0;
      m->rd_bank[slot] = &m->cart[static_cast<size_t>(bank) * 0x4000];
    } else {
      m->rd_bank[slot] = m->lower_rom;
    }
  }
  if (upper_rom_on(m)) {
    if (m->cart) {
      const int bank = m->cart_upper < m->cart_banks ? m->cart_upper : 0;
      m->rd_bank[3] = &m->cart[static_cast<size_t>(bank) * 0x4000];
    } else {
      const uint8_t* rom = m->roms[m->rom_select];
      m->rd_bank[3] = rom ? rom : m->upper_rom;
    }
  }
  m->fast_dirty = 0;
}

void mem_tick(void* self, const Bus* __restrict in, Bus* __restrict out) {
  mem_state* m = self_of(self);

  if (in->cpu.iorq && in->cpu.wr)
    mem_io_write_decode(m, in->cpu.addr, in->cpu.data);

  // Commit last tick's latched write unless an expansion vetoed it (its
  // /RAMDIS settles one tick behind the strobes — docs §4b).
  if (m->wr_armed) {
    if (!in->cpu.ramdis) *banked_ptr(m, m->wr_addr) = m->wr_val;
    m->wr_armed = 0;
  }

  // Memory access. Writes land in the banked RAM page (through ROM overlays)
  // via the one-tick latch; reads yield to expansion overlays (/ROMDIS
  // silences the internal ROM decode, /RAMDIS the RAM).
  if (in->cpu.mreq && in->cpu.rd) {
    const uint16_t addr = in->cpu.addr;
    const bool rom_serves =
        lower_rom_serves(m, addr) || (addr >= 0xC000 && upper_rom_on(m));
    if (!(rom_serves ? in->cpu.romdis : in->cpu.ramdis))
      out->cpu.data = mem_read(m, addr);
  }
  const uint8_t wr_now = (in->cpu.mreq && in->cpu.wr) ? 1 : 0;
  if (wr_now && !m->wr_prev) {  // one latch per access
    m->wr_addr = in->cpu.addr;
    m->wr_val = in->cpu.data;
    m->wr_armed = 1;
  }
  m->wr_prev = wr_now;

  // Video fetch port (the GA's video slots): always straight from RAM — the ROM
  // overlays apply only to CPU reads, never to the GA's display fetches.
  if (in->ram.fetch) out->ram.data = m->ram[in->ram.addr];
}

void mem_reset(void* self) {
  mem_state* m = self_of(self);
  // ROMs enabled by default; RAM/ROM/expansion contents persist (ROM is the
  // firmware; the expansion is caller-owned live storage).
  m->rom_config = 0;
  m->ram_config = 0;
  m->ram_ext = 0;
  m->rom_select = 0;  // back to BASIC
  m->cart_lower = 0;       // Plus: low ROM = cartridge bank 0 (OS)
  m->cart_lower_slot = 0;  // ...mapped at &0000-&3FFF (RMR2 membank 0)
  m->cart_upper = 1;  // Plus: high ROM = cartridge bank 1 (BASIC)
  m->fast_dirty = 1;
}

// Blob layout: [version:1][mem_state, wiring pointers zeroed][expansion RAM].
// The struct's caller-owned pointers (expansion/roms/cart/asic) are wiring, not
// logical state — zeroed in the blob so it's deterministic. The expansion RAM
// *contents* behind `expansion` ARE logical state, so they're appended.
size_t mem_dev_state_size(const void* self) {
  const mem_state* m = static_cast<const mem_state*>(self);
  return 1 + sizeof(mem_state) + m->expansion_len;
}
void mem_save(const void* self, void* buf) {
  const mem_state* m = static_cast<const mem_state*>(self);
  uint8_t* b = static_cast<uint8_t*>(buf);
  b[0] = 1;
  std::memcpy(b + 1, self, sizeof(mem_state));
  // Zero the wiring-pointer fields in the blob (their host addresses are not
  // logical state and would make the blob non-deterministic).
  std::memset(b + 1 + offsetof(mem_state, expansion), 0, sizeof(uint8_t*));
  std::memset(b + 1 + offsetof(mem_state, roms), 0, sizeof(mem_state::roms));
  std::memset(b + 1 + offsetof(mem_state, cart), 0, sizeof(const uint8_t*));
  std::memset(b + 1 + offsetof(mem_state, asic), 0, sizeof(const Device*));
  // The fast-seam tables are a derived cache of host pointers: zero them and
  // force dirty in the blob so saves stay deterministic and a load rebuilds.
  std::memset(b + 1 + offsetof(mem_state, rd_bank), 0,
              sizeof(mem_state::rd_bank) + sizeof(mem_state::wr_bank));
  b[1 + offsetof(mem_state, fast_dirty)] = 1;
  // Append the expansion RAM contents (logical state living behind the pointer).
  if (m->expansion && m->expansion_len)
    std::memcpy(b + 1 + sizeof(mem_state), m->expansion, m->expansion_len);
}
void mem_load(void* self, const void* buf) {
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  if (b[0] != 1) return;
  // Preserve the caller-owned attachments across the blob copy (the expansion
  // pointer/length and the ROM table are live wiring, not serializable state).
  mem_state* m = self_of(self);
  uint8_t* exp = m->expansion;
  const size_t exp_len = m->expansion_len;
  const uint8_t* cart = m->cart;  // cartridge image + ASIC handle are live wiring
  const int cart_banks = m->cart_banks;
  const Device* asic = m->asic;
  const uint8_t* saved_roms[256];
  std::memcpy(saved_roms, m->roms, sizeof(saved_roms));
  // How many appended expansion bytes the blob carries (its own saved length).
  size_t blob_exp_len = 0;
  std::memcpy(&blob_exp_len, b + 1 + offsetof(mem_state, expansion_len),
              sizeof(size_t));
  std::memcpy(self, b + 1, sizeof(mem_state));
  m->expansion = exp;
  m->expansion_len = exp_len;
  m->cart = cart;
  m->cart_banks = cart_banks;
  m->asic = asic;
  std::memcpy(m->roms, saved_roms, sizeof(saved_roms));
  // Restore expansion RAM contents when the live buffer matches the saved size.
  if (exp && exp_len && blob_exp_len == exp_len)
    std::memcpy(exp, b + 1 + sizeof(mem_state), exp_len);
}

}  // namespace

extern "C" {

size_t mem_state_size(void) { return sizeof(mem_state); }

Device mem_init(void* storage) {
  mem_state* m = new (storage) mem_state();
  mem_reset(m);
  return Device{m,        "memory", mem_tick, mem_reset, mem_dev_state_size,
                mem_save, mem_load};
}

void mem_peek(const Device* dev, MemRegs* out) {
  const mem_state* m = static_cast<const mem_state*>(dev->self);
  out->rom_config = m->rom_config;
  out->ram_config = m->ram_config;
  out->ram_ext = m->ram_ext;
  out->rom_select = m->rom_select;
}

void mem_attach_rom(const Device* dev, uint8_t n, const uint8_t* data) {
  mem_state* m = static_cast<mem_state*>(dev->self);
  m->roms[n] = data;
  m->fast_dirty = 1;
}

void mem_attach_expansion(const Device* dev, uint8_t* buf, size_t len) {
  mem_state* m = static_cast<mem_state*>(dev->self);
  m->expansion = buf;
  m->expansion_len = len - (len % 0x10000);  // whole 64K banks only
  if (m->expansion_len == 0) m->expansion = nullptr;
  m->fast_dirty = 1;
}

// Plus (6128+): overlay the low/high ROM windows with a parsed CPR image of
// `bytes` (caller-owned, 16K banks). Enables cartridge banking; boots low=bank0
// (OS), high=bank1 (BASIC). len==0 clears it (back to a plain ROM machine).
void mem_load_cartridge(const Device* dev, const uint8_t* image, size_t bytes) {
  mem_state* m = static_cast<mem_state*>(dev->self);
  m->cart = bytes >= 0x4000 ? image : nullptr;
  m->cart_banks = m->cart ? static_cast<int>(bytes / 0x4000) : 0;
  m->cart_lower = 0;
  m->cart_upper = 1;
  m->fast_dirty = 1;
}

// Intra-chip: the ASIC handle whose asic_unlocked() gates the RMR2 low-ROM remap
// (the low-ROM bank select is invisible until the register page is knocked open).
void mem_attach_asic(const Device* dev, const Device* asic) {
  static_cast<mem_state*>(dev->self)->asic = asic;
}

void mem_load_lower_rom(const Device* dev, const uint8_t* data, size_t len) {
  mem_state* m = static_cast<mem_state*>(dev->self);
  len = std::min(len, sizeof(m->lower_rom));
  std::memcpy(m->lower_rom, data, len);
}
void mem_load_upper_rom(const Device* dev, const uint8_t* data, size_t len) {
  mem_state* m = static_cast<mem_state*>(dev->self);
  len = std::min(len, sizeof(m->upper_rom));
  std::memcpy(m->upper_rom, data, len);
}
void mem_write_ram(const Device* dev, uint16_t addr, uint8_t val) {
  static_cast<mem_state*>(dev->self)->ram[addr] = val;
}
uint8_t mem_read_ram(const Device* dev, uint16_t addr) {
  return static_cast<const mem_state*>(dev->self)->ram[addr];
}

uint8_t mem_peek_cpu(const Device* dev, uint16_t addr) {
  return mem_read(static_cast<mem_state*>(dev->self), addr);
}

void mem_poke_cpu(const Device* dev, uint16_t addr, uint8_t val) {
  // The banked RAM byte, never ROM — exactly like a real mreq write.
  *banked_ptr(static_cast<mem_state*>(dev->self), addr) = val;
}

uint8_t mem_fast_read(const Device* dev, uint16_t addr) {
  mem_state* m = static_cast<mem_state*>(dev->self);
  if (m->fast_dirty) fast_rebuild(m);
  return m->rd_bank[addr >> 14][addr & 0x3FFF];
}

void mem_fast_write(const Device* dev, uint16_t addr, uint8_t val) {
  mem_state* m = static_cast<mem_state*>(dev->self);
  if (m->fast_dirty) fast_rebuild(m);
  m->wr_bank[addr >> 14][addr & 0x3FFF] = val;
}

int32_t mem_fast_write_off(const Device* dev, uint16_t addr) {
  mem_state* m = static_cast<mem_state*>(dev->self);
  if (m->fast_dirty) fast_rebuild(m);
  const uint8_t* p = m->wr_bank[addr >> 14] + (addr & 0x3FFF);
  // uintptr arithmetic: expansion banks live in a caller-owned buffer, so a
  // plain pointer difference against `ram` would be UB across objects.
  const uintptr_t off =
      reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(m->ram);
  return off < 0x10000 ? static_cast<int32_t>(off) : -1;
}

void mem_fast_io_write(const Device* dev, uint16_t port, uint8_t val) {
  mem_io_write_decode(static_cast<mem_state*>(dev->self), port, val);
}

const uint8_t* mem_video_ram(const Device* dev) {
  return static_cast<const mem_state*>(dev->self)->ram;
}

}  // extern "C"
