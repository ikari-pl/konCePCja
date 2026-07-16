/* fdc.cpp — the µPD765A floppy disc controller Device. See
 * docs/hardware/fdc-device.md.
 *
 * Partial decode A10 = 0 AND A7 = 0: A8 = 0 is the motor latch, A8 = 1 the FDC
 * (A0: main status vs data register). Polled non-DMA only — /INT and DRQ are
 * unconnected on the CPC, so nothing but cpu.data is ever driven. The command /
 * execution / result machinery mirrors the legacy core (the golden master); the
 * timing is the documented "fast FDC": every handshake is immediate per access.
 */

#include "fdc.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <new>

#include "flux.h"  // Stage 3: per-track, per-revolution flux decode

namespace {

enum : uint8_t { PH_CMD = 0, PH_EXEC = 1, PH_RESULT = 2, PH_BUSY = 3 };

constexpr int kMaxTracks = 102;  // what fits in a DSK header
constexpr int kMaxSides = 2;
constexpr int kMaxSectors = 29;  // what fits in a Track-Info block

// --- The rotating medium (docs §7): everything exact in 16 MHz master cycles
// ---
constexpr uint32_t kRevCycles = 3200000;  // 200 ms/revolution at 300 RPM
constexpr uint32_t kByteCycles = 512;  // 32 µs/byte cell (DD MFM, 250 kbit/s)
constexpr uint32_t kCellsPerTrack = kRevCycles / kByteCycles;  // 6250
constexpr uint64_t kSpinUpCycles = 8000000;  // 500 ms motor spin-up → ready
constexpr uint32_t kCyclesPerMs = 16000;

// Motor state machine.
enum : uint8_t { M_OFF = 0, M_SPINUP = 1, M_READY = 2 };
// Scheduled (BUSY-phase) completions.
enum : uint8_t {
  P_NONE = 0,
  P_READ_ID = 1,
  P_DATA_START = 2,
  P_FAIL = 3,
  P_FORMAT_DONE = 4
};

// Command-phase bytes (index into cmd[]), µPD765A layout.
enum : uint8_t {
  C_CODE = 0,
  C_UNIT = 1,
  C_C = 2,
  C_H = 3,
  C_R = 4,
  C_N = 5,
  C_EOT = 6,
  C_DTL = 8
};
// Result-phase bytes (index into res[]).
enum : uint8_t {
  R_ST0 = 0,
  R_ST1 = 1,
  R_ST2 = 2,
  R_C = 3,
  R_H = 4,
  R_R = 5,
  R_N = 6
};

// One sector's descriptor: the ID field plus a window into the caller's image,
// plus its angular position on the spinning track (System 34 layout, docs §7).
struct fdc_sector {
  uint8_t chrn[4];  // C, H, R, N as recorded in the ID field
  uint8_t st1,
      st2;          // per-sector status from the DSK (CRC-error emulation etc.)
  uint32_t off;     // absolute offset of the data in the attached image
  uint16_t stored;  // bytes actually stored for this sector
  uint16_t idam_at;  // byte cell where the ID field (incl. CRC) completes
  uint16_t data_at;  // byte cell where the first data byte passes the head
};

struct fdc_track {
  uint8_t sectors = 0;    // 0 = unformatted
  uint32_t data_off = 0;  // absolute offset of the track's data block
  uint32_t data_len =
      0;  // total stored bytes (transfer overruns wrap inside it)
  fdc_sector sec[kMaxSectors] = {};
};

constexpr size_t kFluxPayload = 8192;  // one DD revolution carries ~6250 bytes
constexpr int kFluxRevs = 2;           // revolutions cached per track

// The caller attachment + parsed geometry: live wiring (docs §5), never
// serialized. MUST stay the LAST member of fdc_state — save/load cover only the
// bytes before it (offsetof(fdc_state, media)).
struct fdc_media {
  uint8_t* image = nullptr;  // caller-owned DSK buffer, mutated by writes (§10)
  size_t len = 0;
  bool dirty = false;  // buffer diverged from its file since attach/mark_clean
  uint8_t tracks = 0;
  uint8_t sides = 0;  // 1 or 2
  fdc_track track[kMaxTracks][kMaxSides];

  // Flux backend (Stage 3): when scp != nullptr it replaces the DSK image. The
  // track under the head is decoded on demand into a per-revolution cache; the
  // serving revolution follows the platter (docs flux-media.md §7).
  const uint8_t* scp = nullptr;  // caller-owned SCP dump
  size_t scp_len = 0;
  uint8_t scp_revs = 0;      // revolutions captured in the file
  uint8_t cache_cyl = 0xFF;  // cylinder currently cached (0xFF = none)
  uint8_t cache_revs = 1;    // revolutions represented in the cache (1..2)
  fdc_track cache_trk[kFluxRevs];
  uint8_t cache_pay[kFluxRevs][kFluxPayload];

  // Hybrid writable-flux state (Stage 2). backing == FDC_BACKING_FLUX marks a
  // flux medium; a written track flips track_dirty[t] and serves `image` (the
  // synthesized DSK overlay) from then on, while clean tracks keep serving the
  // rotating cache above. Both members sit AFTER kSaveBytes (media is excluded
  // from the state hash) and are value-initialized — never serialized.
  uint8_t backing = FDC_BACKING_SECTOR;
  bool track_dirty[kMaxTracks] = {};  // per-cylinder: written → serve `image`
};

bool flux_backed(const fdc_media* m) { return m->scp != nullptr; }

// Does cylinder `t` read from the rotating flux cache (vs the DSK `image`)? A
// flux medium serves the cache for every track that has NOT been written; a
// written (dirty) track — and any sector-backed medium — serves `image`. This
// is the whole hybrid: untouched protected tracks stay lossless, written tracks
// become standard MFM (docs flux-media.md §7).
bool serve_from_flux(const fdc_media* m, uint8_t t) {
  return m->backing == FDC_BACKING_FLUX && t < kMaxTracks && !m->track_dirty[t];
}
bool parse_dsk(fdc_media* m, uint8_t* p, size_t len);  // defined below

struct fdc_state {
  uint8_t phase = PH_CMD;
  uint8_t cmd[9] = {0};  // command-phase accumulator (opcode + parameters)
  uint8_t cmd_len = 0, cmd_count = 0;
  uint8_t mt = 0, mf = 0,
          sk = 0;        // opcode modifier bits (accepted, not modeled)
  uint8_t res[7] = {0};  // result-phase buffer
  uint8_t res_len = 0, res_count = 0;
  bool dir_to_cpu = false;  // execution-phase direction (DIO while EXM)
  bool motor = false;       // the any-drive motor latch (as written by the CPU)
  uint8_t track_pos[2] = {0, 0};  // physical head position per unit (mechanics)
  uint8_t sector_idx = 0;  // rotational index on the current track (unit 0)
  bool seek_done[2] = {false, false};
  bool status_changed[2] = {false, false};
  uint8_t srt_hut = 0,
          hlt_nd = 0;  // SPECIFY latches (SRT drives the seek time)
  // In-flight execution-phase transfer window (absolute image offsets).
  uint32_t data_pos = 0, remaining = 0;
  uint32_t trk_begin = 0, trk_end = 0;
  // --- the rotating medium (docs §7) ---
  uint64_t now = 0;          // master-cycle clock (advances every tick)
  uint32_t rot = 0;          // rotation phase in master cycles [0, kRevCycles)
  uint32_t rev_count = 0;    // completed revolutions (selects the flux capture)
  uint8_t from_flux = 0;     // current transfer reads the flux cache
  uint8_t serve_rev = 0;     // which cached revolution the transfer serves
  uint8_t motor_st = M_OFF;  // OFF → SPINUP → READY
  uint64_t motor_ready_at = 0;  // spin-up completion time
  bool seeking[2] = {false, false};
  uint8_t seek_target[2] = {0, 0};
  uint64_t next_step_at[2] = {0,
                              0};  // per-step engine: when the next STEP fires
  uint64_t step_period[2] = {0, 0};  // (16 - SRT) * 2 ms, latched per seek
  uint8_t pending = P_NONE;          // scheduled completion while PH_BUSY
  uint8_t pend_idx = 0;              // sector index the pending op targets
  uint64_t due_at = 0;  // when the pending op completes / data starts
  uint64_t next_byte_at =
      0;  // execution phase: when the current byte is readable
  uint8_t out_latch = 0xFF;  // byte held across the master cycles of one access
  bool access_prev = false;  // previous cycle was an owned data/motor access
  uint8_t st_latch[3] = {0, 0, 0};  // ST0..ST2 of the last completed operation
  uint32_t sectors_read = 0;  // introspection: sectors delivered by READ DATA
  // --- writes + FORMAT (docs §10) ---
  uint8_t wr_deleted = 0;   // in-flight write is WRITE DELETED (sets ST2 CM)
  uint32_t wr_st2_off = 0;  // image offset of the sector's Track-Info ST2 byte
  uint8_t fmt_ids[4 * kMaxSectors] = {0};  // FORMAT: collected C/H/R/N stream
  uint8_t fmt_pos = 0;                     // bytes of fmt_ids received

