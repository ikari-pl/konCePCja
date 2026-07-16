/* flux_synth.h — the test-side MFM/flux/SCP SYNTHESIS toolkit (header-only).
 * The oracle for the flux decoder and the rotating-flux FDC: sector maps →
 * IBM System 34 track bitstreams → flux intervals → SCP containers, with
 * deterministic jitter. Extracted from flux_test.cpp so the FDC tests can
 * build the same discs. See docs/hardware/flux-media.md. */
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace fluxsynth {

// ------------------------------------------------------------ MFM encoder ---

inline uint16_t crc_ccitt(uint16_t crc, uint8_t b) {
  crc ^= static_cast<uint16_t>(b) << 8;
  for (int i = 0; i < 8; i++)
    crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                         : static_cast<uint16_t>(crc << 1);
  return crc;
}

// MFM bitcell emitter: data bytes get the clock rule (clock = 1 only between
// two zero data bits); raw16 injects the missing-clock sync words verbatim.
struct MfmBits {
  std::vector<uint8_t> bits;  // one 0/1 per bitcell
  int last_data = 0;
  void data_byte(uint8_t d) {
    for (int k = 7; k >= 0; k--) {
      const int bit = (d >> k) & 1;
      bits.push_back(static_cast<uint8_t>((!last_data && !bit) ? 1 : 0));
      bits.push_back(static_cast<uint8_t>(bit));
      last_data = bit;
    }
  }
  void raw16(uint16_t w) {
    for (int k = 15; k >= 0; k--)
      bits.push_back(static_cast<uint8_t>((w >> k) & 1));
    last_data = w & 1;
  }
  void gap(int n, uint8_t v) {
    for (int i = 0; i < n; i++) data_byte(v);
  }
};

struct Sector {
  uint8_t c, h, r, n;
  std::vector<uint8_t> data;
  bool corrupt_data = false;  // flip a payload byte AFTER computing its CRC
};

// One IBM System 34 track: gap4a, index mark, then per sector the ID field,
// gap2, the data field, gap3. Returns the bitcell stream.
inline std::vector<uint8_t> track_bits(const std::vector<Sector>& secs) {
  MfmBits t;
  t.gap(80, 0x4E);  // gap 4a
  t.gap(12, 0x00);
  for (int i = 0; i < 3; i++) t.raw16(0x5224);  // C2* index mark
  t.data_byte(0xFC);
  t.gap(50, 0x4E);  // gap 1
  for (const Sector& s : secs) {
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < 3; i++) crc = crc_ccitt(crc, 0xA1);
    const uint16_t preset = crc;
    t.gap(12, 0x00);
    for (int i = 0; i < 3; i++) t.raw16(0x4489);  // A1* sync
    t.data_byte(0xFE);
    crc = crc_ccitt(preset, 0xFE);
    const uint8_t id[4] = {s.c, s.h, s.r, s.n};
    for (uint8_t b : id) {
      t.data_byte(b);
      crc = crc_ccitt(crc, b);
    }
    t.data_byte(static_cast<uint8_t>(crc >> 8));
    t.data_byte(static_cast<uint8_t>(crc & 0xFF));
    t.gap(22, 0x4E);  // gap 2
    t.gap(12, 0x00);
    for (int i = 0; i < 3; i++) t.raw16(0x4489);
    t.data_byte(0xFB);
    crc = crc_ccitt(preset, 0xFB);
    for (size_t i = 0; i < s.data.size(); i++) {
      crc = crc_ccitt(crc, s.data[i]);  // CRC of the TRUE byte...
      const uint8_t wire =
          (s.corrupt_data && i == 10)
              ? static_cast<uint8_t>(s.data[i] ^
                                     0xFF)  // ...but a flipped wire byte
              : s.data[i];
      t.data_byte(wire);
    }
    t.data_byte(static_cast<uint8_t>(crc >> 8));
    t.data_byte(static_cast<uint8_t>(crc & 0xFF));
    t.gap(54, 0x4E);  // gap 3
  }
  t.gap(120, 0x4E);  // gap 4b
  return t.bits;
}

// ------------------------------------------------- bitcells → flux → SCP ----

// Deterministic PRNG (LCG, fixed seed) — no wall-clock seeding, ever.
struct Prng {
  uint32_t s;
  explicit Prng(uint32_t seed) : s(seed) {}
  double unit() {  // [0, 1)
    s = s * 1664525u + 1013904223u;
    return static_cast<double>(s >> 8) / 16777216.0;
  }
};

// Bitcells → flux intervals (ticks). `cell` is the half-cell in ticks (80 =
// nominal 2 µs at 25 ns); `jitter` is the per-interval relative error bound.
inline std::vector<uint32_t> bits_to_flux(const std::vector<uint8_t>& bits,
                                          double cell, double jitter,
                                          Prng* rng) {
  std::vector<uint32_t> out;
  int run = 0;
  for (uint8_t b : bits) {
    run++;
    if (!b) continue;
    double t = run * cell;
    if (jitter > 0.0) t *= 1.0 + jitter * (2.0 * rng->unit() - 1.0);
    out.push_back(static_cast<uint32_t>(std::lround(t)));
    run = 0;
  }
  return out;
}

