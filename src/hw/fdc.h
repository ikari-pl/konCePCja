/* fdc.h — the µPD765A floppy disc controller as a Device. See
 * docs/hardware/fdc-device.md.
 *
 * The CPC disc interface: partial decode A10 = 0 AND A7 = 0; A8 = 0 is the
 * drive-motor latch (&FA7E), A8 = 1 the FDC with A0 picking main status (&FB7E)
 * vs the data register (&FB7F). Polled non-DMA operation only — the CPC leaves
 * the chip's /INT and DRQ pins unconnected, so the Device never drives cpu.irq.
 *
 * Caller-owned, no heap: allocate fdc_state_size() bytes, hand them to
 * fdc_init(). The disc is a caller-owned DSK image attached with
 * fdc_attach_disk() — live wiring like mem_attach_expansion: it persists across
 * reset and save/load and is never serialized. */
#ifndef KONCPC_HW_FDC_H
#define KONCPC_HW_FDC_H

#include <stddef.h>
#include <stdint.h>

#include <cstdint>

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Introspection snapshot (tests / debugging). */
typedef struct FdcRegs {
  uint8_t
      phase; /* 0 = command, 1 = execution, 2 = result, 3 = busy (mechanics) */
  uint8_t msr;      /* main status register as a read would return it */
  uint8_t motor;    /* 1 while the motor latch is on */
  uint8_t ready;    /* 1 when drive A is ready (disc in + motor spun up) */
  uint8_t last_cmd; /* opcode (low 5 bits) of the last dispatched command */
  uint8_t unit;     /* drive selected by the last command (US0: 0 = A, 1 = B) */
  uint8_t track[2]; /* physical head position per unit */
  uint8_t st0, st1, st2; /* status bytes of the last completed operation */
  uint32_t sectors_read; /* sectors fully delivered by READ DATA since reset */
} FdcRegs;

/* --- Mechanical event ring (drive sounds / telemetry) ---
 * The FDC logs its physical events with master-cycle timestamps; the audio
 * bridge drains them and schedules samples at exact offsets (16 cycles = 1 µs,
 * so sample = cycle * rate / 16e6 is exact math). Live state: never serialized;
 * ring drops the OLDEST event on overflow. */
enum : std::uint8_t {
  FDC_EV_MOTOR_ON = 1,    /* latch switched on: spin-up begins                */
  FDC_EV_MOTOR_READY = 2, /* platter up to speed (500 ms after MOTOR_ON)      */
  FDC_EV_MOTOR_OFF = 3,   /* latch off: platter stops                         */
  FDC_EV_STEP = 4,        /* one head step; arg = the new physical track      */
  FDC_EV_INDEX = 5,       /* index hole passed (every 200 ms while spinning)  */
};

typedef struct FdcEvent {
  uint64_t cycle; /* master-cycle timestamp (the FDC's own clock)             */
  uint8_t type;   /* FDC_EV_*                                                 */
  uint8_t arg;    /* STEP: the new track; others: 0                           */
} FdcEvent;

/* Move up to `max` pending events into `out`; returns the count moved. */
int fdc_drain_events(const Device* dev, FdcEvent* out, int max);

/* Clock-wake contract (Gate B6 wake scheduler): nonzero when fdc_tick's only
 * per-cycle work is its master-cycle counter — the motor is off (no rotation,
 * no spin-up deadline), no seek is stepping, the command FSM is in an untimed
 * phase (command/result, not BUSY/EXEC), and the I/O access edge-detector is
 * clear. A scheduler may then skip the tick on cycles where the FDC is not
 * I/O-selected, re-applying the skipped count with fdc_advance() before the
 * next real tick so `now` (event timestamps + every deadline comparison) lands
 * exactly where a per-cycle run would have put it. */
int fdc_quiet(const Device* dev);
void fdc_advance(const Device* dev, uint64_t skipped_cycles);

size_t fdc_state_size(void);
Device fdc_init(void* storage);
void fdc_peek(const Device* dev, FdcRegs* out);

/* Restore motor latch and head positions (SNA v3). */
void fdc_poke_mechanics(const Device* dev, uint8_t motor, uint8_t track_a,
                        uint8_t track_b);

