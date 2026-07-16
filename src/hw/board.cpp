/* board.cpp — central scheduler implementation. See board.h and
 * docs/hw-spec.md. */

#include "board.h"

extern "C" {

Bus bus_resting(void) {
  Bus bus{};              // all lines deasserted, addresses 0, phase 0
  bus.cpu.data = 0xFF;    // data bus floats high (pull-up)
  bus.ram.data = 0xFF;    // RAM-side data floats high too
  bus.ay.row_ext = 0xFF;  // external matrix columns idle high (pull-ups)
  bus.serial.txd = true;  // serial line idles at mark (rs232-device.md §1)
  bus.serial.rxd = true;
  bus.pen.strobe = false;  // LPEN idle (light-gun-device.md §1)
  return bus;
}

void board_init(Board* board) {
  board->count = 0;
  board->active_count = 0;
  board->master_cycles = 0;
  board->bus = bus_resting();
}

int board_add(Board* board, Device device) {
  if (board->count >= BOARD_MAX_DEVICES) return -1;
  const int index = board->count;
  board->dev[index] = device;
  board->tick_order[index] = index;  // identity until a recompose reorders it
  board->count += 1;
  board->active_count = board->count;  // all fitted + dispatched by default
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
  for (int i = 0; i < board->active_count; ++i) {
    const Device* dev = &board->dev[board->tick_order[i]];
    dev->tick(dev->self, &board->bus, &next);
  }
  board->bus = next;
  board->master_cycles += 1;
}

}  // extern "C"
