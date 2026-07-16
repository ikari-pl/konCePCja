/* memory_test.cpp — the CPC memory map: ROM overlays, banking I/O, and a boot
 * from lower ROM. See docs/hardware/memory-device.md. */

#include "hw/memory.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "hw/board.h"
#include "hw/z80.h"

namespace {

struct MemRig {
  std::vector<uint8_t> mem = std::vector<uint8_t>(mem_state_size());
  Board board;
  Device dev;
};

void make_mem(MemRig& rig) {
  rig.dev = mem_init(rig.mem.data());
  board_init(&rig.board);
  board_add(&rig.board, rig.dev);
  board_reset(&rig.board);
}

uint8_t rd(MemRig& rig, uint16_t addr) {
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.mreq = true;
  rig.board.bus.cpu.rd = true;
  rig.board.bus.cpu.addr = addr;
  board_tick(&rig.board);
  return rig.board.bus.cpu.data;
}
void wr(MemRig& rig, uint16_t addr, uint8_t val) {
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.mreq = true;
  rig.board.bus.cpu.wr = true;
  rig.board.bus.cpu.addr = addr;
  rig.board.bus.cpu.data = val;
  board_tick(&rig.board);
  // RAM commits the latched write on the next cycle (docs §4b) — a real
  // access is always followed by more bus cycles; give the rig one.
  rig.board.bus = bus_resting();
  board_tick(&rig.board);
}
void io(MemRig& rig, uint8_t data) {  // GA banking write
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.iorq = true;
  rig.board.bus.cpu.wr = true;
  rig.board.bus.cpu.addr = 0x7F00;
  rig.board.bus.cpu.data = data;
  board_tick(&rig.board);
}

// Always-on clock for the boot test.
void mclk_tick(void*, const Bus*, Bus* out) { out->clk.cpu = true; }
size_t mclk_size(const void*) { return 1; }
void mclk_save(const void*, void* b) { *static_cast<uint8_t*>(b) = 0; }
void mclk_load(void*, const void*) {}
Device mclk_device() {
  static uint8_t d = 0;
  return Device{&d,        "clk",     mclk_tick, [](void*) {},
                mclk_size, mclk_save, mclk_load};
}

}  // namespace

// An expansion's /RAMDIS settles one tick behind the strobes, so the memory
// Device commits a latched write on the FOLLOWING cycle and lets that late
// /RAMDIS veto it (memory.cpp §4b). And /RAMDIS on a read makes the internal
// RAM decode yield the data bus to the expansion. Previously untested.
TEST(Memory, RamdisVetoesTheLateWriteAndYieldsReads) {
  MemRig rig;
  make_mem(rig);

  // Baseline: an unvetoed write lands in RAM.
  wr(rig, 0x8000, 0xAB);
  EXPECT_EQ(rd(rig, 0x8000), 0xAB);

  // A write whose COMMIT cycle carries /RAMDIS (an expansion claimed it): the
  // one-tick-late latch is vetoed and RAM keeps its old value.
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.mreq = true;
  rig.board.bus.cpu.wr = true;
  rig.board.bus.cpu.addr = 0x8000;
  rig.board.bus.cpu.data = 0xCD;
  board_tick(&rig.board);  // access: latches the write
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.ramdis = true;  // the commit cycle, but /RAMDIS is up
  board_tick(&rig.board);
  EXPECT_EQ(rd(rig, 0x8000), 0xAB) << "the late /RAMDIS vetoed the write";

  // /RAMDIS on a READ: the memory Device must NOT drive the bus (it yields to
  // the expansion), so the data line stays the floating pull-up 0xFF instead of
  // the RAM byte 0xAB it would otherwise drive.
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.mreq = true;
  rig.board.bus.cpu.rd = true;
  rig.board.bus.cpu.ramdis = true;
  rig.board.bus.cpu.addr = 0x8000;
  board_tick(&rig.board);
  EXPECT_EQ(rig.board.bus.cpu.data, 0xFF)
      << "/RAMDIS: internal RAM yields — the bus floats high, not 0xAB";
}

