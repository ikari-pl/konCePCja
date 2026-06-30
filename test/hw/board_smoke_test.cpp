/* board_smoke_test.cpp — proves the device/bus/board foundation end-to-end with
 * two trivial devices, before any real chip exists.
 *
 *   c++ -std=c++17 -Wall -Wextra -Wconversion -Wshadow -I src/hw \
 *       test/hw/board_smoke_test.cpp src/hw/board.cpp -o /tmp/boardsmoke
 *
 * It demonstrates the whole model: a single Pins value threaded through devices
 * in bus order each tick, a memory device responding to mreq+rd/wr, the clock
 * advancing, and uniform per-device save/load.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "board.h"

namespace {

int failures = 0;
void check(bool ok, const char* what) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++failures;
}

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
  return Device{storage,      "ticker",     ticker_tick, ticker_reset,
                ticker_state_size, ticker_save, ticker_load};
}

}  // namespace

int main() {
  static Ram ram_storage;        // caller-owned device state, off the stack
  static Ticker ticker_storage;

  Board board;
  board_init(&board);
  int ram_idx = board_add(&board, ram_device(&ram_storage));
  int tick_idx = board_add(&board, ticker_device(&ticker_storage));
  board_reset(&board);

  check(ram_idx == 0 && tick_idx == 1, "devices added in bus order");
  check(board.count == 2, "board has 2 devices");
  check(board.tstates == 0, "clock starts at 0");

  // Memory WRITE transaction: drive addr+data, assert mreq+wr, tick once.
  board.pins = Pins{};
  board.pins.addr = 0x1234;
  board.pins.data = 0xAB;
  board.pins.mreq = true;
  board.pins.wr = true;
  board_tick(&board);
  check(ram_storage.cells[0x1234] == 0xAB, "RAM latched a write transaction");

  // Memory READ transaction: drive addr, assert mreq+rd, tick once.
  board.pins = Pins{};
  board.pins.addr = 0x1234;
  board.pins.mreq = true;
  board.pins.rd = true;
  board_tick(&board);
  check(board.pins.data == 0xAB, "RAM drove the data bus on read");

  check(board.tstates == 2, "clock advanced one per board_tick");
  check(ticker_storage.count == 2, "every device ticked once per T-state");

  // Uniform save/load round-trip on a device.
  uint8_t buf[sizeof(Ticker)];
  ticker_save(&ticker_storage, buf);
  ticker_storage.count = 999;
  ticker_load(&ticker_storage, buf);
  check(ticker_storage.count == 2, "device save/load round-trips state");

  std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILURES",
              failures, failures == 1 ? "" : "s");
  return failures == 0 ? 0 : 1;
}