  fdc_media media;   // live wiring — everything from here on is NOT serialized
  fdc_media media1;  // drive B (unit 1) image; attach-wired, not yet read (§6)

  // Mechanical event ring (fdc.h): live telemetry for the audio bridge.
  // Sits AFTER `media`, outside the kSaveBytes prefix. Drop-oldest overflow.
  FdcEvent ev[64];
  uint8_t ev_head = 0, ev_count = 0;
};

fdc_state* self_of(void* self) { return static_cast<fdc_state*>(self); }

// Log a mechanical event for the audio bridge (drop-oldest on overflow).
void push_event(fdc_state* f, uint8_t type, uint8_t arg) {
  constexpr uint8_t kCap = 64;
  if (f->ev_count == kCap) {  // full: sacrifice the oldest
    f->ev_head = static_cast<uint8_t>((f->ev_head + 1) % kCap);
    f->ev_count--;
  }
  f->ev[(f->ev_head + f->ev_count) % kCap] = FdcEvent{f->now, type, arg};
  f->ev_count++;
}

// ---------------------------------------------------------------- helpers ---

// The DSK medium behind the addressed unit: 0 = drive A (media), 1 = drive B
// (media1). The µPD765A drives two units and the CPC wires both; flux capture
// stays drive-A-only (fdc_attach_flux only ever fills `media`, so media1 is
// never flux_backed — the flux branches below are unreachable for unit 1).
fdc_media* sel_media(fdc_state* f, uint8_t unit) {
  return (unit & 1) ? &f->media1 : &f->media;
}
const fdc_media* sel_media(const fdc_state* f, uint8_t unit) {
  return (unit & 1) ? &f->media1 : &f->media;
}

bool drive_ready(const fdc_state* f, uint8_t unit) {
  // Either drive can hold media now; ready = disc in + motor SPUN UP (docs
  // §6/§7 — the latch alone is not enough, the platter takes ~500 ms to reach
  // speed; the one motor line spins whichever unit is selected).
  if (unit > 1)
    return false;  // the CPC disc interface wires exactly two drives
  const fdc_media* m = sel_media(f, unit);
  const bool disc_in = (m->image != nullptr || flux_backed(m)) && m->tracks > 0;
  return disc_in && f->motor_st == M_READY;
}

// Decode the cylinder under the head into the per-revolution cache (flux
// backend). Called lazily whenever the map is needed; a no-op when the cache
// already holds this cylinder. Decode errors leave an unformatted track.
void ensure_flux_cache(fdc_state* f) {
  fdc_media* m = &f->media;
  const uint8_t cyl = f->track_pos[0];
  if (m->cache_cyl == cyl) return;
  m->cache_revs = (m->scp_revs >= kFluxRevs) ? kFluxRevs : 1;
  for (uint8_t r = 0; r < m->cache_revs; ++r) {
    fdc_track* trk = &m->cache_trk[r];
    *trk = fdc_track{};
    FluxTrack ft;
    if (flux_decode_track_rev(m->scp, m->scp_len, cyl, r, &ft, m->cache_pay[r],
                              kFluxPayload) != 0)
      continue;  // unreadable: nothing under the head this revolution
    trk->sectors =
        static_cast<uint8_t>(ft.count < kMaxSectors ? ft.count : kMaxSectors);
    trk->data_off = 0;  // offsets are relative to the payload
    trk->data_len = ft.payload_used;
    for (int i = 0; i < trk->sectors; ++i) {
      const FluxSector* fs = &ft.sec[i];
      fdc_sector* sec = &trk->sec[i];
      std::memcpy(sec->chrn, fs->chrn, 4);
      sec->st1 = fs->st1;
      sec->st2 = fs->st2;
      sec->off = fs->off;
      sec->stored = fs->len;
      sec->idam_at = fs->idam_cell;  // angles measured from the real bitstream
      sec->data_at = fs->data_cell;
    }
  }
  m->cache_cyl = cyl;
}

// The cached revolution passing the head right now (flux backend).
uint8_t passing_rev(const fdc_state* f) {
  return static_cast<uint8_t>(f->rev_count % f->media.cache_revs);
}

// The revolution that will be passing when byte cell `cell` next arrives
// (crossing the index hole advances the capture — the platter keeps turning).
uint8_t rev_at_cell(const fdc_state* f, uint16_t cell) {
  const uint32_t target = static_cast<uint32_t>(cell) * kByteCycles;
  uint8_t rev = passing_rev(f);
  if (target <= f->rot)
    rev = static_cast<uint8_t>((rev + 1) % f->media.cache_revs);
  return rev;
}

// --- Angular position helpers (docs §7) ------------------------------------

// Master cycles until byte cell `cell` next passes under the head (a full
// revolution if it is passing right now).
uint64_t cycles_until_cell(const fdc_state* f, uint16_t cell) {
  const uint32_t target = static_cast<uint32_t>(cell) * kByteCycles;
  const uint32_t delta =
      (target >= f->rot) ? target - f->rot : kRevCycles - f->rot + target;
  return delta ? delta : kRevCycles;
}

// Index of the sector whose ID field arrives next (angularly) on `trk`.
uint8_t next_idam_index(const fdc_state* f, const fdc_track* trk) {
  uint8_t best = 0;
  uint64_t best_dt = ~0ull;
  for (uint8_t i = 0; i < trk->sectors; ++i) {
    const uint64_t dt = cycles_until_cell(f, trk->sec[i].idam_at);
    if (dt < best_dt) {
      best_dt = dt;
      best = i;
    }
  }
  return best;
}

// The track under the head of the addressed unit, or nullptr if the head sits
// past the image / the track is unformatted. Single-sided images ignore HD.
// Flux backend: the on-demand cache of the currently-passing revolution.
fdc_track* head_track(fdc_state* f, uint8_t unit, uint8_t head) {
  if (unit > 1) return nullptr;
  fdc_media* m = sel_media(f, unit);
  const uint8_t t = f->track_pos[unit];
  if (serve_from_flux(m,
                      t)) {  // drive-A-only: media1 is never flux (sel_media)
    if (t >= m->tracks) return nullptr;
    ensure_flux_cache(f);
    fdc_track* trk = &m->cache_trk[passing_rev(f)];
    return trk->sectors ? trk : nullptr;
  }
  if (m->image == nullptr) return nullptr;
  if (t >= m->tracks) return nullptr;
  const uint8_t side = (m->sides > 1) ? (head & 1) : 0;
  fdc_track* trk = &m->track[t][side];
  return trk->sectors ? trk : nullptr;
}

// Clear the result buffer and preload ST0 with unit/head + Abnormal/Not-Ready
// when the drive is not ready. Returns readiness (the legacy init_status_regs).
bool init_status(fdc_state* f, uint8_t unit) {
  std::memset(f->res, 0, sizeof(f->res));
  uint8_t st0 = f->cmd[C_UNIT] & 7;
  const bool ready = drive_ready(f, unit);
  if (!ready) st0 |= 0x48;  // Abnormal Termination + Not Ready
  f->res[R_ST0] = st0;
  return ready;
}

// GOLDEN-MASTER DIVERGENCE A (docs §4b, beads-7mo4): the result CHRN is the
// command's CHRN as it stands, so a read that ran to EOT reports R = EOT — NOT
// the datasheet Table-2 cylinder rollover (C+1, R=01). That rollover is
// specified for *Terminal-Count*-terminated reads; the CPC has no TC line and
// always terminates abnormally with EN, a path the primary sources do not
// unambiguously resolve. AMSDOS never reads this R (success = ST-bytes;
// disc-change = READ ID), so we keep the oracle value rather than guess.
void load_result_chrn(fdc_state* f) {
  f->res[R_C] = f->cmd[C_C];
  f->res[R_H] = f->cmd[C_H];
  f->res[R_R] = f->cmd[C_R];
  f->res[R_N] = f->cmd[C_N];
}

void enter_result(fdc_state* f) {
  f->phase = PH_RESULT;
  f->res_count = 0;
  f->st_latch[0] = f->res[R_ST0];
  f->st_latch[1] = f->res[R_ST1];
  f->st_latch[2] = f->res[R_ST2];
}

void enter_cmd(fdc_state* f) {
  f->phase = PH_CMD;
  f->cmd_count = 0;
}

// Normal end-of-transfer termination: AT + End-of-Cylinder, then GOLDEN-MASTER
// DIVERGENCE C (docs §4b, beads-7mo4): the datasheet defines no result-code
// masking; this priority (error over cylinder-end; data-error over control
// mark) is Caprice32's synthesis, which AMSDOS's retry path was validated
// against. Keep it verbatim.
void finish_with_status(fdc_state* f) {
  f->res[R_ST0] |= 0x40;                                   // AT
  f->res[R_ST1] |= 0x80;                                   // End of Cylinder
  if ((f->res[R_ST1] & 0x7F) || (f->res[R_ST2] & 0x7F)) {  // real error bits?
    f->res[R_ST1] &= 0x7F;  // the error, not the cylinder end, is the story
    if ((f->res[R_ST1] & 0x20) || (f->res[R_ST2] & 0x20)) {  // DE and/or DD?
      f->res[R_ST2] &= 0xBF;            // mask out Control Mark
    } else if (f->res[R_ST2] & 0x40) {  // Control Mark only?
      f->res[R_ST0] &= 0x3F;            // not abnormal
      f->res[R_ST1] &= 0x7F;
    }
  }
  load_result_chrn(f);
  enter_result(f);
}

// Locate the sector whose ID matches the command's C/H/R/N on `trk`, starting
// at the rotational index, at most two index passes. Accumulates ST2 Bad/No
// Cylinder along the way and leaves the rotational index where the scan ended
// (the legacy find_sector).
fdc_sector* find_sector(fdc_state* f, fdc_track* trk) {
  fdc_sector* found = nullptr;
  int loops = 0;
  uint32_t idx = f->sector_idx;
  if (idx >= trk->sectors) idx = 0;
  do {
    if (std::memcmp(trk->sec[idx].chrn, &f->cmd[C_C], 4) == 0) {
      found = &trk->sec[idx];
      f->res[R_ST2] &= static_cast<uint8_t>(~(0x02 | 0x10));  // clear BC + NC
      break;
    }
    const uint8_t cyl = trk->sec[idx].chrn[0];
    if (cyl == 0xFF)
      f->res[R_ST2] |= 0x02;  // Bad Cylinder
    else if (cyl != f->cmd[C_C])
      f->res[R_ST2] |= 0x10;  // No Cylinder
    if (++idx >= trk->sectors) {
      idx = 0;
      loops++;
    }
  } while (loops < 2);
  if (f->res[R_ST2] & 0x02) f->res[R_ST2] &= static_cast<uint8_t>(~0x10);
  f->sector_idx = static_cast<uint8_t>(idx);
  return found;
}

// Transfer length demanded by the command: 128 << N, or DTL (capped at 128)
// when N = 0 (the legacy rule).
uint32_t xfer_length(const fdc_state* f) {
  if (f->cmd[C_N] == 0) {
    uint32_t const dtl = f->cmd[C_DTL];
    return dtl > 0x80 ? 0x80 : dtl;
  }
  return 128u << (f->cmd[C_N] & 0x07);
}

// Start (or chain to) the transfer of the sector the command currently names.
// The scan begins at the current angular position; a miss is reported as No
// Data only after two index passes (the real search bound). On a hit the
// transfer begins when the sector's data field reaches the head. `to_cpu`
// selects the direction: reads serve bytes, writes (docs §10) consume them.
void start_sector(fdc_state* f, fdc_track* trk, bool to_cpu) {
  f->sector_idx = next_idam_index(f, trk);  // scan from the head's position
  fdc_sector* sec = find_sector(f, trk);
  if (sec == nullptr) {
    f->res[R_ST0] |= 0x40;  // AT
    f->res[R_ST1] |= 0x04;  // No Data
    load_result_chrn(f);
    f->pending = P_FAIL;
    f->due_at = f->now + (2ull * kRevCycles);  // two index pulses of searching
    f->phase = PH_BUSY;
    return;
  }
  const uint8_t sunit = f->cmd[C_UNIT] & 1;
  f->from_flux =
      serve_from_flux(sel_media(f, sunit), f->track_pos[sunit]) ? 1 : 0;
  if (!to_cpu) {
    // Writes are DSK-only (the dispatcher rejects flux with NW). Remember
    // where this sector's Track-Info ST2 byte lives so WRITE DELETED can set
    // the Control Mark in the image itself (docs §10).
    const uint32_t track_info = trk->data_off - 0x100;
    f->wr_st2_off = track_info + 0x18 + (8u * f->sector_idx) + 5;
  }
  if (f->from_flux) {
    // Serve the revolution that will be passing when the data field arrives —
    // the physical substrate of weak/fuzzy protection bits (flux-media.md §7):
    // re-reads land on different captures and return their differing bytes,
    // with each capture's own CRC status.
    f->serve_rev = rev_at_cell(f, sec->data_at);
    fdc_track* strk = &f->media.cache_trk[f->serve_rev];
    for (int i = 0; i < strk->sectors; ++i) {
      if (std::memcmp(strk->sec[i].chrn, sec->chrn, 4) == 0) {
        sec = &strk->sec[i];  // this revolution's copy: payload + status
        trk = strk;
        break;
      }  // absent on that capture: fall back to the scanned revolution's copy
    }
  }
  f->res[R_ST1] = sec->st1 & 0x25;  // DE / ND / MA (per revolution when flux)
  f->res[R_ST2] =
      static_cast<uint8_t>((f->res[R_ST2] & 0x12) | (sec->st2 & 0x61));
  f->data_pos = sec->off;
  f->remaining = xfer_length(f);
  f->trk_begin = trk->data_off;
  f->trk_end = trk->data_off + trk->data_len;
  f->dir_to_cpu = to_cpu;
  f->pending = P_DATA_START;
  f->due_at =
      f->now + cycles_until_cell(f, sec->data_at);  // rotational latency
  f->phase = PH_BUSY;
}

// FORMAT TRACK commit (docs §10): the index hole arrived with the ID stream
// complete — rewrite the track inside its existing DSK block and re-parse.
// A layout the block cannot hold terminates NW with the image untouched.
void do_format(fdc_state* f) {
  const uint8_t unit = f->cmd[C_UNIT] & 1;
  fdc_media* m = sel_media(f, unit);
  const uint8_t head = (f->cmd[C_UNIT] >> 2) & 1;
  const uint8_t n = f->cmd[2], sc = f->cmd[3], gpl = f->cmd[4],
                fill = f->cmd[5];
  const uint8_t t = f->track_pos[unit];
  const uint8_t side = (m->sides > 1) ? head : 0;
  fdc_track const* trk =
      (t < m->tracks && m->image != nullptr) ? &m->track[t][side] : nullptr;
  const uint32_t sec_size = 128u << n;

  // Capacity of the allocated track block, from the DSK header itself.
  uint32_t block = 0;
  if (m->image != nullptr && trk != nullptr && trk->data_off != 0) {
    if (std::memcmp(m->image, "EXTENDED", 8) == 0)
      block = static_cast<uint32_t>(m->image[0x34 + (t * m->sides) + side])
              << 8;
    else
      block = static_cast<uint32_t>(m->image[0x32]) | (m->image[0x33] << 8);
  }
  const bool fits = block > 0x100 && sc * sec_size <= block - 0x100;
  // Standard images size every sector by the track's shared code (docs §5):
  // the new N must keep that rule or the parse would lie about the windows.
  const bool std_ok =
      m->image != nullptr && (std::memcmp(m->image, "EXTENDED", 8) == 0 ||
                              (trk != nullptr && trk->data_off != 0 &&
                               m->image[trk->data_off - 0x100 + 0x14] == n));
  if (trk == nullptr || trk->data_off == 0 || !fits || !std_ok) {
    f->res[R_ST0] |= 0x40;  // AT
    f->res[R_ST1] |= 0x02;  // Not Writable: the container cannot hold it
    enter_result(f);
    return;
  }

  // Rewrite the Track-Info block in place, fill the data area, re-parse.
  uint8_t* th = m->image + (trk->data_off - 0x100);
  th[0x14] = n;
  th[0x15] = sc;
  th[0x16] = gpl;
  th[0x17] = fill;
  for (uint8_t i = 0; i < kMaxSectors; ++i) {
    uint8_t* si = th + 0x18 + (8 * i);
    if (i < sc) {
      std::memcpy(si, &f->fmt_ids[4 * i], 4);
      si[4] = si[5] = 0;  // fresh format: no error state
      si[6] = static_cast<uint8_t>(sec_size & 0xFF);
      si[7] = static_cast<uint8_t>(sec_size >> 8);
    } else {
      std::memset(si, 0, 8);
    }
  }
  std::memset(m->image + trk->data_off, fill, sc * sec_size);
  uint8_t* image = m->image;
  const size_t len = m->len;
  // parse_dsk clears the whole medium; on a flux-backed overlay preserve the
  // pristine SCP + dirty map across the rebuild, then promote this track (it is
  // now standard MFM and must serve `image`, not the stale rotating cache).
  const uint8_t backing = m->backing;
  const uint8_t* saved_scp = m->scp;
  const size_t saved_scp_len = m->scp_len;
  const uint8_t saved_scp_revs = m->scp_revs;
  bool saved_dirty[kMaxTracks];
  std::memcpy(saved_dirty, m->track_dirty, sizeof(saved_dirty));
  parse_dsk(m, image, len);  // rebuild every window and angle
  if (backing == FDC_BACKING_FLUX) {
    m->backing = backing;
    m->scp = saved_scp;
    m->scp_len = saved_scp_len;
    m->scp_revs = saved_scp_revs;
    m->cache_cyl = 0xFF;  // force a re-decode if a clean track is read again
    std::memcpy(m->track_dirty, saved_dirty, sizeof(saved_dirty));
    if (t < kMaxTracks) m->track_dirty[t] = true;
  }
  m->dirty = true;
  f->sector_idx = 0;
  f->res[R_ST0] = f->cmd[C_UNIT] & 7;  // normal termination
  f->res[R_C] = f->track_pos[unit];
  f->res[R_H] = head;
  f->res[R_N] = n;
  enter_result(f);
}

// A scheduled (PH_BUSY) operation's moment has arrived: the target passed under
// the head (or the search exhausted two index pulses).
void complete_pending(fdc_state* f) {
  const uint8_t op = f->pending;
  f->pending = P_NONE;
  switch (op) {
    case P_READ_ID: {
      fdc_track* trk =
          head_track(f, f->cmd[C_UNIT] & 1, (f->cmd[C_UNIT] >> 2) & 1);
      if (trk && f->pend_idx < trk->sectors) {
        std::memcpy(&f->res[R_C], trk->sec[f->pend_idx].chrn, 4);
        f->sector_idx = static_cast<uint8_t>(f->pend_idx + 1);
      }
      enter_result(f);
      break;
    }
    case P_DATA_START:
      f->phase = PH_EXEC;  // first byte is under the head right now
      f->next_byte_at = f->now;
      break;
    case P_FORMAT_DONE:
      do_format(f);  // the index hole arrived: commit the new track layout
      break;
    case P_FAIL:  // result bytes were prepared at dispatch time
    default:
      enter_result(f);
      break;
  }
}

// ------------------------------------------------------------- dispatch -----

void cmd_specify(fdc_state* f) {
  f->srt_hut = f->cmd[1];
  f->hlt_nd = f->cmd[2];
  enter_cmd(f);  // no result phase
}

void cmd_drive_status(fdc_state* f) {
  // ST3 is, on the real µPD765A, a straight pin-image of the addressed drive's
  // status lines (datasheet §"Status Register 3" + the FLT/TRKO, WPRT/2SIDE,
  // SIDE pin descriptions). We reproduce those semantics, NOT the legacy core's
  // fabricated "WP+two-sided when no disc" quirk (docs beads-7mo4 / §4):
  //   D7 FT   fault  — the CPC never wires the drive fault line → 0
  //   D6 WP   write-protect line — set when the media cannot be written
  //   D5 RY   ready  — disc present and up to speed
  //   D4 T0   track-0 sensor
  //   D3 TS   two-side line — the CPC's 3" drive is single-sided → 0
  //   D2..D0  HD/US1/US0 echoed from the command (the side/unit-select outputs)
  const uint8_t unit = f->cmd[C_UNIT] & 1;
  const fdc_media* m = sel_media(f, unit);
  // Disc present = a DSK overlay OR a flux dump; writable = an `image` overlay
  // exists (a plain read-only flux dump has none → write-protected).
  const bool has_disc =
      (m->image != nullptr || flux_backed(m)) && m->tracks > 0;
  const bool writable = m->image != nullptr && m->tracks > 0;
  uint8_t st3 = f->cmd[C_UNIT] & 0x07;                  // HD, US1, US0
  if (!writable) st3 |= 0x40;                           // WP (also no disc)
  if (has_disc && f->motor_st == M_READY) st3 |= 0x20;  // RY
  if (f->track_pos[unit] == 0) st3 |= 0x10;             // T0
  f->res[R_ST0] = st3;  // single result byte = ST3
  enter_result(f);
}

void cmd_seek_to(fdc_state* f, uint8_t target) {
  const uint8_t unit = f->cmd[C_UNIT] & 1;
  if (init_status(f, unit)) {  // head only moves on a ready drive
    const uint8_t tgt = (target >= kMaxTracks) ? (kMaxTracks - 1) : target;
    const uint8_t cur = f->track_pos[unit];
    const uint8_t delta = (tgt > cur) ? (tgt - cur) : (cur - tgt);
    if (delta == 0) {
      f->seek_done[unit] = true;  // already there: seek-end at once
    } else {
      // Step time from SPECIFY's SRT nibble: (16 − SRT) ms at the datasheet's
      // 8 MHz clock, doubled at the CPC's 4 MHz — AMSDOS's SRT = 0xA → 12 ms.
      // The head now steps ONE TRACK PER PERIOD (mid-seek position observable;
      // each step logs an FDC_EV_STEP for the audio bridge).
      const uint32_t srt = (f->srt_hut >> 4) & 0x0F;
      f->seeking[unit] = true;
      f->seek_target[unit] = tgt;
      f->step_period[unit] = static_cast<uint64_t>(16 - srt) * 2 * kCyclesPerMs;
      f->next_step_at[unit] = f->now + f->step_period[unit];
    }
  } else {
    f->seek_done[unit] = true;  // not ready: "completes" carrying the NR status
  }
  enter_cmd(f);  // no result phase; completion is polled via SENSE INTERRUPT
}

void cmd_sense_interrupt(fdc_state* f) {
  // The lingering ST0 from the seek carries its AT/NR bits only (legacy
  // behaviour: SEEK writes the result buffer, SENSE INTERRUPT reads it back).
  // Anything else — e.g. the 0x80 a mid-seek "nothing pending" reply left
  // behind — must not leak into the eventual seek-end report.
  uint8_t st0 = f->res[R_ST0] & 0x48;
  if (f->seek_done[0]) {
    f->seek_done[0] = f->status_changed[0] = false;
    f->res[R_ST0] = static_cast<uint8_t>(st0 | 0x20);  // Seek End, unit 0
    f->res[R_ST1] = f->track_pos[0];                   // PCN
  } else if (f->seek_done[1]) {
    f->seek_done[1] = f->status_changed[1] = false;
    f->res[R_ST0] = static_cast<uint8_t>(st0 | 0x21);  // Seek End, unit 1
    f->res[R_ST1] = f->track_pos[1];
  } else if (f->status_changed[0] || f->status_changed[1]) {
    const uint8_t unit = f->status_changed[0] ? 0 : 1;
    f->status_changed[unit] = false;
    st0 = static_cast<uint8_t>(0xC0 | unit);  // Ready changed
    if (!drive_ready(f, unit)) st0 |= 0x08;
    f->res[R_ST0] = st0;
    f->res[R_ST1] = f->track_pos[unit];
  } else {
    f->res[R_ST0] = 0x80;  // nothing pending: Invalid Command
    f->res_len = 1;
  }
  enter_result(f);
}

void cmd_read_id(fdc_state* f) {
  const uint8_t unit = f->cmd[C_UNIT] & 1;
  if (init_status(f, unit)) {
    fdc_track const* trk = head_track(f, unit, (f->cmd[C_UNIT] >> 2) & 1);
    if (trk) {
      // Complete when the NEXT ID field passes under the head (rotational).
      f->pend_idx = next_idam_index(f, trk);
      f->pending = P_READ_ID;
      f->due_at = f->now + cycles_until_cell(f, trk->sec[f->pend_idx].idam_at);
      f->phase = PH_BUSY;
      return;
    }
    // Unformatted / absent track: Missing Address Mark, reported only after the
    // chip has searched two index pulses' worth of nothing.
    f->res[R_ST0] |= 0x40;  // AT
    f->res[R_ST1] |= 0x01;  // MA
    f->res[R_C] = f->track_pos[unit];
    f->res[R_H] = (f->cmd[C_UNIT] >> 2) & 1;
    f->pending = P_FAIL;
    f->due_at = f->now + (2ull * kRevCycles);
    f->phase = PH_BUSY;
    return;
  }
  f->res[R_C] = f->track_pos[unit];
  f->res[R_H] = (f->cmd[C_UNIT] >> 2) & 1;
  enter_result(f);  // not ready: immediate NR result
}

void cmd_read_data(fdc_state* f) {
  const uint8_t unit = f->cmd[C_UNIT] & 1;
  if (!init_status(f, unit)) {  // drive not ready
    load_result_chrn(f);
    enter_result(f);
    return;
  }
  fdc_track* trk = head_track(f, unit, (f->cmd[C_UNIT] >> 2) & 1);
  if (trk == nullptr) {     // unformatted track
    f->res[R_ST0] |= 0x40;  // AT
    f->res[R_ST1] |= 0x01;  // Missing Address Mark
    load_result_chrn(f);
    enter_result(f);
    return;
  }
  start_sector(f, trk, true);
}

// Promote the cylinder under the head of `unit` from flux to the DSK overlay:
// the FDC is about to write it, so from now on it serves `image` (standard
// MFM), not the rotating flux cache — a real drive erases a track's flux fuzz
// when it writes it, too. A no-op on sector-backed or read-only media.
void promote_flux_track(fdc_state* f, uint8_t unit) {
  fdc_media* m = sel_media(f, unit);
  if (m->backing != FDC_BACKING_FLUX) return;
  const uint8_t t = f->track_pos[unit];
  if (t < kMaxTracks) m->track_dirty[t] = true;
}

// WRITE DATA / WRITE DELETED DATA (docs §10): the read machinery with the
// direction reversed. Writable media only — a medium with no DSK image (a
// plain read-only flux dump) terminates Not Writable, image untouched.
void cmd_write_data(fdc_state* f) {
  const uint8_t unit = f->cmd[C_UNIT] & 1;
  f->wr_deleted = (f->cmd[C_CODE] == 0x09) ? 1 : 0;
  if (!init_status(f, unit)) {  // drive not ready
    load_result_chrn(f);
    enter_result(f);
    return;
  }
  if (sel_media(f, unit)->image == nullptr) {  // no writable overlay
    f->res[R_ST0] |= 0x40;                     // AT
    f->res[R_ST1] |= 0x02;                     // Not Writable
    load_result_chrn(f);
    enter_result(f);
    return;
  }
  // A flux track becomes the DSK overlay the moment the head writes it. The
  // overlay's rev-0 layout equals the flux cache's (same source flux), so the
  // addressed sector exists in `image` here; a missing one falls through to the
  // Missing-Address-Mark path below exactly as a sector-backed miss would.
  promote_flux_track(f, unit);
  fdc_track* trk = head_track(f, unit, (f->cmd[C_UNIT] >> 2) & 1);
  if (trk == nullptr) {     // unformatted track (the golden master's rule)
    f->res[R_ST0] |= 0x40;  // AT
    f->res[R_ST1] |= 0x01;  // Missing Address Mark
    load_result_chrn(f);
    enter_result(f);
    return;
  }
  start_sector(f, trk, false);
}

void cmd_read_deleted_stub(fdc_state* f) {
  init_status(f, f->cmd[C_UNIT] & 1);
  f->res[R_ST0] |= 0x40;  // AT
  f->res[R_ST1] |= 0x04;  // No Data (no deleted-mark model in V1)
  load_result_chrn(f);
  enter_result(f);
}

// FORMAT TRACK (docs §10): consume SC×4 ID bytes in the execution phase, then
// rewrite the track inside its existing DSK block at the next index hole.
void cmd_format_track(fdc_state* f) {
  const uint8_t unit = f->cmd[C_UNIT] & 1;
  f->res[R_C] = f->track_pos[unit];
  f->res[R_H] = (f->cmd[C_UNIT] >> 2) & 1;
  f->res[R_N] = f->cmd[2];  // N from the FORMAT parameter block
  if (!init_status(f, unit)) {
    enter_result(f);
    return;
  }
  f->res[R_C] = f->track_pos[unit];  // init_status cleared the buffer
  f->res[R_H] = (f->cmd[C_UNIT] >> 2) & 1;
  f->res[R_N] = f->cmd[2];
  const uint8_t sc = f->cmd[3];
  if (sel_media(f, unit)->image == nullptr || sc == 0 || sc > kMaxSectors ||
      (f->cmd[2] & 0x07) != f->cmd[2]) {
    f->res[R_ST0] |= 0x40;  // AT
    f->res[R_ST1] |= 0x02;  // Not Writable (no overlay / a layout no DSK holds)
    enter_result(f);
    return;
  }
  f->fmt_pos = 0;
  f->dir_to_cpu = false;
  f->phase = PH_EXEC;  // the CPU feeds C/H/R/N per sector, byte-cell paced
  f->next_byte_at = f->now;
}

// The V1 command set: opcode (low 5 bits) → command/result byte counts.
struct cmd_info {
  uint8_t op, cmd_bytes, res_bytes;
  void (*handler)(fdc_state*);
};
constexpr cmd_info kCommands[] = {
    {0x03, 3, 0, cmd_specify},            // SPECIFY
    {0x04, 2, 1, cmd_drive_status},       // SENSE DRIVE STATUS
    {0x05, 9, 7, cmd_write_data},         // WRITE DATA (docs §10)
    {0x06, 9, 7, cmd_read_data},          // READ DATA
    {0x07, 2, 0, nullptr},                // RECALIBRATE (dispatched specially)
    {0x08, 1, 2, cmd_sense_interrupt},    // SENSE INTERRUPT STATUS
    {0x09, 9, 7, cmd_write_data},         // WRITE DELETED DATA (§10)
    {0x0A, 2, 7, cmd_read_id},            // READ ID
    {0x0C, 9, 7, cmd_read_deleted_stub},  // READ DELETED DATA (stub: ND)
    {0x0D, 6, 7, cmd_format_track},       // FORMAT TRACK (docs §10)
    {0x0F, 3, 0, nullptr},                // SEEK (dispatched specially)
};

const cmd_info* lookup_cmd(uint8_t op) {
  for (const cmd_info& c : kCommands)
    if (c.op == op) return &c;
  return nullptr;  // READ TRACK, SCANs and true garbage → invalid command
}

void dispatch(fdc_state* f) {
  switch (f->cmd[C_CODE]) {
    case 0x07:
      cmd_seek_to(f, 0);
      break;  // RECALIBRATE
    case 0x0F:
      cmd_seek_to(f, f->cmd[C_C]);
      break;  // SEEK
    default:
      lookup_cmd(f->cmd[C_CODE])->handler(f);
      break;
  }
}

// ------------------------------------------------- the data-register FSM ----

uint8_t msr_value(const fdc_state* f) {
  // D0B/D1B: per-unit seek-busy bits, visible in every phase.
  const uint8_t seek_bits = static_cast<uint8_t>((f->seeking[0] ? 0x01 : 0) |
                                                 (f->seeking[1] ? 0x02 : 0));
  switch (f->phase) {
    case PH_EXEC: {
      // RQM only while the current byte cell has passed the head (32 µs
      // pacing).
      uint8_t v = static_cast<uint8_t>(0x30 | (f->dir_to_cpu ? 0x40 : 0x00));
      if (f->now >= f->next_byte_at) v |= 0x80;
      return static_cast<uint8_t>(v | seek_bits);
    }
    case PH_RESULT:
      return static_cast<uint8_t>(0xD0 | seek_bits);  // RQM + DIO + CB
    case PH_BUSY:
      return static_cast<uint8_t>(0x10 |
                                  seek_bits);  // CB only: mechanics turning
    default:                                   // command phase
      return static_cast<uint8_t>(0x80 | (f->cmd_count ? 0x10 : 0x00) |
                                  seek_bits);
  }
}

// One execution-phase byte fed by the CPU (docs §10): a sector-write byte
// lands in the image at the head's position; a FORMAT byte joins the ID
// stream. Pacing mirrors the read side: each byte advances the cell clock.
void exec_write_byte(fdc_state* f, uint8_t val) {
  if (f->cmd[C_CODE] == 0x0D) {  // FORMAT TRACK: collecting C/H/R/N per sector
    f->fmt_ids[f->fmt_pos++] = val;
    f->next_byte_at += kByteCycles;
    if (f->fmt_pos == f->cmd[3] * 4) {
      // All IDs are in: the chip writes gaps and data until the index hole.
      f->pending = P_FORMAT_DONE;
      f->due_at = f->now + (kRevCycles - f->rot);  // the next index pulse
      f->phase = PH_BUSY;
    }
    return;
  }
  // WRITE DATA / WRITE DELETED: one byte onto the addressed unit's medium.
  fdc_media* wm = sel_media(f, f->cmd[C_UNIT] & 1);
  if (wm->image != nullptr && f->data_pos < wm->len) {
    wm->image[f->data_pos] = val;
    wm->dirty = true;
  }
  if (++f->data_pos >= f->trk_end) f->data_pos = f->trk_begin;  // track wrap
  f->next_byte_at += kByteCycles;
  if (--f->remaining == 0) {  // sector complete
    f->sector_idx++;          // the head has rotated past this sector
    if (f->wr_deleted) {
      // Record the Control Mark where a re-read will report it: the parsed
      // descriptor and the image's own Track-Info entry (docs §10).
      fdc_track* trk =
          head_track(f, f->cmd[C_UNIT] & 1, (f->cmd[C_UNIT] >> 2) & 1);
      if (trk != nullptr && f->sector_idx >= 1 && f->sector_idx <= trk->sectors)
        trk->sec[f->sector_idx - 1].st2 |= 0x40;
      if (f->wr_st2_off != 0 && f->wr_st2_off < wm->len) {
        wm->image[f->wr_st2_off] |= 0x40;
        wm->dirty = true;
      }
    }
    if (f->cmd[C_R] != f->cmd[C_EOT]) {
      f->cmd[C_R]++;  // multi-sector: chain to the next
      fdc_track* trk =
          head_track(f, f->cmd[C_UNIT] & 1, (f->cmd[C_UNIT] >> 2) & 1);
      if (trk != nullptr)
        start_sector(f, trk, false);
      else
        finish_with_status(f);  // media vanished mid-command
    } else {
      finish_with_status(f);  // reached EOT: AT + EN (docs §4)
    }
  }
}

void data_write(fdc_state* f, uint8_t val) {
  if (f->phase == PH_EXEC && !f->dir_to_cpu) {
    exec_write_byte(f, val);
    return;
  }
  if (f->phase != PH_CMD) return;  // RESULT/BUSY: writes are ignored
  if (f->cmd_count == 0) {         // opcode byte
    f->mt = val & 0x80;
    f->mf = val & 0x40;
    f->sk = val & 0x20;
    const uint8_t op = val & 0x1F;
    const cmd_info* info = lookup_cmd(op);
    if (info == nullptr) {  // invalid command: single ST0 = 0x80 result
      f->res[R_ST0] = 0x80;
      f->res_len = 1;
      enter_result(f);
      return;
    }
    f->cmd[f->cmd_count++] = op;
    f->cmd_len = info->cmd_bytes;
    f->res_len = info->res_bytes;
  } else {  // parameter byte
    f->cmd[f->cmd_count++] = val;
  }
  if (f->cmd_count == f->cmd_len) {
    f->cmd_count = 0;
    dispatch(f);
  }
}

uint8_t data_read(fdc_state* f) {
  switch (f->phase) {
    case PH_EXEC: {
      if (!f->dir_to_cpu) return 0xFF;
      // One byte of the current sector; overruns past the stored data wrap
      // inside the track's data block (docs §4, the legacy behaviour).
      uint8_t val = 0xFF;
      if (f->from_flux) {  // the passing revolution's payload cache (drive A)
        const uint8_t* pay = f->media.cache_pay[f->serve_rev % kFluxRevs];
        if (f->data_pos < kFluxPayload) val = pay[f->data_pos];
      } else {  // DSK image of the addressed unit (drive A or B)
        const fdc_media* m = sel_media(f, f->cmd[C_UNIT] & 1);
        if (m->image && f->data_pos < m->len) val = m->image[f->data_pos];
      }
      if (++f->data_pos >= f->trk_end) f->data_pos = f->trk_begin;
      f->next_byte_at += kByteCycles;  // the next byte arrives one cell later
      if (--f->remaining == 0) {       // sector complete
        f->sectors_read++;
        f->sector_idx++;  // the head has rotated past this sector
        if ((f->res[R_ST1] & 0x31) || (f->res[R_ST2] & 0x21)) {
          finish_with_status(f);  // image-flagged error terminates
        } else if (f->cmd[C_R] != f->cmd[C_EOT]) {
          f->cmd[C_R]++;  // multi-sector: chain to the next
          fdc_track* trk =
              head_track(f, f->cmd[C_UNIT] & 1, (f->cmd[C_UNIT] >> 2) & 1);
          if (trk)
            start_sector(f, trk, true);
          else
            finish_with_status(f);  // media vanished mid-command
        } else {
          finish_with_status(f);  // reached EOT: AT + EN (docs §4)
        }
      }
      return val;
    }
    case PH_RESULT: {
      const uint8_t val = f->res[f->res_count++];
      if (f->res_count >= f->res_len) enter_cmd(f);
      return val;
    }
    default:
      return 0xFF;  // command phase: nothing to read, bus floats
  }
}

// ----------------------------------------------------------- DSK parsing ----

// Parse a standard or extended DSK image into `m` (offsets into `p`). Returns
// false (m cleared) on anything malformed. See docs §5.
bool parse_dsk(fdc_media* m, uint8_t* p, size_t len) {
  *m = fdc_media{};
  if (p == nullptr || len < 0x100) return false;
  bool ext;
  if (std::memcmp(p, "MV - CPC", 8) == 0)
    ext = false;
  else if (std::memcmp(p, "EXTENDED", 8) == 0)
    ext = true;
  else
    return false;

  uint32_t tracks = p[0x30];
  const uint32_t sides = ext ? (p[0x31] & 3) : p[0x31];
  if (tracks == 0 || sides < 1 || sides > kMaxSides) return false;
  tracks = std::min<uint32_t>(tracks, kMaxTracks);
  const uint32_t std_block =
      ext ? 0 : (static_cast<uint32_t>(p[0x32]) | (p[0x33] << 8));
  if (!ext && std_block < 0x100) return false;

  size_t off = 0x100;
  for (uint32_t t = 0; t < tracks; t++) {
    for (uint32_t s = 0; s < sides; s++) {
      uint32_t block = std_block;
      if (ext) {
        block = static_cast<uint32_t>(p[0x34 + (t * sides) + s]) << 8;
        if (block == 0) continue;  // unformatted track, no block present
      }
      if (off + 0x100 > len) return false;
      const uint8_t* th = p + off;
      if (std::memcmp(th, "Track-Info", 10) != 0) return false;
      const uint8_t size_code = th[0x14];
      const uint32_t sectors = th[0x15];
      if (sectors > kMaxSectors || size_code > 7) return false;

      fdc_track* trk = &m->track[t][s];
      trk->sectors = static_cast<uint8_t>(sectors);
      trk->data_off = static_cast<uint32_t>(off) + 0x100;
      uint32_t pos = trk->data_off;
      for (uint32_t i = 0; i < sectors; i++) {
        const uint8_t* si = th + 0x18 + (8 * i);
        fdc_sector* sec = &trk->sec[i];
        std::memcpy(sec->chrn, si, 4);
        sec->st1 = si[4];
        sec->st2 = si[5];
        const uint32_t stored =
            ext ? (static_cast<uint32_t>(si[6]) | (si[7] << 8))
                : (0x80u << size_code);
        if (stored > 0xFFFF) return false;
        sec->off = pos;
        sec->stored = static_cast<uint16_t>(stored);
        pos += stored;
      }
      trk->data_len = pos - trk->data_off;
      if (pos > len) return false;  // sector data must lie inside the image

      // Angular System 34 layout (docs §7): 146-cell post-index preamble, then
      // per sector sync+IDAM+CHRN+CRC (22) + GAP2 (22) + sync+DAM (16) + data +
      // CRC (2); the remaining track space is distributed evenly as GAP3 (which
      // absorbs the physical format's GAP4b). Long tracks wrap modulo the
      // revolution — the documented approximation.
      {
        uint32_t fixed = 146;
        for (uint32_t i = 0; i < sectors; i++)
          fixed += 62u + trk->sec[i].stored;
        uint32_t gap3 = 2;
        if (sectors > 0 && fixed + (2 * sectors) < kCellsPerTrack)
          gap3 = (kCellsPerTrack - fixed) / sectors;
        uint32_t cell = 146;
        for (uint32_t i = 0; i < sectors; i++) {
          fdc_sector* sec = &trk->sec[i];
          sec->idam_at = static_cast<uint16_t>((cell + 22) % kCellsPerTrack);
          sec->data_at = static_cast<uint16_t>((cell + 60) % kCellsPerTrack);
          cell = (cell + 62u + sec->stored + gap3) % kCellsPerTrack;
        }
      }
      off += block;
    }
  }
  m->image = p;
  m->len = len;
  m->tracks = static_cast<uint8_t>(tracks);
  m->sides = static_cast<uint8_t>(sides);
  return true;
}

// ------------------------------------------------------ Device functions ----

void fdc_tick(void* self, const Bus* __restrict in, Bus* __restrict out) {
  fdc_state* f = self_of(self);

  // --- mechanics: one master cycle of real time (docs §7) ---
  f->now++;
  if (f->motor_st != M_OFF) {  // the platter turns while the motor runs
    if (++f->rot >= kRevCycles) {
      f->rot = 0;
      f->rev_count++;
      push_event(f, FDC_EV_INDEX, 0);  // the index hole passes the sensor
    }
    if (f->motor_st == M_SPINUP && f->now >= f->motor_ready_at) {
      f->motor_st = M_READY;  // up to speed: the ready line rises
      // DIVERGENCE B (docs §4b): flag BOTH units on the ready-line event, not
      // just the one whose RY transitioned — a harmless superset AMSDOS drains.
      f->status_changed[0] = f->status_changed[1] = true;
      push_event(f, FDC_EV_MOTOR_READY, 0);
    }
  }
  for (int u = 0; u < 2; ++u) {
    if (f->seeking[u] && f->now >= f->next_step_at[u]) {
      // One physical head step per SRT period (audible: the seek click train).
      f->track_pos[u] = static_cast<uint8_t>(
          f->track_pos[u] + (f->seek_target[u] > f->track_pos[u] ? 1 : -1));
      push_event(f, FDC_EV_STEP, f->track_pos[u]);
      if (f->track_pos[u] == f->seek_target[u]) {
        f->seeking[u] = false;
        f->seek_done[u] = true;  // Seek End becomes visible to SENSE INTERRUPT
      } else {
        f->next_step_at[u] += f->step_period[u];
      }
    }
  }
  if (f->phase == PH_BUSY && f->now >= f->due_at) complete_pending(f);
  if (f->phase == PH_EXEC && f->now > f->next_byte_at + kByteCycles) {
    // The next byte cell arrived with the current byte still unread (reads)
    // or unfed (writes / FORMAT): the CPU missed the 32 µs window — OVERRUN,
    // abnormal termination (docs §7/§10, symmetric in both directions).
    f->res[R_ST1] |= 0x10;  // OR
    finish_with_status(f);
  }

  // Floppy-subsystem select: A10 AND A7 low on an I/O cycle. An interrupt
  // acknowledge also drives iorq (with m1) and must not be decoded.
  const bool sel = in->cpu.iorq && !in->cpu.m1 && (in->cpu.addr & 0x0480) == 0;
  const bool access = sel && (in->cpu.rd || in->cpu.wr);
  const bool edge = access && !f->access_prev;  // first cycle of this access
  f->access_prev = access;
  if (!sel) return;

  if ((in->cpu.addr & 0x0100) == 0) {  // A8 = 0: the motor latch (&FA7E)
    if (in->cpu.wr && edge) {
      const bool on = (in->cpu.data & 0x01) != 0;
      f->motor = on;
      if (on && f->motor_st == M_OFF) {
        f->motor_st = M_SPINUP;  // platter starts turning; ready in 500 ms
        f->motor_ready_at = f->now + kSpinUpCycles;
        push_event(f, FDC_EV_MOTOR_ON, 0);
      } else if (!on && f->motor_st != M_OFF) {
        f->motor_st = M_OFF;  // coast-down not modeled: ready drops at once
        f->status_changed[0] = f->status_changed[1] = true;
        push_event(f, FDC_EV_MOTOR_OFF, 0);
      }
    }
    return;  // write-only: reads float
  }

  // A8 = 1: the µPD765A. A0 picks main status vs the data register.
  const bool data_reg = (in->cpu.addr & 0x0001) != 0;
  if (in->cpu.rd) {
    if (!data_reg) {
      out->cpu.data = msr_value(f);  // side-effect free, answered every cycle
    } else {
      if (edge) f->out_latch = data_read(f);  // one byte per access (docs §1)
      out->cpu.data = f->out_latch;
    }
  } else if (in->cpu.wr && data_reg && edge) {
    data_write(f, in->cpu.data);
  }  // writes to the main status register are ignored
}

void fdc_dev_reset(void* self) {
  fdc_state* f = self_of(self);
  // Registers and FSM to cold-boot state. Head positions and the media
  // attachment persist (mechanics / live wiring — docs §9).
  f->phase = PH_CMD;
  std::memset(f->cmd, 0, sizeof(f->cmd));
  f->cmd_len = f->cmd_count = 0;
  f->mt = f->mf = f->sk = 0;
  std::memset(f->res, 0, sizeof(f->res));
  f->res_len = f->res_count = 0;
  f->dir_to_cpu = false;
  f->motor = false;
  f->sector_idx = 0;
  f->seek_done[0] = f->seek_done[1] = false;
  f->status_changed[0] = f->status_changed[1] = false;
  f->srt_hut = f->hlt_nd = 0;
  f->data_pos = f->remaining = f->trk_begin = f->trk_end = 0;
  f->now = 0;
  f->rot = 0;
  f->rev_count = 0;
  f->from_flux = 0;
  f->serve_rev = 0;
  f->motor_st = M_OFF;
  f->motor_ready_at = 0;
  f->seeking[0] = f->seeking[1] = false;
  f->seek_target[0] = f->seek_target[1] = 0;
  f->next_step_at[0] = f->next_step_at[1] = 0;
  f->step_period[0] = f->step_period[1] = 0;
  f->ev_head = f->ev_count = 0;  // the event ring restarts with the machine
  f->pending = P_NONE;
  f->pend_idx = 0;
  f->due_at = f->next_byte_at = 0;
  f->out_latch = 0xFF;
  f->access_prev = false;
  std::memset(f->st_latch, 0, sizeof(f->st_latch));
  f->sectors_read = 0;
  f->wr_deleted = 0;
  f->wr_st2_off = 0;
  std::memset(f->fmt_ids, 0, sizeof(f->fmt_ids));
  f->fmt_pos = 0;
}

// Serialize only the logical registers — everything BEFORE `media` (the
// attachment + parsed geometry is live wiring, docs §5/§9).
constexpr size_t kSaveBytes = offsetof(fdc_state, media);

size_t fdc_dev_state_size(const void* /*unused*/) { return kSaveBytes + 1; }
void fdc_save(const void* self, void* buf) {
  uint8_t* b = static_cast<uint8_t*>(buf);
  b[0] = 2;  // v2: §10 write/FORMAT fields joined the serialized prefix
  std::memcpy(b + 1, self, kSaveBytes);
}
void fdc_load(void* self, const void* buf) {
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  if (b[0] == 2) std::memcpy(self, b + 1, kSaveBytes);
}

}  // namespace

