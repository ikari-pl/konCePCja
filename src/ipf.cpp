// IPF disk image support (Interchangeable Preservation Format).
//
// The clean-room decoder in src/ipf_decode.{h,cpp} (namespace ipf) is the ONLY
// IPF path: it parses the container, decodes the SPS-encoder stream, and fills
// the legacy t_drive sector view (disc-tools / DSK-export). The vendored,
// non-commercial SPS Decoder Library has been deleted — a CAPS-encoder
// (encoderType 1) IPF is now rejected with a clear error, with no fallback.
//
// This file also carries the SCP flux emitter (scp_from_mfm_tracks + helpers),
// which is NOT part of the IPF decode path: mfm_encode, hfe, and the IPF
// side-0 flux mirror all feed it to build in-memory SuperCard Pro containers.

#include "ipf.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "errors.h"
#include "hw_views.h"
#include "ipf_decode.h"
#include "log.h"
#include "slotshandler.h"  // dsk_eject

// Decode `len` bytes of an IPF image into the legacy sector view via the
// clean-room decoder. A CAPS-encoder (encoderType 1) IPF is rejected outright
// — there is no fallback.
namespace {
int ipf_load_bytes(const uint8_t* data, size_t len, t_drive* drive) {
  dsk_eject(drive);  // release any previously-loaded medium first

  ipf::Image img;
  ipf::Status const st = img.open(data, len);
  if (st != ipf::Status::Ok) {
    LOG_ERROR("IPF: " << ipf::status_str(st)
                      << (st == ipf::Status::UnsupportedEncoder
                              ? " (CAPS-encoder IPF unsupported — re-image as "
                                "SPS/SCP/HFE)"
                              : ""));
    return ERR_DSK_INVALID;
  }
  if (img.fill_drive(drive) != ipf::Status::Ok) {
    LOG_ERROR("IPF: sector-view fill failed");
    return ERR_DSK_INVALID;
  }
  return 0;
}
}  // namespace

// Read an open IPF file handle fully into memory (the clean decoder takes raw
// bytes, so there is no temp-file round-trip), then decode it. Reads from the
// current position to EOF, which works for both freshly-opened files and the
// zip-extracted temp files the slot loader hands over.
int ipf_load(FILE* pfileIn, t_drive* drive) {
  if (pfileIn == nullptr) {
    LOG_ERROR("IPF: null file handle");
    return ERR_DSK_INVALID;
  }

  std::vector<uint8_t> bytes;
  uint8_t chunk[64 * 1024];
  size_t n = 0;
  while ((n = fread(chunk, 1, sizeof(chunk), pfileIn)) > 0)
    bytes.insert(bytes.end(), chunk, chunk + n);
  if (ferror(pfileIn) != 0) {
    LOG_ERROR("IPF: error reading file");
    return ERR_DSK_INVALID;
  }
  if (bytes.empty()) {
    LOG_ERROR("IPF: empty file");
    return ERR_DSK_INVALID;
  }
  return ipf_load_bytes(bytes.data(), bytes.size(), drive);
}

// Attempt to load the named file as an IPF disk image.
int ipf_load(const std::string& filename, t_drive* drive) {
  FILE* f = fopen(filename.c_str(), "rb");
  if (f == nullptr) {
    LOG_ERROR("Couldn't open file: " << filename);
    return ERR_DSK_INVALID;
  }
  int const rc = ipf_load(f, drive);
  fclose(f);
  return rc;
}

// ---- engine=1 flux: MFM bitcell tracks → in-memory SCP flux container ----
//
// The sub-cycle FDC eats flux (fdc_attach_flux → hw/flux.cpp). Each track
// arrives as an MFM bitcell stream (2 µs cells at double density); a transition
// on every '1' bitcell IS the flux timeline, so the conversion is exact: the
// interval between two transitions is the bitcell distance times 80 ticks of
// the SCP's 25 ns clock. Fed by the clean IPF decoder's side-0 mirror,
// hfe_to_scp, and mfm_encode.

