/* tape.h — the cassette deck as a Device: CDT/TZX playback + live line-in.
 * THE SPEC: docs/hardware/tape-device.md. Drives tape.rdata; reads tape.motor.
 * Media is a caller-owned CDT buffer (live wiring, never serialized). */
#ifndef KONCPC_HW_TAPE_H
#define KONCPC_HW_TAPE_H

#include <stddef.h>
#include <stdint.h>

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TapeRegs {
  uint8_t attached;  /* a CDT is in the deck */
  uint8_t playing;   /* PLAY pressed */
  uint8_t motor;     /* the PPI relay, as last seen on the bus */
  uint8_t level;     /* current rdata level */
  uint8_t line_mode; /* live line-in follows tape_line_level() */
  uint8_t error;     /* unknown block stopped playback */
  uint32_t block;    /* current CDT block ordinal (0-based) */
  uint32_t pos;      /* byte offset into the CDT buffer */
} TapeRegs;

size_t tape_state_size(void);
Device tape_init(void* storage);
void tape_peek(const Device* dev, TapeRegs* out);

/* Insert a CDT/TZX (caller-owned; must outlive the attachment). Returns 0,
 * -1 if the header is not "ZXTape!\x1A". Rewinds. */
int tape_attach_cdt(const Device* dev, const uint8_t* data, size_t len);
void tape_eject(const Device* dev);

/* The deck's PLAY button (the firmware owns the motor relay separately). */
void tape_play(const Device* dev, int on);
void tape_rewind(const Device* dev);

/* Seek the deck to the Nth CDT block: walks the deck's own cdt by block size to
 * that ordinal (layout-independent — do NOT pass a legacy pbTapeImage offset).
 * Playback resets to the block's start; if PLAY was down it resumes there.
 * Out-of-range ordinals are ignored. */
void tape_seek(const Device* dev, uint32_t block_ordinal);

/* Byte length of the CDT block at `pos` in a raw CDT/TZX buffer — the SAME
 * sizing the deck walks with, exported so the host's block table
 * (tape_scan_blocks) cannot drift from the deck's ordinals. Returns 0 only
 * for an unsizable position (walkers must stop). */
uint32_t tape_cdt_block_len(const uint8_t* cdt, uint32_t len, uint32_t pos);

/* Drain decoded data bits recorded since the last call (host BITS-view scope):
 * copies up to `max` bits (each 0/1) into `out`, oldest first, returns the
 * count. The deck decodes these while playing data blocks. */
int tape_drain_bits(const Device* dev, uint8_t* out, int max);

/* Live line-in: rdata follows the host-fed level (post-Schmitt digital). */
void tape_line_mode(const Device* dev, int on);
void tape_line_level(const Device* dev, int level);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_TAPE_H */
