/* board.h — the central scheduler: the bus fabric plus the device list.
 *
 * `board_tick` advances the whole machine by one T-state by threading a single
 * Pins value through every device in order. DEVICE ORDER IS THE WIRING: the Z80
 * runs first (driving address + control), then memory/peripherals respond on the
 * same bus value, so by the end of the tick the data lines reflect the access.
 * The exact order is the machine's topology and is documented where the board is
 * assembled (the CPC wiring), not here.
 */
#ifndef KONCPC_HW_BOARD_H
#define KONCPC_HW_BOARD_H

#include <stdint.h>

#include "device.h"
#include "pins.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { BOARD_MAX_DEVICES = 16 };

typedef struct Board {
  Device dev[BOARD_MAX_DEVICES];  /* device views (state is caller-owned) */
  int count;
  Pins pins;          /* current bus state                              */
  uint64_t tstates;   /* cumulative T-states since power-on             */
} Board;

/* Empty board: no devices, all lines deasserted, clock at zero. */
void board_init(Board* board);

/* Append a device in bus order. Returns its index, or -1 if the board is full. */
int board_add(Board* board, Device device);

/* Reset every device and deassert the bus; the clock keeps running. */
void board_reset(Board* board);

/* Advance the whole machine by one T-state. */
void board_tick(Board* board);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_BOARD_H */
