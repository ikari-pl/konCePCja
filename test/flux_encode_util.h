/* flux_encode_util.h — synthetic DSK builders / readers shared by the flux
 * WRITE encoder tests (mfm_encode / scp_write / hfe_write). Header-only,
 * inline; no external fixtures so the tests always run. */
#ifndef KONCPC_TEST_FLUX_ENCODE_UTIL_H
#define KONCPC_TEST_FLUX_ENCODE_UTIL_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace fluxtest {

struct SectorSpec {
  uint8_t chrn[4] = {0, 0, 1, 2};  // C, H, R, N (N=2 -> 512 bytes)
  std::vector<uint8_t> payload;
  uint8_t dam = 0xFB;
};

struct DecodedSector {
  uint8_t chrn[4] = {0, 0, 0, 0};
  std::vector<uint8_t> payload;
};

inline void put16le(std::vector<uint8_t>& buf, std::size_t offset,
                    uint16_t value) {
  buf[offset] = static_cast<uint8_t>(value & 0xFF);
  buf[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

inline uint16_t get16le(const uint8_t* base) {
  return static_cast<uint16_t>(base[0] | (base[1] << 8));
}

// A deterministic per-(track,sector,size) payload so comparisons are exact.
inline std::vector<uint8_t> make_payload(int track, int sector, std::size_t len,
                                         uint8_t salt = 0) {
  std::vector<uint8_t> data(len, 0);
  for (std::size_t i = 0; i < len; i++)
    data[i] = static_cast<uint8_t>(
        ((track * 37) + (sector * 11) + (static_cast<int>(i) * 7) + salt) &
        0xFF);
  return data;
}

// Build a standard "MV - CPCEMU" single-sided DSK from per-track sector lists.
// All sectors are assumed to share a size code (the CPC standard case).
inline std::vector<uint8_t> build_standard_dsk(
    const std::vector<std::vector<SectorSpec>>& tracks) {
  const std::size_t num_tracks = tracks.size();

  // Uniform track block size: header + every sector's stored bytes, padded up.
  std::size_t max_payload = 0;
  for (const auto& sectors : tracks) {
    std::size_t sum = 0;
    for (const auto& sec : sectors)
      sum += static_cast<std::size_t>(128) << (sec.chrn[3] & 0x07);
    max_payload = std::max(max_payload, sum);
  }
  const std::size_t block =
      (0x100 + max_payload + 0xFF) & ~static_cast<std::size_t>(0xFF);

  std::vector<uint8_t> dsk(0x100 + (block * num_tracks), 0);
  std::memcpy(dsk.data(), "MV - CPCEMU Disk-File\r\nDisk-Info\r\n", 34);
  std::memcpy(dsk.data() + 0x22, "konCePCja-test", 14);
  dsk[0x30] = static_cast<uint8_t>(num_tracks);
  dsk[0x31] = 1;  // single sided
  put16le(dsk, 0x32, static_cast<uint16_t>(block));

  for (std::size_t track = 0; track < num_tracks; track++) {
    const std::vector<SectorSpec>& sectors = tracks[track];
    uint8_t* info = dsk.data() + 0x100 + (block * track);
    std::memcpy(info, "Track-Info\r\n", 12);
    info[0x10] = static_cast<uint8_t>(track);
    info[0x11] = 0;  // side 0
    info[0x14] = sectors.empty() ? 2 : sectors[0].chrn[3];
    info[0x15] = static_cast<uint8_t>(sectors.size());
    info[0x16] = 0x4E;
    info[0x17] = 0xE5;

    std::size_t data_off = 0x100;
    for (std::size_t sec = 0; sec < sectors.size(); sec++) {
      uint8_t* sinfo = info + 0x18 + (8 * sec);
      std::memcpy(sinfo, sectors[sec].chrn, 4);
      sinfo[4] = 0;                                         // ST1
      sinfo[5] = (sectors[sec].dam == 0xF8) ? 0x40 : 0x00;  // ST2 control mark
      const std::size_t len = static_cast<std::size_t>(128)
                              << (sectors[sec].chrn[3] & 0x07);
      std::memcpy(info + data_off, sectors[sec].payload.data(),
                  sectors[sec].payload.size());
      data_off += len;
    }
  }
  return dsk;
}

// Read a standard or EXTENDED DSK into per-track sector lists (CHRN + payload).
inline std::vector<std::vector<DecodedSector>> read_dsk(const uint8_t* dsk,
                                                        std::size_t len) {
  std::vector<std::vector<DecodedSector>> out;
  if (dsk == nullptr || len < 0x100) return out;
  const bool extended = std::memcmp(dsk, "EXTENDED", 8) == 0;
  const int num_tracks = dsk[0x30];
  const int num_sides = dsk[0x31] == 0 ? 1 : dsk[0x31];
  const uint16_t std_size = get16le(dsk + 0x32);

  std::size_t off = 0x100;
  for (int idx = 0; idx < num_tracks * num_sides; idx++) {
    const std::size_t block =
        extended ? static_cast<std::size_t>(dsk[0x34 + idx]) * 256u : std_size;
    const int side = idx % num_sides;
    if (block == 0 || off + 0x100 > len) {
      if (side == 0) out.emplace_back();
      off += block;
      continue;
    }
    const uint8_t* info = dsk + off;
    std::vector<DecodedSector> sectors;
    if (std::memcmp(info, "Track-Info", 10) == 0) {
      const int num_sec = info[0x15];
      std::size_t data_off = 0x100;
      for (int sec = 0; sec < num_sec; sec++) {
        const uint8_t* sinfo =
            info + 0x18 + (8 * static_cast<std::size_t>(sec));
        DecodedSector decoded;
        std::memcpy(decoded.chrn, sinfo, 4);
        const std::size_t logical = static_cast<std::size_t>(128)
                                    << (sinfo[3] & 0x07);
        const std::size_t actual = extended ? get16le(sinfo + 6) : logical;
        if (off + data_off + logical <= len)
          decoded.payload.assign(info + data_off, info + data_off + logical);
        data_off += (actual != 0 ? actual : logical);
        sectors.push_back(std::move(decoded));
      }
    }
    if (side == 0) out.push_back(std::move(sectors));
    off += block;
  }
  return out;
}

}  // namespace fluxtest

#endif  // KONCPC_TEST_FLUX_ENCODE_UTIL_H
