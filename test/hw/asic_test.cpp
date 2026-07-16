/* asic_test.cpp — the Plus/6128+ ASIC Device, increment 1: the unlock knock
 * FSM and the &4000-&7FFF register-page overlay. Proves the page is invisible
 * (RAM shows through) until the knock unlocks it, that writes then land in the
 * ASIC (vetoing the internal RAM via /RAMDIS), that the config block decodes,
 * and that reads answer from the page. See docs/hardware/asic-device.md. */

#include "hw/asic.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "hw/board.h"
#include "hw/memory.h"

namespace {

struct AsicRig {
  std::vector<uint8_t> mmem = std::vector<uint8_t>(mem_state_size());
  std::vector<uint8_t> amem = std::vector<uint8_t>(asic_state_size());
  Board board;
  Device mem;
  Device asic;
};

void make_rig(AsicRig& rig) {
  rig.mem = mem_init(rig.mmem.data());
  rig.asic = asic_init(rig.amem.data());
  board_init(&rig.board);
  board_add(&rig.board, rig.mem);
  board_add(&rig.board, rig.asic);
  board_reset(&rig.board);
  asic_set_plugged(&rig.asic, 1);  // model 3
}

void idle(AsicRig& rig, int n = 2) {
  for (int i = 0; i < n; ++i) {
    rig.board.bus = bus_resting();
    board_tick(&rig.board);
  }
}

// A CRTC register-select write (&BC00) — where the unlock knock is sent.
void knock(AsicRig& rig, uint8_t val) {
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.iorq = true;
  rig.board.bus.cpu.wr = true;
  rig.board.bus.cpu.addr = 0xBC00;
  rig.board.bus.cpu.data = val;
  board_tick(&rig.board);
  idle(rig);
}

// The 17-byte unlock knock: a non-zero prime, then kLockSeq[1..15], then a
// trigger byte (the golden master's asic_poke_lock_sequence).
void send_unlock(AsicRig& rig) {
  const uint8_t seq[17] = {0xFF, 0x00, 0xFF, 0x77, 0xB3, 0x51, 0xA8, 0xD4, 0x62,
                           0x39, 0x9C, 0x46, 0x2B, 0x15, 0x8A, 0xCD, 0x01};
  for (uint8_t b : seq) knock(rig, b);
}

// RMR2: page the register page into &4000-&7FFF (on) or map a low-ROM bank / RAM
// there instead (off). A Gate-Array fn-2 write (port &7Fxx, data 10xxxxxx) with
// bit5 set; the membank field (bits 4-3) selects the map — value 3 = page on.
// The unlock knock alone only ENABLES RMR2; the CPU must still page the register
// page in before its writes reach the ASIC (asic-device.md).
void map_page(AsicRig& rig, bool on) {
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.iorq = true;
  rig.board.bus.cpu.wr = true;
  rig.board.bus.cpu.addr = 0x7F00;
  rig.board.bus.cpu.data = on ? 0xB8 : 0xB0;  // membank 3 = on, 2 = off
  board_tick(&rig.board);
  idle(rig);
}

// A raw RMR2 / Gate-Array function-2 write with an explicit data byte (port
// &7Fxx). Used to exercise the membank (bits 4-3) and page (bits 0-2) fields.
void rmr2_write(AsicRig& rig, uint8_t data) {
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.iorq = true;
  rig.board.bus.cpu.wr = true;
  rig.board.bus.cpu.addr = 0x7F00;
  rig.board.bus.cpu.data = data;
  board_tick(&rig.board);
  idle(rig);
}

// The usual bring-up: knock to unlock, then RMR2-map the register page in.
void unlock_and_map(AsicRig& rig) {
  send_unlock(rig);
  map_page(rig, true);
}

// A held memory write. PRESERVE the device-driven bus outputs across the held
// ticks (only re-drive the CPU pins) so the ASIC's /RAMDIS survives into the
// memory Device's one-tick commit and vetoes the internal RAM write
// (memory-device.md §4b) — resetting the bus each tick would wipe it.
void cpu_write(AsicRig& rig, uint16_t addr, uint8_t val) {
  for (int i = 0; i < 3; ++i) {
    Bus b = rig.board.bus;
    b.cpu.m1 = b.cpu.iorq = b.cpu.rd = false;
    b.cpu.mreq = true;
    b.cpu.wr = true;
    b.cpu.addr = addr;
    b.cpu.data = val;
    rig.board.bus = b;
    board_tick(&rig.board);
  }
  idle(rig);
}

// A held memory read (sample at the end — the T3 discipline).
uint8_t cpu_read(AsicRig& rig, uint16_t addr) {
  for (int i = 0; i < 6; ++i) {
    Bus b = rig.board.bus;
    b.cpu.m1 = b.cpu.iorq = b.cpu.wr = false;
    b.cpu.mreq = b.cpu.rd = true;
    b.cpu.addr = addr;
    rig.board.bus = b;
    board_tick(&rig.board);
  }
  const uint8_t v = rig.board.bus.cpu.data;
  idle(rig);
  return v;
}

}  // namespace

