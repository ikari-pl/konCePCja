/* a2r.cpp — see a2r.h. Parse an A2R3 file's RWCP flux chunk and emit an SCP
 * byte image the flux decoder can read directly.
 *
 * A2R3 layout (verified empirically against real Applesauce captures):
 *   8-byte signature "A2R3" FF 0A 0D 0A, then chunks {id[4], size[4 LE], body}.
 *   RWCP body: version(1), resolution(4 LE, ps/tick), reserved(11), then
 *   captures each starting 'C': type(1), location(2 LE), num_index(1),
 *   index[num_index] (4 LE ticks each), data_len(4 LE), flux[data_len]; the
 *   run ends with 'X'. Flux is accumulated bytes: 0xFF adds 255 with no
 *   transition, any other byte adds its value AND marks a transition.
 *   location = cyl*2 + side, so even locations are side 0 — exactly SCP's
 *   non-legacy TLUT slot for that cylinder.
 */
#include "hw/a2r.h"

#include <cstring>

namespace {

uint16_t rd16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
void wr32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
  v[at] = static_cast<uint8_t>(x);
  v[at + 1] = static_cast<uint8_t>(x >> 8);
  v[at + 2] = static_cast<uint8_t>(x >> 16);
  v[at + 3] = static_cast<uint8_t>(x >> 24);
}
void put_be16(std::vector<uint8_t>& v, uint16_t x) {  // SCP flux words are BE
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x));
}

constexpr int kSlots = 168;   // SCP track-lookup-table entries
constexpr size_t kHdrEnd = 0x2B0;  // header (0x10) + TLUT (168*4)

// One side-0 flux capture: where its flux bytes live in the A2R buffer and the
// revolution length (first index tick) used to truncate it to one turn.
struct Cap {
  size_t flux_off;
  uint32_t flux_len;
  uint32_t index_ticks;
};

// Append `cap`'s flux to `out` as SCP big-endian words, truncated to one
// revolution. Writes the emitted tick span (index_time) and word count.
void emit_rev(const uint8_t* a2r, const Cap& cap, std::vector<uint8_t>& out,
              uint32_t& index_time, uint32_t& words) {
  const uint8_t* f = a2r + cap.flux_off;
  uint32_t acc = 0, cum = 0, nwords = 0;
  for (uint32_t i = 0; i < cap.flux_len; i++) {
    acc += f[i];
    if (f[i] == 0xFF) continue;  // no transition yet: keep accumulating
    if (cap.index_ticks != 0 && cum >= cap.index_ticks) break;  // one rev done
    uint32_t iv = acc != 0 ? acc : 1;  // 0-tick would collide with SCP overflow
    acc = 0;
    while (iv > 0xFFFF) {  // SCP 0x0000 word carries 65536 ticks
      put_be16(out, 0);
      iv -= 0x10000;
      nwords++;
    }
    put_be16(out, static_cast<uint16_t>(iv));
    nwords++;
    cum += iv;
  }
  words = nwords;
  index_time = cap.index_ticks != 0 ? cap.index_ticks : cum;
}

}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other translation units/tests; internal linkage would break the link
int a2r_to_scp(const uint8_t* a2r, size_t len, std::vector<uint8_t>& out) {
  out.clear();
  if (a2r == nullptr || len < 8 || std::memcmp(a2r, "A2R3", 4) != 0)
    return A2R_E_NOT_A2R;

  // Locate the RWCP (raw flux) chunk.
  const uint8_t* rwcp = nullptr;
  uint32_t rwcp_size = 0;
  bool saw_strm = false;
  for (size_t off = 8; off + 8 <= len;) {
    const uint8_t* id = a2r + off;
    const uint32_t sz = rd32(a2r + off + 4);
    if (off + 8 + static_cast<size_t>(sz) > len) return A2R_E_TRUNCATED;
    if (std::memcmp(id, "RWCP", 4) == 0) {
      rwcp = a2r + off + 8;
      rwcp_size = sz;
    } else if (std::memcmp(id, "STRM", 4) == 0) {
      saw_strm = true;
    }
    off += 8 + sz;
  }
  if (rwcp == nullptr) return saw_strm ? A2R_E_UNSUPPORTED : A2R_E_NO_FLUX;
  if (rwcp_size < 16) return A2R_E_TRUNCATED;

  // Collect up to two revolutions per side-0 (even-location) timing capture.
  std::vector<std::vector<Cap>> per_slot(kSlots);
  for (size_t p = 16; p < rwcp_size && rwcp[p] == 'C';) {
    if (p + 5 > rwcp_size) return A2R_E_TRUNCATED;
    const uint8_t ctype = rwcp[p + 1];
    const uint16_t loc = rd16(rwcp + p + 2);
    const uint8_t nidx = rwcp[p + 4];
    const size_t ip = p + 5;
    if (ip + (4u * nidx) + 4 > rwcp_size) return A2R_E_TRUNCATED;
    const uint32_t index_ticks = nidx >= 1 ? rd32(rwcp + ip) : 0;
    const size_t dp = ip + (4u * nidx);
    const uint32_t dlen = rd32(rwcp + dp);
    const size_t fp = dp + 4;
    if (fp + dlen > rwcp_size) return A2R_E_TRUNCATED;
    if (ctype == 1 && loc < kSlots && (loc & 1) == 0 &&
        per_slot[loc].size() < 2)
      per_slot[loc].push_back(
          {static_cast<size_t>(rwcp - a2r) + fp, dlen, index_ticks});
    p = fp + dlen;
  }

  int max_slot = -1;
  for (int s = 0; s < kSlots; s++)
    if (!per_slot[s].empty()) max_slot = s;
  if (max_slot < 0) return A2R_E_NO_FLUX;

  // SCP header + track-lookup table.
  out.assign(kHdrEnd, 0);
  std::memcpy(out.data(), "SCP", 3);
  out[0x05] = 2;                                 // revolutions per track
  out[0x06] = 0;                                 // start track
  out[0x07] = static_cast<uint8_t>(max_slot);    // end track
  out[0x08] = 0x01;                              // flags: index recorded
  out[0x09] = 0;                                 // 16-bit cell width
  out[0x0A] = 0;                                 // heads: both (side-0 filled)
  out[0x0B] = 4;                                 // resolution: 125 ns/tick

  for (int s = 0; s <= max_slot; s++) {
    if (per_slot[s].empty()) continue;  // absent cylinder / odd (side-1) slot
    const uint32_t toff = static_cast<uint32_t>(out.size());
    wr32(out, 0x10 + (4u * static_cast<size_t>(s)), toff);  // TLUT entry

    out.push_back('T');
    out.push_back('R');
    out.push_back('K');
    out.push_back(static_cast<uint8_t>(s));
    const size_t entries_at = out.size();
    out.resize(out.size() + 24, 0);  // two 12-byte revolution entries
    for (int r = 0; r < 2; r++) {
      const Cap& cap = per_slot[s][per_slot[s].size() >= 2 ? r : 0];
      const uint32_t doff = static_cast<uint32_t>(out.size()) - toff;
      uint32_t index_time = 0, words = 0;
      emit_rev(a2r, cap, out, index_time, words);
      wr32(out, entries_at + (12u * r) + 0, index_time);
      wr32(out, entries_at + (12u * r) + 4, words);
      wr32(out, entries_at + (12u * r) + 8, doff);
    }
  }
  return 0;
}
