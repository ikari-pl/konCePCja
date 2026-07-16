/* kryoflux_stream.cpp — see kryoflux_stream.h. Parse a KryoFlux STREAM byte
 * stream and emit an SCP byte image the flux decoder (src/hw/flux) reads
 * directly. Clean-room from the Rev 1.1 documentation and this project's specs;
 * no GPL or SPS Decoder Library code consulted.
 *
 * STREAM byte format (in-band "flux buffer" blocks), by header byte:
 *   0x0E..0xFF  Flux1  1 byte : value = header
 *   0x00..0x07  Flux2  2 bytes: value = (header << 8) | data[1]
 *   0x0C        Flux3  3 bytes: value = (data[1] << 8) | data[2]
 *   0x0B        Ovl16  1 byte : add 0x10000 to the NEXT flux value (stackable)
 *   0x08        Nop1   1 byte : padding, no flux
 *   0x09        Nop2   2 bytes: padding, no flux
 *   0x0A        Nop3   3 bytes: padding, no flux
 *   0x0D        OOB    out-of-band block (see below)
 *
 * OOB block: 0x0D, type(1), size(2 LE), body(size). Types:
 *   0x00 Invalid, 0x01 StreamInfo(8), 0x02 Index(12), 0x03 StreamEnd(8),
 *   0x04 KFInfo (ASCII "name=value,..."; may carry sck=/ick=),
 *   0x0D EOF (stop parsing; the size field is a sentinel, do NOT consume it).
 * OOB blocks are "out of band": they do NOT advance the in-band STREAM
 * POSITION counter that Index blocks reference — only flux/Nop bytes do.
 *
 * TWO-PASS INDEX ALIGNMENT (the one hard part)
 * --------------------------------------------
 * An Index block carries StreamPosition (the in-band byte offset of the flux
 * transition that begins the next revolution — "the next flux") and
 * SampleCounter (sck ticks from the PREVIOUS flux transition to the index
 * pulse), per flux-formats-feasibility.md §2.2.
 * Pass 1 walks the stream once, recording every flux transition tagged with its
 * in-band start position and interval (sck), plus every Index (StreamPosition,
 * SampleCounter). Pass 2 binds each Index to the first flux whose start
 * position >= StreamPosition, and slices the flux list into revolutions between
 * consecutive indices. For index i at flux fi with previous-flux absolute time
 * T(fi-1):
 *   I(i)          = T(fi-1) + SampleCounter(i)          [absolute index time]
 *   rev i words   = { T(fi..fj-1) - I(i) } differenced  [fj = next index flux]
 *   rev i duration= I(i+1) - I(i)
 * The first word is thus (interval(fi) - SampleCounter(i)) = time from the
 * index to the first transition, exactly the SCP per-revolution convention.
 * Documented edge cases handled: an index before any flux (fi==0 -> previous
 * time 0), an index after the last flux (fi==nflux -> closes a revolution,
 * opens none), and flux-value overflow via stacked Ovl16 right before an index.
 */
#include "kryoflux_stream.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace {

constexpr int kSlots = 168;        // SCP track-lookup-table entries
constexpr size_t kHdrEnd = 0x2B0;  // header (0x10) + TLUT (168 * 4)
constexpr size_t kTlutOff = 0x10;

uint32_t rd32le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t rd16le(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
void wr32le(std::vector<uint8_t>& v, size_t at, uint32_t x) {
  v[at] = static_cast<uint8_t>(x);
  v[at + 1] = static_cast<uint8_t>(x >> 8);
  v[at + 2] = static_cast<uint8_t>(x >> 16);
  v[at + 3] = static_cast<uint8_t>(x >> 24);
}
void put_be16(std::vector<uint8_t>& v, uint16_t x) {  // SCP flux words are BE
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x));
}

// A flux transition: where its encoding starts in the in-band stream (including
// any Ovl16 prefix) and the reconstructed interval in sck ticks.
struct Transition {
  uint64_t pos;
  uint32_t interval;
};

// An Index OOB: the in-band position of the next flux, and the sck ticks from
// the previous flux to the index pulse.
struct IndexRec {
  uint64_t stream_pos;
  uint32_t sample_counter;
};