TEST(Memory, RomOverlaysAndBankingIO) {
  MemRig rig;
  make_mem(rig);
  uint8_t lower[0x4000] = {0};
  lower[0] = 0xAA;
  lower[1] = 0xBB;
  uint8_t upper[0x4000] = {0};
  upper[0] = 0xCC;
  mem_load_lower_rom(&rig.dev, lower, sizeof(lower));
  mem_load_upper_rom(&rig.dev, upper, sizeof(upper));

  EXPECT_EQ(rd(rig, 0x0000), 0xAA) << "lower ROM overlays 0x0000 by default";
  EXPECT_EQ(rd(rig, 0xC000), 0xCC) << "upper ROM overlays 0xC000 by default";
  EXPECT_EQ(rd(rig, 0x8000), 0x00) << "middle is RAM";

  wr(rig, 0x0000, 0x11);  // write-through: lands in RAM under the ROM
  EXPECT_EQ(rd(rig, 0x0000), 0xAA) << "ROM still visible after the RAM write";
  io(rig, 0x80 | 0x04);  // mode reg, bit2 = lower-ROM disable
  EXPECT_EQ(rd(rig, 0x0000), 0x11)
      << "RAM revealed when the lower ROM is disabled";
  io(rig, 0x80);  // re-enable lower ROM
  EXPECT_EQ(rd(rig, 0x0000), 0xAA);

  io(rig, 0x80 | 0x08);  // bit3 = upper-ROM disable
  EXPECT_EQ(rd(rig, 0xC000), 0x00)
      << "RAM revealed when the upper ROM is disabled";

  MemRegs mr{};
  mem_peek(&rig.dev, &mr);
  EXPECT_EQ(mr.rom_config & 0x08, 0x08);
}

TEST(Memory, Z80BootsFromLowerRom) {
  // Program in lower ROM: LD A,0x42 ; LD (0x8000),A ; HALT — proving the Z80
  // fetches and runs code from ROM at reset (PC=0).
  const uint8_t boot[] = {0x3E, 0x42, 0x32, 0x00, 0x80, 0x76};

  std::vector<uint8_t> mmem(mem_state_size());
  Device mdev = mem_init(mmem.data());
  std::vector<uint8_t> zmem(z80_state_size());
  Device zdev = z80_init(zmem.data());
  Board board;
  board_init(&board);
  board_add(&board, mclk_device());
  board_add(&board, mdev);
  board_add(&board, zdev);
  board_reset(&board);
  mem_load_lower_rom(&mdev, boot,
                     sizeof(boot));  // after reset (reset keeps ROM anyway)

  Z80Regs r{};
  for (int tick = 0; tick < 4000; ++tick) {
    board_tick(&board);
    z80_peek(&zdev, &r);
    if (r.halted) break;
  }
  EXPECT_EQ(r.halted, 1) << "the Z80 ran the ROM program to HALT";
  EXPECT_EQ(static_cast<uint8_t>(r.af >> 8), 0x42);
  EXPECT_EQ(mem_read_ram(&mdev, 0x8000), 0x42)
      << "ROM code wrote 0x42 to RAM at 0x8000";
}

// The GA's video fetch port reads RAM even where a ROM overlays the same
// address for CPU reads — on the real board the GA's DRAM access never sees the
// ROMs.
TEST(Memory, VideoFetchPortIgnoresRomOverlays) {
  MemRig rig;
  make_mem(rig);
  std::vector<uint8_t> upper(0x4000, 0xAA);
  mem_load_upper_rom(&rig.dev, upper.data(), upper.size());
  mem_write_ram(&rig.dev, 0xC123, 0x55);

  EXPECT_EQ(rd(rig, 0xC123), 0xAA) << "CPU read sees the upper-ROM overlay";

  rig.board.bus = bus_resting();
  rig.board.bus.ram.fetch = true;
  rig.board.bus.ram.addr = 0xC123;
  board_tick(&rig.board);
  EXPECT_EQ(rig.board.bus.ram.data, 0x55) << "the video fetch always reads RAM";
}

