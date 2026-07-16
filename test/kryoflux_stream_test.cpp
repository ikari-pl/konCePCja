// kryoflux_stream_test.cpp — clean-room correctness proof for the KryoFlux
// STREAM -> SCP transcoder. Every expected value below is computed by hand in
// the comments so the test doubles as the license-free correctness argument.
//
// Trick used throughout the decode tests: we inject a KFInfo OOB carrying
// "sck=40000000" so the sck -> 25 ns conversion becomes the IDENTITY
// (40000000 / 40000000 = 1). That lets us assert decoded flux values directly
// in sck units. The REAL default-clock conversion (4375/2628) is pinned
// separately in KryoFluxStream.ClockConversion with concrete numbers.

#include "kryoflux_stream.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "gtest/gtest.h"

extern "C" {
#include "hw/flux.h"  // flux_scp_probe — proves the emitted SCP is well-formed
}

namespace {

// ---- small stream builders -------------------------------------------------

void put_kfinfo_sck(std::vector<uint8_t>& s, const std::string& body) {
  // OOB: 0x0D, type 0x04 (KFInfo), size (LE), body. OOB does not advance spos.
  s.push_back(0x0D);
  s.push_back(0x04);
  s.push_back(static_cast<uint8_t>(body.size() & 0xFF));
  s.push_back(static_cast<uint8_t>((body.size() >> 8) & 0xFF));
  for (char c : body) s.push_back(static_cast<uint8_t>(c));
}

void put_index(std::vector<uint8_t>& s, uint32_t stream_pos,
               uint32_t sample_counter) {
  // OOB: 0x0D, type 0x02 (Index), size=12, then 3x u32 LE.
  s.push_back(0x0D);
  s.push_back(0x02);
  s.push_back(12);
  s.push_back(0);
  auto put32 = [&](uint32_t v) {
    s.push_back(static_cast<uint8_t>(v));
    s.push_back(static_cast<uint8_t>(v >> 8));
    s.push_back(static_cast<uint8_t>(v >> 16));
    s.push_back(static_cast<uint8_t>(v >> 24));
  };
  put32(stream_pos);
  put32(sample_counter);
  put32(0);  // IndexCounter — unused by the transcoder
}

void put_eof(std::vector<uint8_t>& s) {
  s.push_back(0x0D);
  s.push_back(0x0D);  // EOF type
  s.push_back(0x0D);  // sentinel size bytes (must NOT be consumed)
  s.push_back(0x0D);
}

uint32_t rd32le(const std::vector<uint8_t>& v, size_t at) {
  return static_cast<uint32_t>(v[at]) |
         (static_cast<uint32_t>(v[at + 1]) << 8) |
         (static_cast<uint32_t>(v[at + 2]) << 16) |
         (static_cast<uint32_t>(v[at + 3]) << 24);
}
uint16_t rd16be(const std::vector<uint8_t>& v, size_t at) {
  return static_cast<uint16_t>((v[at] << 8) | v[at + 1]);
}

}  // namespace

// ===========================================================================
// 1. Clock / units conversion, with concrete hand-computed numbers.
//
// Default sck = 24027428.571428 Hz. ticks_25ns = sck * 40000000 / sck_hz, which
// for the default clock is the EXACT rational sck * 4375/2628.
//   48 * 4375/2628 = 210000/2628 = 79.909  -> 80   (a 2 us DD flux interval)
//   24 * 4375/2628 = 105000/2628 = 39.954  -> 40
//   96 * 4375/2628 = 420000/2628 = 159.817 -> 160
//    0                                      ->  0
// ===========================================================================
TEST(KryoFluxStream, ClockConversion) {
  const double sck = KFSTREAM_DEFAULT_SCK_HZ;
  EXPECT_EQ(kryoflux_sck_to_25ns(0, sck), 0u);
  EXPECT_EQ(kryoflux_sck_to_25ns(24, sck), 40u);
  EXPECT_EQ(kryoflux_sck_to_25ns(48, sck), 80u);
  EXPECT_EQ(kryoflux_sck_to_25ns(96, sck), 160u);

  // Sanity on the 2 us cross-check: 80 ticks * 25 ns = 2000 ns.
  EXPECT_EQ(kryoflux_sck_to_25ns(48, sck) * 25u, 2000u);

  // A KFInfo override of sck=40000000 makes the conversion the identity.
  EXPECT_EQ(kryoflux_sck_to_25ns(1234, 40000000.0), 1234u);
  // Doubling the clock halves the tick count (round-to-nearest).
  EXPECT_EQ(kryoflux_sck_to_25ns(100, 80000000.0), 50u);
}