// Parse a KFInfo ASCII body for "sck=<float>" and update `sck_hz` if present.
void parse_kfinfo(const uint8_t* body, size_t sz, double& sck_hz) {
  const std::string s(reinterpret_cast<const char*>(body), sz);
  const size_t k = s.find("sck=");
  if (k == std::string::npos) return;
  const char* start = s.c_str() + k + 4;
  char* end = nullptr;
  const double v = std::strtod(start, &end);
  if (end != start && v > 0.0) sck_hz = v;
}

// Pass 1: walk the stream once, collecting flux transitions, indices, and the
// effective sck. Returns 0 or a negative KFSTREAM_E_* code.
int scan_stream(const uint8_t* data, size_t len, std::vector<Transition>& flux,
                std::vector<IndexRec>& idx, double& sck_hz) {
  uint64_t spos = 0;     // in-band stream position (flux/Nop bytes only)
  uint32_t ovl = 0;      // pending Ovl16 count (each adds 0x10000)
  uint64_t ovl_pos = 0;  // in-band position where the Ovl16 run began
  bool ovl_active = false;

  for (size_t off = 0; off < len;) {
    const uint8_t h = data[off];
    if (h >= 0x0E) {  // Flux1
      const uint64_t fpos = ovl_active ? ovl_pos : spos;
      flux.push_back({fpos, (ovl * 0x10000u) + h});
      ovl = 0;
      ovl_active = false;
      off += 1;
      spos += 1;
    } else if (h <= 0x07) {  // Flux2
      if (off + 2 > len) return KFSTREAM_E_TRUNCATED;
      const uint64_t fpos = ovl_active ? ovl_pos : spos;
      const uint32_t v = (static_cast<uint32_t>(h) << 8) | data[off + 1];
      flux.push_back({fpos, (ovl * 0x10000u) + v});
      ovl = 0;
      ovl_active = false;
      off += 2;
      spos += 2;
    } else if (h == 0x0C) {  // Flux3
      if (off + 3 > len) return KFSTREAM_E_TRUNCATED;
      const uint64_t fpos = ovl_active ? ovl_pos : spos;
      const uint32_t v =
          (static_cast<uint32_t>(data[off + 1]) << 8) | data[off + 2];
      flux.push_back({fpos, (ovl * 0x10000u) + v});
      ovl = 0;
      ovl_active = false;
      off += 3;
      spos += 3;
    } else if (h == 0x0B) {  // Ovl16
      if (!ovl_active) {
        ovl_pos = spos;
        ovl_active = true;
      }
      ovl += 1;
      off += 1;
      spos += 1;
    } else if (h == 0x08) {  // Nop1
      off += 1;
      spos += 1;
    } else if (h == 0x09) {  // Nop2
      if (off + 2 > len) return KFSTREAM_E_TRUNCATED;
      off += 2;
      spos += 2;
    } else if (h == 0x0A) {  // Nop3
      if (off + 3 > len) return KFSTREAM_E_TRUNCATED;
      off += 3;
      spos += 3;
    } else {  // h == 0x0D: OOB
      if (off + 4 > len) return KFSTREAM_E_TRUNCATED;
      const uint8_t type = data[off + 1];
      const uint16_t sz = rd16le(data + off + 2);
      if (type == 0x0D) break;  // EOF: stop; size is a sentinel, do not consume
      const size_t body = off + 4;
      if (body + sz > len) return KFSTREAM_E_TRUNCATED;
      if (type == 0x02) {  // Index
        if (sz < 12) return KFSTREAM_E_BAD_OOB;
        idx.push_back({rd32le(data + body), rd32le(data + body + 4)});
      } else if (type == 0x04) {  // KFInfo
        parse_kfinfo(data + body, sz, sck_hz);
      }
      // Invalid/StreamInfo/StreamEnd carry no flux timing we need.
      off = body + sz;  // OOB does NOT advance the in-band stream position
    }
  }
  return 0;
}

