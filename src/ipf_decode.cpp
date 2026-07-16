// Clean-room IPF decoder core — implementation. See ipf_decode.h for scope and
// provenance. Every format fact here traces to docs/hardware/ipf-format.md
// (the authoritative clean-room spec); no SPS Decoder Library or GPL source was
// consulted.

#include "ipf_decode.h"

#include <cstring>

#include "log.h"

namespace ipf {
namespace {

// ---- Big-endian reads (§6: never type-pun) --------------------------------
inline uint32_t rd32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// Read `width` big-endian bytes (1..7) into a 32-bit value. Widths >4 are
// clamped-safe: values that large never occur in real streams, and the caller
// bounds-checks the byte span first.
inline uint32_t rd_be(const uint8_t* p, uint32_t width) {
  uint32_t v = 0;
  for (uint32_t i = 0; i < width; ++i) v = (v << 8) | p[i];
  return v;
}

// ---- Sanity caps ----------------------------------------------------------
constexpr uint32_t kMaxTrackBits = 4000000u;  // §4.2 step 1
constexpr uint32_t kRecordHeader = 12u;
constexpr uint32_t kInfoRecordLen = 96u;
constexpr uint32_t kImgeRecordLen = 80u;
constexpr uint32_t kDataRecordLen = 28u;
constexpr uint32_t kBlockDescLen = 32u;

// ---- Sample bit extraction ------------------------------------------------
// Expand a byte sample to `nbits` bits, MSB-first within each byte. Used for
// both raw (Sync/Raw) verbatim bits and decoded (Data/Gap) source bits.
void sample_bits(const std::vector<uint8_t>& bytes, uint32_t nbits,
                 std::vector<uint8_t>& out) {
  out.clear();
  out.reserve(nbits);
  for (uint32_t i = 0; i < nbits; ++i) {
    uint32_t const byte = i >> 3;
    uint32_t const bit = 7u - (i & 7u);
    uint8_t const v = (byte < bytes.size()) ? ((bytes[byte] >> bit) & 1u) : 0u;
    out.push_back(v);
  }
}

// ---- Weak-bit RNG (§4.3) --------------------------------------------------
// splitmix64 — deterministic when seeded, decent distribution. Exact choice is
// not load-bearing (spec §4.3): only "flakey tracks vary, stable don't".
inline uint64_t splitmix64(uint64_t& state) {
  uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

// ---- MFM bit assembler (§4.1/§4.2) ----------------------------------------
// Grows a raw MFM bitcell stream, MSB-first packed, carrying the "previous
// data bit" across element/block/gap seams for the clock rule
// C[n] = !D[n-1] AND !D[n].
struct MfmBuilder {
  std::vector<uint8_t> bits;
  uint32_t nbits = 0;
  uint8_t last_data = 0;  // D[-1], initialised 0 at track start (§4.1)

  void reserve_bits(uint32_t n) { bits.reserve((n + 7u) / 8u); }

  void push_raw(uint8_t b) {
    uint32_t const byte = nbits >> 3;
    uint32_t const bit = 7u - (nbits & 7u);
    if (byte >= bits.size()) bits.push_back(0);
    if (b) bits[byte] |= static_cast<uint8_t>(1u << bit);
    ++nbits;
  }

  // A verbatim raw sample (Sync/Raw): emit bits unchanged; the carried
  // previous-data bit becomes the sample's final raw bit (§4.1).
  void emit_verbatim(const std::vector<uint8_t>& rawbits) {
    for (uint8_t const b : rawbits) push_raw(b);
    if (!rawbits.empty()) last_data = rawbits.back();
  }

  // MFM-encode one decoded data bit: clock then data (§4.1).
  void emit_decoded_bit(uint8_t d) {
    uint8_t const c = (!last_data && !d) ? 1u : 0u;
    push_raw(c);
    push_raw(d);
    last_data = d;
  }
};

}  // namespace

// ---- CRC-32/ISO-HDLC ------------------------------------------------------
uint32_t crc32(const uint8_t* data, size_t len) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k)
        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    init = true;
  }
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i)
    crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

const char* status_str(Status s) {
  switch (s) {
    case Status::Ok: return "ok";
    case Status::Truncated: return "truncated";
    case Status::BadMagic: return "bad magic (not CAPS)";
    case Status::BadCrc: return "bad record CRC";
    case Status::BadRecord: return "malformed record";
    case Status::UnsupportedEncoder: return "unsupported encoder (CAPS/1)";
    case Status::BadGeometry: return "geometry out of bounds";
    case Status::TooLarge: return "track too large";
    case Status::DecodeError: return "decode inconsistency";
  }
  return "unknown";
}

