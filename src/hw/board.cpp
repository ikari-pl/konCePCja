/* board.cpp — central scheduler implementation. See board.h and docs/hw-spec.md. */

#include "board.h"

extern "C" {

Bus bus_resting(void) {
  Bus bus{};            // all lines deasserted, addresses 0, phase 0
  bus.cpu.data = 0xFF;  // data bus floats high (pull-up)
  return bus;
}

void board_init(Board* board) {
  board->count = 0;
  board->master_cycles = 0;
  board->bus = bus_resting();
}

int board_add(Board* board, Device device) {
  if (board->count >= BOARD_MAX_DEVICES) return -1;
  const int index = board->count;
  board->dev[index] = device;
  board->count += 1;
  return index;
}

void board_reset(Board* board) {
  board->bus = bus_resting();
  for (int i = 0; i < board->count; ++i) {
    const Device* dev = &board->dev[i];
    if (dev->reset) dev->reset(dev->self);
  }
}

void board_tick(Board* board) {
  // Two-phase: every device reads the committed bus, drives the next (resting)
  // bus; then we commit. Order-independent (see docs/hw-spec.md §7).
  Bus next = bus_resting();
  for (int i = 0; i < board->count; ++i) {
    const Device* dev = &board->dev[i];
    dev->tick(dev->self, &board->bus, &next);
  }
  board->bus = next;
  board->master_cycles += 1;
}

}  // extern "C"