// SCP container: header + 168-slot offset table + per-cylinder TRK blocks.
// tracks[cyl][rev] = flux intervals. Standard slot mapping (cyl*2, side 0).
inline std::vector<uint8_t> build_scp(
    const std::vector<std::vector<std::vector<uint32_t>>>& tracks) {
  const size_t cyls = tracks.size();
  const size_t revs = tracks[0].size();
  std::vector<uint8_t> f(0x2B0, 0);
  std::memcpy(f.data(), "SCP", 3);
  f[0x03] = 0x22;  // version (uninterpreted)
  f[0x04] = 0x80;  // disk type (uninterpreted)
  f[0x05] = static_cast<uint8_t>(revs);
  f[0x06] = 0;                                     // start track
  f[0x07] = static_cast<uint8_t>((cyls - 1) * 2);  // end track
  f[0x08] = 0x01;                                  // flags: index-cued
  f[0x09] = 0;                                     // 16-bit cells
  f[0x0A] = 1;                                     // side 0 only
  f[0x0B] = 0;                                     // resolution: 25 ns ticks
  auto w32 = [&](size_t off, uint32_t v) {
    f[off] = static_cast<uint8_t>(v & 0xFF);
    f[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    f[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    f[off + 3] = static_cast<uint8_t>(v >> 24);
  };
  for (size_t cyl = 0; cyl < cyls; cyl++) {
    const size_t tdh = f.size();
    w32(0x10 + 4 * (cyl * 2), static_cast<uint32_t>(tdh));
    f.insert(f.end(), {'T', 'R', 'K', static_cast<uint8_t>(cyl * 2)});
    f.resize(f.size() + 12 * revs, 0);  // revolution table, patched below
    for (size_t r = 0; r < revs; r++) {
      const size_t data_off = f.size() - tdh;
      uint32_t duration = 0, words = 0;
      for (uint32_t v : tracks[cyl][r]) {
        duration += v;
        while (v >= 0x10000) {  // overflow convention: 0x0000 = +65536 ticks
          f.push_back(0);
          f.push_back(0);
          words++;
          v -= 0x10000;
        }
        EXPECT_NE(v, 0u) << "test flux must not need a zero remainder";
        f.push_back(static_cast<uint8_t>(v >> 8));  // flux words: BIG-endian
        f.push_back(static_cast<uint8_t>(v & 0xFF));
        words++;
      }
      const size_t e = tdh + 4 + 12 * r;
      w32(e, duration);
      w32(e + 4, words);
      w32(e + 8, static_cast<uint32_t>(data_off));
    }
  }
  uint32_t sum = 0;
  for (size_t i = 0x10; i < f.size(); i++) sum += f[i];
  w32(0x0C, sum);
  return f;
}

// Convenience: single-revolution SCP from per-cylinder sector maps.
inline std::vector<uint8_t> scp_from_sectors(
    const std::vector<std::vector<Sector>>& d, double cell = 80.0,
    double jitter = 0.0, uint32_t seed = 0x5EED) {
  Prng rng(seed);
  std::vector<std::vector<std::vector<uint32_t>>> tracks;
  for (const std::vector<Sector>& t : d)
    tracks.push_back({bits_to_flux(track_bits(t), cell, jitter, &rng)});
  return build_scp(tracks);
}

// ------------------------------------------------------------ disc content --

// The same AMSDOS DATA-format content the fdc_test builder uses: 9×512-byte
// sectors per track (&C1..&C9), 0xE5 fill, and a directory in track 0 sector
// &C1 holding HELLO.BAS and README.TXT (1K each, blocks 2 and 3).
inline std::vector<std::vector<Sector>> amsdos_content(int tracks) {
  std::vector<std::vector<Sector>> d;
  for (int t = 0; t < tracks; t++) {
    std::vector<Sector> ts;
    for (int s = 0; s < 9; s++)
      ts.push_back({static_cast<uint8_t>(t), 0, static_cast<uint8_t>(0xC1 + s),
                    2, std::vector<uint8_t>(512, 0xE5), false});
    d.push_back(ts);
  }
  auto dirent = [&](int idx, const char* name, const char* ext, uint8_t blk) {
    uint8_t* e = &d[0][0].data[static_cast<size_t>(idx) * 32];
    const size_t nl = std::strlen(name), el = std::strlen(ext);
    e[0] = 0;  // user 0
    for (size_t i = 0; i < 8; i++)
      e[1 + i] = static_cast<uint8_t>(i < nl ? name[i] : ' ');
    for (size_t i = 0; i < 3; i++)
      e[9 + i] = static_cast<uint8_t>(i < el ? ext[i] : ' ');
    e[12] = e[13] = e[14] = 0;  // EX, S1, S2
    e[15] = 8;                  // RC: 8 records = 1K
    std::memset(e + 16, 0, 16);
    e[16] = blk;
  };
  dirent(0, "HELLO", "BAS", 2);
  dirent(1, "README", "TXT", 3);
  return d;
}

}  // namespace fluxsynth
