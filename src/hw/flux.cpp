/* flux.cpp — SCP flux-dump → DSK conversion, pure functions. See
 * docs/hardware/flux-media.md (the spec) and flux.h (the ABI).
 *
 * Pipeline per track: SCP revolution(s) → software PLL (2 µs half-cell,
 * adaptive ±15 %) → packed MFM bitcell stream → 0x4489 sync scan → IBM
 * System 34 ID/data fields with CRC-CCITT → sectors appended to the DSK
 * track block. Revolutions 0 and 1 are decoded back-to-back when available:
 * the second sighting of each sector provides the weak-sector comparison
 * (docs §4) and recovers sectors torn across the index in revolution 0.
 *
 * No heap. Fixed scratch per call (docs §5): a 32 KB packed bitcell buffer,
 * an 8 KB track-payload buffer and a 4 KB sector buffer, all function-local. */

#include "flux.h"

#include <algorithm>
#include <cstring>

namespace {

constexpr int kMaxCyls = 84;         // SCP slot space / 2; CPC target is 40/42
constexpr int kMaxSectors = 29;      // what a DSK Track-Info block holds
constexpr uint8_t kMaxSizeCode = 5;  // 4096 bytes; larger cannot fit DD
constexpr size_t kMaxBits = 262144;  // bitcells: 2 revolutions + tolerance
constexpr size_t kMaxSectorBytes = 128u << kMaxSizeCode;
constexpr size_t kMaxTrackBytes = 8192;  // a DD revolution carries ~6250 bytes
constexpr size_t kTlutOff = 0x10;
constexpr size_t kTlutEnd = kTlutOff + (4 * 168);  // 0x2B0
constexpr size_t kDamWindow = 1280;  // max ID-end → data-mark gap, bitcells

uint32_t rd32(const uint8_t* p) {  // little-endian (header/TDH fields)
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint32_t rd16be(const uint8_t* p) {  // big-endian (the flux words)
  return (static_cast<uint32_t>(p[0]) << 8) | p[1];
}

// ------------------------------------------------------- PLL → bitcells -----

struct BitBuf {
  uint8_t bits[kMaxBits / 8];  // packed, MSB-first
  size_t n;                    // bitcells decoded so far
};

int bit_at(const BitBuf* bb, size_t i) {
  return (bb->bits[i >> 3] >> (7 - (i & 7))) & 1;
}

// Estimate this revolution's true half-cell width from the interval
// population itself (docs §2): two reclassification passes of mean(t / n)
// over the intervals that look like legal MFM (2..4 half-cells), clamped to
// nominal ±15 %. Spindle-speed error scales every interval, so the mean is a
// direct speed measurement; zero-mean jitter averages out over a revolution.
double estimate_cell(const uint8_t* flux, uint32_t words, double nominal) {
  double cell = nominal;
  for (int pass = 0; pass < 2; pass++) {
    double sum_t = 0.0;
    long sum_n = 0;
    uint32_t carry = 0;
    for (uint32_t i = 0; i < words; i++) {
      const uint32_t w = rd16be(flux + (2u * i));
      if (w == 0) {  // overflow run: not a legal MFM interval anyway
        carry += 0x10000u;
        continue;
      }
      const double t = static_cast<double>(carry) + static_cast<double>(w);
      carry = 0;
      const long n = static_cast<long>((t / cell) + 0.5);
      if (n >= 2 && n <= 4) {
        sum_t += t;
        sum_n += n;
      }
    }
    if (sum_n >= 128)
      cell = sum_t / static_cast<double>(sum_n);  // enough signal
  }
  if (cell < nominal * 0.85)
    cell = nominal * 0.85;
  else if (cell > nominal * 1.15)
    cell = nominal * 1.15;
  return cell;
}

// Decode one revolution's flux words into bb (appending). The software PLL of
// docs §2: classify against the estimated half-cell by rounding to the
// nearest whole cell count, then correct the clock by 2 % of the per-cell
// error, clamped to the estimate ±1.5 %. The tight clamp is what makes
// worst-case ±10 % per-interval jitter provably classification-safe (a
// 4-cell interval 10 % short against a 1.5 %-fast clock still reads
// 3.6 / 1.015 = 3.55 > 3.5).
int pll_decode(const uint8_t* flux, uint32_t words, double nominal,
               BitBuf* bb) {
  const double cell = estimate_cell(flux, words, nominal);
  double clock = cell;
  const double lo = cell * 0.985, hi = cell * 1.015;
  uint32_t carry = 0;
  for (uint32_t i = 0; i < words; i++) {
    const uint32_t w = rd16be(flux + (2u * i));
    if (w == 0) {  // 0x0000 = overflow: 65536 ticks carried into the next word
      carry += 0x10000u;
      continue;
    }
    const double t = static_cast<double>(carry) + static_cast<double>(w);
    carry = 0;
    long n = static_cast<long>((t / clock) + 0.5);
    n = std::max<long>(n, 1);
    if (bb->n + static_cast<size_t>(n) > kMaxBits) return FLUX_E_TOO_LONG;
    bb->n += static_cast<size_t>(n) - 1;  // the zeros: buffer is pre-cleared
    bb->bits[bb->n >> 3] |= static_cast<uint8_t>(0x80u >> (bb->n & 7));
    bb->n++;
    clock += ((t - (static_cast<double>(n) * clock)) / static_cast<double>(n)) *
             0.02;
    if (clock < lo)
      clock = lo;
    else if (clock > hi)
      clock = hi;
  }
  return 0;  // a trailing unterminated overflow run is discarded (docs §1.4)
}

// --------------------------------------------------- MFM byte extraction ----

// Read 16 raw bitcells at *pos as one byte (data bits at the odd offsets —
// the stream is byte-synced right after a 0x4489 match). -1 = past the end.
int read_mfm_byte(const BitBuf* bb, size_t* pos) {
  if (*pos + 16 > bb->n) return -1;
  unsigned v = 0;
  for (int k = 0; k < 8; k++)
    v = (v << 1) | static_cast<unsigned>(
                       bit_at(bb, *pos + (2 * static_cast<size_t>(k)) + 1));
  *pos += 16;
  return static_cast<int>(v);
}

// Read 16 raw bitcells as a raw word (sync detection). -1 = past the end.
long read_raw16(const BitBuf* bb, size_t* pos) {
  if (*pos + 16 > bb->n) return -1;
  unsigned v = 0;
  for (int k = 0; k < 16; k++)
    v = (v << 1) |
        static_cast<unsigned>(bit_at(bb, *pos + static_cast<size_t>(k)));
  *pos += 16;
  return static_cast<long>(v);
}

// CRC-CCITT: poly 0x1021, MSB-first. Preset with init 0xFFFF over A1 A1 A1
// (always exactly three, per the standard — docs §3) before the mark byte.
uint16_t crc_byte(uint16_t crc, uint8_t b) {
  crc ^= static_cast<uint16_t>(b) << 8;
  for (int i = 0; i < 8; i++)
    crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                         : static_cast<uint16_t>(crc << 1);
  return crc;
}
uint16_t crc_sync_preset() {
  uint16_t crc = 0xFFFF;
  for (int i = 0; i < 3; i++) crc = crc_byte(crc, 0xA1);
  return crc;
}

// ----------------------------------------------------------- track scan -----

struct SecRec {
  uint8_t chrn[4];
  uint8_t st1, st2;  // DSK per-sector status (Data Error / Control Mark)
  uint16_t len;      // stored bytes = 128 << N
  uint32_t off;      // into the track payload buffer
  bool crc_bad;      // data CRC failed on the first (revolution-0) sighting
  bool differs;      // a later sighting's payload differed
  size_t idam_bit;   // bitcell index where the ID field (incl. CRC) completed
  size_t data_bit;   // bitcell index of the data field's first payload bit
};

struct TrackScan {
  int count;
  SecRec sec[kMaxSectors];
};

SecRec* find_sec(TrackScan* ts, const uint8_t chrn[4]) {
  for (int i = 0; i < ts->count; i++)
    if (std::memcmp(ts->sec[i].chrn, chrn, 4) == 0) return &ts->sec[i];
  return nullptr;
}

// Slide over the bitcell stream, byte-sync on 0x4489 runs, collect sectors
// (docs §3). First sightings append payload to `data` (cap kMaxTrackBytes);
// duplicates (revolution 1) only feed the weak comparison. Returns bytes used.
size_t scan_track(const BitBuf* bb, uint8_t* data, TrackScan* ts) {
  const uint16_t preset = crc_sync_preset();
  size_t used = 0;
  uint32_t sr = 0;
  bool have_hdr = false;
  uint8_t hdr[4] = {0, 0, 0, 0};
  size_t hdr_end = 0;
  uint8_t secbuf[kMaxSectorBytes];

  size_t pos = 0;
  while (pos < bb->n) {
    sr = ((sr << 1) | static_cast<uint32_t>(bit_at(bb, pos))) & 0xFFFF;
    pos++;
    if (sr != 0x4489) continue;

    // Byte-synced. Consume the rest of the A1 sync run, then the mark byte.
    size_t p = pos;
    for (;;) {
      size_t q = p;
      if (read_raw16(bb, &q) != 0x4489) break;
      p = q;
    }
    const int mark = read_mfm_byte(bb, &p);
    if (mark < 0) break;  // fell off the end of the stream

    if (mark == 0xFE) {  // ID address mark: C H R N + CRC
      uint8_t id[4];
      uint16_t crc = crc_byte(preset, 0xFE);
      bool ok = true;
      for (int i = 0; i < 4 && ok; i++) {
        const int b = read_mfm_byte(bb, &p);
        if (b < 0) {
          ok = false;
        } else {
          id[i] = static_cast<uint8_t>(b);
          crc = crc_byte(crc, id[i]);
        }
      }
      int c_hi = -1, c_lo = -1;
      if (ok) {
        c_hi = read_mfm_byte(bb, &p);
        c_lo = read_mfm_byte(bb, &p);
      }
      if (ok && c_hi >= 0 && c_lo >= 0 &&
          crc == ((static_cast<unsigned>(c_hi) << 8) |
                  static_cast<unsigned>(c_lo)) &&
          id[3] <= kMaxSizeCode) {
        std::memcpy(hdr, id, 4);  // a CRC-valid header is now pending
        have_hdr = true;
        hdr_end = p;
      }  // bad header: dropped (the sector may be recovered from rev 1)
    } else if ((mark == 0xFB || mark == 0xF8) && have_hdr &&
               p - hdr_end <= kDamWindow) {  // this sector's data field
      const size_t len = 128u << hdr[3];
      const size_t data_start = p;  // first payload bitcell (angular position)
      uint16_t crc = crc_byte(preset, static_cast<uint8_t>(mark));
      bool complete = true;
      for (size_t i = 0; i < len && complete; i++) {
        const int b = read_mfm_byte(bb, &p);
        if (b < 0) {
          complete = false;
        } else {
          secbuf[i] = static_cast<uint8_t>(b);
          crc = crc_byte(crc, secbuf[i]);
        }
      }
      int c_hi = -1, c_lo = -1;
      if (complete) {
        c_hi = read_mfm_byte(bb, &p);
        c_lo = read_mfm_byte(bb, &p);
      }
      if (complete && c_hi >= 0 && c_lo >= 0) {
        const bool crc_ok = crc == ((static_cast<unsigned>(c_hi) << 8) |
                                    static_cast<unsigned>(c_lo));
        SecRec* ex = find_sec(ts, hdr);
        if (ex != nullptr) {  // second sighting: the weak comparison (docs §4)
          if (ex->len != len || std::memcmp(data + ex->off, secbuf, len) != 0)
            ex->differs = true;
        } else if (ts->count < kMaxSectors && used + len <= kMaxTrackBytes) {
          SecRec* s = &ts->sec[ts->count++];
          std::memcpy(s->chrn, hdr, 4);
          s->len = static_cast<uint16_t>(len);
          s->off = static_cast<uint32_t>(used);
          s->crc_bad = !crc_ok;
          s->differs = false;
          s->idam_bit = hdr_end;
          s->data_bit = data_start;
          s->st1 = crc_ok ? 0 : 0x20;  // ST1 Data Error
          s->st2 =
              static_cast<uint8_t>((crc_ok ? 0 : 0x20) |  // ST2 DE in data
                                   (mark == 0xF8 ? 0x40 : 0));  // Control Mark
          std::memcpy(data + used, secbuf, len);
          used += len;
        }  // over 29 sectors / track payload full: bounded drop (docs §5)
        have_hdr = false;
      }  // incomplete data field (index-torn): dropped, header stays consumed
    }
    // Resume the sync hunt after whatever was consumed.
    pos = p;
    sr = 0;
  }
  return used;
}

// --------------------------------------------------------- SCP plumbing -----

struct ScpGeom {
  uint8_t revs;
  bool legacy;     // old single-sided layout: slot = cyl (docs §1.2)
  double nominal;  // half-cell in ticks: 80 / (resolution + 1)
  int cyls;        // last present cylinder + 1
};

// Header + offset-table validation shared by probe and convert. Returns 0 or
// a FLUX_E_* code; fills *g on success.
int scp_geometry(const uint8_t* scp, size_t len, ScpGeom* g) {
  if (scp == nullptr || len < kTlutEnd || std::memcmp(scp, "SCP", 3) != 0)
    return FLUX_E_NOT_SCP;
  const uint8_t width = scp[0x09], heads = scp[0x0A];
  if (width != 0 && width != 16) return FLUX_E_GEOMETRY;
  if (scp[0x05] == 0) return FLUX_E_GEOMETRY;  // no revolutions
  if (heads == 2) return FLUX_E_GEOMETRY;      // side-1-only dump
  if (scp[0x06] > scp[0x07]) return FLUX_E_GEOMETRY;
  g->revs = scp[0x05];
  g->nominal = 80.0 / (static_cast<double>(scp[0x0B]) + 1.0);
  g->legacy = false;
  if (heads != 0) {  // legacy consecutive single-sided layout?
    for (int slot = 1; slot < 168; slot += 2)
      if (rd32(scp + kTlutOff + (4 * static_cast<size_t>(slot))) != 0) {
        g->legacy = true;
        break;
      }
  }
  g->cyls = 0;
  for (int cyl = 0; cyl < kMaxCyls; cyl++) {
    const int slot = g->legacy ? cyl : cyl * 2;
    if (rd32(scp + kTlutOff + (4 * static_cast<size_t>(slot))) != 0)
      g->cyls = cyl + 1;
  }
  if (g->cyls == 0) return FLUX_E_GEOMETRY;  // nothing on side 0
  return 0;
}

// Append ONE revolution `r` of the track at `toff` to bb. 0 or a FLUX_E_* code.
int decode_one_rev(const uint8_t* scp, size_t len, uint32_t toff,
                   const ScpGeom* g, int r, BitBuf* bb) {
  const uint64_t tdh_end =
      static_cast<uint64_t>(toff) + 4 + (12u * static_cast<uint64_t>(g->revs));
  if (tdh_end > len || std::memcmp(scp + toff, "TRK", 3) != 0)
    return FLUX_E_TRUNCATED;
  const uint8_t* e = scp + toff + 4 + (12 * static_cast<size_t>(r));
  const uint32_t words = rd32(e + 4);
  const uint32_t doff = rd32(e + 8);
  if (static_cast<uint64_t>(toff) + doff + (2ull * words) > len)
    return FLUX_E_TRUNCATED;
  return pll_decode(scp + toff + doff, words, g->nominal, bb);
}

// Decode revolution 0 (+ revolution 1 when present) of the track at `toff`
// into bb as one concatenated bitcell stream (the Stage-1 weak-compare path).
int decode_track(const uint8_t* scp, size_t len, uint32_t toff,
                 const ScpGeom* g, BitBuf* bb) {
  bb->n = 0;
  std::memset(bb->bits, 0, sizeof(bb->bits));
  const int use_revs = g->revs >= 2 ? 2 : 1;
  for (int r = 0; r < use_revs; r++) {
    const int rc = decode_one_rev(scp, len, toff, g, r, bb);
    if (rc != 0) return rc;
  }
  return 0;
}

// The side-0 track-offset-table entry for `cyl` (0 = absent).
uint32_t track_offset(const uint8_t* scp, const ScpGeom* g, int cyl) {
  const int slot = g->legacy ? cyl : cyl * 2;
  return rd32(scp + kTlutOff + (4 * static_cast<size_t>(slot)));
}

}  // namespace

extern "C" {

int flux_scp_probe(const uint8_t* scp, size_t len) {
  ScpGeom g;
  return scp_geometry(scp, len, &g) == 0 ? 1 : 0;
}

long flux_scp_to_dsk(const uint8_t* scp, size_t scp_len, uint8_t* dsk_out,
                     size_t dsk_cap, FluxWeakReport* weak) {
  if (weak != nullptr) std::memset(weak, 0, sizeof(*weak));
  ScpGeom g;
  {
    const int rc = scp_geometry(scp, scp_len, &g);
    if (rc != 0) return rc;
  }
  if (dsk_out == nullptr || dsk_cap < 0x100) return FLUX_E_DSK_OVERFLOW;
  std::memset(dsk_out, 0, 0x100);  // disc header, finished at the end

  static_assert(kMaxCyls <= 102, "must fit a DSK header's track count");
  BitBuf bb;  // 32 KB packed bitcells (docs §5)
  uint8_t trk_data[kMaxTrackBytes];
  uint32_t block_of[kMaxCyls];
  size_t off = 0x100;
  long total_sectors = 0;
  bool uniform = true;
  uint32_t common_block = 0;
  int common_n = -1;

  for (int cyl = 0; cyl < g.cyls; cyl++) {
    const int slot = g.legacy ? cyl : cyl * 2;
    const uint32_t toff =
        rd32(scp + kTlutOff + (4 * static_cast<size_t>(slot)));

    TrackScan ts;
    ts.count = 0;
    size_t used = 0;
    if (toff != 0) {  // absent slot mid-range → empty (unformatted) track
      const int rc = decode_track(scp, scp_len, toff, &g, &bb);
      if (rc != 0) return rc;
      used = scan_track(&bb, trk_data, &ts);
    }

    // Emit the Track-Info block (padded to 256 — required extended, harmless
    // standard), payload straight after the 0x100 header.
    uint32_t const block =
        static_cast<uint32_t>((0x100 + used + 0xFF) & ~0xFFu);
    if (off + block > dsk_cap) return FLUX_E_DSK_OVERFLOW;
    uint8_t* th = dsk_out + off;
    std::memset(th, 0, block);
    std::memcpy(th, "Track-Info\r\n", 12);
    th[0x10] = static_cast<uint8_t>(cyl);
    th[0x11] = 0;  // side 0
    const uint8_t size_code = ts.count ? ts.sec[0].chrn[3] : 2;
    th[0x14] = size_code;
    th[0x15] = static_cast<uint8_t>(ts.count);
    th[0x16] = 0x4E;  // GAP3
    th[0x17] = 0xE5;  // filler
    for (int i = 0; i < ts.count; i++) {
      const SecRec* s = &ts.sec[i];
      uint8_t* si = th + 0x18 + (8 * static_cast<size_t>(i));
      std::memcpy(si, s->chrn, 4);
      si[4] = s->st1;
      si[5] = s->st2;
      si[6] = static_cast<uint8_t>(s->len & 0xFF);  // zeroed again if standard
      si[7] = static_cast<uint8_t>(s->len >> 8);
      if (common_n < 0)
        common_n = s->chrn[3];
      else if (common_n != s->chrn[3])
        uniform = false;
      if (weak != nullptr && (s->crc_bad || s->differs)) {
        if (weak->count < FLUX_WEAK_MAX) {
          FluxWeakSector* w = &weak->sec[weak->count];
          w->cyl = static_cast<uint8_t>(cyl);
          w->side = 0;
          w->sector_id = s->chrn[2];
          w->reason = static_cast<uint8_t>((s->crc_bad ? FLUX_WEAK_CRC : 0) |
                                           (s->differs ? FLUX_WEAK_DIFFER : 0));
        }
        weak->count++;
      }
    }
    std::memcpy(th + 0x100, trk_data, used);
    block_of[cyl] = block;
    dsk_out[0x34 + cyl] = static_cast<uint8_t>(block >> 8);  // extended table
    if (cyl == 0)
      common_block = block;
    else if (block != common_block)
      uniform = false;
    total_sectors += ts.count;
    off += block;
  }
  if (total_sectors == 0) return FLUX_E_NO_SECTORS;

  // Disc header: standard when every block and every sector size agree.
  if (uniform) {
    std::memcpy(dsk_out, "MV - CPCEMU Disk-File\r\nDisk-Info\r\n", 34);
    std::memcpy(dsk_out + 0x22, "konCePCja-flux", 14);
    dsk_out[0x32] = static_cast<uint8_t>(common_block & 0xFF);
    dsk_out[0x33] = static_cast<uint8_t>(common_block >> 8);
    std::memset(dsk_out + 0x34, 0, static_cast<size_t>(g.cyls));  // unused
    size_t t_off = 0x100;
    for (int t = 0; t < g.cyls; t++) {  // stored-length fields: unused too
      uint8_t* th = dsk_out + t_off;
      for (int i = 0; i < th[0x15]; i++)
        th[0x18 + (8 * static_cast<size_t>(i)) + 6] =
            th[0x18 + (8 * static_cast<size_t>(i)) + 7] = 0;
      t_off += block_of[t];
    }
  } else {
    std::memcpy(dsk_out, "EXTENDED CPC DSK File\r\nDisk-Info\r\n", 34);
    std::memcpy(dsk_out + 0x22, "konCePCja-flux", 14);
  }
  dsk_out[0x30] = static_cast<uint8_t>(g.cyls);
  dsk_out[0x31] = 1;  // side 0 only (docs §5)
  return static_cast<long>(off);
}

// --- Stage 3: per-track, per-revolution decode (docs §7) ---------------------

int flux_scp_revolutions(const uint8_t* scp, size_t len) {
  ScpGeom g;
  return scp_geometry(scp, len, &g) == 0 ? g.revs : 0;
}

int flux_scp_cylinders(const uint8_t* scp, size_t len) {
  ScpGeom g;
  return scp_geometry(scp, len, &g) == 0 ? g.cyls : 0;
}

int flux_decode_track_rev(const uint8_t* scp, size_t len, uint8_t cyl,
                          uint8_t rev, FluxTrack* out, uint8_t* payload,
                          size_t payload_cap) {
  std::memset(out, 0, sizeof(*out));
  ScpGeom g;
  {
    const int rc = scp_geometry(scp, len, &g);
    if (rc != 0) return rc;
  }
  if (cyl >= g.cyls) return 0;  // past the dump: nothing under the head
  const uint32_t toff = track_offset(scp, &g, cyl);
  if (toff == 0) return 0;  // unformatted / absent cylinder

  BitBuf bb;  // one revolution only
  bb.n = 0;
  std::memset(bb.bits, 0, sizeof(bb.bits));
  {
    const int rc = decode_one_rev(scp, len, toff, &g, rev % g.revs,
                                  &bb);  // captures repeat (§7)
    if (rc != 0) return rc;
  }
  if (bb.n == 0) return 0;

  TrackScan ts;
  ts.count = 0;
  uint8_t trk_data[kMaxTrackBytes];
  const size_t used = scan_track(&bb, trk_data, &ts);

  // Assemble the public view: payloads into the caller's buffer, bitcell
  // positions normalized to the FDC's angular unit (6250 byte cells per
  // revolution — measured from the ACTUAL stream, so real gap geometry and
  // long tracks carry through).
  const size_t copy = used <= payload_cap ? used : payload_cap;
  std::memcpy(payload, trk_data, copy);
  for (int i = 0; i < ts.count && out->count < FLUX_TRACK_MAX_SECTORS; i++) {
    const SecRec* s = &ts.sec[i];
    if (s->off + s->len > copy) continue;  // payload didn't fit: bounded drop
    FluxSector* fs = &out->sec[out->count++];
    std::memcpy(fs->chrn, s->chrn, 4);
    fs->st1 = s->st1;
    fs->st2 = s->st2;
    fs->len = s->len;
    fs->off = s->off;
    fs->idam_cell = static_cast<uint16_t>(
        (static_cast<uint64_t>(s->idam_bit) * 6250u) / bb.n % 6250u);
    fs->data_cell = static_cast<uint16_t>(
        (static_cast<uint64_t>(s->data_bit) * 6250u) / bb.n % 6250u);
  }
  out->payload_used = static_cast<uint32_t>(copy);
  return 0;
}

}  // extern "C"
