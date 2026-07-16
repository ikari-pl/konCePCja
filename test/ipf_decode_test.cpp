// Clean-room IPF decoder unit tests.
//
// The primary correctness proofs (§7 of docs/hardware/ipf-format.md) are the
// license-free worked-example regression vectors — record CRCs, IMGE/DATA
// field values, and the block-descriptor accounting identities from the Theme
// Park Mystery track 00.0 walkthrough — plus the §6 CRC self-check, hostile
// input rejection, and a hand-built minimal-IPF round-trip through MFM. There
// is no reference decoder to oracle against, so these hand-typed byte-level
// vectors are the ground truth.

#include "ipf_decode.h"

#include "flux_ingest.h"  // flux::to_scp — the unified dispatcher read path

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "hw/flux.h"

namespace {

using ipf::Status;

// ---- little byte-builder helpers ------------------------------------------
void put_be32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(static_cast<uint8_t>(x >> 24));
  v.push_back(static_cast<uint8_t>(x >> 16));
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x));
}

void put_bytes(std::vector<uint8_t>& v, const std::vector<uint8_t>& b) {
  v.insert(v.end(), b.begin(), b.end());
}

// CRC-16/CCITT (poly 0x1021), the field checksum a real IPF's Data elements
// carry (we compute it to build valid ID/data fields for the round-trip).
uint16_t crc16(uint16_t crc, uint8_t b) {
  crc ^= static_cast<uint16_t>(b) << 8;
  for (int i = 0; i < 8; ++i)
    crc = static_cast<uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0x1021
                                               : (crc << 1));
  return crc;
}

// A Data stream element: head (width<<5|type), size (BE, width bytes), sample.
std::vector<uint8_t> data_elem(ipf::DataType type,
                               const std::vector<uint8_t>& sample) {
  uint32_t size = static_cast<uint32_t>(sample.size());  // bytes (DataInBit=0)
  uint32_t width = (size > 0xFFFF) ? 3 : (size > 0xFF) ? 2 : 1;
  std::vector<uint8_t> e;
  e.push_back(static_cast<uint8_t>((width << 5) |
                                   static_cast<uint32_t>(type)));
  for (uint32_t i = 0; i < width; ++i)
    e.push_back(static_cast<uint8_t>(size >> (8 * (width - 1 - i))));
  put_bytes(e, sample);
  return e;
}

// A Fuzzy element carries no sample; size is the region length in bytes.
std::vector<uint8_t> fuzzy_elem(uint32_t size_bytes) {
  uint32_t width = (size_bytes > 0xFFFF) ? 3 : (size_bytes > 0xFF) ? 2 : 1;
  std::vector<uint8_t> e;
  e.push_back(static_cast<uint8_t>(
      (width << 5) | static_cast<uint32_t>(ipf::DataType::Fuzzy)));
  for (uint32_t i = 0; i < width; ++i)
    e.push_back(static_cast<uint8_t>(size_bytes >> (8 * (width - 1 - i))));
  return e;
}

// Gap elements (size always in bits).
std::vector<uint8_t> gap_length(uint32_t len_bits) {
  std::vector<uint8_t> e;
  e.push_back(static_cast<uint8_t>((1u << 5) |
                                   static_cast<uint32_t>(ipf::GapKind::GapLength)));
  e.push_back(static_cast<uint8_t>(len_bits >> 8));
  e.push_back(static_cast<uint8_t>(len_bits));
  // width 2 for values > 255; keep it 2 always for simplicity of the vector
  e[0] = static_cast<uint8_t>((2u << 5) |
                              static_cast<uint32_t>(ipf::GapKind::GapLength));
  return e;
}
std::vector<uint8_t> gap_sample8(uint8_t val) {
  std::vector<uint8_t> e;
  e.push_back(static_cast<uint8_t>(
      (1u << 5) | static_cast<uint32_t>(ipf::GapKind::SampleLength)));
  e.push_back(8);  // sample bit-length
  e.push_back(val);
  return e;
}

std::vector<uint8_t> sync_sample() {  // 3x A1 (0x4489)
  return {0x44, 0x89, 0x44, 0x89, 0x44, 0x89};
}