namespace {

// Verify a record's header CRC: CRC32 over the full `len` bytes of the record
// with the CRC field (offset 8..11) treated as zero (§6).
bool verify_record_crc(const uint8_t* rec, uint32_t len, uint32_t stored) {
  // Compute in two spans so we never mutate the input buffer.
  static uint32_t const table_ok = 1;
  (void)table_ok;
  // Build a small zeroed-field copy only around the CRC field is not enough —
  // CRC is not a simple split, so hash prefix [0,8), then four zero bytes, then
  // the tail [12,len). Use an incremental helper.
  // Simpler + safe: copy is cheap (records are <= a few hundred bytes for the
  // header portion we hash here — DATA hashes only its 28-byte record).
  std::vector<uint8_t> tmp(rec, rec + len);
  tmp[8] = tmp[9] = tmp[10] = tmp[11] = 0;
  return crc32(tmp.data(), len) == stored;
}

}  // namespace

Status Image::open(const uint8_t* data, size_t len) {
  imges_.clear();
  datas_.clear();
  info_ = CleanImageInfo{};
  open_ = false;

  if (data == nullptr || len < kRecordHeader) return Status::Truncated;
  // Magic gate: the first record must be CAPS (§2.3).
  if (std::memcmp(data, "CAPS", 4) != 0) return Status::BadMagic;

  buf_.assign(data, data + len);
  const uint8_t* base = buf_.data();

  bool have_info = false;
  size_t pos = 0;
  while (pos + kRecordHeader <= len) {
    const uint8_t* rec = base + pos;
    uint32_t const rlen = rd32(rec + 4);
    uint32_t const rcrc = rd32(rec + 8);
    if (rlen < kRecordHeader || pos + rlen > len) return Status::Truncated;

    char type[5] = {0};
    std::memcpy(type, rec, 4);

    // DATA records own an Extra Data Block *outside* rlen (§2.1 traversal).
    size_t next = pos + rlen;

    if (std::memcmp(type, "CAPS", 4) == 0) {
      if (!verify_record_crc(rec, rlen, rcrc)) {
        LOG_ERROR("IPF: CAPS record CRC mismatch");
        return Status::BadCrc;
      }
    } else if (std::memcmp(type, "INFO", 4) == 0) {
      if (rlen < kInfoRecordLen) return Status::BadRecord;
      if (!verify_record_crc(rec, rlen, rcrc)) {
        LOG_ERROR("IPF: INFO record CRC mismatch");
        return Status::BadCrc;
      }
      const uint8_t* b = rec + kRecordHeader;
      uint32_t const encoder = rd32(b + 4);
      info_.encoder_type = static_cast<int>(encoder);
      info_.min_cyl = static_cast<int>(rd32(b + 24));
      info_.max_cyl = static_cast<int>(rd32(b + 28));
      info_.min_head = static_cast<int>(rd32(b + 32));
      info_.max_head = static_cast<int>(rd32(b + 36));
      have_info = true;
      if (encoder != 2u) {
        // v1: SPS encoder only (§1.2). CAPS encoder (1) is a Phase C item.
        LOG_ERROR("IPF: encoderType " << encoder
                                       << " unsupported (v1 requires SPS=2)");
        return Status::UnsupportedEncoder;
      }
    } else if (std::memcmp(type, "IMGE", 4) == 0) {
      if (rlen < kImgeRecordLen) return Status::BadRecord;
      if (!verify_record_crc(rec, rlen, rcrc)) {
        LOG_ERROR("IPF: IMGE record CRC mismatch");
        return Status::BadCrc;
      }
      const uint8_t* b = rec + kRecordHeader;
      ImgeRec ir;
      ir.track = rd32(b + 0);
      ir.side = rd32(b + 4);
      ir.density = rd32(b + 8);
      ir.signal_type = rd32(b + 12);
      ir.track_bytes = rd32(b + 16);
      ir.start_byte_pos = rd32(b + 20);
      ir.start_bit_pos = rd32(b + 24);
      ir.data_bits = rd32(b + 28);
      ir.gap_bits = rd32(b + 32);
      ir.track_bits = rd32(b + 36);
      ir.block_count = rd32(b + 40);
      ir.track_flags = rd32(b + 48);
      ir.data_key = rd32(b + 52);
      imges_.push_back(ir);
    } else if (std::memcmp(type, "DATA", 4) == 0) {
      if (rlen < kDataRecordLen) return Status::BadRecord;
      if (!verify_record_crc(rec, rlen, rcrc)) {
        LOG_ERROR("IPF: DATA record CRC mismatch");
        return Status::BadCrc;
      }
      const uint8_t* b = rec + kRecordHeader;
      uint32_t const extra_len = rd32(b + 0);
      uint32_t const extra_crc = rd32(b + 8);
      uint32_t const data_key = rd32(b + 12);
      size_t const extra_off = pos + kDataRecordLen;
      if (extra_off + extra_len > len) return Status::Truncated;
      // Extra Data Block CRC: plain CRC32, no field zeroing (§2.6/§6).
      if (extra_len != 0 &&
          crc32(base + extra_off, extra_len) != extra_crc) {
        LOG_ERROR("IPF: DATA extra-block CRC mismatch (dataKey " << data_key
                                                                  << ")");
        return Status::BadCrc;
      }
      DataRec dr;
      dr.data_key = data_key;
      dr.extra_off = extra_off;
      dr.extra_len = extra_len;
      datas_.push_back(dr);
      next = extra_off + extra_len;  // DATA traversal exception (§2.1)
    } else {
      // Unknown / IPX (CTEI, CTEX, DUMP, TRCK, …): skip via length (§2.1/§2.8).
      // No CRC requirement imposed on records we do not interpret.
    }

    if (next <= pos) return Status::BadRecord;  // no forward progress
    pos = next;
  }

  if (!have_info) return Status::BadRecord;

  // Geometry validation (§5.4b) — reject an out-of-bounds "floppy".
  if (info_.min_cyl > info_.max_cyl) {
    LOG_ERROR("IPF: minTrack " << info_.min_cyl << " > maxTrack "
                                << info_.max_cyl);
    return Status::BadGeometry;
  }
  if (info_.max_cyl + 1 > DSK_TRACKMAX) {
    LOG_ERROR("IPF: maxTrack " << info_.max_cyl << " exceeds DSK_TRACKMAX "
                                << DSK_TRACKMAX);
    return Status::BadGeometry;
  }
  if (info_.min_head != 0) {
    LOG_ERROR("IPF: minSide " << info_.min_head << " != 0");
    return Status::BadGeometry;
  }
  if (info_.max_head + 1 > DSK_SIDEMAX) {
    LOG_ERROR("IPF: maxSide " << info_.max_head << " exceeds DSK_SIDEMAX "
                               << DSK_SIDEMAX);
    return Status::BadGeometry;
  }

  open_ = true;
  return Status::Ok;
}

