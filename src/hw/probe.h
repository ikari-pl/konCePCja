/* probe.h — ICE-style bus probe: the debug harness's breakpoint/watchpoint
 * engine as a Device. See docs/hardware/probe-device.md.
 *
 * Bench equipment, not a CPC chip: it watches the committed bus image every
 * master cycle and DRIVES NOTHING (infinite input impedance). The first match
 * latches {kind, addr, data, cycle}; the host polls probe_pending() and stops
 * ticking the board — the probe itself never halts anything. Comparators
 * survive a CPC reset (an ICE keeps its setup when the target reboots). */
#ifndef KONCPC_HW_PROBE_H
#define KONCPC_HW_PROBE_H

#include <stddef.h>
#include <stdint.h>

#include <cstdint>

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

enum : std::uint8_t {
  PROBE_HIT_NONE = 0,
  PROBE_HIT_EXEC = 1,      /* M1 opcode fetch at a breakpoint address    */
  PROBE_HIT_MEM_READ = 2,  /* mreq read at a watched address             */
  PROBE_HIT_MEM_WRITE = 3, /* mreq write at a watched address            */
  PROBE_HIT_IO_READ = 4,   /* iorq read matching a port comparator       */
  PROBE_HIT_IO_WRITE = 5,  /* iorq write matching a port comparator      */
};

typedef struct ProbeHit {
  uint8_t kind;   /* PROBE_HIT_*                                          */
  uint16_t addr;  /* the address/port that matched                        */
  uint8_t data;   /* the data bus at the matching edge                    */
  uint64_t cycle; /* master cycles since probe init when the latch closed */
} ProbeHit;

size_t probe_state_size(void);
Device probe_init(void* storage);

/* Exec breakpoints (up to 32 addresses). add: 0 = added, -1 = full/duplicate;
 * del: 0 = removed, -1 = not found; list returns the count copied. */
int probe_add_exec(const Device* dev, uint16_t addr);
int probe_del_exec(const Device* dev, uint16_t addr);
void probe_clear_exec(const Device* dev);
int probe_list_exec(const Device* dev, uint16_t* out, int max);

/* Memory watchpoints (up to 16; fires on the selected access directions).
 * Opcode fetches count as reads — that is what the wires do. */
int probe_add_watch(const Device* dev, uint16_t addr, uint16_t len,
                    uint8_t on_read, uint8_t on_write);
int probe_del_watch(const Device* dev, uint16_t addr);
void probe_clear_watch(const Device* dev);
int probe_list_watch(const Device* dev, uint16_t* out, int max);

/* I/O comparators (up to 8): match (port & mask) == (value & mask) on the
 * selected directions; interrupt acknowledges (m1 with iorq) never match. */
int probe_add_io(const Device* dev, uint16_t value, uint16_t mask,
                 uint8_t on_read, uint8_t on_write);
int probe_del_io(const Device* dev, uint16_t value, uint16_t mask);
void probe_clear_io(const Device* dev);

/* TAPS: notify comparators (up to 4). A tap fires on the M1 fetch edge at its
 * address like an exec breakpoint, but never latches/halts — it raises a
 * one-cycle flag the host takes with probe_tap_take (a logic analyzer's
 * trigger-out line; the firmware-vector hooks ride on this). Taps keep firing
 * while the halt latch is closed. */
int probe_add_tap(const Device* dev, uint16_t addr);
void probe_clear_taps(const Device* dev);
/* Take a pending tap: returns 1 and fills *addr (clears the flag), else 0. */
int probe_tap_take(const Device* dev, uint16_t* addr);

/* Nonzero while a hit is latched; fills *out when non-null. Does not clear. */
int probe_pending(const Device* dev, ProbeHit* out);
/* Clear the latch and resume matching. */
void probe_ack(const Device* dev);
/* Nonzero when any comparator is armed (host: poll pending only while armed).
 */
int probe_armed(const Device* dev);

/* Nonzero when the probe has any work: a comparator, a tap, or a latched hit.
 * While this is 0, probe_tick() is a no-op, so the scheduler may drop the probe
 * from the per-cycle loop (see subcycle recompose_active). Broader than
 * probe_armed(), which ignores taps and the latch. */
int probe_active(const Device* dev);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_PROBE_H */