/* Attach a caller-owned DSK image (standard "MV - CPC" or "EXTENDED") to a
 * drive. `unit` selects the drive: 0 = A (default, for source compatibility),
 * 1 = B. The buffer is parsed in place and must outlive the attachment.
 * MUTABLE: WRITE DATA / FORMAT TRACK edit it in place (docs §10) — the dirty
 * flag below tells the host when the buffer diverged from its file. Returns 0
 * on success, -1 on a malformed image (drive left empty). Live wiring: persists
 * across reset and save/load, never serialized. */
int fdc_attach_disk(const Device* dev, uint8_t* dsk, size_t len,
                    uint8_t unit = 0);

/* Nonzero when the attached DSK buffer has been mutated since attach (or the
 * last mark_clean). Persistence is the host's decision (docs §10). */
int fdc_media_dirty(const Device* dev);
int fdc_media_dirty_unit(const Device* dev, uint8_t unit);
void fdc_media_mark_clean(const Device* dev);
void fdc_media_mark_clean_unit(const Device* dev, uint8_t unit);

/* How the medium behind a drive is backed (fdc_media.backing). SECTOR = a plain
 * DSK image; FLUX = a flux dump. A flux medium may still carry a writable DSK
 * overlay (fdc_attach_flux_writable): clean tracks serve the rotating flux
 * cache, tracks the FDC has written serve the overlay (docs flux-media.md §7).
 */
enum FdcBacking : uint8_t { FDC_BACKING_SECTOR = 0, FDC_BACKING_FLUX = 1 };

/* Attach a caller-owned SCP flux dump as drive A's medium (Stage 3). The FDC
 * decodes the track under the head on demand — one captured revolution at a
 * time — and serves whichever revolution is physically passing the head, so
 * weak/fuzzy protection bits emerge from capture differences with no special
 * casing. Live wiring like fdc_attach_disk (replaces any attached DSK).
 * READ-ONLY: with no DSK overlay the medium has image == nullptr, so WRITE DATA
 * / FORMAT terminate Not-Writable. Returns 0, or -1 if the buffer is not a
 * usable SCP. */
int fdc_attach_flux(const Device* dev, const uint8_t* scp, size_t len);

/* Attach a WRITABLE flux medium to drive A (Stage 2): the pristine `scp` stays
 * the verbatim source for unwritten tracks, while `dsk` — a standard/extended
 * DSK the caller synthesized from that same SCP (flux_scp_to_dsk) — is the
 * mutable overlay. A clean (unwritten) track keeps serving the rotating flux
 * cache (weak bits intact); the first WRITE DATA / FORMAT to a track promotes
 * it (per-track dirty map) so it serves the overlay from then on — just as a
 * real drive replaces a track's flux fuzz with fresh MFM when it writes. Both
 * buffers are caller-owned and must outlive the attachment. `dsk` is MUTABLE —
 * writes/FORMAT edit it in place (like fdc_attach_disk); `scp` stays const, the
 * pristine source. Returns 0, or -1 if the DSK overlay is malformed (nothing
 * attached). */
int fdc_attach_flux_writable(const Device* dev, const uint8_t* scp,
                             size_t scp_len, uint8_t* dsk, size_t dsk_len);

/* Export introspection for a writable flux medium on drive A (Stage 4 save-as
 * feeds these to scp_from_disk). `fdc_media_track_dirty` returns the per-track
 * dirty map and fills `ntracks_out`; `fdc_media_flux_scp` / `fdc_media_image`
 * return the pristine SCP and the mutable DSK overlay with their lengths. Any
 * pointer is nullptr when the medium does not carry that backing. */
const bool* fdc_media_track_dirty(const Device* dev, int& ntracks_out);
const uint8_t* fdc_media_flux_scp(const Device* dev, size_t& len_out);
const uint8_t* fdc_media_image(const Device* dev, size_t& len_out);

/* Unit-aware DSK/overlay image accessor (unit 0 = A, 1 = B); fdc_media_image is
 * the unit-0 shorthand. A unit whose medium carries no writable image (empty
 * drive, or a read-only flux dump with no overlay) returns nullptr. Save-As
 * uses this to source drive B's live bytes from the FDC, not the legacy
 * driveB struct (which engine=1 writes bypass). */
const uint8_t* fdc_media_image_unit(const Device* dev, uint8_t unit,
                                    size_t& len_out);

/* Remove the disc from a drive. `unit`: 0 = A (default), 1 = B. */
void fdc_eject_disk(const Device* dev, uint8_t unit = 0);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_FDC_H */