const Image::ImgeRec* Image::find_imge(int cyl, int head) const {
  for (const ImgeRec& ir : imges_) {
    if (static_cast<int>(ir.track) == cyl && static_cast<int>(ir.side) == head)
      return &ir;
  }
  return nullptr;
}

const Image::DataRec* Image::find_data(uint32_t data_key) const {
  for (const DataRec& dr : datas_) {
    if (dr.data_key == data_key) return &dr;
  }
  return nullptr;
}

bool Image::has_track(int cyl, int head) const {
  return find_imge(cyl, head) != nullptr;
}

bool Image::track_flakey(int cyl, int head) const {
  const ImgeRec* ir = find_imge(cyl, head);
  return ir != nullptr && (ir->track_flags & 1u) != 0u;
}

namespace {

// Parse the block descriptors + Data-Area stream lists of one track. All
// offsets bounds-checked against the extra block. Returns false on any OOB.
bool parse_streams(const uint8_t* extra, uint32_t extra_len, uint32_t encoder,
                   uint32_t block_count, std::vector<BlockDesc>& descs,
                   std::vector<BlockStreams>& streams) {
  descs.clear();
  streams.clear();
  if (static_cast<uint64_t>(block_count) * kBlockDescLen > extra_len)
    return false;

  for (uint32_t i = 0; i < block_count; ++i) {
    const uint8_t* d = extra + (i * kBlockDescLen);
    BlockDesc bd;
    bd.data_bits = rd32(d + 0);
    bd.gap_bits = rd32(d + 4);
    bd.gap_offset = rd32(d + 8);
    bd.cell_type = rd32(d + 12);
    bd.block_encoder = rd32(d + 16);
    bd.block_flags = rd32(d + 20);
    bd.gap_default = rd32(d + 24);
    bd.data_offset = rd32(d + 28);
    descs.push_back(bd);
  }

  for (uint32_t i = 0; i < block_count; ++i) {
    const BlockDesc& bd = descs[i];
    BlockStreams bs;
    bool const data_in_bit = (bd.block_flags & 0x4u) != 0u;

    // Data stream (§3.1): present iff dataBits != 0.
    if (bd.data_bits != 0 && bd.data_offset != 0) {
      uint32_t p = bd.data_offset;
      while (true) {
        if (p >= extra_len) return false;
        uint8_t const head = extra[p++];
        if (head == 0) break;  // null terminator
        uint32_t const width = head >> 5;
        uint32_t const type = head & 0x1Fu;
        if (width == 0 || p + width > extra_len) return false;
        uint32_t const size = rd_be(extra + p, width);
        p += width;
        DataElem e;
        e.type = static_cast<DataType>(type);
        e.size = size;
        if (type == static_cast<uint32_t>(DataType::Fuzzy)) {
          // No sample stored (§3.1, v1.5 correction) — size is the region len.
        } else {
          uint32_t const nbytes = data_in_bit ? ((size + 7u) / 8u) : size;
          if (p + nbytes > extra_len) return false;
          e.sample.assign(extra + p, extra + p + nbytes);
          p += nbytes;
        }
        bs.data.push_back(std::move(e));
      }
    }

    // Gap streams (§3.2): present iff encoder==2, gapBits!=0, blockFlags&3.
    if (encoder == 2u && bd.gap_bits != 0 && (bd.block_flags & 0x3u) != 0u &&
        bd.gap_offset != 0) {
      uint32_t p = bd.gap_offset;
      // Number of gap lists at gapOffset: forward-then-backward (§3.2).
      int lists = 0;
      if (bd.block_flags & 0x1u) ++lists;  // forward
      if (bd.block_flags & 0x2u) ++lists;  // backward
      for (int li = 0; li < lists; ++li) {
        std::vector<GapElem>& dst =
            (li == 0 && (bd.block_flags & 0x1u)) ? bs.fwd : bs.bwd;
        while (true) {
          if (p >= extra_len) return false;
          uint8_t const head = extra[p++];
          if (head == 0) break;
          uint32_t const width = head >> 5;
          uint32_t const kind = head & 0x1Fu;
          if (width == 0 || p + width > extra_len) return false;
          uint32_t const size = rd_be(extra + p, width);
          p += width;
          GapElem g;
          g.kind = static_cast<GapKind>(kind);
          g.size = size;  // bits
          if (kind == static_cast<uint32_t>(GapKind::SampleLength)) {
            uint32_t const nbytes = (size + 7u) / 8u;
            if (p + nbytes > extra_len) return false;
            g.sample.assign(extra + p, extra + p + nbytes);
            p += nbytes;
          }
          dst.push_back(std::move(g));
        }
      }
    }

    streams.push_back(std::move(bs));
  }
  return true;
}

// Produce the decoded (pre-MFM) gap bit sequence for one inter-block gap,
// front->back, of exactly `need` bits (need == gapBits/2). Implements the four
// fill modes of §3.2. Phase alignment at loop seams is (oracle-verify); the
// bit COUNT is always exact.
bool build_gap_decoded(const BlockStreams& s, const BlockDesc& d, uint32_t need,
                       std::vector<uint8_t>& out) {
  out.clear();
  if (need == 0) return true;
  out.reserve(need);
  uint32_t const mode = d.block_flags & 0x3u;

  if (mode == 0) {
    // Default fill: repeat gapDefault byte (MSB-first), fwd+bwd identical.
    for (uint32_t i = 0; i < need; ++i)
      out.push_back(static_cast<uint8_t>((d.gap_default >> (7u - (i & 7u))) & 1u));
    return true;
  }

  // Decode a gap element list into its "specified" decoded bit sequence.
  // `fill_cap` bounds a lone-SampleLength fill; `last` receives the final
  // sample's bits (for loop-fill of the unspecified remainder).
  auto decode_list = [](const std::vector<GapElem>& list, uint32_t fill_cap,
                        std::vector<uint8_t>& seq, std::vector<uint8_t>& last) {
    for (size_t i = 0; i < list.size();) {
      const GapElem& e = list[i];
      if (e.kind == GapKind::GapLength) {
        uint32_t const len_bits = e.size;  // decoded bits to produce
        std::vector<uint8_t> sbits;
        if (i + 1 < list.size() &&
            list[i + 1].kind == GapKind::SampleLength) {
          sample_bits(list[i + 1].sample, list[i + 1].size, sbits);
          i += 2;
        } else {
          i += 1;  // degenerate GapLength w/o sample — nothing to emit
        }
        if (!sbits.empty()) {
          for (uint32_t k = 0; k < len_bits; ++k)
            seq.push_back(sbits[k % sbits.size()]);
          last = sbits;
        }
      } else {  // SampleLength alone → fill toward fill_cap (§3.2)
        std::vector<uint8_t> sbits;
        sample_bits(e.sample, e.size, sbits);
        last = sbits;
        if (!sbits.empty())
          while (seq.size() < fill_cap)
            seq.push_back(sbits[seq.size() % sbits.size()]);
        i += 1;
      }
    }
  };

  if (mode == 1) {  // forward only
    std::vector<uint8_t> seq, last;
    decode_list(s.fwd, need, seq, last);
    if (seq.size() > need) seq.resize(need);
    // Loop the last sample to fill the remainder (§3.2 case 01).
    while (seq.size() < need && !last.empty())
      seq.push_back(last[(seq.size()) % last.size()]);
    while (seq.size() < need) seq.push_back(0);  // no sample at all — zero-fill
    out.swap(seq);
    return true;
  }

  if (mode == 2) {  // backward only — specified bits flush at the BACK
    std::vector<uint8_t> seq, last;
    decode_list(s.bwd, need, seq, last);
    if (seq.size() > need) seq.resize(need);
    uint32_t const frontfill = need - static_cast<uint32_t>(seq.size());
    out.resize(need, 0);
    // Front region: loop the last sample toward the beginning (§3.2 case 10).
    for (uint32_t i = 0; i < frontfill; ++i)
      out[i] = last.empty() ? 0u : last[i % last.size()];
    for (size_t i = 0; i < seq.size(); ++i) out[frontfill + i] = seq[i];
    return true;
  }

  // mode == 3: both. Forward fills from the front, backward from the back;
  // any middle loops the forward list's last sample (§3.2 case 11).
  std::vector<uint8_t> fseq, flast, bseq, blast;
  decode_list(s.fwd, need, fseq, flast);
  decode_list(s.bwd, need, bseq, blast);
  if (fseq.size() > need) fseq.resize(need);
  if (bseq.size() + fseq.size() > need) {
    // Overlap — clamp the backward list to what remains.
    size_t const room = need - fseq.size();
    if (bseq.size() > room) bseq.erase(bseq.begin(), bseq.end() - room);
  }
  uint32_t const mid = need - static_cast<uint32_t>(fseq.size()) -
                       static_cast<uint32_t>(bseq.size());
  out.resize(need, 0);
  for (size_t i = 0; i < fseq.size(); ++i) out[i] = fseq[i];
  for (uint32_t i = 0; i < mid; ++i)
    out[fseq.size() + i] = flast.empty() ? 0u : flast[i % flast.size()];
  for (size_t i = 0; i < bseq.size(); ++i)
    out[fseq.size() + mid + i] = bseq[i];
  return true;
}

}  // namespace