// ---- 6128 PAL RAM banking (docs/hardware/memory-device.md §2b) ----

namespace {
// An I/O write to an arbitrary port (the PAL decodes A15 low; Yarek uses
// A13..A11).
void iow(MemRig& rig, uint16_t port, uint8_t data) {
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.iorq = true;
  rig.board.bus.cpu.wr = true;
  rig.board.bus.cpu.addr = port;
  rig.board.bus.cpu.data = data;
  board_tick(&rig.board);
}
}  // namespace

TEST(Memory, BankingInertWithoutExpansion) {
  MemRig rig;
  make_mem(rig);
  wr(rig, 0x8000, 0x11);
  iow(rig, 0x7F00,
      0xC2);  // config 2 (whole 64K → expansion) — but none attached
  EXPECT_EQ(rd(rig, 0x8000), 0x11) << "no expansion → the PAL latch is inert";
}

TEST(Memory, Config2MapsWholeExpansionBank) {
  MemRig rig;
  make_mem(rig);
  std::vector<uint8_t> exp(0x10000, 0);
  mem_attach_expansion(&rig.dev, exp.data(), exp.size());

  iow(rig, 0x7F00, 0xC2);  // config 2: slots 0-3 → expansion pages 4-7
  wr(rig, 0x8000, 0x77);   // slot 2 → page 6 → expansion offset 2*16K
  EXPECT_EQ(exp[0x8000], 0x77) << "write landed in the expansion bank";
  EXPECT_EQ(mem_read_ram(&rig.dev, 0x8000), 0x00) << "base RAM untouched";
  EXPECT_EQ(rd(rig, 0x8000), 0x77) << "read comes back from the expansion";

  iow(rig, 0x7F00, 0xC0);  // back to config 0
  EXPECT_EQ(rd(rig, 0x8000), 0x00) << "base RAM visible again";
}

TEST(Memory, Config3AliasesBasePage3IntoSlot1) {
  MemRig rig;
  make_mem(rig);
  std::vector<uint8_t> exp(0x10000, 0);
  mem_attach_expansion(&rig.dev, exp.data(), exp.size());

  iow(rig, 0x7F00, 0xC3);  // config 3: slot 1 → base page 3 (the CP/M+ map)
  wr(rig, 0x4123, 0x5A);
  EXPECT_EQ(mem_read_ram(&rig.dev, 0xC123), 0x5A)
      << "slot 1 write surfaced at base 0xC000 (page 3)";
}

TEST(Memory, Configs4To7MapExpansionPagesIntoSlot1) {
  MemRig rig;
  make_mem(rig);
  std::vector<uint8_t> exp(0x10000, 0);
  mem_attach_expansion(&rig.dev, exp.data(), exp.size());

  for (uint8_t cfg = 4; cfg <= 7; ++cfg) {
    iow(rig, 0x7F00, static_cast<uint8_t>(0xC0 | cfg));
    wr(rig, 0x4000, static_cast<uint8_t>(0x90 + cfg));
  }
  for (uint8_t cfg = 4; cfg <= 7; ++cfg) {
    EXPECT_EQ(exp[static_cast<size_t>(cfg - 4) << 14], 0x90 + cfg)
        << "config " << int(cfg) << " → expansion page " << int(cfg - 4);
  }
}

TEST(Memory, DkTronicsBankSelectBits3To5) {
  MemRig rig;
  make_mem(rig);
  std::vector<uint8_t> exp(8 * 0x10000, 0);  // 512K = 8 banks
  mem_attach_expansion(&rig.dev, exp.data(), exp.size());

  for (uint8_t bank = 0; bank < 8; ++bank) {
    iow(rig, 0x7F00, static_cast<uint8_t>(0xC4 | (bank << 3)));  // cfg 4 + bank
    wr(rig, 0x4000, static_cast<uint8_t>(0xA0 + bank));
  }
  for (uint8_t bank = 0; bank < 8; ++bank) {
    EXPECT_EQ(exp[static_cast<size_t>(bank) * 0x10000], 0xA0 + bank)
        << "bank " << int(bank) << " selected by data bits 3-5";
  }
}