// ===========================================================================
// 2. Decode a hand-built single-revolution stream: Flux1, Flux2, and an
//    Ovl16+Flux1 (value > 0xFFFF), then check both the decoded flux and the
//    emitted SCP bytes exactly.
//
// With sck=40000000 (identity), the in-band layout and its stream positions:
//   spos 0: Flux1 0x50            -> 80  sck
//   spos 1: Flux1 0x30            -> 48  sck
//   spos 2: Flux2 0x01 0x00       -> 256 sck
//   spos 4: Ovl16 + Flux1 0x10    -> 0x10000 + 16 = 65552 sck  (pos = 4)
//   spos 6: (end)
// Prefix times: T0=80, T1=128, T2=384, T3=65936.
// index0: StreamPosition=0, SampleCounter=0 -> fi=0, I0 = T(-1)=0 + 0 = 0.
// index1: StreamPosition=6, SampleCounter=20 -> fi=4 (past end),
//         I1 = T3 + 20 = 65956.
// Rev 0 spans flux [0..3]; words = differenced (cum - I0):
//   w0 = 80-0   = 80
//   w1 = 128-80 = 48
//   w2 = 384-128= 256
//   w3 = 65936-384 = 65552
//   duration = I1 - I0 = 65956
// SCP flux words (BE, 65552 splits into a 0x0000 carry + 0x0010): total 5 words
//   0x0050 0x0030 0x0100 0x0000 0x0010
// ===========================================================================
TEST(KryoFluxStream, DecodesSingleRevAndOverflow) {
  std::vector<uint8_t> s;
  put_kfinfo_sck(s, "sck=40000000, ick=3003428.5714");
  s.push_back(0x50);  // Flux1 -> 80,  spos 0
  s.push_back(0x30);  // Flux1 -> 48,  spos 1
  s.push_back(0x01);  // Flux2 hi,     spos 2
  s.push_back(0x00);  // Flux2 lo -> 256
  s.push_back(0x0B);  // Ovl16,        spos 4
  s.push_back(0x10);  // Flux1 -> 0x10000+16 = 65552, spos 5
  put_index(s, 0, 0);
  put_index(s, 6, 20);
  put_eof(s);

  KryoFluxTrack trk;
  ASSERT_EQ(kryoflux_decode_stream(s.data(), s.size(), trk), 0);
  ASSERT_EQ(trk.sck_hz, 40000000.0);
  ASSERT_EQ(trk.revs.size(), 1u);
  const std::vector<uint32_t> expect = {80, 48, 256, 65552};
  EXPECT_EQ(trk.revs[0].flux_25ns, expect);
  EXPECT_EQ(trk.revs[0].duration_25ns, 65956u);

  // Now the SCP image, checked byte-region by byte-region.
  std::vector<uint8_t> scp;
  ASSERT_EQ(
      kryoflux_stream_to_scp(s.data(), s.size(), /*cyl=*/0, /*side=*/0, scp),
      0);
  ASSERT_GE(scp.size(), 0x2B0u);
  EXPECT_EQ(std::string(scp.begin(), scp.begin() + 3), "SCP");
  EXPECT_EQ(scp[0x05], 1);  // one revolution
  EXPECT_EQ(scp[0x09], 0);  // 16-bit cells
  EXPECT_EQ(scp[0x0A], 0);  // both heads (side 0 filled)
  EXPECT_EQ(scp[0x0B], 0);  // resolution 0 -> 25 ns/tick

  // TLUT slot 0 (cyl0/side0) points at the first track, which starts at 0x2B0.
  const uint32_t toff = rd32le(scp, 0x10);
  EXPECT_EQ(toff, 0x2B0u);
  EXPECT_EQ(std::string(scp.begin() + toff, scp.begin() + toff + 3), "TRK");
  EXPECT_EQ(scp[toff + 3], 0);  // slot number

  // One 12-byte revolution entry follows the 4-byte TDH id.
  const size_t e = toff + 4;
  EXPECT_EQ(rd32le(scp, e + 0), 65956u);  // duration (25 ns units)
  EXPECT_EQ(rd32le(scp, e + 4), 5u);  // word count (with the overflow carry)
  const uint32_t doff = rd32le(scp, e + 8);
  EXPECT_EQ(doff, 16u);  // 4 (TDH id) + 12 (one rev entry)

  // Flux words: 0x0050 0x0030 0x0100 0x0000 0x0010.
  const size_t f = toff + doff;
  EXPECT_EQ(rd16be(scp, f + 0), 0x0050);
  EXPECT_EQ(rd16be(scp, f + 2), 0x0030);
  EXPECT_EQ(rd16be(scp, f + 4), 0x0100);
  EXPECT_EQ(rd16be(scp, f + 6), 0x0000);  // 65536-tick carry
  EXPECT_EQ(rd16be(scp, f + 8), 0x0010);  // remainder 16

  // The emitted SCP is structurally valid to the real flux decoder.
  EXPECT_EQ(flux_scp_probe(scp.data(), scp.size()), 1);
}

