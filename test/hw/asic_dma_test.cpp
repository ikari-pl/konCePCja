/* asic_dma_test.cpp — the Plus ASIC DMA sound sequencer (increment C-1). The
 * ASIC drives the bus as a master: raises BUSRQ, takes BUSAK, reads the DMA
 * list from RAM over CpuBus, and writes the PSG over the AY bus as the second
 * master, once per scanline off the HSYNC leading edge. See
 * docs/hardware/asic-device.md §4. (The real Z80's BUSRQ/BUSAK arbitration is
 * proven separately by Z80Busrq.GrantsBusakFreezesThenResumes; here a minimal
 * grant arbiter stands in so the DMA engine can be exercised in isolation.) */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "hw/asic.h"
#include "hw/board.h"
#include "hw/psg.h"

namespace {

// Flat 64K RAM answering CpuBus reads/writes (banking is the memory Device's
// job; a flat RAM suffices to prove the DMA drives the address and reads back).
struct Ram {
  uint8_t cells[0x10000];
};
void ram_tick(void* self, const Bus* in, Bus* out) {
  Ram* r = static_cast<Ram*>(self);
  if (in->cpu.mreq && in->cpu.wr)
    r->cells[in->cpu.addr] = in->cpu.data;
  else if (in->cpu.mreq && in->cpu.rd)
    out->cpu.data = r->cells[in->cpu.addr];
}
void noreset(void*) {}
size_t ram_size(const void*) { return sizeof(Ram); }
void ram_save(const void* s, void* b) { std::memcpy(b, s, sizeof(Ram)); }
void ram_load(void* s, const void* b) { std::memcpy(s, b, sizeof(Ram)); }
Device ram_device(Ram* s) {
  return Device{s, "ram", ram_tick, noreset, ram_size, ram_save, ram_load};
}

// Minimal bus arbiter: grant BUSAK whenever BUSRQ is asserted.
void grant_tick(void*, const Bus* in, Bus* out) {
  out->cpu.busak = in->cpu.busrq;
}
size_t one_size(const void*) { return 1; }
void one_save(const void*, void* b) { *static_cast<uint8_t*>(b) = 0; }
void one_load(void*, const void*) {}
Device grant_device() {
  static uint8_t d = 0;
  return Device{&d, "grant", grant_tick, noreset, one_size, one_save, one_load};
}

// HSYNC generator: a rising edge every `period` master cycles (a "scanline").
struct Hsync {
  int t;
  int period;
  int lines_per_frame = 0;  // 0 = no wrap (frame_line grows monotonically)
};
void hsync_tick(void* self, const Bus*, Bus* out) {
  Hsync* h = static_cast<Hsync*>(self);
  out->vid.hsync = (h->t % h->period) < 4;
  // Drive the frame scanline too, as the real CRTC does — the ASIC's PRI counts
  // off VidBus.frame_line (one per scanline from frame top), not raw HSYNCs.
  const int lpf = h->lines_per_frame > 0 ? h->lines_per_frame : 1000000;
  out->vid.frame_line = static_cast<uint16_t>((h->t / h->period) % lpf);
  h->t++;
}
size_t hsync_size(const void*) { return sizeof(Hsync); }
void hsync_save(const void* s, void* b) { std::memcpy(b, s, sizeof(Hsync)); }
void hsync_load(void* s, const void* b) { std::memcpy(s, b, sizeof(Hsync)); }
Device hsync_device(Hsync* s) {
  return Device{s,          "hsync",    hsync_tick, noreset,
                hsync_size, hsync_save, hsync_load};
}

// Program the ASIC register page over the bus (unlock knock + memory writes).
void knock(Board* bd, uint8_t val) {
  bd->bus = bus_resting();
  bd->bus.cpu.iorq = true;
  bd->bus.cpu.wr = true;
  bd->bus.cpu.addr = 0xBC00;
  bd->bus.cpu.data = val;
  board_tick(bd);
  bd->bus = bus_resting();
  board_tick(bd);
  bd->bus = bus_resting();
  board_tick(bd);
}
void unlock(Board* bd) {
  const uint8_t seq[17] = {0xFF, 0x00, 0xFF, 0x77, 0xB3, 0x51, 0xA8, 0xD4, 0x62,
                           0x39, 0x9C, 0x46, 0x2B, 0x15, 0x8A, 0xCD, 0x01};
  for (uint8_t b : seq) knock(bd, b);
  // RMR2: page the register page into &4000-&7FFF (Gate-Array fn-2, port &7Fxx,
  // data 10 11 1000 = membank field 3). The knock only enables RMR2; the CPU
  // still has to page the register page in before pgwrite() reaches the ASIC.
  bd->bus = bus_resting();
  bd->bus.cpu.iorq = true;
  bd->bus.cpu.wr = true;
  bd->bus.cpu.addr = 0x7F00;
  bd->bus.cpu.data = 0xB8;
  board_tick(bd);
  bd->bus = bus_resting();
  board_tick(bd);
}
void pgwrite(Board* bd, uint16_t addr, uint8_t val) {
  bd->bus = bus_resting();
  bd->bus.cpu.mreq = true;
  bd->bus.cpu.wr = true;
  bd->bus.cpu.addr = addr;
  bd->bus.cpu.data = val;
  board_tick(bd);
  bd->bus = bus_resting();
  board_tick(bd);
}
// Read a register-page byte back over the bus (mreq+rd; the mapped page drives
// cpu.data).
uint8_t pgread(Board* bd, uint16_t addr) {
  bd->bus = bus_resting();
  bd->bus.cpu.mreq = true;
  bd->bus.cpu.rd = true;
  bd->bus.cpu.addr = addr;
  board_tick(bd);
  return bd->bus.cpu.data;
}

}  // namespace