TEST(Memory, YarekExtendedBanksFromInvertedAddressBits) {
  MemRig rig;
  make_mem(rig);
  std::vector<uint8_t> exp(16 * 0x10000, 0);  // 1 MB = 16 banks
  mem_attach_expansion(&rig.dev, exp.data(), exp.size());

  // Standard port &7Fxx: A13..A11 = 111 → ext 0 → banks 0-7 (backward
  // compatible).
  iow(rig, 0x7F00, 0xC4);
  wr(rig, 0x4000, 0x11);
  EXPECT_EQ(exp[0x0000], 0x11) << "port &7F00 → ext 0 → bank 0";

  // Port &77xx: A11 = 0 → ext 1 → banks 8-15.
  iow(rig, 0x7700, 0xC4);
  wr(rig, 0x4000, 0x22);
  EXPECT_EQ(exp[8u * 0x10000], 0x22) << "port &7700 → ext 1 → bank 8";

  iow(rig, 0x7700, static_cast<uint8_t>(0xC4 | (7 << 3)));  // bank 8+7 = 15
  wr(rig, 0x4000, 0x33);
  EXPECT_EQ(exp[15u * 0x10000], 0x33)
      << "ext 1 + bits 111 → bank 15 (the megabyte)";
}

TEST(Memory, SmallExpansionsIgnoreYarekBits) {
  MemRig rig;
  make_mem(rig);
  std::vector<uint8_t> exp(8 * 0x10000, 0);  // 512K: 3-bit banks only
  mem_attach_expansion(&rig.dev, exp.data(), exp.size());

  iow(rig, 0x7700, 0xC4);  // would be bank 8 under Yarek — must stay bank 0
  wr(rig, 0x4000, 0x44);
  EXPECT_EQ(exp[0x0000], 0x44) << "≤512K expansions use only data bits 3-5";
}

TEST(Memory, OutOfRangeBankFallsBackToBank0) {
  MemRig rig;
  make_mem(rig);
  std::vector<uint8_t> exp(0x10000, 0);  // one bank only
  mem_attach_expansion(&rig.dev, exp.data(), exp.size());

  iow(rig, 0x7F00, static_cast<uint8_t>(0xC4 | (3 << 3)));  // bank 3 of 1
  wr(rig, 0x4000, 0x66);
  EXPECT_EQ(exp[0x0000], 0x66) << "out-of-range bank select → bank 0";
}

TEST(Memory, VideoFetchIsNeverBanked) {
  MemRig rig;
  make_mem(rig);
  std::vector<uint8_t> exp(0x10000, 0xEE);
  mem_attach_expansion(&rig.dev, exp.data(), exp.size());
  mem_write_ram(&rig.dev, 0xC123, 0x33);

  iow(rig, 0x7F00, 0xC2);  // whole 64K banked to the expansion for the CPU...
  rig.board.bus = bus_resting();
  rig.board.bus.ram.fetch = true;
  rig.board.bus.ram.addr = 0xC123;
  board_tick(&rig.board);
  EXPECT_EQ(rig.board.bus.ram.data, 0x33)
      << "...but the GA's video fetch still reads the base 64K";
}

TEST(Memory, RomOverlayWinsOverBankedPage) {
  MemRig rig;
  make_mem(rig);
  std::vector<uint8_t> exp(0x10000, 0);
  mem_attach_expansion(&rig.dev, exp.data(), exp.size());
  std::vector<uint8_t> upper(0x4000, 0xAA);
  mem_load_upper_rom(&rig.dev, upper.data(), upper.size());

  iow(rig, 0x7F00,
      0xC2);  // slot 3 → expansion page 7, but the upper ROM is enabled
  EXPECT_EQ(rd(rig, 0xC000), 0xAA)
      << "ROM overlay beats the banked page for reads";
  wr(rig, 0xC000, 0x55);
  EXPECT_EQ(exp[3u << 14], 0x55) << "the write went through to the banked page";
}