// Index of the first flux transition whose start position is >= sp, or
// flux.size() if the index points past the last flux.
size_t resolve_index(const std::vector<Transition>& flux, uint64_t sp) {
  size_t lo = 0, hi = flux.size();
  while (lo < hi) {
    const size_t mid = lo + ((hi - lo) / 2);
    if (flux[mid].pos < sp)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

// Append one revolution's flux words to `out` as SCP big-endian words,
// splitting any value > 0xFFFF into 0x0000 carry words. Returns the word count
// written.
uint32_t emit_flux_words(const std::vector<uint32_t>& flux_25ns,
                         std::vector<uint8_t>& out) {
  uint32_t n = 0;
  for (uint32_t const iv : flux_25ns) {
    uint32_t x = iv != 0 ? iv : 1;  // a 0-tick word would mean SCP overflow
    while (x > 0xFFFF) {
      put_be16(out, 0);  // 0x0000 carries 65536 ticks
      x -= 0x10000;
      n++;
    }
    put_be16(out, static_cast<uint16_t>(x));
    n++;
  }
  return n;
}

// Write a whole SCP image from decoded tracks placed at their SCP slots. Every
// track must have at least `nrevs` revolutions (the caller normalizes).
int build_scp(const std::vector<std::pair<int, const KryoFluxTrack*>>& tracks,
              int nrevs, std::vector<uint8_t>& out) {
  out.clear();
  int max_slot = -1;
  for (const auto& t : tracks) max_slot = std::max(t.first, max_slot);
  if (max_slot < 0 || nrevs < 1) return KFSTREAM_E_NO_FLUX;

  out.assign(kHdrEnd, 0);
  std::memcpy(out.data(), "SCP", 3);
  out[0x05] = static_cast<uint8_t>(nrevs > 255 ? 255 : nrevs);  // revolutions
  out[0x06] = 0;                                                // start track
  out[0x07] = static_cast<uint8_t>(max_slot);  // end track (slot)
  out[0x08] = 0x01;                            // flags: index recorded
  out[0x09] = 0;                               // 16-bit cell width
  out[0x0A] = 0;                               // heads: both (side-0 read)
  out[0x0B] = 0;                               // resolution: 25 ns / tick

  for (const auto& t : tracks) {
    const int slot = t.first;
    const KryoFluxTrack& trk = *t.second;
    const uint32_t toff = static_cast<uint32_t>(out.size());
    wr32le(out, kTlutOff + (4u * static_cast<size_t>(slot)), toff);

    out.push_back('T');
    out.push_back('R');
    out.push_back('K');
    out.push_back(static_cast<uint8_t>(slot));
    const size_t entries_at = out.size();
    out.resize(out.size() + (12u * static_cast<size_t>(nrevs)), 0);
    for (int r = 0; r < nrevs; r++) {
      const KryoFluxRev& rev = trk.revs[static_cast<size_t>(r)];
      const uint32_t doff = static_cast<uint32_t>(out.size()) - toff;
      const uint32_t words = emit_flux_words(rev.flux_25ns, out);
      wr32le(out, entries_at + (12u * static_cast<size_t>(r)) + 0,
             rev.duration_25ns);
      wr32le(out, entries_at + (12u * static_cast<size_t>(r)) + 4, words);
      wr32le(out, entries_at + (12u * static_cast<size_t>(r)) + 8, doff);
    }
  }
  return 0;
}

}  // namespace

uint32_t kryoflux_sck_to_25ns(uint64_t sck_ticks, double sck_hz) {
  if (sck_hz <= 0.0) sck_hz = KFSTREAM_DEFAULT_SCK_HZ;
  // ticks_25ns = sck_ticks * (1e9/25) / sck_hz = sck_ticks * 40000000 / sck_hz.
  const double t = static_cast<double>(sck_ticks) * 40000000.0 / sck_hz;
  const double r = std::floor(t + 0.5);
  if (r < 0.0) return 0;
  return static_cast<uint32_t>(r);
}

int kryoflux_decode_stream(const uint8_t* data, size_t len,
                           KryoFluxTrack& out) {
  out.revs.clear();
  out.sck_hz = KFSTREAM_DEFAULT_SCK_HZ;
  if (data == nullptr || len == 0) return KFSTREAM_E_NO_FLUX;

  std::vector<Transition> flux;
  std::vector<IndexRec> idx;
  double sck_hz = KFSTREAM_DEFAULT_SCK_HZ;
  const int rc = scan_stream(data, len, flux, idx, sck_hz);
  if (rc != 0) return rc;
  out.sck_hz = sck_hz;

  if (flux.empty()) return KFSTREAM_E_NO_FLUX;
  if (idx.size() < 2) return KFSTREAM_E_NO_INDEX;  // need 2 indices for 1 rev

  // Prefix sums: cum[k] = absolute time (sck) of transition k.
  std::vector<uint64_t> cum(flux.size());
  uint64_t acc = 0;
  for (size_t k = 0; k < flux.size(); k++) {
    acc += flux[k].interval;
    cum[k] = acc;
  }
  auto abs_prev = [&](size_t fi) -> uint64_t {  // time of transition fi-1
    return fi == 0 ? 0 : cum[fi - 1];
  };

  // Bind each index to a flux and compute its absolute index time.
  struct Bound {
    size_t fi;
    uint64_t iabs;
  };
  std::vector<Bound> bound;
  bound.reserve(idx.size());
  for (const IndexRec& ir : idx) {
    const size_t fi = resolve_index(flux, ir.stream_pos);
    const uint64_t iabs = abs_prev(fi) + ir.sample_counter;
    bound.push_back({fi, iabs});
  }

  // Pass 2: slice into revolutions between consecutive indices.
  for (size_t i = 0; i + 1 < bound.size(); i++) {
    const size_t fa = bound[i].fi;
    const size_t fb = bound[i + 1].fi;
    const uint64_t ia = bound[i].iabs;
    const uint64_t ib = bound[i + 1].iabs;
    if (fb <= fa || ib <= ia) continue;  // degenerate / empty rev — skip

    KryoFluxRev rev;
    rev.duration_25ns = kryoflux_sck_to_25ns(ib - ia, sck_hz);
    uint32_t prev25 = 0;
    for (size_t k = fa; k < fb; k++) {
      // Relative time from the index to transition k, in sck ticks.
      const uint64_t rel = cum[k] >= ia ? cum[k] - ia : 0;
      const uint32_t c25 = kryoflux_sck_to_25ns(rel, sck_hz);
      const uint32_t word = c25 > prev25 ? c25 - prev25 : 1;  // floor at 1 tick
      rev.flux_25ns.push_back(word);
      prev25 = c25;
    }
    out.revs.push_back(std::move(rev));
  }

  if (out.revs.empty()) return KFSTREAM_E_NO_INDEX;
  return 0;
}

int kryoflux_stream_to_scp(const uint8_t* data, size_t len, uint8_t cyl,
                           uint8_t side, std::vector<uint8_t>& out) {
  out.clear();
  const int slot = (static_cast<int>(cyl) * 2) + static_cast<int>(side);
  if (slot >= kSlots) return KFSTREAM_E_GEOMETRY;

  KryoFluxTrack trk;
  const int rc = kryoflux_decode_stream(data, len, trk);
  if (rc != 0) return rc;
  if (trk.revs.empty()) return KFSTREAM_E_NO_INDEX;

  std::vector<std::pair<int, const KryoFluxTrack*>> tracks;
  tracks.emplace_back(slot, &trk);
  return build_scp(tracks, static_cast<int>(trk.revs.size()), out);
}

int kryoflux_streams_to_scp(const std::vector<KryoFluxMember>& members,
                            std::vector<uint8_t>& out) {
  out.clear();
  if (members.empty()) return KFSTREAM_E_NO_FLUX;

  // Decode all members first (fixed-size vector so the pointers below stay
  // valid — no reallocation while build_scp borrows them).
  std::vector<KryoFluxTrack> decoded(members.size());
  std::vector<std::pair<int, const KryoFluxTrack*>> tracks;
  size_t nrevs = static_cast<size_t>(-1);
  for (size_t i = 0; i < members.size(); i++) {
    const KryoFluxMember& m = members[i];
    const int slot = (static_cast<int>(m.cyl) * 2) + static_cast<int>(m.side);
    if (slot >= kSlots) return KFSTREAM_E_GEOMETRY;
    const int rc = kryoflux_decode_stream(m.data, m.len, decoded[i]);
    if (rc != 0) return rc;
    if (decoded[i].revs.empty()) return KFSTREAM_E_NO_INDEX;
    nrevs = std::min(decoded[i].revs.size(), nrevs);
    tracks.emplace_back(slot, &decoded[i]);
  }
  if (nrevs < 1) return KFSTREAM_E_NO_INDEX;
  return build_scp(tracks, static_cast<int>(nrevs), out);
}