TEST(AsicDma, FetchesListWritesPsgAdvancesSource) {
  Ram ram;
  std::memset(ram.cells, 0, sizeof(ram.cells));
  // DMA list at 0x8000: LOAD reg 8 = 0x0F (0x080F), then STOP (0x4020).
  ram.cells[0x8000] = 0x0F;
  ram.cells[0x8001] = 0x08;
  ram.cells[0x8002] = 0x20;
  ram.cells[0x8003] = 0x40;

  std::vector<uint8_t> amem(asic_state_size());
  Device adev = asic_init(amem.data());
  std::vector<uint8_t> pmem(psg_state_size());
  Device pdev = psg_init(pmem.data());
  Hsync hs{0, 64};

  Board board;
  board_init(&board);
  board_add(&board, ram_device(&ram));
  board_add(&board, pdev);
  board_add(&board, adev);
  board_add(&board, grant_device());
  board_add(&board, hsync_device(&hs));
  board_reset(&board);
  asic_set_plugged(&adev, 1);

  unlock(&board);
  pgwrite(&board, 0x6C00, 0x00);  // channel 0 source low
  pgwrite(&board, 0x6C01, 0x80);  // channel 0 source high → 0x8000
  pgwrite(&board, 0x6C0F, 0x01);  // enable channel 0

  bool saw_busak = false;
  for (int i = 0; i < 300; ++i) {  // ~4.5 scanlines: LOAD then STOP
    board_tick(&board);
    if (board.bus.cpu.busak) saw_busak = true;
  }

  PsgRegs pr{};
  psg_peek(&pdev, &pr);
  EXPECT_TRUE(saw_busak) << "the ASIC took the bus (BUSRQ → BUSAK)";
  EXPECT_EQ(pr.reg[8], 0x0F) << "LOAD wrote 0x0F to PSG register 8";

  uint16_t src = 0;
  uint8_t pre = 0, en = 0;
  asic_dma_regs(&adev, 0, &src, &pre, &en);
  EXPECT_EQ(src, 0x8004) << "source advanced past both instructions";
  EXPECT_EQ(en, 0) << "STOP disabled the channel";
}

// PAUSE holds the channel for (N × prescaler+1) scanlines before the next
// fetch.
TEST(AsicDma, PauseDelaysTheNextFetch) {
  Ram ram;
  std::memset(ram.cells, 0, sizeof(ram.cells));
  // list: PAUSE 2 (0x1002), then LOAD reg 8 = 0x0F (0x080F).
  ram.cells[0x8000] = 0x02;
  ram.cells[0x8001] = 0x10;
  ram.cells[0x8002] = 0x0F;
  ram.cells[0x8003] = 0x08;

  std::vector<uint8_t> amem(asic_state_size());
  Device adev = asic_init(amem.data());
  std::vector<uint8_t> pmem(psg_state_size());
  Device pdev = psg_init(pmem.data());
  Hsync hs{0, 64};

  Board board;
  board_init(&board);
  board_add(&board, ram_device(&ram));
  board_add(&board, pdev);
  board_add(&board, adev);
  board_add(&board, grant_device());
  board_add(&board, hsync_device(&hs));
  board_reset(&board);
  asic_set_plugged(&adev, 1);

  unlock(&board);
  pgwrite(&board, 0x6C00, 0x00);
  pgwrite(&board, 0x6C01, 0x80);
  pgwrite(&board, 0x6C02, 0x00);  // prescaler 0 → 1 scanline per pause tick
  pgwrite(&board, 0x6C0F, 0x01);

  // One scanline: fetch PAUSE 2 (no PSG write yet).
  for (int i = 0; i < 70; ++i) board_tick(&board);
  PsgRegs pr{};
  psg_peek(&pdev, &pr);
  EXPECT_EQ(pr.reg[8], 0x00) << "PAUSE fetched; LOAD not yet reached";

  // Run enough scanlines for the pause to expire and the LOAD to fire.
  for (int i = 0; i < 300; ++i) board_tick(&board);
  psg_peek(&pdev, &pr);
  EXPECT_EQ(pr.reg[8], 0x0F) << "after the pause, LOAD wrote register 8";
}