// Build a valid ID Data element for C/H/R/N with a correct CRC16.
std::vector<uint8_t> id_field(uint8_t c, uint8_t h, uint8_t r, uint8_t n) {
  std::vector<uint8_t> body = {0xFE, c, h, r, n};
  uint16_t crc = 0xFFFF;
  for (uint8_t x : {0xA1, 0xA1, 0xA1}) crc = crc16(crc, static_cast<uint8_t>(x));
  for (uint8_t x : body) crc = crc16(crc, x);
  body.push_back(static_cast<uint8_t>(crc >> 8));
  body.push_back(static_cast<uint8_t>(crc));
  return data_elem(ipf::DataType::Data, body);
}

// Build a valid data field Data element (DAM + payload + CRC16).
std::vector<uint8_t> data_field(const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> body = {0xFB};
  put_bytes(body, payload);
  uint16_t crc = 0xFFFF;
  for (uint8_t x : {0xA1, 0xA1, 0xA1}) crc = crc16(crc, static_cast<uint8_t>(x));
  for (uint8_t x : body) crc = crc16(crc, x);
  body.push_back(static_cast<uint8_t>(crc >> 8));
  body.push_back(static_cast<uint8_t>(crc));
  return data_elem(ipf::DataType::Data, body);
}

// ---- container-record builders --------------------------------------------
// A generic record: type + length + crc(zeroed while hashing) + payload.
std::vector<uint8_t> record(const char* type,
                            const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> r;
  r.insert(r.end(), type, type + 4);
  put_be32(r, static_cast<uint32_t>(12 + payload.size()));
  put_be32(r, 0);  // crc placeholder
  put_bytes(r, payload);
  uint32_t crc = ipf::crc32(r.data(), r.size());
  r[8] = static_cast<uint8_t>(crc >> 24);
  r[9] = static_cast<uint8_t>(crc >> 16);
  r[10] = static_cast<uint8_t>(crc >> 8);
  r[11] = static_cast<uint8_t>(crc);
  return r;
}

std::vector<uint8_t> caps_record() { return record("CAPS", {}); }

std::vector<uint8_t> info_record(uint32_t encoder, uint32_t max_track,
                                 uint32_t max_side) {
  std::vector<uint8_t> p;
  put_be32(p, 1);         // mediaType
  put_be32(p, encoder);   // encoderType
  put_be32(p, 1);         // encoderRev
  put_be32(p, 1);         // fileKey
  put_be32(p, 1);         // fileRev
  put_be32(p, 0);         // origin
  put_be32(p, 0);         // minTrack
  put_be32(p, max_track); // maxTrack
  put_be32(p, 0);         // minSide
  put_be32(p, max_side);  // maxSide
  put_be32(p, 0);         // creationDate
  put_be32(p, 0);         // creationTime
  put_be32(p, 4);         // platforms[0] = Amstrad CPC
  put_be32(p, 0);
  put_be32(p, 0);
  put_be32(p, 0);
  put_be32(p, 0);  // diskNumber
  put_be32(p, 0);  // creatorId
  put_be32(p, 0);  // reserved[3]
  put_be32(p, 0);
  put_be32(p, 0);
  return record("INFO", p);
}

std::vector<uint8_t> imge_record(uint32_t track, uint32_t side,
                                 uint32_t density, uint32_t data_bits,
                                 uint32_t gap_bits, uint32_t block_count,
                                 uint32_t track_flags, uint32_t start_bit_pos,
                                 uint32_t data_key) {
  uint32_t track_bits = data_bits + gap_bits;
  std::vector<uint8_t> p;
  put_be32(p, track);
  put_be32(p, side);
  put_be32(p, density);
  put_be32(p, 1);  // signalType
  put_be32(p, (track_bits + 7) / 8);  // trackBytes
  put_be32(p, start_bit_pos / 8);     // startBytePos
  put_be32(p, start_bit_pos);         // startBitPos
  put_be32(p, data_bits);
  put_be32(p, gap_bits);
  put_be32(p, track_bits);
  put_be32(p, block_count);
  put_be32(p, 0);            // encoderProcess
  put_be32(p, track_flags);  // trackFlags
  put_be32(p, data_key);
  put_be32(p, 0);  // reserved[3]
  put_be32(p, 0);
  put_be32(p, 0);
  return record("IMGE", p);
}