// ---- Multi-ROM select (&DF) — docs/hardware/memory-device.md §2c ----

TEST(Memory, RomSelectLatchesOnA13LowWrites) {
  MemRig rig;
  make_mem(rig);
  MemRegs r{};

  iow(rig, 0xDF00, 7);  // the conventional port
  mem_peek(&rig.dev, &r);
  EXPECT_EQ(r.rom_select, 7);

  iow(rig, 0x9F00,
      3);  // any A13-low write latches (board logic decodes A13 only)
  mem_peek(&rig.dev, &r);
  EXPECT_EQ(r.rom_select, 3);

  iow(rig, 0xFF00, 5);  // A13 high → not the ROM select
  mem_peek(&rig.dev, &r);
  EXPECT_EQ(r.rom_select, 3) << "A13-high writes must not touch the latch";
}

TEST(Memory, RomSelectSwitchesUpperRomAndFallsBackToBasic) {
  MemRig rig;
  make_mem(rig);
  std::vector<uint8_t> basic(0x4000, 0xAA);
  mem_load_upper_rom(&rig.dev, basic.data(), basic.size());  // "BASIC"
  std::vector<uint8_t> amsdos(0x4000, 0xD5);
  mem_attach_rom(&rig.dev, 7, amsdos.data());  // "AMSDOS" at slot 7

  EXPECT_EQ(rd(rig, 0xC000), 0xAA) << "select 0 (reset) = BASIC";
  iow(rig, 0xDF00, 7);
  EXPECT_EQ(rd(rig, 0xC000), 0xD5) << "select 7 = the attached ROM";
  iow(rig, 0xDF00, 42);
  EXPECT_EQ(rd(rig, 0xC000), 0xAA) << "unpopulated select falls back to BASIC";
  iow(rig, 0xDF00, 0);
  EXPECT_EQ(rd(rig, 0xC000), 0xAA) << "select 0 = BASIC again";
}

TEST(Memory, RomSelectRespectsUpperRomEnableAndWrites) {
  MemRig rig;
  make_mem(rig);
  std::vector<uint8_t> amsdos(0x4000, 0xD5);
  mem_attach_rom(&rig.dev, 7, amsdos.data());
  iow(rig, 0xDF00, 7);

  io(rig, 0x80 | 0x08);  // GA: disable the upper ROM
  EXPECT_EQ(rd(rig, 0xC000), 0x00)
      << "upper ROM disabled → RAM, whatever is selected";
  io(rig, 0x80);  // re-enable
  EXPECT_EQ(rd(rig, 0xC000), 0xD5);

  wr(rig, 0xC123, 0x66);  // writes pass through the selected ROM into RAM
  EXPECT_EQ(mem_read_ram(&rig.dev, 0xC123), 0x66);
  EXPECT_EQ(rd(rig, 0xC123), 0xD5) << "the ROM still answers reads";
}

// ---------------------------------------------------------------------------
// Fast-tier batch seam (memory.h §batch, beads-f0bq): mem_fast_read/write must
// be VIEW-IDENTICAL to the proven per-cycle decode (mem_peek_cpu / mem_poke_cpu
// resolve through the same helpers) under every banking configuration, and
// mem_fast_io_write must be LATCH-IDENTICAL to the bus-snooped decode.
// ---------------------------------------------------------------------------

namespace {

// Distinguishable content everywhere: base pages 0xA0+page, expansion banks
// 0xE0+bank(+page in low nibble), ROMs 0x10/0x20.
void seed_all(MemRig& rig, std::vector<uint8_t>& expansion) {
  for (uint32_t a = 0; a < 0x10000; ++a)
    mem_write_ram(&rig.dev, static_cast<uint16_t>(a),
                  static_cast<uint8_t>(0xA0 + (a >> 14)));
  for (size_t i = 0; i < expansion.size(); ++i)
    expansion[i] = static_cast<uint8_t>(0xE0 + (i >> 14));
  std::vector<uint8_t> lo(0x4000, 0x10), hi(0x4000, 0x20);
  mem_load_lower_rom(&rig.dev, lo.data(), lo.size());
  mem_load_upper_rom(&rig.dev, hi.data(), hi.size());
  mem_attach_expansion(&rig.dev, expansion.data(), expansion.size());
}

}  // namespace