// Drive an interrupt-acknowledge cycle (m1 + iorq) and return the byte the ASIC
// put on the data bus (the IM2 vector).
uint8_t ack(Board* bd) {
  bd->bus = bus_resting();
  bd->bus.cpu.m1 = true;
  bd->bus.cpu.iorq = true;
  board_tick(bd);
  return bd->bus.cpu.data;
}

// Increment D: a DMA INT opcode raises cpu.irq; the ack drives the IM2 vector
// and clears the source.
TEST(AsicDma, IntOpcodeRaisesIrqAndVectorsOnAck) {
  Ram ram;
  std::memset(ram.cells, 0, sizeof(ram.cells));
  ram.cells[0x8000] = 0x10;  // INT (0x4010)
  ram.cells[0x8001] = 0x40;

  std::vector<uint8_t> amem(asic_state_size());
  Device adev = asic_init(amem.data());
  std::vector<uint8_t> pmem(psg_state_size());
  Device pdev = psg_init(pmem.data());
  Hsync hs{0, 64};

  Board board;
  board_init(&board);
  board_add(&board, ram_device(&ram));
  board_add(&board, pdev);
  board_add(&board, adev);
  board_add(&board, grant_device());
  board_add(&board, hsync_device(&hs));
  board_reset(&board);
  asic_set_plugged(&adev, 1);

  unlock(&board);
  pgwrite(&board, 0x6805, 0x30);  // IM2 vector (& 0xF8)
  pgwrite(&board, 0x6C00, 0x00);
  pgwrite(&board, 0x6C01, 0x80);
  pgwrite(&board, 0x6C0F, 0x01);

  bool saw_irq = false;
  for (int i = 0; i < 300; ++i) {
    board_tick(&board);
    if (board.bus.cpu.irq) saw_irq = true;
  }
  EXPECT_TRUE(saw_irq) << "the DMA INT opcode raised cpu.irq";

  // DCSR (&6C0F) readback: channel 0 enable = bit 0, channel 0 INT flag = bit 6.
  const uint8_t dcsr = pgread(&board, 0x6C0F);
  EXPECT_EQ(dcsr & 0x01, 0x01) << "DCSR bit 0 = channel 0 enabled";
  EXPECT_EQ(dcsr & 0x40, 0x40) << "DCSR bit 6 = channel 0 interrupt flag (cpcwiki)";

  EXPECT_EQ(ack(&board), 0x34)
      << "IM2 vector = (0x30 & 0xF8) | 4: DMA channel 0's source code (cpcwiki "
         "ASIC — the low 3 bits identify the source, not just the raw vector)";
  for (int i = 0; i < 4; ++i) board_tick(&board);
  EXPECT_FALSE(board.bus.cpu.irq) << "the ack cleared the interrupt source";
}

// The programmable raster interrupt fires when the frame scanline reaches
// pri_line.
TEST(AsicDma, PriRaisesIrqAtScanline) {
  Ram ram;
  std::memset(ram.cells, 0, sizeof(ram.cells));

  std::vector<uint8_t> amem(asic_state_size());
  Device adev = asic_init(amem.data());
  Hsync hs{0, 64};

  Board board;
  board_init(&board);
  board_add(&board, ram_device(&ram));
  board_add(&board, adev);
  board_add(&board, grant_device());
  board_add(&board, hsync_device(&hs));
  board_reset(&board);
  asic_set_plugged(&adev, 1);

  unlock(&board);
  pgwrite(&board, 0x6800, 3);     // pri_line = 3
  pgwrite(&board, 0x6805, 0x28);  // IM2 vector

  bool saw_irq = false;
  for (int i = 0; i < 300; ++i) {
    board_tick(&board);
    if (board.bus.cpu.irq) saw_irq = true;
  }
  EXPECT_TRUE(saw_irq) << "PRI raised cpu.irq at pri_line";
  EXPECT_EQ(pgread(&board, 0x6C0F) & 0x80, 0x80)
      << "DCSR bit 7 = raster interrupt flag (cpcwiki)";
  EXPECT_EQ(ack(&board), 0x2E)
      << "IM2 vector = (0x28 & 0xF8) | 6: raster interrupt's source code — "
         "distinct from a DMA ack (which would be | 4/2/0)";
}

