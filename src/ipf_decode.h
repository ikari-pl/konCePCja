// Clean-room IPF (Interchangeable Preservation Format) decoder core.
//
// Built ONLY from docs/hardware/ipf-format.md (the authoritative clean-room
// spec, derived from the public DrCoolZic IPF documentation v1.6) — never from
// the non-commercially-licensed SPS Decoder Library nor any GPL IPF reader.
// This is now the sole IPF path: src/ipf.cpp calls straight into it (the SPS
// Decoder Library has been deleted).
//
// The module parses the IPF container (§2), decodes the SPS-encoder stream
// elements (§3), reconstructs the raw MFM bitcell stream (§4) into the
// t_mfm_rev type, and fills the legacy t_drive/t_track/t_sector sector view
// (§5) with the spec's prescribed geometry-bounds fix (§5.4b) and the
// zero-based `sides` convention (§5.4a).
//
// Scope (v1): SPS encoder (INFO.encoderType == 2) only; Auto density (2)
// modelled at uniform 2 us cells; densities 3-9 decode with uniform cells + a
// warning; the CAPS encoder (encoderType 1) is detected and rejected. See
// ipf-format.md §8 for the full coverage/risk table.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "hw_views.h"  // t_drive / t_track / t_sector, DSK_TRACKMAX/SIDEMAX
#include "ipf.h"       // t_mfm_rev / t_mfm_track (the flux-path output type)

namespace ipf {

// ---- CRC-32/ISO-HDLC (§6) -------------------------------------------------
// polynomial 0x04C11DB7, reflected (0xEDB88320 table), init 0xFFFFFFFF, final
// XOR 0xFFFFFFFF — the zlib/PNG CRC. The container's record CRCs use this.
// Exposed so the §6 self-test vector can pin the routine directly.
uint32_t crc32(const uint8_t* data, size_t len);

// ---- Status ---------------------------------------------------------------
enum class Status : std::uint8_t {
  Ok = 0,
  Truncated,           // a header/block/element ran past the end of the file
  BadMagic,            // first record is not "CAPS"
  BadCrc,              // a record CRC did not verify
  BadRecord,           // malformed record (length < 12, bad structure)
  UnsupportedEncoder,  // INFO.encoderType != 2 (CAPS encoder — Phase C)
  BadGeometry,         // geometry out of DSK_TRACKMAX/SIDEMAX bounds (§5.4b)
  TooLarge,            // a track's trackBits exceeds the sanity cap
  DecodeError          // stream-element/accounting inconsistency
};
const char* status_str(Status s);

// ---- Parsed geometry (mirrors CleanImageInfo, spec §5.1) ------------------
struct CleanImageInfo {
  int min_cyl = 0;
  int max_cyl = 0;
  int min_head = 0;
  int max_head = 0;
  int encoder_type = 0;  // always 2 (SPS) once open() succeeds
};

// ---- One decoded MFM pass of one track (mirrors CleanTrackMFM, §5.1) ------
// bits: raw MFM bitcells (clock+data interleaved), packed MSB-first.
// nbits: valid bitcell count (bits.size() == ceil(nbits/8)).
// nbits == 0 means an empty/unformatted track.
struct CleanTrackMFM {
  std::vector<uint8_t> bits;
  uint32_t nbits = 0;
  bool flakey = false;
};

// ---- Stream-element model (§3), exposed for unit tests --------------------
enum class DataType : uint8_t {
  Sync = 1,   // already-encoded raw MFM bits, emitted verbatim
  Data = 2,   // decoded data bytes/bits, MFM-encoded x2
  Gap = 3,    // decoded intra-block gap fill, MFM-encoded x2
  Raw = 4,    // raw MFM bits, verbatim
  Fuzzy = 5,  // no sample stored; freshly generated random data, MFM-encoded
};

enum class GapKind : uint8_t {
  GapLength = 1,     // length (in decoded bits) to produce from the sample
  SampleLength = 2,  // the sample value (size = sample bit-length)
};

struct DataElem {
  DataType type = DataType::Data;
  uint32_t size = 0;            // bytes if !data_in_bit, else bits
  std::vector<uint8_t> sample;  // empty for Fuzzy
};

struct GapElem {
  GapKind kind = GapKind::GapLength;
  uint32_t size = 0;            // always in bits
  std::vector<uint8_t> sample;  // only for SampleLength
};

// One block's decoded stream lists (data + forward/backward gap lists).
struct BlockStreams {
  std::vector<DataElem> data;
  std::vector<GapElem> fwd;
  std::vector<GapElem> bwd;
};

// One 32-byte block descriptor (§2.7, SPS variant).
struct BlockDesc {
  uint32_t data_bits = 0;
  uint32_t gap_bits = 0;
  uint32_t gap_offset = 0;  // relative to the Extra Data Block
  uint32_t cell_type = 0;
  uint32_t block_encoder = 0;
  uint32_t block_flags = 0;  // bit0 ForwardGap, bit1 BackwardGap, bit2 DataInBit
  uint32_t gap_default = 0;
  uint32_t data_offset = 0;  // relative to the Extra Data Block
};

// ---- The open image ------------------------------------------------------
// Holds a copy of the file bytes + the parsed record set + a per-image weak-bit
// RNG. lock_track() runs one decode pass and advances the RNG so consecutive
// passes of a flakey track differ (§4.3/§4.4).
class Image {
 public:
  Image() = default;