namespace {

constexpr uint32_t kTicksPerCell = 80;  // 2 µs bitcell / 25 ns SCP tick
constexpr size_t kScpTlutEnd = 0x10 + (4 * 168);  // header + track offset table
constexpr size_t kScpMaxCyls = 84;                // slot space / 2 (side 0)

void push_be16(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back((x >> 8) & 0xff);
  v.push_back(x & 0xff);
}

void put_le32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
  v[at] = x & 0xff;
  v[at + 1] = (x >> 8) & 0xff;
  v[at + 2] = (x >> 16) & 0xff;
  v[at + 3] = (x >> 24) & 0xff;
}

// Append one revolution's flux words (16-bit big-endian, 0x0000 = +65536
// carried into the next word). Returns the word count. A trailing
// transition-less gap is dropped — exactly what a real capture does.
uint32_t push_flux_rev(std::vector<uint8_t>& out, const t_mfm_rev& rev) {
  uint32_t words = 0, gap = 0;
  for (uint32_t i = 0; i < rev.nbits; i++) {
    gap++;
    if (!((rev.bits[i >> 3] >> (7 - (i & 7))) & 1)) continue;
    uint32_t t = gap * kTicksPerCell;
    gap = 0;
    while (t >= 0x10000u) {
      if (t == 0x10000u) {  // an exact multiple has no 2-word encoding:
        t = 0xffffu;        // shave 25 ns, far below the PLL's jitter floor
        break;
      }
      push_be16(out, 0);
      words++;
      t -= 0x10000u;
    }
    push_be16(out, t);
    words++;
  }
  return words;
}

}  // namespace

std::vector<uint8_t> scp_from_mfm_tracks(const std::vector<t_mfm_track>& cyls) {
  size_t revs = 0;
  for (const auto& c : cyls)
    if (!c.empty()) {
      revs = c.size();
      break;
    }
  if (revs == 0 || revs > 255 || cyls.size() > kScpMaxCyls) return {};

  std::vector<uint8_t> scp(kScpTlutEnd, 0);
  scp[0] = 'S';
  scp[1] = 'C';
  scp[2] = 'P';
  scp[3] = 0x19;  // creator/version byte — informational only
  scp[4] = 0x80;  // disk type: "other"
  scp[5] = static_cast<uint8_t>(revs);
  scp[6] = 0;                                            // start track
  scp[7] = static_cast<uint8_t>((cyls.size() - 1) * 2);  // last side-0 slot
  scp[8] = 0x01;  // flags: index-synced capture
  scp[9] = 16;    // 16-bit bitcell words
  scp[0x0a] = 0;  // both-heads slot layout (side-0 slots = cyl*2)
  scp[0x0b] = 0;  // resolution: 25 ns

  for (size_t cyl = 0; cyl < cyls.size(); cyl++) {
    const t_mfm_track& trk = cyls[cyl];
    if (trk.empty()) continue;          // absent slot = unformatted track
    if (trk.size() != revs) return {};  // one revolution count per file
    const uint32_t toff = static_cast<uint32_t>(scp.size());
    put_le32(scp, 0x10 + (4 * (cyl * 2)), toff);
    scp.push_back('T');
    scp.push_back('R');
    scp.push_back('K');
    scp.push_back(static_cast<uint8_t>(cyl * 2));
    const size_t tdh_entries = scp.size();
    scp.resize(scp.size() + (12 * revs), 0);
    for (size_t r = 0; r < revs; r++) {
      const uint32_t data_off = static_cast<uint32_t>(scp.size()) - toff;
      const uint32_t words = push_flux_rev(scp, trk[r]);
      const size_t e = tdh_entries + (12 * r);
      put_le32(scp, e, trk[r].nbits * kTicksPerCell);  // index-to-index ticks
      put_le32(scp, e + 4, words);
      put_le32(scp, e + 8, data_off);
    }
  }

  uint32_t sum = 0;  // container checksum: every byte from 0x10 to EOF
  for (size_t i = 0x10; i < scp.size(); i++) sum += scp[i];
  put_le32(scp, 0x0c, sum);
  return scp;
}