// A DATA record = 28-byte record + Extra Data Block (outside length).
std::vector<uint8_t> data_record(uint32_t data_key,
                                 const std::vector<uint8_t>& extra) {
  std::vector<uint8_t> block;
  put_be32(block, static_cast<uint32_t>(extra.size()));      // length
  put_be32(block, static_cast<uint32_t>(extra.size() * 8));  // bitSize
  put_be32(block, ipf::crc32(extra.data(), extra.size()));   // extra crc
  put_be32(block, data_key);
  std::vector<uint8_t> r = {'D', 'A', 'T', 'A'};
  put_be32(r, 28);  // record length excludes extra block
  put_be32(r, 0);   // crc placeholder
  put_bytes(r, block);
  uint32_t crc = ipf::crc32(r.data(), r.size());
  r[8] = static_cast<uint8_t>(crc >> 24);
  r[9] = static_cast<uint8_t>(crc >> 16);
  r[10] = static_cast<uint8_t>(crc >> 8);
  r[11] = static_cast<uint8_t>(crc);
  put_bytes(r, extra);
  return r;
}

// One 32-byte block descriptor.
std::vector<uint8_t> block_desc(uint32_t data_bits, uint32_t gap_bits,
                                uint32_t gap_offset, uint32_t block_flags,
                                uint32_t gap_default, uint32_t data_offset) {
  std::vector<uint8_t> d;
  put_be32(d, data_bits);
  put_be32(d, gap_bits);
  put_be32(d, gap_offset);
  put_be32(d, 1);  // cellType 2us
  put_be32(d, 1);  // block encoderType MFM
  put_be32(d, block_flags);
  put_be32(d, gap_default);
  put_be32(d, data_offset);
  return d;
}

// Assemble a whole single-block IPF around a data area + descriptor fields.
struct SingleBlockIpf {
  std::vector<uint8_t> bytes;
  uint32_t data_bits = 0;
  uint32_t gap_bits = 0;
};

SingleBlockIpf build_single_block(const std::vector<uint8_t>& data_area,
                                  const std::vector<uint8_t>& gap_stream,
                                  uint32_t data_bits, uint32_t gap_bits,
                                  uint32_t block_flags, uint32_t gap_default,
                                  uint32_t density = 2,
                                  uint32_t track_flags = 0) {
  // Extra Data Block = one 32-byte descriptor + [gap stream] + [data area].
  // Gap stream (if any) comes first at gapOffset=32; data area at dataOffset.
  uint32_t gap_offset = gap_stream.empty() ? 0u : 32u;
  uint32_t data_offset = 32u + static_cast<uint32_t>(gap_stream.size());
  std::vector<uint8_t> extra;
  put_bytes(extra, block_desc(data_bits, gap_bits, gap_offset, block_flags,
                              gap_default, data_offset));
  put_bytes(extra, gap_stream);
  put_bytes(extra, data_area);

  SingleBlockIpf out;
  out.data_bits = data_bits;
  out.gap_bits = gap_bits;
  put_bytes(out.bytes, caps_record());
  put_bytes(out.bytes, info_record(2, 0, 0));
  put_bytes(out.bytes, imge_record(0, 0, density, data_bits, gap_bits, 1,
                                   track_flags, 0, 1));
  put_bytes(out.bytes, data_record(1, extra));
  return out;
}

// The §7.4 block-0 data area (standard 512-byte Atari sector layout): the
// canonical accounting vector — 48 + 2·56 + 2·272 + 48 + 2·4120 = 8992.
std::vector<uint8_t> block0_data_area() {
  std::vector<uint8_t> gap(34);  // 22×4E + 12×00
  for (int i = 0; i < 22; ++i) gap[i] = 0x4E;
  std::vector<uint8_t> payload(512);
  for (int i = 0; i < 512; ++i) payload[i] = static_cast<uint8_t>(i);
  std::vector<uint8_t> a;
  put_bytes(a, data_elem(ipf::DataType::Sync, sync_sample()));
  put_bytes(a, id_field(0, 0, 1, 2));
  put_bytes(a, data_elem(ipf::DataType::Gap, gap));
  put_bytes(a, data_elem(ipf::DataType::Sync, sync_sample()));
  put_bytes(a, data_field(payload));
  a.push_back(0x00);  // list terminator
  return a;
}

}  // namespace