bool Image::parse_block_streams(int cyl, int head, std::vector<BlockDesc>& descs,
                                std::vector<BlockStreams>& streams) const {
  const ImgeRec* ir = find_imge(cyl, head);
  if (ir == nullptr || ir->block_count == 0) return false;
  const DataRec* dr = find_data(ir->data_key);
  if (dr == nullptr || dr->extra_len == 0) return false;
  return parse_streams(buf_.data() + dr->extra_off, dr->extra_len,
                       static_cast<uint32_t>(info_.encoder_type),
                       ir->block_count, descs, streams);
}

bool Image::decode_track(const ImgeRec& imge, const DataRec& data,
                         CleanTrackMFM& out) {
  out.bits.clear();
  out.nbits = 0;
  out.flakey = (imge.track_flags & 1u) != 0u;

  // Unformatted / empty tracks decode to nothing (§4.2).
  if (imge.density == 1u || imge.block_count == 0u || data.extra_len == 0u) {
    return true;
  }

  // Density warning (§4.5): densities 3-9 model uniform cells in v1.
  if (imge.density >= 3u) {
    LOG_WARNING("IPF: density " << imge.density
                                << " decoded with uniform cells — protection "
                                   "timing not modelled");
  }

  uint32_t const track_bits = imge.data_bits + imge.gap_bits;
  if (track_bits == 0) return true;
  if (track_bits > kMaxTrackBits) {
    LOG_ERROR("IPF: trackBits " << track_bits << " exceeds sanity cap");
    return false;
  }
  if (imge.track_bits != 0 && imge.track_bits != track_bits) {
    LOG_ERROR("IPF: IMGE trackBits " << imge.track_bits << " != dataBits+gapBits "
                                      << track_bits);
    return false;
  }

  std::vector<BlockDesc> descs;
  std::vector<BlockStreams> streams;
  if (!parse_streams(buf_.data() + data.extra_off, data.extra_len,
                     static_cast<uint32_t>(info_.encoder_type),
                     imge.block_count, descs, streams)) {
    LOG_ERROR("IPF: stream parse failed / out of bounds (dataKey "
              << data.data_key << ")");
    return false;
  }

  // Accounting identities (§2.7).
  uint64_t sum_data = 0, sum_gap = 0;
  for (const BlockDesc& d : descs) {
    sum_data += d.data_bits;
    sum_gap += d.gap_bits;
  }
  if (sum_data != imge.data_bits || sum_gap != imge.gap_bits) {
    LOG_ERROR("IPF: block accounting mismatch (Σdata " << sum_data << " vs "
              << imge.data_bits << ", Σgap " << sum_gap << " vs "
              << imge.gap_bits << ")");
    return false;
  }

  MfmBuilder mb;
  mb.reserve_bits(track_bits);

  std::vector<uint8_t> tmpbits;  // reused sample-bit scratch
  for (uint32_t bi = 0; bi < imge.block_count; ++bi) {
    const BlockDesc& d = descs[bi];
    const BlockStreams& s = streams[bi];
    bool const data_in_bit = (d.block_flags & 0x4u) != 0u;
    uint32_t const before = mb.nbits;

    for (const DataElem& e : s.data) {
      switch (e.type) {
        case DataType::Sync:
        case DataType::Raw: {
          uint32_t const rawn = data_in_bit ? e.size : e.size * 8u;
          sample_bits(e.sample, rawn, tmpbits);
          mb.emit_verbatim(tmpbits);
          break;
        }
        case DataType::Data:
        case DataType::Gap: {
          uint32_t const decn = data_in_bit ? e.size : e.size * 8u;
          sample_bits(e.sample, decn, tmpbits);
          for (uint8_t const b : tmpbits) mb.emit_decoded_bit(b);
          break;
        }
        case DataType::Fuzzy: {
          uint32_t const decn = data_in_bit ? e.size : e.size * 8u;
          for (uint32_t k = 0; k < decn; ++k) {
            uint64_t const r = splitmix64(rng_state_);
            mb.emit_decoded_bit(static_cast<uint8_t>(r & 1u));
          }
          break;
        }
      }
    }

    uint32_t const rendered = mb.nbits - before;
    if (rendered != d.data_bits) {
      LOG_ERROR("IPF: block " << bi << " rendered " << rendered
                              << " data bits, expected " << d.data_bits);
      return false;
    }

    // Inter-block gap (§3.2). gapBits raw = 2 × decoded need.
    if (d.gap_bits != 0) {
      uint32_t const need = d.gap_bits / 2u;
      std::vector<uint8_t> gapbits;
      if (!build_gap_decoded(s, d, need, gapbits)) return false;
      for (uint8_t const b : gapbits) mb.emit_decoded_bit(b);
      // Odd gapBits would leave one raw bit short; pad with a clocked 0.
      while (mb.nbits - before - rendered < d.gap_bits) mb.push_raw(0);
    }
  }

  if (mb.nbits != track_bits) {
    LOG_ERROR("IPF: assembled " << mb.nbits << " raw bits, expected "
                                << track_bits);
    return false;
  }

  // Rotate to index alignment (§4.2 step 4): out[(i+startBitPos) % N] = built[i]
  out.bits.assign((track_bits + 7u) / 8u, 0);
  out.nbits = track_bits;
  uint32_t const start = imge.start_bit_pos % track_bits;
  for (uint32_t i = 0; i < track_bits; ++i) {
    uint32_t const src_byte = i >> 3;
    uint32_t const src_bit = 7u - (i & 7u);
    uint8_t const v = (mb.bits[src_byte] >> src_bit) & 1u;
    if (!v) continue;
    uint32_t j = i + start;
    if (j >= track_bits) j -= track_bits;
    out.bits[j >> 3] |= static_cast<uint8_t>(1u << (7u - (j & 7u)));
  }
  return true;
}