extern "C" {

size_t fdc_state_size(void) { return sizeof(fdc_state); }

Device fdc_init(void* storage) {
  // NOLINTNEXTLINE(misc-const-correctness): pointer is stored in Device::self
  // (void*), cannot be const
  fdc_state* f = new (storage) fdc_state();
  return Device{f,        "fdc",   fdc_tick, fdc_dev_reset, fdc_dev_state_size,
                fdc_save, fdc_load};
}

void fdc_peek(const Device* dev, FdcRegs* out) {
  const fdc_state* f = static_cast<const fdc_state*>(dev->self);
  out->phase = f->phase;
  out->msr = msr_value(f);
  out->motor = f->motor ? 1 : 0;
  out->last_cmd = f->cmd[C_CODE];
  out->unit = f->cmd[C_UNIT] & 1;
  out->track[0] = f->track_pos[0];
  out->track[1] = f->track_pos[1];
  out->st0 = f->st_latch[0];
  out->st1 = f->st_latch[1];
  out->st2 = f->st_latch[2];
  out->sectors_read = f->sectors_read;
  out->ready = drive_ready(f, 0) ? 1 : 0;
}

void fdc_poke_mechanics(const Device* dev, uint8_t motor, uint8_t track_a,
                        uint8_t track_b) {
  fdc_state* f = static_cast<fdc_state*>(dev->self);
  f->motor = motor != 0;
  f->track_pos[0] = track_a;
  f->track_pos[1] = track_b;
}

int fdc_attach_disk(const Device* dev, uint8_t* dsk, size_t len, uint8_t unit) {
  fdc_state* f = static_cast<fdc_state*>(dev->self);
  const uint8_t u = unit ? 1 : 0;
  fdc_media* m = u ? &f->media1 : &f->media;
  const bool ok = parse_dsk(m, dsk, len);
  f->sector_idx = 0;
  f->status_changed[u] = true;  // the disc (and the ready line) changed
  return ok ? 0 : -1;
}

int fdc_media_dirty(const Device* dev) {
  const fdc_state* f = static_cast<const fdc_state*>(dev->self);
  return f->media.dirty ? 1 : 0;
}

int fdc_media_dirty_unit(const Device* dev, uint8_t unit) {
  const fdc_state* f = static_cast<const fdc_state*>(dev->self);
  const fdc_media* m = sel_media(f, unit ? 1 : 0);
  return m && m->dirty ? 1 : 0;
}

void fdc_media_mark_clean(const Device* dev) {
  static_cast<fdc_state*>(dev->self)->media.dirty = false;
}

void fdc_media_mark_clean_unit(const Device* dev, uint8_t unit) {
  fdc_state* f = static_cast<fdc_state*>(dev->self);
  (unit ? f->media1 : f->media).dirty = false;
}

const bool* fdc_media_track_dirty(const Device* dev, int& ntracks_out) {
  const fdc_state* f = static_cast<const fdc_state*>(dev->self);
  // Only a WRITABLE flux medium (flux backing + a DSK overlay to serve written
  // tracks from) has an exportable dirty map; a read-only dump has nothing to
  // export per-track, so it reports none.
  if (f->media.backing != FDC_BACKING_FLUX || f->media.image == nullptr) {
    ntracks_out = 0;
    return nullptr;
  }
  ntracks_out = f->media.tracks;
  return f->media.track_dirty;
}

const uint8_t* fdc_media_flux_scp(const Device* dev, size_t& len_out) {
  const fdc_state* f = static_cast<const fdc_state*>(dev->self);
  len_out = f->media.scp_len;
  return f->media.scp;
}

const uint8_t* fdc_media_image(const Device* dev, size_t& len_out) {
  const fdc_state* f = static_cast<const fdc_state*>(dev->self);
  len_out = f->media.len;
  return f->media.image;
}

const uint8_t* fdc_media_image_unit(const Device* dev, uint8_t unit,
                                    size_t& len_out) {
  const fdc_state* f = static_cast<const fdc_state*>(dev->self);
  const fdc_media* m = sel_media(f, unit);
  len_out = m->len;
  return m->image;
}

int fdc_attach_flux(const Device* dev, const uint8_t* scp, size_t len) {
  fdc_state* f = static_cast<fdc_state*>(dev->self);
  const int cyls = flux_scp_cylinders(scp, len);
  const int revs = flux_scp_revolutions(scp, len);
  if (cyls <= 0 || revs <= 0) return -1;
  f->media = fdc_media{};               // replaces any DSK attachment
  f->media.backing = FDC_BACKING_FLUX;  // read-only: no `image` overlay
  f->media.scp = scp;
  f->media.scp_len = len;
  f->media.scp_revs = static_cast<uint8_t>(revs < 255 ? revs : 255);
  f->media.tracks = static_cast<uint8_t>(cyls < kMaxTracks ? cyls : kMaxTracks);
  f->media.sides = 1;  // side 0 (flux-media.md §5)
  f->sector_idx = 0;
  f->status_changed[0] = true;  // the disc (and the ready line) changed
  return 0;
}

int fdc_attach_flux_writable(const Device* dev, const uint8_t* scp,
                             size_t scp_len, uint8_t* dsk, size_t dsk_len) {
  fdc_state* f = static_cast<fdc_state*>(dev->self);
  const int revs = flux_scp_revolutions(scp, scp_len);
  // Parse the mutable DSK overlay in place (WRITE DATA / FORMAT edit it), then
  // overlay the flux backing parse_dsk cleared. scp stays const — the pristine
  // source for verbatim export of unwritten tracks.
  fdc_media* m = &f->media;
  if (!parse_dsk(m, dsk, dsk_len)) return -1;
  m->backing =
      FDC_BACKING_FLUX;  // hybrid: overlay present, clean tracks = flux
  m->scp = scp;
  m->scp_len = scp_len;
  const int rev_count = revs > 0 ? revs : 1;  // ≥1 revolution; a flux dump has
  m->scp_revs = static_cast<uint8_t>(rev_count < 255 ? rev_count : 255);
  // track_dirty stays all-false: every track starts clean (serves the cache).
  f->sector_idx = 0;
  f->status_changed[0] = true;  // the disc (and the ready line) changed
  return 0;
}

void fdc_eject_disk(const Device* dev, uint8_t unit) {
  fdc_state* f = static_cast<fdc_state*>(dev->self);
  const uint8_t u = unit ? 1 : 0;
  (u ? f->media1 : f->media) = fdc_media{};
  f->sector_idx = 0;
  f->status_changed[u] = true;
}

int fdc_drain_events(const Device* dev, FdcEvent* out, int max) {
  fdc_state* f = static_cast<fdc_state*>(dev->self);
  int n = 0;
  while (n < max && f->ev_count > 0) {
    out[n++] = f->ev[f->ev_head];
    f->ev_head = static_cast<uint8_t>((f->ev_head + 1) % 64);
    f->ev_count--;
  }
  return n;
}

int fdc_quiet(const Device* dev) {
  // Mirrors fdc_tick's per-cycle body exactly (fdc.h): with the motor off the
  // rotation/spin-up block is dead, with no seek stepping the SRT loop is
  // dead, PH_CMD/PH_RESULT have no due_at/next_byte_at timers, and a clear
  // access_prev means the edge detector needs no falling-edge tick. What
  // remains is `now++` — re-appliable in bulk by fdc_advance.
  const fdc_state* f = static_cast<const fdc_state*>(dev->self);
  return (f->motor_st == M_OFF && !f->seeking[0] && !f->seeking[1] &&
          (f->phase == PH_CMD || f->phase == PH_RESULT) && !f->access_prev)
             ? 1
             : 0;
}

void fdc_advance(const Device* dev, uint64_t skipped_cycles) {
  static_cast<fdc_state*>(dev->self)->now += skipped_cycles;
}

}  // extern "C"