// ---------------------------------------------------------------------------
// §6 — CRC-32 self-check vector.
// ---------------------------------------------------------------------------
TEST(IpfDecodeCrc, KnownCapsRecordVector) {
  // "CAPS" ‖ 00 00 00 0C ‖ 00 00 00 00 → 0x1CD573BA (spec §6, verified against
  // zlib independently of any decoder source).
  const uint8_t v[12] = {'C', 'A', 'P', 'S', 0, 0, 0, 0x0C, 0, 0, 0, 0};
  EXPECT_EQ(ipf::crc32(v, sizeof(v)), 0x1CD573BAu);
}

TEST(IpfDecodeCrc, EmptyAndKnownAscii) {
  EXPECT_EQ(ipf::crc32(nullptr, 0), 0u);
  const uint8_t abc[3] = {'1', '2', '3'};
  // CRC32("123") = 0x884863D2 (standard zlib value).
  EXPECT_EQ(ipf::crc32(abc, 3), 0x884863D2u);
}

// ---------------------------------------------------------------------------
// §7 — worked-example parser / accounting vectors.
// ---------------------------------------------------------------------------
TEST(IpfDecode, Block0ParsesElementsAndAccounts) {
  auto ipf = build_single_block(
      block0_data_area(),
      // §7.4 gap stream: fwd GapLength 192 · 4E, bwd GapLength 64 · 00.
      [] {
        std::vector<uint8_t> g;
        put_bytes(g, gap_length(192));
        put_bytes(g, gap_sample8(0x4E));
        g.push_back(0x00);  // end forward
        put_bytes(g, gap_length(64));
        put_bytes(g, gap_sample8(0x00));
        g.push_back(0x00);  // end backward
        return g;
      }(),
      /*data_bits=*/8992, /*gap_bits=*/512, /*block_flags=*/3,
      /*gap_default=*/0);

  ipf::Image img;
  ASSERT_EQ(img.open(ipf.bytes), Status::Ok);

  std::vector<ipf::BlockDesc> descs;
  std::vector<ipf::BlockStreams> streams;
  ASSERT_TRUE(img.parse_block_streams(0, 0, descs, streams));
  ASSERT_EQ(descs.size(), 1u);
  ASSERT_EQ(streams.size(), 1u);

  const auto& d = streams[0].data;
  ASSERT_EQ(d.size(), 5u);
  // §7.4: Sync 6, Data 7, Gap 34, Sync 6, Data 515.
  EXPECT_EQ(d[0].type, ipf::DataType::Sync);
  EXPECT_EQ(d[0].size, 6u);
  EXPECT_EQ(d[1].type, ipf::DataType::Data);
  EXPECT_EQ(d[1].size, 7u);
  EXPECT_EQ(d[2].type, ipf::DataType::Gap);
  EXPECT_EQ(d[2].size, 34u);
  EXPECT_EQ(d[3].type, ipf::DataType::Sync);
  EXPECT_EQ(d[3].size, 6u);
  EXPECT_EQ(d[4].type, ipf::DataType::Data);
  EXPECT_EQ(d[4].size, 515u);  // needs dataSizeWidth = 2

  // §7.4 gap lists: forward 192·4E, backward 64·00.
  ASSERT_EQ(streams[0].fwd.size(), 2u);
  EXPECT_EQ(streams[0].fwd[0].kind, ipf::GapKind::GapLength);
  EXPECT_EQ(streams[0].fwd[0].size, 192u);
  EXPECT_EQ(streams[0].fwd[1].kind, ipf::GapKind::SampleLength);
  ASSERT_EQ(streams[0].fwd[1].sample.size(), 1u);
  EXPECT_EQ(streams[0].fwd[1].sample[0], 0x4Eu);
  ASSERT_EQ(streams[0].bwd.size(), 2u);
  EXPECT_EQ(streams[0].bwd[0].size, 64u);
  EXPECT_EQ(streams[0].bwd[1].sample[0], 0x00u);

  // The load-bearing accounting: a successful decode reconstructs exactly
  // dataBits + gapBits = 8992 + 512 raw bits (decode asserts the identity).
  ipf::CleanTrackMFM mfm;
  ASSERT_TRUE(img.lock_track(0, 0, mfm));
  EXPECT_EQ(mfm.nbits, 8992u + 512u);
}