bool Image::lock_track(int cyl, int head, CleanTrackMFM& out) {
  out.bits.clear();
  out.nbits = 0;
  out.flakey = false;
  if (!open_) return false;
  const ImgeRec* ir = find_imge(cyl, head);
  if (ir == nullptr) return true;  // absent ⇒ empty (unformatted)
  const DataRec* dr = find_data(ir->data_key);
  if (dr == nullptr) return true;  // no data ⇒ empty
  return decode_track(*ir, *dr, out);
}

// ---------------------------------------------------------------------------
// Sector-view scanner (§5.3) — clean-room reimplementation of the IBM
// System-34 MFM extraction the codebase already uses (ours). Operates on a
// decoded MFM bitcell stream; identical contract to ReadTrack.
// ---------------------------------------------------------------------------
namespace {

// CRC-16/CCITT (poly 0x1021), used for ID/data field checksums.
uint16_t crc16_ccitt_step(uint16_t crc, uint8_t b) {
  static uint16_t table[256];
  static bool init = false;
  if (!init) {
    for (int i = 0; i < 256; ++i) {
      uint16_t w = static_cast<uint16_t>(i << 8);
      for (int j = 0; j < 8; ++j)
        w = static_cast<uint16_t>((w << 1) ^ ((w & 0x8000u) ? 0x1021u : 0u));
      table[i] = w;
    }
    init = true;
  }
  return static_cast<uint16_t>((crc << 8) ^
                               table[((crc >> 8) ^ b) & 0xFFu]);
}

struct Scanner {
  const uint8_t* buf;  // padded copy (+2 spare bytes for the wrap read)
  uint32_t nbits;
  uint32_t pos = 0, last_pos = 0;
  bool wrapped = false;
  uint8_t last_data = 0;
  uint16_t crc = 0;
  std::vector<uint8_t> decoded;

