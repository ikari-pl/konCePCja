/* board.h — the central scheduler: the bus fabric plus the device list.
 *
 * THE SPEC IS docs/hw-spec.md §7. `board_tick` advances the whole machine by
 * one 16 MHz master cycle using a two-phase synchronous update: every device
 * reads the committed bus and drives the next (resting) bus; then the next bus
 * is committed. Device ORDER DOES NOT AFFECT THE RESULT.
 */
#ifndef KONCPC_HW_BOARD_H
#define KONCPC_HW_BOARD_H

#include <stdint.h>

#include <cstdint>

#include "buses.h"
#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

enum : std::uint8_t {
  BOARD_MAX_DEVICES = 24
};  // the full 6128+ machine fits 18; headroom

typedef struct Board {
  Device dev[BOARD_MAX_DEVICES]; /* device views (state is caller-owned) */
  int count;
  /* Devices actually dispatched per tick (a prefix of dev[], <= count).
   * Defaults to count. Unfitted trailing peripherals stay constructed + in
   * dev[] (attach/ UI/IPC keep working) but are excluded from board_tick — the
   * runtime seed of config-driven composition. */
  int active_count;
  /* The dev[] indices dispatched each tick, in order — the first `active_count`
   * entries. Lets the active set be any SUBSET (not just a trailing prefix): a
   * dormant device (an unplugged peripheral, whose tick is structurally inert)
   * is dropped from this list so board_tick skips its per-cycle indirect call.
   * Defaults to identity (0,1,2,…); rebuilt at composition/plug changes. */
  int tick_order[BOARD_MAX_DEVICES];
  Bus bus;                /* committed bus state                          */
  uint64_t master_cycles; /* cumulative 16 MHz master cycles since power-on */
} Board;

/* The floating/resting bus: data pulled up to 0xFF, every line deasserted. */
Bus bus_resting(void);

/* Empty board: no devices, resting bus, clock at zero. */
void board_init(Board* board);

/* Append a device. Returns its index, or -1 if the board is full. (Order does
 * not affect tick results; it only documents the wiring.) */
int board_add(Board* board, Device device);

/* Cold-boot every device and rest the bus; the clock keeps running. */
void board_reset(Board* board);

/* Advance the whole machine by one 16 MHz master cycle. */
void board_tick(Board* board);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_BOARD_H */