TEST(IpfDecode, Block10GaplessAccounts) {
  // §7.5: dataBits 800, gapBits 0; 48 + 2·56 + 2·224 + 48 + 2·72 = 800.
  std::vector<uint8_t> gap(28);  // 22×4E + 6×00
  for (int i = 0; i < 22; ++i) gap[i] = 0x4E;
  std::vector<uint8_t> a;
  put_bytes(a, data_elem(ipf::DataType::Sync, sync_sample()));
  put_bytes(a, data_elem(ipf::DataType::Data,
                         {0xFE, 0x00, 0x00, 0x0B, 0x02, 0x25, 0xA4}));  // 7 B
  put_bytes(a, data_elem(ipf::DataType::Gap, gap));                    // 28 B
  put_bytes(a, data_elem(ipf::DataType::Sync, sync_sample()));
  put_bytes(a, data_elem(ipf::DataType::Data,
                         {0xFB, 0, 0, 0, 0, 0, 0, 0, 0}));  // 9 B
  a.push_back(0x00);

  auto ipf = build_single_block(a, {}, 800, 0, /*flags=*/0, 0);
  ipf::Image img;
  ASSERT_EQ(img.open(ipf.bytes), Status::Ok);
  ipf::CleanTrackMFM mfm;
  ASSERT_TRUE(img.lock_track(0, 0, mfm));
  EXPECT_EQ(mfm.nbits, 800u);
}

TEST(IpfDecode, Block11BackwardLoopingGapAccounts) {
  // §7.6: dataBits 2336, gapBits 2280, blockFlags 2 (backward only). The gap
  // specifies only 2·(192+64)=512 of 2280 raw bits — the rest loops to fill.
  // Data accounting: 48 + 2·56 + 2·64 + 2·64 + 2·64 + 2·48 + 1696 = 2336.
  std::vector<uint8_t> a;
  put_bytes(a, data_elem(ipf::DataType::Sync, sync_sample()));           // 48
  put_bytes(a, data_elem(ipf::DataType::Data,
                         {0xFE, 0x00, 0x00, 0x0C, 0x02, 0xBC, 0x33}));   // 7 B
  put_bytes(a, data_elem(ipf::DataType::Gap, std::vector<uint8_t>(8, 0x4E)));
  put_bytes(a, data_elem(ipf::DataType::Data,
                         {0xD9, 0x23, 0x76, 0xC5, 0xE6, 0xD3, 0x31, 0xB2}));
  put_bytes(a, data_elem(ipf::DataType::Gap, std::vector<uint8_t>(8, 0x4E)));
  put_bytes(a, data_elem(ipf::DataType::Data,
                         {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE}));          // 6 B
  put_bytes(a, data_elem(ipf::DataType::Sync,
                         std::vector<uint8_t>(212, 0x00)));  // 212 B verbatim
  a.push_back(0x00);

  std::vector<uint8_t> gap;  // backward only
  put_bytes(gap, gap_length(192));
  put_bytes(gap, gap_sample8(0x4E));
  gap.push_back(0x00);
  put_bytes(gap, gap_length(64));
  put_bytes(gap, gap_sample8(0x00));
  gap.push_back(0x00);

  auto ipf = build_single_block(a, gap, 2336, 2280, /*flags=*/2, 0);
  ipf::Image img;
  ASSERT_EQ(img.open(ipf.bytes), Status::Ok);
  ipf::CleanTrackMFM mfm;
  ASSERT_TRUE(img.lock_track(0, 0, mfm));
  EXPECT_EQ(mfm.nbits, 2336u + 2280u);
}

