/* board_smoke_test.cpp — proves the device/bus/board foundation end-to-end with
 * two trivial devices, before any real chip exists. A gtest translation unit (no
 * main(); test/main.cpp owns it) so it links into test_runner and runs in CI.
 *
 * NOTE: this validates only the forward dataflow path. The hard properties the
 * model must eventually guarantee — WAIT stall, multi-driver bus resolution,
 * reset-via-pin — are intentionally NOT covered yet; they depend on the
 * two-phase tick / bus-resolution decisions still being settled (see
 * docs/plans/2026-06-30-cycle-exact-machine.md).
 */

#include <cstdint>
#include <cstring>
#include <memory>

#include <gtest/gtest.h>

#include "hw/board.h"

namespace {

/* ---- trivial RAM device: responds to memory reads/writes ---- */
struct Ram {
  uint8_t cells[0x10000];
};
Pins ram_tick(void* self, Pins pins) {
  Ram* ram = static_cast<Ram*>(self);
  if (pins.mreq && pins.rd) {
    pins.data = ram->cells[pins.addr];  // drive the data bus
  } else if (pins.mreq && pins.wr) {
    ram->cells[pins.addr] = pins.data;  // latch the data bus
  }
  return pins;
}
void ram_reset(void* self) { std::memset(static_cast<Ram*>(self)->cells, 0, sizeof(Ram::cells)); }
size_t ram_state_size(const void*) { return sizeof(Ram); }
void ram_save(const void* self, void* buf) { std::memcpy(buf, self, sizeof(Ram)); }
void ram_load(void* self, const void* buf) { std::memcpy(self, buf, sizeof(Ram)); }
Device ram_device(Ram* storage) {
  return Device{storage, "ram", ram_tick, ram_reset, ram_state_size, ram_save, ram_load};
}

/* ---- trivial ticker device: counts T-states, drives nothing ---- */
struct Ticker {
  uint64_t count;
};
Pins ticker_tick(void* self, Pins pins) {
  static_cast<Ticker*>(self)->count += 1;
  return pins;
}
void ticker_reset(void* self) { static_cast<Ticker*>(self)->count = 0; }
size_t ticker_state_size(const void*) { return sizeof(Ticker); }
void ticker_save(const void* self, void* buf) { std::memcpy(buf, self, sizeof(Ticker)); }
void ticker_load(void* self, const void* buf) { std::memcpy(self, buf, sizeof(Ticker)); }
Device ticker_device(Ticker* storage) {
  return Device{storage,           "ticker",    ticker_tick, ticker_reset,
                ticker_state_size, ticker_save, ticker_load};
}

}  // namespace

TEST(BoardFoundation, ForwardMemoryAccessClockAndSnapshot) {
  auto ram = std::make_unique<Ram>();
  auto ticker = std::make_unique<Ticker>();

  Board board;
  board_init(&board);
  const int ram_idx = board_add(&board, ram_device(ram.get()));
  const int tick_idx = board_add(&board, ticker_device(ticker.get()));
  board_reset(&board);

  EXPECT_EQ(ram_idx, 0) << "devices added in bus order";
  EXPECT_EQ(tick_idx, 1);
  EXPECT_EQ(board.count, 2);
  EXPECT_EQ(board.tstates, 0u) << "clock starts at 0";

  // Memory WRITE transaction: drive addr+data, assert mreq+wr, tick once.
  board.pins = Pins{};
  board.pins.addr = 0x1234;
  board.pins.data = 0xAB;
  board.pins.mreq = true;
  board.pins.wr = true;
  board_tick(&board);
  EXPECT_EQ(ram->cells[0x1234], 0xAB) << "RAM latched a write transaction";

  // Memory READ transaction: drive addr, assert mreq+rd, tick once.
  board.pins = Pins{};
  board.pins.addr = 0x1234;
  board.pins.mreq = true;
  board.pins.rd = true;
  board_tick(&board);
  EXPECT_EQ(board.pins.data, 0xAB) << "RAM drove the data bus on read";

  EXPECT_EQ(board.tstates, 2u) << "clock advanced one per board_tick";
  EXPECT_EQ(ticker->count, 2u) << "every device ticked once per T-state";

  // Uniform save/load round-trip on a device.
  uint8_t buf[sizeof(Ticker)];
  ticker_save(ticker.get(), buf);
  ticker->count = 999;
  ticker_load(ticker.get(), buf);
  EXPECT_EQ(ticker->count, 2u) << "device save/load round-trips state";
}
