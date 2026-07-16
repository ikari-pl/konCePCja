/* symbiface.h — the Symbiface II expansion board as a Device. THE SPEC:
 * docs/hardware/symbiface-device.md. IDE (ATA PIO) against caller-owned
 * mutable images, DS12887 RTC with host-fed time + serialized CMOS, PS/2
 * mouse behind the multiplexed FIFO. Full 16-bit decode on &FDxx. */
#ifndef KONCPC_HW_SYMBIFACE_H
#define KONCPC_HW_SYMBIFACE_H

#include <stddef.h>
#include <stdint.h>

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Sf2Regs {
  uint8_t plugged;
  uint8_t active_drive; /* 0 = master, 1 = slave */
  uint8_t ide_status;   /* the active drive's status byte */
  uint8_t rtc_index;    /* selected DS12887 register */
  uint8_t fifo_used;    /* pending mouse FIFO entries */
} Sf2Regs;

size_t sf2_state_size(void);
Device sf2_init(void* storage);
void sf2_peek(const Device* dev, Sf2Regs* out);

/* Attach a caller-owned MUTABLE raw image to a drive (0 = master, 1 =
 * slave); total sectors = len / 512. WRITE SECTORS mutates it in place —
 * the dirty flag carries the host's persistence decision (spec §2). Live
 * wiring: must outlive the attachment, never serialized. */
void sf2_ide_attach(const Device* dev, int drive, uint8_t* img, size_t len);
void sf2_ide_detach(const Device* dev, int drive);
int sf2_media_dirty(const Device* dev);
void sf2_media_mark_clean(const Device* dev);

/* The DS12887 clock registers 0..9 (BCD, the golden master's layout:
 * sec, x, min, x, hour, x, day-of-week, day, month, year). The host
 * refreshes whenever it likes; reads serve the fed values (spec §3). */
void sf2_rtc_set_time(const Device* dev, const uint8_t regs10[10]);

/* Whole mickeys + host button mask (bit0 left, bit1 middle, bit2 right);
 * the Device encodes SF2 packets, chunking large deltas (spec §4). */
void sf2_mouse_feed(const Device* dev, int dx, int dy, uint8_t buttons);

/* Push one pre-encoded SF2 status packet (host-side encoders — the bridge
 * drains the legacy FIFO so SDL and IPC input keep their exact packet
 * stream). The Device-side encoder above remains for direct feeding. */
void sf2_mouse_push_packet(const Device* dev, uint8_t pkt);

/* Model plugging/unplugging the expansion. */
void sf2_set_plugged(const Device* dev, int on);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_SYMBIFACE_H */