  uint8_t read_byte() {
    uint32_t const off = pos >> 3;
    uint32_t const shift = pos & 7u;
    pos += 8;
    uint8_t b = shift == 0
                    ? buf[off]
                    : static_cast<uint8_t>((buf[off] << shift) |
                                           (buf[off + 1] >> (8u - shift)));
    if (pos >= nbits) {
      uint32_t const wrapbits = pos - nbits;
      if (wrapbits < 8) {
        b = static_cast<uint8_t>(b & ~((1u << wrapbits) - 1u));
        b |= static_cast<uint8_t>(buf[0] >> (8u - wrapbits));
      } else {
        b = buf[0];
      }
      pos -= nbits;
      wrapped = true;
    }
    return b;
  }

  uint16_t read_word() {
    last_pos = pos;
    uint8_t const b1 = read_byte();
    uint8_t const b2 = read_byte();
    uint8_t clock =
        static_cast<uint8_t>(((b1 << 0) & 0x80) | ((b1 << 1) & 0x40) |
                             ((b1 << 2) & 0x20) | ((b1 << 3) & 0x10) |
                             ((b2 >> 4) & 0x08) | ((b2 >> 3) & 0x04) |
                             ((b2 >> 2) & 0x02) | ((b2 >> 1) & 0x01));
    uint8_t const dat = static_cast<uint8_t>(
        ((b1 << 1) & 0x80) | ((b1 << 2) & 0x40) | ((b1 << 3) & 0x20) |
        ((b1 << 4) & 0x10) | ((b2 >> 3) & 0x08) | ((b2 >> 2) & 0x04) |
        ((b2 >> 1) & 0x02) | ((b2 >> 0) & 0x01));
    uint8_t good = 0;
    if (!(dat & 0x80) && !(last_data & 1)) good |= 0x80;
    if (!(dat & 0xc0)) good |= 0x40;
    if (!(dat & 0x60)) good |= 0x20;
    if (!(dat & 0x30)) good |= 0x10;
    if (!(dat & 0x18)) good |= 0x08;
    if (!(dat & 0x0c)) good |= 0x04;
    if (!(dat & 0x06)) good |= 0x02;
    if (!(dat & 0x03)) good |= 0x01;
    clock ^= good;
    decoded.push_back(dat);
    last_data = dat;
    return static_cast<uint16_t>((clock << 8) | dat);
  }