TEST(Asic, LockedUntilTheKnockThenTheRegisterPageAppears) {
  AsicRig rig;
  make_rig(rig);
  AsicRegs r{};
  asic_peek(&rig.asic, &r);
  EXPECT_EQ(r.locked, 1) << "cold boot: the register page is hidden";

  // Locked: &4000 is ordinary RAM.
  cpu_write(rig, 0x4000, 0xAB);
  EXPECT_EQ(cpu_read(rig, 0x4000), 0xAB) << "locked: RAM answers at &4000";
  EXPECT_EQ(asic_page_peek(&rig.asic, 0x0000), 0x00)
      << "the write did not reach the ASIC page while locked";

  send_unlock(rig);
  asic_peek(&rig.asic, &r);
  EXPECT_EQ(r.locked, 0) << "the knock unlocked the ASIC";
}

TEST(Asic, RegisterWritesLandInTheAsicNotRam) {
  AsicRig rig;
  make_rig(rig);
  cpu_write(rig, 0x4000, 0xAB);  // seed RAM while locked
  unlock_and_map(rig);

  // Sprite pixel data: lands in the page, and the internal RAM is vetoed.
  cpu_write(rig, 0x4000, 0x5C);
  EXPECT_EQ(asic_page_peek(&rig.asic, 0x0000), 0x5C) << "the ASIC took it";
  EXPECT_EQ(cpu_read(rig, 0x4000), 0x5C) << "reads answer from the page";

  // Config block decodes into named registers.
  cpu_write(rig, 0x6800, 0x64);  // PRI scanline
  cpu_write(rig, 0x6804, 0x85);  // scroll: H=5, V=0, border=1
  cpu_write(rig, 0x6805, 0x30);  // IM2 vector (& 0xF8)
  AsicRegs r{};
  asic_peek(&rig.asic, &r);
  EXPECT_EQ(r.pri_line, 0x64);
  EXPECT_EQ(r.hscroll, 5);
  EXPECT_EQ(r.vscroll, 0);
  EXPECT_EQ(r.extend_border, 1);
  EXPECT_EQ(r.int_vector, 0x30);

  // Re-lock: match kLockSeq[1..14] then MISMATCH the last byte (the FSM locks
  // when the match reaches the final position by a wrong byte — asic.cpp).
  // send_unlock left the FSM at position 1, so start from kLockSeq[1].
  const uint8_t relock[15] = {0x00, 0xFF, 0x77, 0xB3, 0x51, 0xA8, 0xD4, 0x62,
                              0x39, 0x9C, 0x46, 0x2B, 0x15, 0x8A, 0x00};
  for (uint8_t b : relock) knock(rig, b);
  asic_peek(&rig.asic, &r);
  EXPECT_EQ(r.locked, 1) << "the sequence-minus-last re-locks";
  EXPECT_EQ(cpu_read(rig, 0x4000), 0xAB) << "RAM shows through again";
}

