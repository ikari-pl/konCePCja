/* board.cpp — central scheduler implementation. See board.h. */

#include "board.h"

extern "C" {

void board_init(Board* board) {
  board->count = 0;
  board->tstates = 0;
  board->pins = Pins{};  // all lines deasserted, bus zeroed
}

int board_add(Board* board, Device device) {
  if (board->count >= BOARD_MAX_DEVICES) return -1;
  const int index = board->count;
  board->dev[index] = device;
  board->count += 1;
  return index;
}

void board_reset(Board* board) {
  board->pins = Pins{};
  for (int i = 0; i < board->count; ++i) {
    Device* d = &board->dev[i];
    if (d->reset) d->reset(d->self);
  }
}

void board_tick(Board* board) {
  Pins pins = board->pins;
  for (int i = 0; i < board->count; ++i) {
    Device* d = &board->dev[i];
    pins = d->tick(d->self, pins);
  }
  board->pins = pins;
  board->tstates += 1;
}

}  // extern "C"