  // Parse + validate every record (§2), verify CRCs (§6), match IMGE<->DATA
  // by dataKey, and validate geometry (§5.4b). No decoding yet.
  Status open(const uint8_t* data, size_t len);
  Status open(const std::vector<uint8_t>& bytes) {
    return open(bytes.data(), bytes.size());
  }

  const CleanImageInfo& info() const { return info_; }
  bool is_open() const { return open_; }

  // Is there an IMGE for this (cyl, head)?
  bool has_track(int cyl, int head) const;
  // Fuzzy flag of the matching IMGE (false if none).
  bool track_flakey(int cyl, int head) const;

  // Decode one MFM pass of (cyl, head). Empty (nbits == 0) for an unformatted
  // track (no IMGE, empty DATA, blockCount 0, or Noise density). Advances the
  // weak-bit RNG when the track carries Fuzzy elements. Returns false only on
  // a genuine decode inconsistency (accounting mismatch, OOB) — a legitimately
  // empty track returns true with out.nbits == 0.
  bool lock_track(int cyl, int head, CleanTrackMFM& out);

  // Seed the weak-bit RNG for reproducible fixtures (§4.3 deterministic hook).
  void seed_rng(uint64_t s) { rng_state_ = s ? s : 0x9E3779B97F4A7C15ULL; }

  // Fill the legacy sector view (§5.3). Allocates each t_track::data with
  // new[] (dsk_eject frees with delete[]). Applies §5.4 fixes. Must have
  // passed open() first. On any per-track decode failure returns an error and
  // does NOT leave a half-filled drive claiming success (§5.4 whole-load
  // abort). The caller owns the new[] buffers (free via free_drive_tracks()).
  Status fill_drive(t_drive* drive);

  // side-0 flux mirror: `revs` decode passes per cylinder (§5.2). Non-flakey
  // cylinders reuse one pass; flakey cylinders decode `revs` times.
  std::vector<t_mfm_track> mirror_side0(int revs);

  // Parse-only accessor for the stream-element unit tests (§7 vectors).
  // Decode the block descriptors + stream lists of (cyl, head) without MFM
  // reconstruction. Returns false if the track is empty/absent.
  bool parse_block_streams(int cyl, int head, std::vector<BlockDesc>& descs,
                           std::vector<BlockStreams>& streams) const;

 private:
  struct ImgeRec {
    uint32_t track = 0, side = 0, density = 0, signal_type = 0;
    uint32_t track_bytes = 0, start_byte_pos = 0, start_bit_pos = 0;
    uint32_t data_bits = 0, gap_bits = 0, track_bits = 0;
    uint32_t block_count = 0, track_flags = 0, data_key = 0;
  };
  struct DataRec {
    uint32_t data_key = 0;
    size_t extra_off = 0;  // offset of the Extra Data Block into buf_
    uint32_t extra_len = 0;
  };

  const ImgeRec* find_imge(int cyl, int head) const;
  const DataRec* find_data(uint32_t data_key) const;

  // Decode one pass into `out` given the matched IMGE + DATA. Returns false on
  // inconsistency; a legitimately empty track returns true, out.nbits == 0.
  bool decode_track(const ImgeRec& imge, const DataRec& data,
                    CleanTrackMFM& out);

  std::vector<uint8_t> buf_;  // owned copy of the whole file
  std::vector<ImgeRec> imges_;
  std::vector<DataRec> datas_;
  CleanImageInfo info_;
  uint64_t rng_state_ = 0x9E3779B97F4A7C15ULL;
  bool open_ = false;
};

// ---- Whole-image convenience (matches the integration entry shape) --------
struct DecodedIpf {
  CleanImageInfo info;
  std::vector<t_mfm_track> side0;  // `revs`-revolution flux mirror, side 0
};

// Parse `bytes`, fill the legacy `drive` sector view, and (if out != nullptr)
// collect the side-0 flux mirror at `revs` revolutions. `seed` seeds the
// weak-bit RNG (0 ⇒ a fixed default, for reproducible tests). Returns Ok or the
// first failure Status; on failure `drive` is left safe to reject (no partial
// success claimed).
Status ipf_decode(const uint8_t* data, size_t len, t_drive* drive,
                  DecodedIpf* out = nullptr, int revs = 3, uint64_t seed = 0);

// Free the new[]-allocated t_track::data buffers fill_drive() produced. Test
// helper mirroring dsk_eject's delete[] (the real app frees via dsk_eject).
void free_drive_tracks(t_drive* drive);

}  // namespace ipf