TEST(Asic, ResetRelocksButKeepsThePage) {
  AsicRig rig;
  make_rig(rig);
  unlock_and_map(rig);
  cpu_write(rig, 0x6801, 0x2A);  // split scanline
  board_reset(&rig.board);
  AsicRegs r{};
  asic_peek(&rig.asic, &r);
  EXPECT_EQ(r.locked, 1) << "reset re-locks";
  EXPECT_EQ(r.plugged, 1) << "still on the board";
  EXPECT_EQ(asic_page_peek(&rig.asic, 0x2801), 0x2A)
      << "the register page survives reset";
}

// Increment 1-A: the register regions decode into structured state. Sprites
// (pixels + X/Y/magnification), the 12-bit palette, and the DMA channel
// registers are parsed as the CPU writes them through the unlocked page.
TEST(Asic, RegisterDecodeSpritesPaletteDma) {
  AsicRig rig;
  make_rig(rig);

  // While locked, page writes decode nothing (the overlay is off).
  cpu_write(rig, 0x6400, 0xC3);
  EXPECT_EQ(asic_vid_palette(&rig.asic, 0), 0x0000)
      << "no decode while the register page is locked";

  unlock_and_map(rig);

  // --- Sprite pixels (&4000-&4FFF): only the low nibble is the index. ---
  // addr = 0x4000 | id<<8 | y<<4 | x  -> sprite 3, pixel (x=2, y=5).
  cpu_write(rig, 0x4000 | (3 << 8) | (5 << 4) | 2, 0x0A);
  EXPECT_EQ(asic_vid_sprite_pixel(&rig.asic, 3, 2, 5), 0x0A);
  cpu_write(rig, 0x4000 | (3 << 8) | (5 << 4) | 2, 0x3C);
  EXPECT_EQ(asic_vid_sprite_pixel(&rig.asic, 3, 2, 5), 0x0C)
      << "index is val & 0xF";

  // --- Sprite attributes (&6000-&607F): X 10-bit, Y 9-bit, magnification. ---
  cpu_write(rig, 0x6000 + 3 * 8 + 0, 0x23);          // X low
  cpu_write(rig, 0x6000 + 3 * 8 + 1, 0x01);          // X high (bit 8)
  cpu_write(rig, 0x6000 + 3 * 8 + 2, 0xAB);          // Y low
  cpu_write(rig, 0x6000 + 3 * 8 + 3, 0x01);          // Y high (bit 8)
  cpu_write(rig, 0x6000 + 3 * 8 + 4, (2 << 2) | 3);  // mag: x=2, y=×4
  uint16_t sx = 0, sy = 0;
  uint8_t mx = 0, my = 0;
  asic_vid_sprite_attr(&rig.asic, 3, &sx, &sy, &mx, &my);
  EXPECT_EQ(sx, 0x123);
  EXPECT_EQ(sy, 0x1AB);
  EXPECT_EQ(mx, 2);
  EXPECT_EQ(my, 4) << "magnification field 3 means ×4";

  // X is 10-bit (cpctech: range 0-1023, or -256..+767 signed): bit 9 must
  // survive the decode. (The spec previously mislabelled X as 9-bit.)
  cpu_write(rig, 0x6000 + 3 * 8 + 1, 0x02);  // X high = bit 9 only
  asic_vid_sprite_attr(&rig.asic, 3, &sx, &sy, &mx, &my);
  EXPECT_EQ(sx & 0x200, 0x200) << "sprite X bit 9 survives (10-bit)";

  // --- Palette (&6400-&643F): the ASIC's 12-bit entry, two bytes per colour.
  // Nibble order per asic-device.md §"&6400-&643F": EVEN byte = RRRR_BBBB,
  // ODD byte = 0000_GGGG, packed as 0x0RGB. Asserted against literal nibbles
  // (independent ground truth), not by re-deriving the packer. ---
  cpu_write(rig, 0x6400 + 5 * 2 + 0, 0xC3);  // R=0xC (hi nibble), B=0x3 (lo)
  cpu_write(rig, 0x6400 + 5 * 2 + 1, 0x09);  // G=0x9
  const uint16_t p5 = asic_vid_palette(&rig.asic, 5);
  EXPECT_EQ(p5, 0x0C93) << "packed 0x0RGB";
  EXPECT_EQ((p5 >> 8) & 0x0F, 0xC) << "R = high nibble of the even byte";
  EXPECT_EQ((p5 >> 4) & 0x0F, 0x9) << "G = low nibble of the odd byte";
  EXPECT_EQ(p5 & 0x0F, 0x3) << "B = low nibble of the even byte";
  // A second, fully asymmetric entry so R and B cannot be silently swapped:
  cpu_write(rig, 0x6400 + 6 * 2 + 0, 0x12);  // R=0x1, B=0x2
  cpu_write(rig, 0x6400 + 6 * 2 + 1, 0x03);  // G=0x3
  EXPECT_EQ(asic_vid_palette(&rig.asic, 6), 0x0132)
      << "R/G/B nibble order is fixed (0x0RGB), not 0x0BGR";

  // --- DMA (&6C00-&6C0F): source word-aligned, prescaler, enable. ---
  cpu_write(rig, 0x6C04 + 0, 0x03);  // ch1 source low (bit 0 dropped -> 0x02)
  cpu_write(rig, 0x6C04 + 1, 0x40);  // ch1 source high
  cpu_write(rig, 0x6C04 + 2, 0x2A);  // ch1 prescaler
  cpu_write(rig, 0x6C0F, 0x02);      // DCSR: enable channel 1
  uint16_t src = 0;
  uint8_t pre = 0, en = 0;
  asic_dma_regs(&rig.asic, 1, &src, &pre, &en);
  EXPECT_EQ(src, 0x4002) << "source is word-aligned (low bit ignored)";
  EXPECT_EQ(pre, 0x2A);
  EXPECT_EQ(en, 1);
  asic_dma_regs(&rig.asic, 0, &src, nullptr, &en);
  EXPECT_EQ(en, 0) << "channel 0 was not enabled";
}

