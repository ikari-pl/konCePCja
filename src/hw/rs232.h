/* rs232.h — the Amstrad Serial Interface card (Z80 DART + Intel 8253) as a
 * Device. THE SPEC: docs/hardware/rs232-device.md. The CPC end of the serial
 * wire: DART channel A framed onto SerialBus.txd/rxd at the 8253-derived
 * baud; whatever hardware sits at the far end is a separate Device. */
#ifndef KONCPC_HW_RS232_H
#define KONCPC_HW_RS232_H

#include <stddef.h>
#include <stdint.h>

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Rs232Regs {
  uint8_t wr[6];       /* DART ch A WR0–WR5 as last written */
  uint8_t rr0;         /* computed status (spec §3)          */
  uint8_t rr1;         /* all-sent + error bits              */
  uint16_t divisor;    /* 8253 counter-0 reload (baud)       */
  uint8_t fifo_depth;  /* RX FIFO fill, 0–3                  */
  uint8_t tx_busy;     /* a TX frame is on the wire          */
  uint8_t txd;         /* current wire levels                */
  uint8_t rxd;
  uint8_t plugged;
} Rs232Regs;

size_t rs232_state_size(void);
Device rs232_init(void* storage);
void rs232_peek(const Device* dev, Rs232Regs* out);

/* Model plugging/unplugging the card: unplugged, nothing decodes and the
 * wire rests at mark. */
void rs232_set_plugged(const Device* dev, int on);

/* Host-side byte feed (backend → CPC RX FIFO). Bypasses bit-serial wire
 * simulation — used by the engine=1 serial bridge for file/TCP/tty backends. */
void rs232_host_rx(const Device* dev, uint8_t byte);
void rs232_set_host_tx(const Device* dev, void (*fn)(uint8_t, void*),
                       void* ctx);

/* Wake contract: 1 when the DART is idle on both wires — no TX frame shifting
 * or double-buffered, RX not mid-frame. The bit timers reset on byte start, so
 * a quiet DART carries no free-running per-cycle counter and skipping its tick
 * is byte-identical (the wire rests at mark). */
int rs232_quiet(const Device* dev);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_RS232_H */