TEST(Memory, FastSeamMatchesCpuViewAcrossBankingConfigs) {
  MemRig rig;
  make_mem(rig);
  std::vector<uint8_t> expansion(128 * 1024);
  seed_all(rig, expansion);

  const uint16_t samples[] = {0x0000, 0x2ABC, 0x3FFF, 0x4000, 0x6DEF,
                              0x7FFF, 0x8000, 0xABCD, 0xC000, 0xFFFF};
  // fn2 ROM enables (bits 2/3) x the eight PAL configs x both dk banks.
  for (uint8_t rom : {0x80, 0x84, 0x88, 0x8C}) {
    for (uint8_t cfg = 0; cfg < 8; ++cfg) {
      for (uint8_t bank = 0; bank < 2; ++bank) {
        io(rig, rom);  // GA fn2 via the snooped bus decode
        io(rig, static_cast<uint8_t>(0xC0 | (bank << 3) | cfg));
        for (uint16_t a : samples) {
          EXPECT_EQ(mem_fast_read(&rig.dev, a), mem_peek_cpu(&rig.dev, a))
              << "rom=" << int(rom) << " cfg=" << int(cfg)
              << " bank=" << int(bank) << " addr=" << a;
        }
      }
    }
  }

  // ROM-overlaid write goes to RAM: with lower ROM enabled, write via the
  // seam, see ROM through both views, then disable the ROM and see the byte.
  io(rig, 0x80);  // both ROMs on
  mem_fast_write(&rig.dev, 0x0123, 0x77);
  EXPECT_EQ(mem_fast_read(&rig.dev, 0x0123), 0x10);  // still the ROM
  EXPECT_EQ(mem_peek_cpu(&rig.dev, 0x0123), 0x10);
  io(rig, 0x8C);  // ROMs off
  EXPECT_EQ(mem_fast_read(&rig.dev, 0x0123), 0x77);
  EXPECT_EQ(mem_peek_cpu(&rig.dev, 0x0123), 0x77);
}

TEST(Memory, FastSeamRomSelectAndYarek) {
  MemRig rig;
  make_mem(rig);
  std::vector<uint8_t> expansion(1024 * 1024);  // Yarek needs > 512K
  seed_all(rig, expansion);

  // Upper-ROM select through the fast decode: attach ROM 7, select it, and
  // both views must switch together (and fall back to BASIC on detach).
  std::vector<uint8_t> amsdos(0x4000, 0x70);
  mem_attach_rom(&rig.dev, 7, amsdos.data());
  mem_fast_io_write(&rig.dev, 0xDF00, 7);
  EXPECT_EQ(mem_fast_read(&rig.dev, 0xC500), 0x70);
  EXPECT_EQ(mem_peek_cpu(&rig.dev, 0xC500), 0x70);
  mem_fast_io_write(&rig.dev, 0xDF00, 3);  // unpopulated → BASIC fallback
  EXPECT_EQ(mem_fast_read(&rig.dev, 0xC500), 0x20);
  EXPECT_EQ(mem_peek_cpu(&rig.dev, 0xC500), 0x20);

  // Yarek extended banks ride the write ADDRESS (inverted A13..A11): the same
  // data byte through a different port lands in a different 64K bank. ext=1
  // → bank 8, which the 1 MB expansion actually holds (ext=7 → bank 56 would
  // fall back to bank 0 and prove nothing).
  mem_fast_io_write(&rig.dev, 0x7F00, 0xC2);  // ext = ~111 = 0 → bank 0
  const uint8_t bank0 = mem_fast_read(&rig.dev, 0x4000);
  EXPECT_EQ(bank0, mem_peek_cpu(&rig.dev, 0x4000));
  mem_fast_io_write(&rig.dev, 0x3000, 0xC2);  // A13..A11=110 → ext 1 → bank 8
  const uint8_t bank8 = mem_fast_read(&rig.dev, 0x4000);
  EXPECT_NE(bank0, bank8) << "Yarek bits must select a different 64K bank";
  EXPECT_EQ(mem_fast_read(&rig.dev, 0x4000), mem_peek_cpu(&rig.dev, 0x4000));
}