// Regression (Burnin' Rubber title derail): the &4000-&7FFF register page is
// overlaid only when the ASIC is BOTH unlocked AND paged in by RMR2 (its membank
// field == 3). The unlock knock alone must NOT map it. Games unlock the ASIC to
// use splits/palette but page the register page back OUT (RMR2 membank != 3, a
// low-ROM bank / RAM there) to bulk-copy data through &6xxx into RAM. If the
// overlay stayed live on unlock alone, that copy would scribble the ASIC DMA
// control (&6C0F) — spuriously enabling DMA channels whose INT flags then add
// extra raster interrupts, corrupting the title's per-frame interrupt script and
// derailing the game back to the cartridge menu after ~2 s.
TEST(Asic, RegisterPageNeedsRmr2MapNotJustUnlock) {
  AsicRig rig;
  make_rig(rig);
  send_unlock(rig);  // the knock only ENABLES RMR2; it does not map the page
  AsicRegs r{};
  asic_peek(&rig.asic, &r);
  EXPECT_EQ(r.locked, 0) << "the knock unlocked the ASIC";
  EXPECT_EQ(r.page_on, 0) << "but the register page is not yet mapped in";

  // Unlocked-but-unmapped: a write to the DMA control address must fall through
  // to RAM and leave the ASIC DMA registers untouched (the copy-through case).
  cpu_write(rig, 0x6C0F, 0x07);  // would enable all 3 channels IF it hit the page
  uint8_t en0 = 1, en1 = 1, en2 = 1;
  asic_dma_regs(&rig.asic, 0, nullptr, nullptr, &en0);
  asic_dma_regs(&rig.asic, 1, nullptr, nullptr, &en1);
  asic_dma_regs(&rig.asic, 2, nullptr, nullptr, &en2);
  EXPECT_EQ(en0, 0) << "channel 0 must NOT be enabled by an unmapped-page write";
  EXPECT_EQ(en1, 0);
  EXPECT_EQ(en2, 0);
  EXPECT_EQ(cpu_read(rig, 0x6C0F), 0x07) << "the write fell through to RAM";
  EXPECT_EQ(asic_page_peek(&rig.asic, 0x2C0F), 0x00)
      << "the ASIC register page did not record the write";

  // RMR2-map the page in (membank 3): the SAME write now reaches the ASIC.
  map_page(rig, true);
  asic_peek(&rig.asic, &r);
  EXPECT_EQ(r.page_on, 1) << "RMR2 membank 3 pages the register page in";
  cpu_write(rig, 0x6C0F, 0x07);
  asic_dma_regs(&rig.asic, 0, nullptr, nullptr, &en0);
  asic_dma_regs(&rig.asic, 2, nullptr, nullptr, &en2);
  EXPECT_EQ(en0, 1) << "paged in, the write reaches the ASIC DMA control";
  EXPECT_EQ(en2, 1);

  // Page it back OUT (membank != 3): the overlay is gone, RAM shows through.
  map_page(rig, false);
  asic_peek(&rig.asic, &r);
  EXPECT_EQ(r.page_on, 0) << "RMR2 membank != 3 pages the register page out";
  cpu_write(rig, 0x4000, 0x99);
  EXPECT_EQ(cpu_read(rig, 0x4000), 0x99)
      << "with the page out, &4000 is ordinary RAM again";
}

