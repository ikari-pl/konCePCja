/* ppi.h — the CPC PPI 8255 as a Device. See docs/hardware/ppi-device.md.
 *
 * The CPC's I/O hub: keyboard row select, the AY-3-8912 control/data path, the
 * cassette, and the machine status inputs (VSYNC, refresh rate, manufacturer
 * id, printer ready). Answers I/O with A11 = 0; A9..A8 pick the port
 * (A/B/C/control).
 *
 * Caller-owned, no heap: allocate ppi_state_size() bytes, hand them to
 * ppi_init(). */
#ifndef KONCPC_HW_PPI_H
#define KONCPC_HW_PPI_H

#include <stddef.h>
#include <stdint.h>

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Introspection snapshot (tests / debugging). */
typedef struct PpiRegs {
  uint8_t portA;      /* AY data-bus latch */
  uint8_t portB;      /* status-input latch (usually unused: Port B is input) */
  uint8_t portC;      /* keyboard row (0..3) + tape (4,5) + AY BC1/BDIR (6,7) */
  uint8_t control;    /* last 8255 mode-set command (direction bits) */
  uint8_t kbd_row;    /* published keyboard row = portC & 0x0F */
  uint8_t tape_motor; /* 1 while the cassette motor bit (portC bit 4) is set */
} PpiRegs;

size_t ppi_state_size(void);
Device ppi_init(void* storage);
void ppi_peek(const Device* dev, PpiRegs* out);

/* Hardware straps (manufacturer id + refresh rate), Port B bits 1..4. Default
 * is 0x1E = Amstrad (id 7) + 50 Hz. Set before use to emulate other straps. */
void ppi_set_jumpers(const Device* dev, uint8_t jumpers);

/* --- Fast-tier batch seam (ppi-device.md §batch) ---
 *
 * The PPI is a pure event device between I/O accesses: its published lines
 * derive from the latches. The Fast scheduler applies each access as one
 * event and relays AY line changes to the PSG (psg_fast_lines). */
typedef struct PpiAyLines {
  uint8_t bdir, bc1;   /* AY bus control state (from Port C bits 7/6) */
  uint8_t kbd_row;     /* selected keyboard row (Port C low nibble) */
  uint8_t da;          /* what the PPI drives on the AY data bus: Port A's
                          latch in output mode, else the floating 0xFF */
  uint8_t tape_motor;  /* cassette relay (Port C bit 4) */
  uint8_t tape_wdata;  /* cassette write line (Port C bit 5) */
} PpiAyLines;

/* The currently-published lines (entry/exit sync, and the read path). */
void ppi_fast_lines(const Device* dev, PpiAyLines* out);
/* Apply one I/O WRITE event (A11=0 decode; ports A/B/C/control). Returns
 * nonzero when the published AY/tape lines changed, filling *after with the
 * new state — the caller relays it to psg_fast_lines / the tape deck. */
int ppi_fast_io_write(const Device* dev, uint16_t port, uint8_t val,
                      PpiAyLines* after);
/* Apply one I/O READ event. `vsync`/`rdata` are the live levels the Port B
 * status read passes through (the scheduler catches up their owners first);
 * `ay_da` is the AY bus value a Port A input read latches (psg_fast_read
 * when the lines sit in the READ state, else 0xFF). Returns nonzero when the
 * PPI drives the bus, filling *out. */
int ppi_fast_io_read(const Device* dev, uint16_t port, int vsync, int rdata,
                     uint8_t ay_da, uint8_t* out);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_PPI_H */