// ---------------------------------------------------------------------------
// Minimal-IPF round-trip (§7.7): parse → MFM → sector view → flux mirror.
// ---------------------------------------------------------------------------
TEST(IpfDecode, MinimalIpfRoundTripsToSector) {
  std::vector<uint8_t> payload(512);
  for (int i = 0; i < 512; ++i) payload[i] = static_cast<uint8_t>(i * 3 + 7);

  std::vector<uint8_t> gap(34);
  for (int i = 0; i < 22; ++i) gap[i] = 0x4E;
  std::vector<uint8_t> a;
  put_bytes(a, data_elem(ipf::DataType::Sync, sync_sample()));
  put_bytes(a, id_field(0, 0, 1, 2));  // R=1, N=2 (512 bytes)
  put_bytes(a, data_elem(ipf::DataType::Gap, gap));
  put_bytes(a, data_elem(ipf::DataType::Sync, sync_sample()));
  put_bytes(a, data_field(payload));
  a.push_back(0x00);

  // Default-fill inter-block gap of 0x4E (§7.7 gapDefault), gapless-list.
  auto ipf = build_single_block(a, {}, 8992, 1024, /*flags=*/0,
                                /*gap_default=*/0x4E);

  t_drive drive;
  std::memset(&drive, 0, sizeof(drive));
  ipf::Image img;
  ASSERT_EQ(img.open(ipf.bytes), Status::Ok);
  EXPECT_EQ(img.info().max_cyl, 0);
  EXPECT_EQ(img.info().max_head, 0);
  EXPECT_EQ(img.info().encoder_type, 2);

  ASSERT_EQ(img.fill_drive(&drive), Status::Ok);
  EXPECT_EQ(drive.tracks, 1u);
  EXPECT_EQ(drive.sides, 0u);  // §5.4a zero-based sides

  t_track& t = drive.track[0][0];
  ASSERT_GE(t.sectors, 1u);
  // Find sector R=1.
  int found = -1;
  for (unsigned int s = 0; s < t.sectors; ++s)
    if (t.sector[s].CHRN[2] == 1) found = static_cast<int>(s);
  ASSERT_GE(found, 0);
  t_sector& sec = t.sector[found];
  EXPECT_EQ(sec.CHRN[0], 0u);
  EXPECT_EQ(sec.CHRN[1], 0u);
  EXPECT_EQ(sec.CHRN[3], 2u);
  EXPECT_EQ(sec.getTotalSize(), 512u);
  EXPECT_EQ((sec.flags[0] & 0x20u), 0u);  // no data-CRC error
  unsigned char* d = sec.getDataForRead();
  for (int i = 0; i < 16; ++i)
    EXPECT_EQ(d[i], static_cast<unsigned char>(i * 3 + 7)) << "byte " << i;

  ipf::free_drive_tracks(&drive);

  // Flux mirror: 3 revolutions, side 0, one cylinder.
  auto cyls = img.mirror_side0(3);
  ASSERT_EQ(cyls.size(), 1u);
  ASSERT_EQ(cyls[0].size(), 3u);
  EXPECT_GT(cyls[0][0].nbits, 0u);
  EXPECT_EQ(cyls[0][0].nbits, cyls[0][1].nbits);  // stable ⇒ identical

  std::vector<uint8_t> scp = scp_from_mfm_tracks(cyls);
  ASSERT_FALSE(scp.empty());
  EXPECT_GE(flux_scp_probe(scp.data(), scp.size()), 0);
  EXPECT_EQ(flux_scp_revolutions(scp.data(), scp.size()), 3);
}

TEST(IpfDecode, WholeImageConvenienceDecode) {
  std::vector<uint8_t> payload(512, 0xE5);
  std::vector<uint8_t> gap(34);
  for (int i = 0; i < 22; ++i) gap[i] = 0x4E;
  std::vector<uint8_t> a;
  put_bytes(a, data_elem(ipf::DataType::Sync, sync_sample()));
  put_bytes(a, id_field(0, 0, 1, 2));
  put_bytes(a, data_elem(ipf::DataType::Gap, gap));
  put_bytes(a, data_elem(ipf::DataType::Sync, sync_sample()));
  put_bytes(a, data_field(payload));
  a.push_back(0x00);
  auto ipf = build_single_block(a, {}, 8992, 1024, 0, 0x4E);

  t_drive drive;
  std::memset(&drive, 0, sizeof(drive));
  ipf::DecodedIpf out;
  ASSERT_EQ(ipf::ipf_decode(ipf.bytes.data(), ipf.bytes.size(), &drive, &out, 3,
                            0x1234),
            Status::Ok);
  EXPECT_EQ(out.side0.size(), 1u);
  EXPECT_GE(drive.track[0][0].sectors, 1u);
  ipf::free_drive_tracks(&drive);
}