  uint8_t read_data_byte() { return static_cast<uint8_t>(read_word() & 0xFF); }
  void crc_byte(uint8_t b) { crc = crc16_ccitt_step(crc, b); }
};

struct SectorTmp {
  uint8_t chrn[4] = {0, 0, 0, 0};
  uint8_t flags[4] = {0, 0, 0, 0};
  uint32_t size = 0;
  size_t data_off = 0;  // index into Scanner::decoded
  bool has_data = false;
};

// Scan `mfm` into per-sector records + a decoded byte buffer. Mirrors
// ReadTrack (src/ipf.cpp, ours) exactly.
void scan_track(const CleanTrackMFM& mfm, std::vector<SectorTmp>& sectors,
                std::vector<uint8_t>& decoded_out) {
  sectors.clear();
  decoded_out.clear();
  if (mfm.nbits == 0) return;

  // Padded copy so read_byte()'s buf[off+1] never reads past the end.
  std::vector<uint8_t> padded = mfm.bits;
  padded.resize(padded.size() + 2, 0);

  Scanner sc;
  sc.buf = padded.data();
  sc.nbits = mfm.nbits;
  sc.decoded.reserve((mfm.nbits / 16u) + 8192u);

  SectorTmp* cur = nullptr;
  size_t cur_index = 0;
  uint32_t header_off = 0;

  while (!sc.wrapped || cur != nullptr) {
    if (sc.read_word() != 0x04a1) {
      sc.pos -= 15;
      sc.decoded.pop_back();
      continue;
    }
    if (sc.read_word() != 0x04a1) continue;
    if (sc.read_word() != 0x04a1) continue;

    sc.crc = 0xcdb4;
    uint8_t const am = sc.read_data_byte();
    sc.crc_byte(am);

    if (am == 0xfe) {  // ID address mark
      if (sectors.size() >= DSK_SECTORMAX) continue;
      SectorTmp st;
      st.chrn[0] = sc.read_data_byte(); sc.crc_byte(st.chrn[0]);
      st.chrn[1] = sc.read_data_byte(); sc.crc_byte(st.chrn[1]);
      st.chrn[2] = sc.read_data_byte(); sc.crc_byte(st.chrn[2]);
      st.chrn[3] = sc.read_data_byte(); sc.crc_byte(st.chrn[3]);
      sc.crc_byte(sc.read_data_byte());
      sc.crc_byte(sc.read_data_byte());
      if (sc.crc != 0) continue;  // bad header CRC ⇒ drop
      header_off = sc.last_pos;
      sectors.push_back(st);
      cur_index = sectors.size() - 1;
      cur = &sectors[cur_index];
      continue;
    }

    if (am == 0xfb || am == 0xfa || am == 0xf8 || am == 0xf9) {  // DAM
      uint32_t const data_pos = sc.pos;
      bool const data_wrapped = sc.wrapped;
      if (cur == nullptr) continue;

      uint32_t const offset = (sc.last_pos - header_off) >> 4;
      if (offset < 32 || offset >= 64) {
        cur->flags[1] &= ~0x01u;  // no data
        cur = nullptr;
        continue;
      }
      if (am == 0xf8 || am == 0xf9) cur->flags[1] |= 0x40u;  // control mark

      cur->data_off = sc.decoded.size();
      cur->has_data = true;
      uint32_t const sector_size =
          (cur->chrn[3] <= 7) ? (128u << cur->chrn[3]) : 0x8000u;
      cur->size = sector_size;

      for (uint32_t u = 0; u < sector_size; ++u) sc.crc_byte(sc.read_data_byte());
      sc.crc_byte(sc.read_data_byte());
      sc.crc_byte(sc.read_data_byte());
      if (sc.crc != 0) {
        cur->flags[0] |= 0x20u;
        cur->flags[1] |= 0x20u;
      }

      // Read-track protection: single-sector overread to 4K.
      if (sectors.size() == 1 && sector_size < 4096u) {
        for (uint32_t u = 0; u < 4096u - sector_size; ++u)
          sc.crc_byte(sc.read_data_byte());
      }

      cur = nullptr;
      sc.pos = data_pos;
      sc.wrapped = data_wrapped;
      // Re-acquire pointer (vector may have reallocated during push_back).
      continue;
    }
    // Any other address mark: ignore, keep scanning.
  }

  decoded_out.swap(sc.decoded);
}

}  // namespace