// PRI must fire exactly when frame_line reaches pri_line — not before.
TEST(AsicDma, PriFiresAtExactLineNotBefore) {
  Ram ram;
  std::memset(ram.cells, 0, sizeof(ram.cells));
  std::vector<uint8_t> amem(asic_state_size());
  Device adev = asic_init(amem.data());
  Hsync hs{0, 64};
  Board board;
  board_init(&board);
  board_add(&board, ram_device(&ram));
  board_add(&board, adev);
  board_add(&board, grant_device());
  board_add(&board, hsync_device(&hs));
  board_reset(&board);
  asic_set_plugged(&adev, 1);
  unlock(&board);
  pgwrite(&board, 0x6800, 5);  // pri_line = 5
  hs.t = 0;                    // clean frame counter after the unlock traffic

  bool early = false;
  for (int i = 0; i < 5 * 64; ++i) {  // scanlines 0..4
    board_tick(&board);
    if (board.bus.cpu.irq) early = true;
  }
  EXPECT_FALSE(early) << "PRI must not fire before frame_line reaches pri_line";
  bool fired = false;
  for (int i = 0; i < 64; ++i) {  // scanline 5
    board_tick(&board);
    if (board.bus.cpu.irq) fired = true;
  }
  EXPECT_TRUE(fired) << "PRI fires when frame_line == pri_line (5)";
}

// pri_line = 0 disables the ASIC PRI (the Gate Array's fixed 52-line interrupt
// owns the raster interrupt instead).
TEST(AsicDma, PriLineZeroDefersToGateArray) {
  Ram ram;
  std::memset(ram.cells, 0, sizeof(ram.cells));
  std::vector<uint8_t> amem(asic_state_size());
  Device adev = asic_init(amem.data());
  Hsync hs{0, 64};
  Board board;
  board_init(&board);
  board_add(&board, ram_device(&ram));
  board_add(&board, adev);
  board_add(&board, grant_device());
  board_add(&board, hsync_device(&hs));
  board_reset(&board);
  asic_set_plugged(&adev, 1);
  unlock(&board);  // pri_line stays 0 (never written)

  bool asic_pri = false;
  for (int i = 0; i < 500; ++i) {
    board_tick(&board);
    if (board.bus.cpu.irq) asic_pri = true;
  }
  EXPECT_FALSE(asic_pri) << "pri_line=0 → ASIC drives no PRI (defers to GA-52)";
}

// The PRI re-arms every frame: it fires once, and after the ack it fires again
// when the next frame's scanline reaches pri_line.
TEST(AsicDma, PriReArmsEachFrame) {
  Ram ram;
  std::memset(ram.cells, 0, sizeof(ram.cells));
  std::vector<uint8_t> amem(asic_state_size());
  Device adev = asic_init(amem.data());
  Hsync hs{0, 64, 8};  // 8 scanlines per frame
  Board board;
  board_init(&board);
  board_add(&board, ram_device(&ram));
  board_add(&board, adev);
  board_add(&board, grant_device());
  board_add(&board, hsync_device(&hs));
  board_reset(&board);
  asic_set_plugged(&adev, 1);
  unlock(&board);
  pgwrite(&board, 0x6800, 3);  // pri_line = 3
  hs.t = 0;

  bool frame1 = false;
  for (int i = 0; i < 8 * 64; ++i) {  // frame 1
    board_tick(&board);
    if (board.bus.cpu.irq) frame1 = true;
  }
  EXPECT_TRUE(frame1) << "PRI fired in frame 1";
  ack(&board);  // acknowledge → clears pri_pending

  bool frame2 = false;
  for (int i = 0; i < 8 * 64; ++i) {  // frame 2 — the scanline wraps to 3 again
    board_tick(&board);
    if (board.bus.cpu.irq) frame2 = true;
  }
  EXPECT_TRUE(frame2) << "PRI re-arms and fires again the next frame";
}
