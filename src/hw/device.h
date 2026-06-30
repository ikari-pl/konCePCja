/* device.h — the ONE interface every chip on the bus implements.
 *
 * A Device is a small value type (a name + function pointers + a `self` pointer
 * to the device's own state). It owns no memory: the caller allocates the state
 * storage (stack, static, or BSS) sized by the module's `*_state_size()` free
 * function and hands it to the module's `*_init(void* storage)`, which returns a
 * ready Device view. No heap — which keeps the door open for a SPARK/Ada device
 * (dynamic allocation is hostile to formal proof) and makes save-states trivial.
 *
 * The uniform shape means the Z80 is not special: it is a Device whose `tick`
 * happens to run a CPU; the Gate Array is a Device whose `tick` happens to drive
 * `wait` and video. The board treats them identically.
 */
#ifndef KONCPC_HW_DEVICE_H
#define KONCPC_HW_DEVICE_H

#include <stddef.h>

#include "pins.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Device {
  void* self;        /* device state, owned by the caller (no heap)        */
  const char* name;  /* stable identifier, e.g. "z80", "gate-array"        */

  /* Advance the device by exactly one T-state. Reads the lines it observes
   * from `pins`, drives the lines it owns, returns the updated bus. Pure in
   * (self, pins) — no globals, no allocation. */
  Pins (*tick)(void* self, Pins pins);

  /* Re-initialise to power-on/reset state. */
  void (*reset)(void* self);

  /* Serialization: `state_size` bytes round-trip through save/load. Used by the
   * board to snapshot/restore the whole machine uniformly. */
  size_t (*state_size)(const void* self);
  void (*save)(const void* self, void* buf);
  void (*load)(void* self, const void* buf);
} Device;

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_DEVICE_H */