// The unified dispatcher must turn a valid SPS IPF into a usable SCP flux image
// — the clean-room read path that is now the sole IPF path. Exercises
// flux::to_scp end-to-end (sniff → Image::open → mirror_side0 →
// scp_from_mfm_tracks). A regression here — or a loader that aborts before
// reaching the dispatcher — would silently drop IPF support with no positive
// test to catch it.
TEST(IpfDecode, DispatcherTranscodesValidSpsIpfToScp) {
  std::vector<uint8_t> payload(512, 0xE5);
  std::vector<uint8_t> gap(34);
  for (int i = 0; i < 22; ++i) gap[i] = 0x4E;
  std::vector<uint8_t> a;
  put_bytes(a, data_elem(ipf::DataType::Sync, sync_sample()));
  put_bytes(a, id_field(0, 0, 1, 2));
  put_bytes(a, data_elem(ipf::DataType::Gap, gap));
  put_bytes(a, data_elem(ipf::DataType::Sync, sync_sample()));
  put_bytes(a, data_field(payload));
  a.push_back(0x00);
  auto ipf = build_single_block(a, {}, 8992, 1024, 0, 0x4E);

  std::vector<uint8_t> scp =
      flux::to_scp(ipf.bytes.data(), ipf.bytes.size(), ".ipf");
  ASSERT_FALSE(scp.empty());
  EXPECT_EQ(flux_scp_probe(scp.data(), scp.size()), 1);
}

// ---------------------------------------------------------------------------
// Fuzzy / weak-bit variance (§4.3/§8.3.5).
// ---------------------------------------------------------------------------
TEST(IpfDecode, FuzzyTrackVariesBetweenPasses) {
  // One block: Sync(48) + Fuzzy(100 bytes → 1600 raw). dataBits = 1648.
  std::vector<uint8_t> a;
  put_bytes(a, data_elem(ipf::DataType::Sync, sync_sample()));  // 48 raw
  put_bytes(a, fuzzy_elem(100));                                // 2·800 = 1600
  a.push_back(0x00);
  auto ipf = build_single_block(a, {}, 48 + 1600, 0, /*flags=*/0, 0,
                                /*density=*/2, /*track_flags=*/1);

  ipf::Image img;
  img.seed_rng(0xC0FFEE);
  ASSERT_EQ(img.open(ipf.bytes), Status::Ok);
  EXPECT_TRUE(img.track_flakey(0, 0));

  ipf::CleanTrackMFM p1, p2;
  ASSERT_TRUE(img.lock_track(0, 0, p1));
  ASSERT_TRUE(img.lock_track(0, 0, p2));
  EXPECT_EQ(p1.nbits, p2.nbits);
  EXPECT_TRUE(p1.flakey);
  EXPECT_NE(p1.bits, p2.bits);  // fuzzy region differs between passes

  auto cyls = img.mirror_side0(3);
  ASSERT_EQ(cyls.size(), 1u);
  ASSERT_EQ(cyls[0].size(), 3u);
  // Flakey ⇒ the three revolutions are not all identical.
  EXPECT_TRUE(cyls[0][0].bits != cyls[0][1].bits ||
              cyls[0][1].bits != cyls[0][2].bits);
}