TEST(Memory, FastIoWriteDecodeMatchesBusDecode) {
  MemRig bus_rig, fast_rig;
  make_mem(bus_rig);
  make_mem(fast_rig);
  std::vector<uint8_t> exp_a(128 * 1024), exp_b(128 * 1024);
  mem_attach_expansion(&bus_rig.dev, exp_a.data(), exp_a.size());
  mem_attach_expansion(&fast_rig.dev, exp_b.data(), exp_b.size());

  // (port, data) pairs covering every decode arm and its address dependence.
  const struct {
    uint16_t port;
    uint8_t data;
  } writes[] = {
      {0x7F00, 0x8C},  // fn2: both ROMs off
      {0x7F00, 0xC5},  // PAL config 5 (A15 low, A13 high → ext 0)
      {0x1800, 0xC2},  // PAL via a non-canonical A15-low port (Yarek ext=4)
      {0xDF00, 0x07},  // ROM select (A13 low)
      {0x4000, 0x85},  // fn2 via the low fn2 port
      {0x0000, 0xC7},  // PAL, Yarek ext=7
      {0xDFFF, 0x80},  // ROM select again
  };
  for (const auto& w : writes) {
    bus_rig.board.bus = bus_resting();
    bus_rig.board.bus.cpu.iorq = true;
    bus_rig.board.bus.cpu.wr = true;
    bus_rig.board.bus.cpu.addr = w.port;
    bus_rig.board.bus.cpu.data = w.data;
    board_tick(&bus_rig.board);
    mem_fast_io_write(&fast_rig.dev, w.port, w.data);

    MemRegs a{}, b{};
    mem_peek(&bus_rig.dev, &a);
    mem_peek(&fast_rig.dev, &b);
    EXPECT_EQ(a.rom_config, b.rom_config) << "port=" << w.port;
    EXPECT_EQ(a.ram_config, b.ram_config) << "port=" << w.port;
    EXPECT_EQ(a.ram_ext, b.ram_ext) << "port=" << w.port;
    EXPECT_EQ(a.rom_select, b.rom_select) << "port=" << w.port;
  }
}

TEST(Memory, FastSeamMatchesCpuViewWithCartridge) {
  MemRig rig;
  make_mem(rig);
  std::vector<uint8_t> expansion(128 * 1024);
  seed_all(rig, expansion);
  // Two-bank synthetic cartridge: bank 0 = 0xB0.., bank 1 = 0xB1..
  std::vector<uint8_t> cart(2 * 0x4000);
  for (size_t i = 0; i < cart.size(); ++i)
    cart[i] = static_cast<uint8_t>(0xB0 + (i >> 14));
  mem_load_cartridge(&rig.dev, cart.data(), cart.size());

  // Boot map: low = bank 0 at slot 0, high = bank 1. (The RMR2 slot remap
  // needs an unlocked ASIC and is proven by construction — the table reads
  // the same cart_lower/cart_lower_slot latches mem_read resolves — plus the
  // Plus boot lockstep at machine level.)
  for (uint16_t a : {0x0000, 0x3FFF, 0x4000, 0x8000, 0xC000, 0xFFFF}) {
    EXPECT_EQ(mem_fast_read(&rig.dev, a), mem_peek_cpu(&rig.dev, a))
        << "cart addr=" << a;
  }
  EXPECT_EQ(mem_fast_read(&rig.dev, 0x0000), 0xB0);
  EXPECT_EQ(mem_fast_read(&rig.dev, 0xC000), 0xB1);
  // ROM select >= 128 maps its low 5 bits — bank 0 here.
  mem_fast_io_write(&rig.dev, 0xDF00, 0x80);
  EXPECT_EQ(mem_fast_read(&rig.dev, 0xC000), 0xB0);
  EXPECT_EQ(mem_peek_cpu(&rig.dev, 0xC000), 0xB0);
}

