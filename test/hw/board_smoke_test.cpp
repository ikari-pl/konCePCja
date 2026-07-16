/* board_smoke_test.cpp — proves the device/bus/board foundation
 * (docs/hw-spec.md) with trivial devices, before any real chip exists. A gtest
 * TU (no main(); CI runs it via test_runner).
 *
 * Covers the load-bearing properties of the model: two-phase reads/writes, the
 * 16 MHz clock generator dividing to 4 MHz, ORDER-INDEPENDENCE of the device
 * list, and versioned save/load. It does NOT yet cover WAIT stall or
 * multi-driver conflict — those arrive with the Gate Array.
 */

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>

#include "hw/board.h"

namespace {

/* ---- trivial RAM device (two-phase): reads `in`, drives `out` ---- */
struct Ram {
  uint8_t cells[0x10000];
};
void ram_tick(void* self, const Bus* in, Bus* out) {
  Ram* ram = static_cast<Ram*>(self);
  if (in->cpu.mreq && in->cpu.wr) {
    ram->cells[in->cpu.addr] = in->cpu.data;  // latch the write
  } else if (in->cpu.mreq && in->cpu.rd) {
    out->cpu.data = ram->cells[in->cpu.addr];  // drive the data bus
  }
}
void ram_reset(void* self) {
  std::memset(static_cast<Ram*>(self)->cells, 0, sizeof(Ram::cells));
}
size_t ram_state_size(const void*) { return sizeof(Ram); }
void ram_save(const void* self, void* buf) {
  std::memcpy(buf, self, sizeof(Ram));
}
void ram_load(void* self, const void* buf) {
  std::memcpy(self, buf, sizeof(Ram));
}
Device ram_device(Ram* storage) {
  return Device{storage,        "ram",    ram_tick, ram_reset,
                ram_state_size, ram_save, ram_load};
}

/* ---- clock generator stub: divides the 16 MHz master, drives clk ---- */
struct ClockGen {
  uint8_t counter;
};
void clockgen_tick(void* self, const Bus* /*in*/, Bus* out) {
  ClockGen* cg = static_cast<ClockGen*>(self);
  const uint8_t c = static_cast<uint8_t>(cg->counter & 0x0F);
  out->clk.phase = c;
  out->clk.cpu = ((c & 0x03) == 0);  // 4 MHz: every 4th master cycle
  out->clk.crtc = (c == 0);          // 1 MHz
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
  return Device{storage,        "clock-gen",         clockgen_tick,
                clockgen_reset, clockgen_state_size, clockgen_save,
                clockgen_load};
}

/* ---- producer/consumer pair that SHARE a line (cpu.nmi), to prove two-phase
 * ---- The producer drives nmi each cycle; the consumer records what it sees on
 * `in`. Because every device reads the committed bus, the consumer must observe
 * the PREVIOUS cycle's value, never the producer's same-cycle drive —
 * regardless of board order. A naive single ordered pass would fail this. */
struct Producer {
  bool level;
};
void producer_tick(void* self, const Bus* /*in*/, Bus* out) {
  Producer* p = static_cast<Producer*>(self);
  out->cpu.nmi = p->level;
  p->level = !p->level;  // toggle every master cycle
}
void producer_reset(void* self) { static_cast<Producer*>(self)->level = true; }
size_t producer_state_size(const void*) { return 1; }
void producer_save(const void* self, void* buf) {
  *static_cast<uint8_t*>(buf) =
      static_cast<const Producer*>(self)->level ? 1 : 0;
}
void producer_load(void* self, const void* buf) {
  static_cast<Producer*>(self)->level = *static_cast<const uint8_t*>(buf) != 0;
}
Device producer_device(Producer* storage) {
  return Device{storage,        "producer",          producer_tick,
                producer_reset, producer_state_size, producer_save,
                producer_load};
}

struct Consumer {
  uint8_t last;  // last value of cpu.nmi seen on `in`
};
void consumer_tick(void* self, const Bus* in, Bus* /*out*/) {
  static_cast<Consumer*>(self)->last = in->cpu.nmi ? 1 : 0;
}
void consumer_reset(void* self) { static_cast<Consumer*>(self)->last = 0; }
size_t consumer_state_size(const void*) { return 1; }
void consumer_save(const void* self, void* buf) {
  *static_cast<uint8_t*>(buf) = static_cast<const Consumer*>(self)->last;
}
void consumer_load(void* self, const void* buf) {
  static_cast<Consumer*>(self)->last = *static_cast<const uint8_t*>(buf);
}
Device consumer_device(Consumer* storage) {
  return Device{storage,        "consumer",          consumer_tick,
                consumer_reset, consumer_state_size, consumer_save,
                consumer_load};
}

/* ---- an interrupt source that OR-asserts cpu.irq when armed ---- */
struct IrqSource {
  bool armed;
};
void irqsource_tick(void* self, const Bus* /*in*/, Bus* out) {
  if (static_cast<IrqSource*>(self)->armed) out->cpu.irq |= true;  // WIRED-OR
}
void irqsource_reset(void* self) {
  static_cast<IrqSource*>(self)->armed = false;
}
size_t irqsource_state_size(const void*) { return 1; }
void irqsource_save(const void* self, void* buf) {
  *static_cast<uint8_t*>(buf) =
      static_cast<const IrqSource*>(self)->armed ? 1 : 0;
}
void irqsource_load(void* self, const void* buf) {
  static_cast<IrqSource*>(self)->armed = *static_cast<const uint8_t*>(buf) != 0;
}
Device irqsource_device(IrqSource* storage) {
  return Device{storage,
                "irq-src",
                irqsource_tick,
                irqsource_reset,
                irqsource_state_size,
                irqsource_save,
                irqsource_load};
}

/* Drive a one-cycle memory transaction by seeding the committed bus, then tick.
 */
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

