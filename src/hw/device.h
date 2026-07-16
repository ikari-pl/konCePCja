/* device.h — the ONE interface every chip on the bus implements.
 *
 * THE SPEC IS docs/hw-spec.md §5. A Device is a small value: a name, a `self`
 * pointer to caller-owned state (no heap → SPARK-friendly), and function
 * pointers. The Z80 is not special — it is a Device whose `tick` runs a CPU.
 *
 * TWO-PHASE TICK (the rule that makes device order irrelevant):
 *   - read ONLY from `in` (the committed bus from the previous master cycle);
 *   - `out` arrives in the resting/floating state — write ONLY the lines this
 *     device drives;
 *   - owned / tri-state lines (addr, data, mreq, hsync, …): ASSIGN;
 *   - wired-OR lines (irq): OR in (`out->cpu.irq |= mine`);
 *   - never read `out` (except to OR a wired-OR line).
 * Signals therefore propagate one master cycle per hop (see spec §2).
 */
#ifndef KONCPC_HW_DEVICE_H
#define KONCPC_HW_DEVICE_H

#include <stddef.h>

#include "buses.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Device {
  void* self;       /* device state, caller-owned; must outlive the Board */
  const char* name; /* stable identifier, e.g. "z80", "gate-array"        */

  /* Advance one 16 MHz master cycle. See the two-phase rules above. */
  void (*tick)(void* self, const Bus* in, Bus* out);

  /* Power-on / cold-boot initializer (the runtime reset sequence travels on
   * cpu.reset instead — see spec §8). */
  void (*reset)(void* self);

  /* Serialization of LOGICAL state only (never raw pointers); blob begins with
   * a 1-byte format version. `state_size` bytes round-trip through save/load.
   */
  size_t (*state_size)(const void* self);
  void (*save)(const void* self, void* buf);
  void (*load)(void* self, const void* buf);
} Device;

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_DEVICE_H */
