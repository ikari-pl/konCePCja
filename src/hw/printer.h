/* printer.h — the CPC printer port latch as a Device. THE SPEC:
 * docs/hardware/printer-device.md. Write-only latch decoded by A12 = 0;
 * bit 7 is /STROBE (inverted by the base machine), bits 6..0 the data
 * lines. Strobe-edge bytes land in a drop-oldest event ring for the host
 * (printer file capture, plotter). The Digiblaster DAC is a host-side
 * interpretation of the latch — see the spec §3. */
#ifndef KONCPC_HW_PRINTER_H
#define KONCPC_HW_PRINTER_H

#include <stddef.h>
#include <stdint.h>

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PrinterRegs {
  uint8_t latch; /* data ^ 0x80 as the connector sees it (reset 0xFF) */
} PrinterRegs;

typedef struct PrinterEvent {
  uint64_t cycle; /* master-cycle timestamp of the strobe edge */
  uint8_t byte;   /* the 7 data bits clocked by /STROBE */
} PrinterEvent;

size_t printer_state_size(void);
Device printer_init(void* storage);
void printer_peek(const Device* dev, PrinterRegs* out);

/* Restore the latched data byte (SNA v3 stores connector view ^ 0x80). */
void printer_poke_latch(const Device* dev, uint8_t latch);

/* Move up to `max` strobe-clocked bytes into `out`; returns the count. */
int printer_drain_events(const Device* dev, PrinterEvent* out, int max);

/* Clock-wake contract (Gate B6 wake scheduler): between I/O write strobes the
 * latch cannot change and the tick's only side effect is its master-cycle
 * counter (the PrinterEvent timestamp base). A scheduler that skipped those
 * cycles calls this right before the next real tick, so `now` lands exactly
 * where a per-cycle run would have put it. */
void printer_advance(const Device* dev, uint64_t skipped_cycles);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_PRINTER_H */