// Save/load: the bank tables are host-pointer cache, so blobs must stay
// deterministic (used-seam vs never-used-seam machines hash equal) and a
// loaded machine must REBUILD rather than reuse the saver's pointers.
TEST(Memory, FastSeamTablesAreNotState) {
  MemRig used, fresh;
  make_mem(used);
  make_mem(fresh);
  // Same logical state on both; only `used` exercises the seam.
  wr(used, 0x8000, 0x55);
  wr(fresh, 0x8000, 0x55);
  (void)mem_fast_read(&used.dev, 0x8000);  // builds tables on `used` only

  std::vector<uint8_t> blob_used(used.dev.state_size(used.dev.self));
  std::vector<uint8_t> blob_fresh(fresh.dev.state_size(fresh.dev.self));
  ASSERT_EQ(blob_used.size(), blob_fresh.size());
  used.dev.save(used.dev.self, blob_used.data());
  fresh.dev.save(fresh.dev.self, blob_fresh.data());
  EXPECT_EQ(blob_used, blob_fresh)
      << "bank tables leaked host pointers into the save blob";

  // Load the blob into a THIRD machine and use the seam immediately: it must
  // resolve through its own storage (a stale pointer would read the wrong
  // machine's RAM — or worse).
  MemRig third;
  make_mem(third);
  third.dev.load(third.dev.self, blob_used.data());
  EXPECT_EQ(mem_fast_read(&third.dev, 0x8000), 0x55);
  mem_fast_write(&third.dev, 0x8000, 0x66);
  EXPECT_EQ(mem_peek_cpu(&third.dev, 0x8000), 0x66);
  EXPECT_EQ(mem_peek_cpu(&used.dev, 0x8000), 0x55)
      << "the third machine's seam wrote through a stale saver pointer";
}

// The decisive write-path proof: drive TWIN devices through an identical
// script of banking changes + writes — one twin entirely via the fast seam,
// the other via per-cycle bus accesses — then require the ENTIRE store (base
// 64K + expansion) byte-identical. A fast write landing in any wrong cell
// (wrong page, wrong bank, ROM-masked aliasing) diverges the stores even if
// every read-back through the same wrong tables would self-agree.
TEST(Memory, FastSeamWritesLandInTheSameCellsAsTheBusPath) {
  MemRig fast, ref;
  make_mem(fast);
  make_mem(ref);
  std::vector<uint8_t> exp_fast(128 * 1024), exp_ref(128 * 1024);
  seed_all(fast, exp_fast);
  seed_all(ref, exp_ref);

  const uint16_t addrs[] = {0x0123, 0x4123, 0x8123, 0xC123};
  uint8_t val = 1;
  for (uint8_t rom : {0x80, 0x8C}) {
    for (uint8_t cfg = 0; cfg < 8; ++cfg) {
      const uint8_t pal = static_cast<uint8_t>(0xC0 | cfg);
      mem_fast_io_write(&fast.dev, 0x7F00, rom);
      mem_fast_io_write(&fast.dev, 0x7F00, pal);
      io(ref, rom);
      io(ref, pal);
      for (uint16_t a : addrs) {
        mem_fast_write(&fast.dev, a, val);
        wr(ref, a, val);
        ++val;
      }
    }
  }
  for (uint32_t a = 0; a < 0x10000; ++a) {
    ASSERT_EQ(mem_read_ram(&fast.dev, static_cast<uint16_t>(a)),
              mem_read_ram(&ref.dev, static_cast<uint16_t>(a)))
        << "base RAM diverged at " << a;
  }
  ASSERT_EQ(exp_fast, exp_ref) << "expansion RAM diverged";
}