// ===========================================================================
// 3. Mid-buffer Index: three indices -> two revolutions, and the split must
//    land exactly on the flux the middle index points at.
//
// sck=40000000 (identity). Six Flux1 blocks at spos 0..5:
//   f0=0x64(100) f1=0x64(100) f2=0x64(100) f3=0x50(80) f4=0x64(100)
//   f5=0x64(100)
// Prefix: 100,200,300,380,480,580.
// index0: SP=0  SC=0 -> fi=0, I0 = 0
// index1: SP=3  SC=0 -> fi=3, I1 = T2 + 0 = 300   (the mid-buffer split)
// index2: SP=6  SC=0 -> fi=6 (past end), I2 = T5 + 0 = 580
// Rev0 = flux[0..2]: words 100,100,100 ; duration I1-I0 = 300
// Rev1 = flux[3..5]: rel = 380-300, 480-300, 580-300 = 80,180,280
//                    words 80,100,100 ; duration I2-I1 = 280
// The distinct 80 as rev1[0] proves the split began exactly at f3.
// ===========================================================================
TEST(KryoFluxStream, MidBufferIndexSplit) {
  std::vector<uint8_t> s;
  put_kfinfo_sck(s, "sck=40000000");
  s.push_back(0x64);  // f0 100
  s.push_back(0x64);  // f1 100
  s.push_back(0x64);  // f2 100
  s.push_back(0x50);  // f3 80  (distinct: marks the split)
  s.push_back(0x64);  // f4 100
  s.push_back(0x64);  // f5 100
  put_index(s, 0, 0);
  put_index(s, 3, 0);  // points at f3
  put_index(s, 6, 0);
  put_eof(s);

  KryoFluxTrack trk;
  ASSERT_EQ(kryoflux_decode_stream(s.data(), s.size(), trk), 0);
  ASSERT_EQ(trk.revs.size(), 2u);

  const std::vector<uint32_t> rev0 = {100, 100, 100};
  const std::vector<uint32_t> rev1 = {80, 100, 100};
  EXPECT_EQ(trk.revs[0].flux_25ns, rev0);
  EXPECT_EQ(trk.revs[0].duration_25ns, 300u);
  EXPECT_EQ(trk.revs[1].flux_25ns, rev1);
  EXPECT_EQ(trk.revs[1].duration_25ns, 280u);
}

// ===========================================================================
// 4. Multi-track assembly: two per-track streams -> one SCP with both tracks
//    at their cyl*2+side slots, normalized to the shared revolution count.
// ===========================================================================
TEST(KryoFluxStream, AssemblesMultiTrackScp) {
  auto make_stream = [](uint8_t flux_val) {
    std::vector<uint8_t> s;
    put_kfinfo_sck(s, "sck=40000000");
    for (int i = 0; i < 4; i++) s.push_back(flux_val);
    put_index(s, 0, 0);
    put_index(s, 2, 0);  // rev0 = flux[0..1]
    put_index(s, 4, 0);  // rev1 = flux[2..3]
    put_eof(s);
    return s;
  };
  const std::vector<uint8_t> t0 = make_stream(0x40);  // cyl 0
  const std::vector<uint8_t> t1 = make_stream(0x50);  // cyl 1

  std::vector<KryoFluxMember> members = {
      {t0.data(), t0.size(), /*cyl=*/0, /*side=*/0},
      {t1.data(), t1.size(), /*cyl=*/1, /*side=*/0},
  };
  std::vector<uint8_t> scp;
  ASSERT_EQ(kryoflux_streams_to_scp(members, scp), 0);
  ASSERT_GE(scp.size(), 0x2B0u);
  EXPECT_EQ(scp[0x05], 2);  // two revolutions, shared

  // Slot 0 (cyl0/side0) and slot 2 (cyl1/side0) both present; slot 1 absent.
  EXPECT_NE(rd32le(scp, 0x10 + 0 * 4), 0u);
  EXPECT_EQ(rd32le(scp, 0x10 + 1 * 4), 0u);
  EXPECT_NE(rd32le(scp, 0x10 + 2 * 4), 0u);
  EXPECT_EQ(flux_scp_probe(scp.data(), scp.size()), 1);
}

