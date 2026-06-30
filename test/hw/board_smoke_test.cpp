/* board_smoke_test.cpp — proves the device/bus/board foundation (docs/hw-spec.md)
 * with trivial devices, before any real chip exists. A gtest TU (no main(); CI
 * runs it via test_runner).
 *
 * Covers the load-bearing properties of the model: two-phase reads/writes, the
 * 16 MHz clock generator dividing to 4 MHz, ORDER-INDEPENDENCE of the device
 * list, and versioned save/load. It does NOT yet cover WAIT stall or multi-driver
 * conflict — those arrive with the Gate Array.
 */

#include <cstdint>
#include <cstring>
#include <memory>

#include <gtest/gtest.h>

#include "hw/board.h"

namespace {

/* ---- trivial RAM device (two-phase): reads `in`, drives `out` ---- */
struct Ram {
  uint8_t cells[0x10000];
};
void ram_tick(void* self, const Bus* in, Bus* out) {
  Ram* ram = static_cast<Ram*>(self);
  if (in->cpu.mreq && in->cpu.wr) {
    ram->cells[in->cpu.addr] = in->cpu.data;       // latch the write
  } else if (in->cpu.mreq && in->cpu.rd) {
    out->cpu.data = ram->cells[in->cpu.addr];      // drive the data bus
  }
}
void ram_reset(void* self) { std::memset(static_cast<Ram*>(self)->cells, 0, sizeof(Ram::cells)); }
size_t ram_state_size(const void*) { return sizeof(Ram); }
void ram_save(const void* self, void* buf) { std::memcpy(buf, self, sizeof(Ram)); }
void ram_load(void* self, const void* buf) { std::memcpy(self, buf, sizeof(Ram)); }
Device ram_device(Ram* storage) {
  return Device{storage, "ram", ram_tick, ram_reset, ram_state_size, ram_save, ram_load};
}

/* ---- clock generator stub: divides the 16 MHz master, drives clk ---- */
struct ClockGen {
  uint8_t counter;
};
void clockgen_tick(void* self, const Bus* /*in*/, Bus* out) {
  ClockGen* cg = static_cast<ClockGen*>(self);
  const uint8_t c = static_cast<uint8_t>(cg->counter & 0x0F);
  out->clk.phase = c;
  out->clk.cpu = ((c & 0x03) == 0);   // 4 MHz: every 4th master cycle
  out->clk.crtc = (c == 0);           // 1 MHz
  out->clk.psg = (c == 0);
  cg->counter = static_cast<uint8_t>(cg->counter + 1);
}
void clockgen_reset(void* self) { static_cast<ClockGen*>(self)->counter = 0; }
size_t clockgen_state_size(const void*) { return 2; }  // version + counter
void clockgen_save(const void* self, void* buf) {
  uint8_t* b = static_cast<uint8_t*>(buf);
  b[0] = 1;  // format version — logical state only (spec §5)
  b[1] = static_cast<const ClockGen*>(self)->counter;
}
void clockgen_load(void* self, const void* buf) {
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  static_cast<ClockGen*>(self)->counter = (b[0] == 1) ? b[1] : 0;
}
Device clockgen_device(ClockGen* storage) {
  return Device{storage,            "clock-gen",   clockgen_tick, clockgen_reset,
                clockgen_state_size, clockgen_save, clockgen_load};
}

/* Drive a one-cycle memory transaction by seeding the committed bus, then tick. */
void seed_cpu(Board* board, const CpuBus& cpu) {
  board->bus = bus_resting();
  board->bus.cpu = cpu;
}

}  // namespace

TEST(BoardFoundation, MemoryReadWrite) {
  auto ram = std::make_unique<Ram>();
  Board board;
  board_init(&board);
  board_add(&board, ram_device(ram.get()));
  board_reset(&board);

  CpuBus w{};
  w.addr = 0x1234;
  w.data = 0xAB;
  w.mreq = true;
  w.wr = true;
  seed_cpu(&board, w);
  board_tick(&board);
  EXPECT_EQ(ram->cells[0x1234], 0xAB) << "RAM latched a write transaction";

  CpuBus r{};
  r.addr = 0x1234;
  r.mreq = true;
  r.rd = true;
  seed_cpu(&board, r);
  board_tick(&board);
  EXPECT_EQ(board.bus.cpu.data, 0xAB) << "RAM drove the data bus on read";

  // An I/O read that no device claims (RAM answers mreq only) leaves the data
  // bus floating high.
  CpuBus io{};
  io.addr = 0x7F00;
  io.iorq = true;
  io.rd = true;
  seed_cpu(&board, io);
  board_tick(&board);
  EXPECT_EQ(board.bus.cpu.data, 0xFF) << "unclaimed bus floats to 0xFF";
}

TEST(BoardFoundation, OrderIndependence) {
  // Same devices, opposite board order, must give identical results.
  auto ram_a = std::make_unique<Ram>();
  auto cg_a = std::make_unique<ClockGen>();
  auto ram_b = std::make_unique<Ram>();
  auto cg_b = std::make_unique<ClockGen>();

  Board a;
  board_init(&a);
  board_add(&a, ram_device(ram_a.get()));
  board_add(&a, clockgen_device(cg_a.get()));
  board_reset(&a);

  Board b;
  board_init(&b);
  board_add(&b, clockgen_device(cg_b.get()));  // reversed
  board_add(&b, ram_device(ram_b.get()));
  board_reset(&b);

  ram_a->cells[0x2000] = 0x5A;
  ram_b->cells[0x2000] = 0x5A;
  CpuBus r{};
  r.addr = 0x2000;
  r.mreq = true;
  r.rd = true;

  seed_cpu(&a, r);
  board_tick(&a);
  seed_cpu(&b, r);
  board_tick(&b);

  EXPECT_EQ(a.bus.cpu.data, b.bus.cpu.data) << "data bus independent of device order";
  EXPECT_EQ(a.bus.clk.phase, b.bus.clk.phase) << "clock independent of device order";
}

TEST(BoardFoundation, ClockDividesMasterTo4MHz) {
  auto cg = std::make_unique<ClockGen>();
  Board board;
  board_init(&board);
  board_add(&board, clockgen_device(cg.get()));
  board_reset(&board);

  int cpu_enables = 0;
  for (int i = 0; i < 16; ++i) {
    board_tick(&board);
    if (board.bus.clk.cpu) ++cpu_enables;
  }
  EXPECT_EQ(cpu_enables, 4) << "CPU enable fires every 4th master cycle (16 MHz / 4 = 4 MHz)";
  EXPECT_EQ(board.master_cycles, 16u);
}

TEST(BoardFoundation, VersionedSaveLoad) {
  auto cg = std::make_unique<ClockGen>();
  cg->counter = 7;
  uint8_t buf[2];
  clockgen_save(cg.get(), buf);
  EXPECT_EQ(buf[0], 1) << "blob starts with a format version";
  cg->counter = 99;
  clockgen_load(cg.get(), buf);
  EXPECT_EQ(cg->counter, 7) << "save/load round-trips logical state";
}
