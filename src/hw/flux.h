/* flux.h — Greaseweazle/SuperCard Pro flux-dump → DSK conversion (pure
 * functions). See docs/hardware/flux-media.md.
 *
 * Stage 1 of flux support: an SCP container parser + software-PLL MFM decoder
 * that turns a flux dump of a CPC double-density disc into a standard (or,
 * when sector sizes vary, extended) DSK image in a caller-provided buffer. The
 * existing FDC Device consumes the result unchanged via fdc_attach_disk().
 * Stage 2 (a rotating flux medium inside the FDC) supersedes this offline
 * path for copy-protection/weak-bit fidelity.
 *
 * House style: pure functions, C ABI, NO heap — every buffer is caller-owned;
 * internal scratch is fixed-size and function-local (bounds in the doc §5). */
#ifndef KONCPC_HW_FLUX_H
#define KONCPC_HW_FLUX_H

#include <stddef.h>
#include <cstdint>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes returned (negative) by flux_scp_to_dsk. */
enum : std::int8_t {
  FLUX_E_NOT_SCP = -1,      /* no "SCP" magic / header shorter than 0x2B0    */
  FLUX_E_GEOMETRY = -2,     /* unsupported: bitcell width not 16, 0 revs,
                             * side-1-only dump, start > end track           */
  FLUX_E_TRUNCATED = -3,    /* a track offset / revolution entry / flux run
                             * points outside the provided buffer            */
  FLUX_E_TOO_LONG = -4,     /* a revolution decodes to more bitcells than the
                             * fixed track buffer holds (doc §5)             */
  FLUX_E_DSK_OVERFLOW = -5, /* dsk_cap too small for the converted image     */
  FLUX_E_NO_SECTORS = -6,   /* no track yielded a single valid sector        */
};

/* Weak/suspect-sector report (docs §4). `reason` bits: */
enum : std::uint8_t {
  FLUX_WEAK_CRC = 1,    /* data CRC failed on the revolution-0 reading  */
  FLUX_WEAK_DIFFER = 2, /* payload differs between revolutions 0 and 1  */
};

typedef struct FluxWeakSector {
  uint8_t cyl;       /* physical cylinder (DSK track number)        */
  uint8_t side;      /* always 0 in Stage 1 (side 0 only)           */
  uint8_t sector_id; /* R from the sector's ID field                */
  uint8_t reason;    /* FLUX_WEAK_* bits                            */
} FluxWeakSector;

enum : std::uint8_t { FLUX_WEAK_MAX = 16 };

typedef struct FluxWeakReport {
  int count; /* total flagged sectors (may exceed FLUX_WEAK_MAX) */
  FluxWeakSector sec[FLUX_WEAK_MAX]; /* the first `min(count, MAX)` of them */
} FluxWeakReport;

/* Is this buffer an SCP flux file with sane geometry (magic, 16-bit cells,
 * ≥1 revolution, a readable side-0 track)? 1 = yes, 0 = no. Cheap: header +
 * offset-table inspection only, no flux decoding. */
int flux_scp_probe(const uint8_t* scp, size_t len);

/* --- Stage 3 API: per-track, per-revolution decode for the rotating FDC ---
 * The FDC decodes the track under the head on demand, one captured revolution
 * at a time; serving the revolution whose turn is passing the head is what
 * makes weak/fuzzy bits emerge physically (docs §7). Angular positions are
 * measured from the ACTUAL bitstream (real gaps, real long tracks) in the
 * FDC's byte-cell unit: 0..6249 per revolution (32 µs cells at 300 RPM). */

enum : std::uint8_t {
  FLUX_TRACK_MAX_SECTORS = 29 /* mirrors the DSK Track-Info bound */
};

typedef struct FluxSector {
  uint8_t chrn[4];    /* C, H, R, N from the ID field                     */
  uint8_t st1, st2;   /* DSK-convention status (Data Error / Control Mark)*/
  uint16_t len;       /* stored bytes = 128 << N                          */
  uint32_t off;       /* payload offset in the caller's buffer            */
  uint16_t idam_cell; /* angular byte cell where the ID field completes   */
  uint16_t data_cell; /* angular byte cell of the first data byte         */
} FluxSector;

typedef struct FluxTrack {
  int count;             /* sectors found in this revolution              */
  uint32_t payload_used; /* bytes written to the caller's payload buffer  */
  FluxSector sec[FLUX_TRACK_MAX_SECTORS];
} FluxTrack;

/* Revolutions captured per track in this SCP (0 = not a usable SCP). */
int flux_scp_revolutions(const uint8_t* scp, size_t len);
/* Highest side-0 cylinder present + 1 (0 = not a usable SCP). */
int flux_scp_cylinders(const uint8_t* scp, size_t len);

/* Decode ONE revolution of ONE side-0 cylinder: PLL → MFM → sector map with
 * angular byte-cell positions, payloads into the caller's buffer. An absent /
 * unformatted cylinder yields count = 0 and returns 0 (a head over nothing
 * reads nothing). `rev` beyond the capture count is reduced modulo it — the
 * platter keeps turning, the captures repeat. Returns 0 or a FLUX_E_* code. */
int flux_decode_track_rev(const uint8_t* scp, size_t len, uint8_t cyl,
                          uint8_t rev, FluxTrack* out, uint8_t* payload,
                          size_t payload_cap);

/* The whole pipeline: for each side-0 track present in the SCP, PLL-decode
 * revolution 0 (and revolution 1 when the dump has ≥2 — docs §2/§4) to MFM
 * bitcells, locate the IBM System 34 address marks, CRC-check ID and data
 * fields, and emit a standard DSK — extended when sector sizes vary — into
 * dsk_out. `weak` (optional, may be NULL) receives the weak/suspect-sector
 * report; the DSK always carries the revolution-0 data.
 *
 * Returns the DSK byte size, or a negative FLUX_E_* code. On error dsk_out's
 * contents are unspecified. */
long flux_scp_to_dsk(const uint8_t* scp, size_t scp_len, uint8_t* dsk_out,
                     size_t dsk_cap, FluxWeakReport* weak);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_FLUX_H */