// ---------------------------------------------------------------------------
// Hostile / malformed input rejection (§2.1 robustness).
// ---------------------------------------------------------------------------
TEST(IpfDecode, RejectsHostileInput) {
  ipf::Image img;
  EXPECT_EQ(img.open(nullptr, 0), Status::Truncated);

  std::vector<uint8_t> tiny = {'C', 'A'};
  EXPECT_EQ(img.open(tiny), Status::Truncated);

  std::vector<uint8_t> badmagic = {'N', 'O', 'P', 'E', 0, 0, 0, 12, 0, 0, 0, 0};
  EXPECT_EQ(img.open(badmagic), Status::BadMagic);

  // CAPS header claiming a length that runs past EOF.
  std::vector<uint8_t> overlong = {'C', 'A', 'P', 'S', 0, 0, 0xFF, 0xFF,
                                   0,   0,   0,   0};
  EXPECT_EQ(img.open(overlong), Status::Truncated);

  // Valid CAPS record but corrupted CRC.
  auto caps = caps_record();
  caps[11] ^= 0xFF;
  EXPECT_EQ(img.open(caps), Status::BadCrc);

  // A DATA record whose extra block runs past EOF.
  std::vector<uint8_t> f;
  put_bytes(f, caps_record());
  put_bytes(f, info_record(2, 0, 0));
  // Hand-craft a DATA record claiming a 9999-byte extra block that isn't there.
  std::vector<uint8_t> block;
  put_be32(block, 9999);
  put_be32(block, 9999 * 8);
  put_be32(block, 0);
  put_be32(block, 1);
  std::vector<uint8_t> dr = {'D', 'A', 'T', 'A'};
  put_be32(dr, 28);
  put_be32(dr, 0);
  put_bytes(dr, block);
  uint32_t crc = ipf::crc32(dr.data(), dr.size());
  dr[8] = static_cast<uint8_t>(crc >> 24);
  dr[9] = static_cast<uint8_t>(crc >> 16);
  dr[10] = static_cast<uint8_t>(crc >> 8);
  dr[11] = static_cast<uint8_t>(crc);
  put_bytes(f, dr);
  EXPECT_EQ(img.open(f), Status::Truncated);
}

TEST(IpfDecode, RejectsCapsEncoder) {
  std::vector<uint8_t> f;
  put_bytes(f, caps_record());
  put_bytes(f, info_record(1, 0, 0));  // encoderType 1 = CAPS (Phase C)
  ipf::Image img;
  EXPECT_EQ(img.open(f), Status::UnsupportedEncoder);
}

// The ipf_load glue (src/ipf.cpp, the sole IPF path) must reject a CAPS-encoder
// (encoderType 1) IPF with a non-zero return and leave the drive empty — there
// is no fallback. Exercises the FILE* overload's read-fully path end to end.
TEST(IpfDecode, IpfLoadRejectsCapsEncoderFile) {
  std::vector<uint8_t> f;
  put_bytes(f, caps_record());
  put_bytes(f, info_record(1, 0, 0));  // encoderType 1 = CAPS

  FILE* fp = tmpfile();
  ASSERT_NE(fp, nullptr);
  ASSERT_EQ(fwrite(f.data(), 1, f.size(), fp), f.size());
  rewind(fp);

  t_drive drive;
  std::memset(&drive, 0, sizeof(drive));
  EXPECT_NE(ipf_load(fp, &drive), 0);
  EXPECT_EQ(drive.tracks, 0u);
  fclose(fp);
  ipf::free_drive_tracks(&drive);  // no-op on an empty drive
}

TEST(IpfDecode, RejectsOutOfBoundsGeometry) {
  std::vector<uint8_t> f;
  put_bytes(f, caps_record());
  put_bytes(f, info_record(2, 200, 0));  // maxTrack 200 > DSK_TRACKMAX
  ipf::Image img;
  EXPECT_EQ(img.open(f), Status::BadGeometry);

  std::vector<uint8_t> f2;
  put_bytes(f2, caps_record());
  put_bytes(f2, info_record(2, 0, 5));  // maxSide 5 > DSK_SIDEMAX
  ipf::Image img2;
  EXPECT_EQ(img2.open(f2), Status::BadGeometry);
}

TEST(IpfDecode, ToleratesUnknownRecords) {
  // Insert a CTEI-like unknown record between INFO and IMGE; it must be
  // skipped via header.length (§2.8).
  std::vector<uint8_t> f;
  put_bytes(f, caps_record());
  put_bytes(f, info_record(2, 0, 0));
  put_bytes(f, record("CTEI", std::vector<uint8_t>(64, 0)));  // 76 bytes
  auto ipf = build_single_block(block0_data_area(), {}, 8992, 0, 0, 0);
  // Append the IMGE + DATA from a freshly built single-block image (skip its
  // CAPS+INFO prefix — 12 + 96 bytes).
  put_bytes(f, std::vector<uint8_t>(ipf.bytes.begin() + 12 + 96,
                                    ipf.bytes.end()));
  ipf::Image img;
  EXPECT_EQ(img.open(f), Status::Ok);
  EXPECT_TRUE(img.has_track(0, 0));
}