// Regression (Burnin' Rubber race-start derail): the 6128+ RMR2 register has
// TWO fields — bits 0-2 select the cartridge PAGE, bits 4-3 (membank) select the
// 16K CPU SLOT the page maps into: 0=&0000, 1=&4000, 2=&8000 (3=register page).
// The legacy Gate Array applies the low ROM at that slot, NOT always at &0000
// (kon_cpc_ja.cpp ga_memory_manager: memory_set_read_bank(lower_ROM_bank, ...)).
// Burnin' Rubber's RAM-LAM restart (RST 20h) does OUT 0xB6 (membank 2, page 6)
// to park the cartridge at &8000, then reads a RET (0xC9) from RAM at &004E; if
// the low ROM is forced to &0000 the CPU instead fetches cartridge data there
// and runs off into a data table — a hard crash on race start.
TEST(Asic, Rmr2MembankSelectsLowRomSlot) {
  AsicRig rig;
  rig.mem = mem_init(rig.mmem.data());
  rig.asic = asic_init(rig.amem.data());
  board_init(&rig.board);
  board_add(&rig.board, rig.mem);
  board_add(&rig.board, rig.asic);
  board_reset(&rig.board);
  asic_set_plugged(&rig.asic, 1);
  mem_attach_asic(&rig.mem, &rig.asic);  // wire the RMR2 gate

  // Synthetic 8-bank cartridge: bank N is filled with the marker 0x10+N.
  std::vector<uint8_t> cart(static_cast<size_t>(8) * 0x4000);
  for (size_t i = 0; i < cart.size(); ++i)
    cart[i] = static_cast<uint8_t>(0x10 + (i / 0x4000));
  mem_load_cartridge(&rig.mem, cart.data(), cart.size());

  send_unlock(rig);  // the knock enables RMR2 (lower ROM stays enabled)

  // membank 0 (0xA6): cartridge page 6 lands at the classic low-ROM slot &0000.
  rmr2_write(rig, 0xA6);
  EXPECT_EQ(cpu_read(rig, 0x0000), 0x16)
      << "RMR2 membank 0 maps cartridge page 6 at &0000-&3FFF";

  // membank 2 (0xB6): the SAME page 6 must move to &8000-&BFFF, and &0000-&3FFF
  // must fall back to RAM — NOT keep showing the cartridge page.
  rmr2_write(rig, 0xB6);
  EXPECT_EQ(cpu_read(rig, 0x8000), 0x16)
      << "RMR2 membank 2 maps the low-ROM page at &8000-&BFFF";
  EXPECT_NE(cpu_read(rig, 0x0000), 0x16)
      << "membank 2 leaves &0000-&3FFF as RAM, not the cartridge page";
}