// ===========================================================================
// 5. Truncated / hostile input is rejected cleanly (no crash, no OOB read).
// ===========================================================================
TEST(KryoFluxStream, RejectsTruncatedAndGarbage) {
  KryoFluxTrack trk;

  // Null / empty.
  EXPECT_LT(kryoflux_decode_stream(nullptr, 0, trk), 0);
  const uint8_t empty[1] = {0};
  EXPECT_LT(kryoflux_decode_stream(empty, 0, trk), 0);

  // A lone Flux2 header with no second byte -> truncated.
  const uint8_t f2trunc[1] = {0x01};
  EXPECT_EQ(kryoflux_decode_stream(f2trunc, sizeof(f2trunc), trk),
            KFSTREAM_E_TRUNCATED);

  // A lone Flux3 header missing its two data bytes -> truncated.
  const uint8_t f3trunc[2] = {0x0C, 0x12};
  EXPECT_EQ(kryoflux_decode_stream(f3trunc, sizeof(f3trunc), trk),
            KFSTREAM_E_TRUNCATED);

  // OOB claiming a 12-byte body that runs past the buffer -> truncated.
  const uint8_t oobtrunc[6] = {0x0D, 0x02, 0x0C, 0x00, 0x00, 0x00};
  EXPECT_EQ(kryoflux_decode_stream(oobtrunc, sizeof(oobtrunc), trk),
            KFSTREAM_E_TRUNCATED);

  // An Index OOB that is too short (size 4, not 12) -> bad OOB.
  const uint8_t oobshort[8] = {0x0D, 0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};
  EXPECT_EQ(kryoflux_decode_stream(oobshort, sizeof(oobshort), trk),
            KFSTREAM_E_BAD_OOB);

  // Flux but no index at all -> cannot form a revolution.
  const uint8_t noidx[3] = {0x50, 0x50, 0x50};
  EXPECT_EQ(kryoflux_decode_stream(noidx, sizeof(noidx), trk),
            KFSTREAM_E_NO_INDEX);
}

// ===========================================================================
// 6. EOF OOB stops parsing: trailing garbage after an EOF block is ignored.
// ===========================================================================
TEST(KryoFluxStream, EofStopsParsing) {
  std::vector<uint8_t> s;
  put_kfinfo_sck(s, "sck=40000000");
  s.push_back(0x64);
  s.push_back(0x64);
  put_index(s, 0, 0);
  put_index(s, 2, 0);
  put_eof(s);
  // Garbage that would otherwise be a truncated Flux2 — must be ignored.
  s.push_back(0x01);

  KryoFluxTrack trk;
  EXPECT_EQ(kryoflux_decode_stream(s.data(), s.size(), trk), 0);
  ASSERT_EQ(trk.revs.size(), 1u);
}

// ===========================================================================
// 7. Optional real-capture oracle. Decode a real per-track KryoFlux stream ->
//    SCP -> DSK and require the decoder to recover at least one sector. Gated
//    on KONCEPCJA_REAL_STREAM (a path to a single trackNN.S.raw). SKIP when
//    absent — never commit possibly-unlicensed captures.
// ===========================================================================
TEST(KryoFluxStream, DecodesARealStreamCaptureWhenProvided) {
  const char* path = std::getenv("KONCEPCJA_REAL_STREAM");
  if (path == nullptr) {
    GTEST_SKIP() << "set KONCEPCJA_REAL_STREAM=<trackNN.S.raw> to run";
  }
  FILE* fp = std::fopen(path, "rb");
  ASSERT_NE(fp, nullptr) << "cannot open " << path;
  std::fseek(fp, 0, SEEK_END);
  const long sz = std::ftell(fp);
  std::fseek(fp, 0, SEEK_SET);
  ASSERT_GT(sz, 0);
  std::vector<uint8_t> raw(static_cast<size_t>(sz));
  ASSERT_EQ(std::fread(raw.data(), 1, raw.size(), fp), raw.size());
  std::fclose(fp);

  std::vector<uint8_t> scp;
  ASSERT_EQ(kryoflux_stream_to_scp(raw.data(), raw.size(), 0, 0, scp), 0);
  ASSERT_EQ(flux_scp_probe(scp.data(), scp.size()), 1);

  std::vector<uint8_t> dsk(1u << 20);
  const long dlen =
      flux_scp_to_dsk(scp.data(), scp.size(), dsk.data(), dsk.size(), nullptr);
  EXPECT_GT(dlen, 0) << "no sectors recovered from the real capture";
}
