/* plotter_hp7470a.h — the HP 7470A pen plotter as a Device. THE SPEC:
 * docs/hardware/plotter-device.md. Sits at the far end of the RS232 card's
 * wire (SerialBus): UART both directions, 255-byte input buffer with
 * XON/XOFF, the HP-GL parser (transplanted from the proven legacy
 * src/plotter.cpp oracle), and the response transmitter. The host renders
 * the PAGE from plotter_hp7470a_segments() — an interpretation of Device
 * state, never behavior. */
#ifndef KONCPC_HW_PLOTTER_HP7470A_H
#define KONCPC_HW_PLOTTER_HP7470A_H

#include <stddef.h>
#include <stdint.h>

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One drawn primitive — a flat record so the page serializes by memcpy.
 * type: 0=Line 1=Circle 2=Arc 3=Label (matches the legacy PlotPrimitive
 * order for the parity oracle). */
typedef struct PlotSeg {
  uint8_t type;
  uint8_t pen;       /* 1 or 2 */
  int8_t line_type;  /* -1 solid, 0..6 patterns */
  float x1, y1;      /* start or center (plotter units) */
  float x2, y2;      /* end, for lines */
  float radius;      /* circles/arcs */
  float start_angle; /* arcs, degrees */
  float sweep_angle; /* arcs, degrees */
  char text[64];     /* labels (NUL-terminated, truncated) */
} PlotSeg;

typedef struct PlotterRegs {
  float pen_x, pen_y; /* carriage position, plotter units */
  uint8_t pen_down;
  int8_t selected_pen;   /* 0 stowed, 1-2 active */
  uint16_t buffer_fill;  /* input buffer bytes waiting, 0..255 */
  uint32_t page_rev;     /* bumps on every segment append / page clear */
  uint8_t page_overflow; /* the fixed segment store filled; drawing stopped */
  uint8_t flow_stopped;  /* XOFF sent, XON not yet */
  uint8_t plugged;
} PlotterRegs;

size_t plotter_hp7470a_state_size(void);
Device plotter_hp7470a_init(void* storage);
void plotter_hp7470a_peek(const Device* dev, PlotterRegs* out);

/* The page, for rendering + export. Returns the segment count; *out points
 * at the Device-owned flat array (valid until the next tick that appends). */
size_t plotter_hp7470a_segments(const Device* dev, const PlotSeg** out);
/* The operator tears the sheet off. */
void plotter_hp7470a_clear_page(const Device* dev);

/* Model plugging/unplugging: unplugged, the wire rests at mark. */
void plotter_hp7470a_set_plugged(const Device* dev, int on);

/* Wake contract: 1 when both UART frames are off the wire AND there is no
 * buffered work with a per-cycle drain — no TX (responses/flow), RX not
 * mid-frame, input ring empty (drain_cnt only counts while in_count > 0), no
 * queued response, no pending flow byte. Quiet ⇒ skipping the tick is
 * byte-identical (serial.rxd rests at mark). */
int plotter_hp7470a_quiet(const Device* dev);
/* DIP-fixed line rate: bit time = divisor * 128 master cycles, the same
 * convention as the RS232 card's 8253 (rs232-device.md §4). */
void plotter_hp7470a_set_baud_divisor(const Device* dev, uint16_t divisor);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_PLOTTER_HP7470A_H */