// A classic Gate Array port write (I/O A15=0/A14=1) — pen select or ink set.
void ga_port_write(AsicRig& rig, uint8_t val) {
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.iorq = true;
  rig.board.bus.cpu.wr = true;
  rig.board.bus.cpu.addr = 0x7F00;
  rig.board.bus.cpu.data = val;
  board_tick(&rig.board);
  idle(rig);
}

// On a Plus the classic Gate Array ink port writes the SAME palette RAM the
// &6400 12-bit registers do — one store, last-writer-wins, lock-independent.
// The regression this pins: Burnin' Rubber's exit back to the cartridge menu
// left the screen black, because the menu repaints via classic inks and the
// old split-palette model ignored them once an entry had a 12-bit write
// (pal_set persists across reset — the palette RAM is soldered-in).
TEST(Asic, ClassicInkWritesShareTheOnePaletteRam) {
  AsicRig rig;
  make_rig(rig);

  // While LOCKED (compatibility mode): pen 1 ← hardware colour 11 (white).
  ga_port_write(rig, 0x01);       // function 0: select pen 1
  ga_port_write(rig, 0x40 | 11);  // function 1: ink = colour 11
  EXPECT_EQ(asic_vid_palette(&rig.asic, 1), 0x0FFF)
      << "classic white lands in the palette RAM as 12-bit F,F,F";
  EXPECT_EQ(asic_vid_palette_set(&rig.asic, 1), 1);

  // Unlocked + paged in: a 12-bit write takes the same entry (last writer
  // wins)...
  unlock_and_map(rig);
  cpu_write(rig, 0x6402, 0x08);  // entry 1 even byte: R=0, B=8
  cpu_write(rig, 0x6403, 0x00);  // entry 1 odd byte:  G=0
  EXPECT_EQ(asic_vid_palette(&rig.asic, 1), 0x0008) << "12-bit blue took over";

  // ...and a later classic ink write takes it back — still one RAM.
  ga_port_write(rig, 0x01);       // pen 1
  ga_port_write(rig, 0x40 | 20);  // ink = colour 20 (black)
  EXPECT_EQ(asic_vid_palette(&rig.asic, 1), 0x0000)
      << "classic write wins as the last writer, even while unlocked";

  // The border select (bit 4) routes to entry 16.
  ga_port_write(rig, 0x10);       // function 0: border
  ga_port_write(rig, 0x40 | 10);  // ink = colour 10 (bright yellow)
  EXPECT_EQ(asic_vid_palette(&rig.asic, 16), 0x0FF0)
      << "border ink lands in palette entry 16";
}

#include "subcycle/machine.h"

TEST(Asic, OnTheMachineGatedByModel3) {
  std::vector<uint8_t> rom(0x8000, 0);
  subcycle::Machine m;
  ASSERT_TRUE(m.build(rom.data(), rom.size()));
  AsicRegs r{};
  asic_peek(m.asic(), &r);
  EXPECT_EQ(r.plugged, 0) << "default board: the ASIC is dormant";
  EXPECT_EQ(r.locked, 1);

  m.set_asic(true);  // model 3
  asic_peek(m.asic(), &r);
  EXPECT_EQ(r.plugged, 1) << "6128+ mode: the ASIC is on the board";
  m.set_asic(false);
  asic_peek(m.asic(), &r);
  EXPECT_EQ(r.plugged, 0);
}