  // ASSIGN, not OR: a cell holding 0x00 must read 0x00, not 0x00|0xFF.
  ram->cells[0x1234] = 0x00;
  seed_cpu(&board, r);
  board_tick(&board);
  EXPECT_EQ(board.bus.cpu.data, 0x00)
      << "data bus is assigned, not OR-ed with the pull-up";

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

TEST(BoardFoundation, TwoPhaseReadsCommittedNotSameCycle) {
  // The real test of two-phase commit: a producer and consumer SHARE cpu.nmi.
  // The consumer must see the producer's PREVIOUS-cycle drive, never the same
  // cycle's — and that must hold regardless of board order. A single ordered
  // pass would make the consumer's result depend on order.
  auto build = [](bool consumer_first, std::array<uint8_t, 4>& seen) {
    auto prod = std::make_unique<Producer>();
    auto cons = std::make_unique<Consumer>();
    Board board;
    board_init(&board);
    if (consumer_first) {
      board_add(&board, consumer_device(cons.get()));
      board_add(&board, producer_device(prod.get()));
    } else {
      board_add(&board, producer_device(prod.get()));
      board_add(&board, consumer_device(cons.get()));
    }
    board_reset(&board);  // producer level=true, consumer last=0
    for (int i = 0; i < 4; ++i) {
      board_tick(&board);
      seen[static_cast<size_t>(i)] = cons->last;
    }
  };

  std::array<uint8_t, 4> a{}, b{};
  build(false, a);  // producer first
  build(true, b);   // consumer first

  // Cycle 0: consumer reads the resting bus (nmi=false), NOT the producer's
  // cycle-0 drive (true) — proving it read `in`, not `out`. Then it lags by
  // one: producer toggles true,false,true,false → consumer sees 0,1,0,1.
  EXPECT_EQ(a[0], 0) << "consumer did not see the producer's same-cycle drive";
  EXPECT_EQ((std::array<uint8_t, 4>{0, 1, 0, 1}), a)
      << "consumer lags the producer by one cycle";
  EXPECT_EQ(a, b) << "result is independent of device order";
}

TEST(BoardFoundation, WiredOrInterrupt) {
  auto src1 = std::make_unique<IrqSource>();
  auto src2 = std::make_unique<IrqSource>();
  Board board;
  board_init(&board);
  board_add(&board, irqsource_device(src1.get()));
  board_add(&board, irqsource_device(src2.get()));
  board_reset(&board);

  board_tick(&board);
  EXPECT_FALSE(board.bus.cpu.irq)
      << "irq rests deasserted when no source is armed";

  src2->armed = true;
  board_tick(&board);
  EXPECT_TRUE(board.bus.cpu.irq) << "one armed source OR-asserts irq";

  src1->armed = true;
  board_tick(&board);
  EXPECT_TRUE(board.bus.cpu.irq) << "two armed sources still assert (wired-OR)";

  src1->armed = false;
  src2->armed = false;
  board_tick(&board);
  EXPECT_FALSE(board.bus.cpu.irq)
      << "irq clears the cycle after all sources disarm";
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
  EXPECT_EQ(cpu_enables, 4)
      << "CPU enable fires every 4th master cycle (16 MHz / 4 = 4 MHz)";
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
