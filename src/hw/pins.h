/* pins.h — the konCePCja system bus, modeled at the pin level.
 *
 * Every device (CPU, Gate Array, CRTC, PSG, FDC, PPI, RAM, ROM) is ticked once
 * per T-state and is a transition `(self, pins) -> (self, pins)`: it reads the
 * lines it cares about and drives the lines it owns. The board threads one Pins
 * value through every device each T-state (see board.h), so the whole machine
 * advances in lockstep — this is what makes cycle-exact CPC timing (including
 * the Gate Array holding the Z80 in WAIT to align accesses to 1 µs) emerge
 * naturally rather than being faked.
 *
 * CONVENTION: lines are modeled ACTIVE-HIGH — `true` means "asserted". Real Z80
 * control lines are active-low (/MREQ, /RD, …); we flip them once, here, so the
 * rest of the code reads as plain boolean logic (`if (p.mreq && p.rd)`), never
 * `if (!p.mreq_n)`. The only place active-low matters is the eventual hardware
 * I/O datasheet cross-check, which is documented at each device.
 *
 * Plain C, no C++ types: a SPARK/Ada device maps this 1:1 to a record and a
 * Rust device to a #[repr(C)] struct.
 */
#ifndef KONCPC_HW_PINS_H
#define KONCPC_HW_PINS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Pins {
  uint16_t addr;  /* A0–A15 — address bus            */
  uint8_t data;   /* D0–D7  — data bus (bidirectional) */

  /* Z80 drives these (outputs): */
  bool m1;    /* opcode-fetch machine cycle      */
  bool mreq;  /* memory request                  */
  bool iorq;  /* I/O request                     */
  bool rd;    /* read strobe                     */
  bool wr;    /* write strobe                    */
  bool rfsh;  /* refresh cycle (DRAM row on addr)  */
  bool halt;  /* CPU is in HALT                  */

  /* Driven toward the Z80 (inputs): */
  bool wait;   /* hold the CPU — the CPC Gate Array's 1 µs access stretch */
  bool irq;    /* maskable interrupt (the /INT line; named for the signal) */
  bool nmi;    /* non-maskable interrupt         */
  bool reset;  /* system reset                   */

  /* DMA arbitration: */
  bool busrq;  /* bus request                    */
  bool busak;  /* bus acknowledge                */
} Pins;

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_PINS_H */