Status Image::fill_drive(t_drive* drive) {
  if (drive == nullptr) return Status::BadRecord;
  if (!open_) return Status::BadRecord;

  // §5.4a: zero-based `sides` (== maxSide when minSide == 0, validated in open).
  drive->tracks = static_cast<unsigned int>(info_.max_cyl + 1);
  drive->sides = static_cast<unsigned int>(info_.max_head - info_.min_head);
  drive->altered = false;

  for (int cyl = info_.min_cyl; cyl <= info_.max_cyl; ++cyl) {
    for (int head = info_.min_head; head <= info_.max_head; ++head) {
      t_track* pt = &drive->track[cyl][head];
      std::memset(pt, 0, sizeof(*pt));

      CleanTrackMFM mfm;
      if (!lock_track(cyl, head, mfm)) {
        LOG_ERROR("IPF: track " << cyl << "." << head << " failed to decode");
        return Status::DecodeError;  // whole-load abort (§5.4)
      }
      if (mfm.nbits == 0) continue;  // unformatted ⇒ zeroed t_track

      std::vector<SectorTmp> sectors;
      std::vector<uint8_t> decoded;
      scan_track(mfm, sectors, decoded);
      if (sectors.empty()) continue;

      pt->sectors = static_cast<unsigned int>(sectors.size());
      pt->size = static_cast<unsigned int>(decoded.size());
      pt->data = new byte[decoded.size()];
      std::memcpy(pt->data, decoded.data(), decoded.size());
      for (size_t i = 0; i < sectors.size(); ++i) {
        const SectorTmp& st = sectors[i];
        t_sector& ps = pt->sector[i];
        for (int k = 0; k < 4; ++k) ps.CHRN[k] = st.chrn[k];
        for (int k = 0; k < 4; ++k) ps.flags[k] = st.flags[k];
        ps.setSizes(st.size, st.size);
        ps.setData(pt->data + (st.has_data ? st.data_off : 0));
      }
    }
  }
  return Status::Ok;
}

std::vector<t_mfm_track> Image::mirror_side0(int revs) {
  std::vector<t_mfm_track> cyls;
  if (!open_ || revs <= 0) return cyls;
  int const max_cyl = info_.max_cyl;
  for (int cyl = 0; cyl <= max_cyl; ++cyl) {
    t_mfm_track track;
    CleanTrackMFM first;
    if (!lock_track(cyl, 0, first)) return {};
    if (first.nbits == 0) {
      cyls.push_back(track);  // empty ⇒ absent slot
      continue;
    }
    t_mfm_rev r0;
    r0.bits = first.bits;
    r0.nbits = first.nbits;
    track.push_back(std::move(r0));
    for (int rv = 1; rv < revs; ++rv) {
      if (first.flakey) {
        CleanTrackMFM pass;
        if (!lock_track(cyl, 0, pass) || pass.nbits == 0) {
          track.push_back(track.back());  // pad with last
          continue;
        }
        t_mfm_rev rr;
        rr.bits = std::move(pass.bits);
        rr.nbits = pass.nbits;
        track.push_back(std::move(rr));
      } else {
        track.push_back(track.front());  // stable ⇒ reuse
      }
    }
    cyls.push_back(std::move(track));
  }
  return cyls;
}

Status ipf_decode(const uint8_t* data, size_t len, t_drive* drive,
                  DecodedIpf* out, int revs, uint64_t seed) {
  Image img;
  if (seed != 0) img.seed_rng(seed);
  Status st = img.open(data, len);
  if (st != Status::Ok) return st;
  if (drive != nullptr) {
    st = img.fill_drive(drive);
    if (st != Status::Ok) return st;
  }
  if (out != nullptr) {
    out->info = img.info();
    out->side0 = img.mirror_side0(revs);
  }
  return Status::Ok;
}

void free_drive_tracks(t_drive* drive) {
  if (drive == nullptr) return;
  for (auto& c : drive->track) {
    for (auto& h : c) {
      t_track* pt = &h;
      if (pt->data != nullptr) {
        delete[] pt->data;
        pt->data = nullptr;
      }
    }
  }
}

}  // namespace ipf
