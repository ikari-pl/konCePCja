/* hfe_test.cpp — the HxC "HFE" v1 -> SCP transcoder (src/hfe). Hand-built
 * minimal HFE images are transcoded, then the emitted SCP is inspected
 * byte-for-byte (header, TLUT slot, revolution entry, flux words) with
 * hand-computed expected values — in the style of test/hw/a2r_test.cpp.
 * Malformed/truncated/hostile inputs are
 * checked for clean rejection (no OOB). An optional gated real-capture test
 * exercises the whole hfe_to_scp -> flux_scp_to_dsk pipeline when a fixture
 * is provided out-of-band (never committed).
 */
#include "hfe.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "hw/flux.h"

namespace {

void put_u16le(std::vector<uint8_t>& v, size_t at, uint16_t x) {
  v[at] = static_cast<uint8_t>(x & 0xFF);
  v[at + 1] = static_cast<uint8_t>((x >> 8) & 0xFF);
}

uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

uint32_t be16(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 8) | p[1];
}

// A minimal, otherwise-valid HFE v1 header (512 bytes, unused = 0xFF per
// spec convention). Only the fields hfe_to_scp reads are meaningfully set;
// everything else is inert filler.
std::vector<uint8_t> make_header(uint8_t number_of_track, uint16_t bit_rate,
                                  uint8_t format_revision = 0,
                                  uint16_t track_list_offset = 1,
                                  const char* signature = "HXCPICFE") {
  std::vector<uint8_t> h(512, 0xFF);
  std::memcpy(h.data(), signature, 8);
  h[0x08] = format_revision;
  h[0x09] = number_of_track;
  h[0x0A] = 2;    // number_of_side — informational only, never read
  h[0x0B] = 0x00; // track_encoding — ignored (see hfe.cpp)
  put_u16le(h, 0x0C, bit_rate);
  put_u16le(h, 0x0E, 300);  // floppyRPM — ignored
  h[0x10] = 0x06;            // floppyinterfacemode: CPC_DD_FLOPPYMODE
  h[0x11] = 0xFF;            // dnu (reserved)
  put_u16le(h, 0x12, track_list_offset);
  h[0x14] = 0xFF;  // write_allowed — ignored
  h[0x15] = 0xFF;  // single_step — ignored
  return h;
}

// Assemble a full HFE v1 image: 512-byte header, 512-byte LUT block
// (track_list_offset == 1), then one 512-byte-aligned data block per track
// starting at block 2. `track_contents[t]` is track t's raw combined
// (side0-then-side1) byte content; empty == absent/unformatted (LUT entry
// {0,0}).
std::vector<uint8_t> make_hfe_image(
    uint8_t number_of_track, uint16_t bit_rate,
    const std::vector<std::vector<uint8_t>>& track_contents) {
  std::vector<uint8_t> img = make_header(number_of_track, bit_rate);
  img.resize(1024, 0);  // header block (0) + LUT block (1)

  for (uint8_t t = 0; t < number_of_track; t++) {
    const std::vector<uint8_t>& content = track_contents[t];
    const uint16_t off_blocks = static_cast<uint16_t>(2 + t);
    const uint16_t trk_len = static_cast<uint16_t>(content.size());
    put_u16le(img, 512 + 4u * t, off_blocks);
    put_u16le(img, 512 + 4u * t + 2, trk_len);
    if (content.empty()) continue;
    const size_t byte_off = static_cast<size_t>(off_blocks) * 512u;
    if (img.size() < byte_off + content.size())
      img.resize(byte_off + content.size(), 0);
    std::copy(content.begin(), content.end(), img.begin() + byte_off);
  }
  return img;
}

std::vector<uint8_t> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

}  // namespace

// ---- hfe_ticks_per_cell: the bitrate -> SCP-tick arithmetic in isolation ----

TEST(HfeTicksPerCell, ConcreteNumbers) {
  // cell_time = 1/(bitRate*2000) s; ticks = cell_time/25ns = 20000/bitRate.
  EXPECT_EQ(hfe_ticks_per_cell(250), 80u)
      << "CPC/IBM DD-MFM standard: 2 us cells = 80 * 25 ns ticks";
  EXPECT_EQ(hfe_ticks_per_cell(500), 40u) << "HD rate: 1 us cells";
  EXPECT_EQ(hfe_ticks_per_cell(125), 160u) << "half-speed: 4 us cells";
  EXPECT_EQ(hfe_ticks_per_cell(0), 0u) << "division-by-zero guard";
  EXPECT_EQ(hfe_ticks_per_cell(300), 66u)
      << "20000/300 = 66.67 truncates to 66 — an inexact, unsupported rate; "
         "hfe_to_scp rejects it rather than emitting mistimed flux";
}

// ---- Malformed / truncated / hostile input: clean rejection, no OOB ----

TEST(Hfe, RejectsTooShortBuffer) {
  std::vector<uint8_t> out;
  const std::vector<uint8_t> junk(100, 0);
  EXPECT_EQ(hfe_to_scp(junk.data(), junk.size(), out), HFE_E_TRUNCATED);
  EXPECT_TRUE(out.empty());
  EXPECT_EQ(hfe_to_scp(nullptr, 0, out), HFE_E_TRUNCATED);
}

TEST(Hfe, RejectsBadSignature) {
  std::vector<uint8_t> out;
  std::vector<uint8_t> img(512, 0);
  EXPECT_EQ(hfe_to_scp(img.data(), img.size(), out), HFE_E_NOT_HFE);
  EXPECT_TRUE(out.empty());
}

TEST(Hfe, RejectsHfeV3Signature) {
  std::vector<uint8_t> out;
  std::vector<uint8_t> img =
      make_header(1, 250, /*format_revision=*/0, /*track_list_offset=*/1,
                  "HXCHFEV3");
  EXPECT_EQ(hfe_to_scp(img.data(), img.size(), out), HFE_E_UNSUPPORTED);
  EXPECT_TRUE(out.empty());
}

TEST(Hfe, RejectsNonV1FormatRevision) {
  std::vector<uint8_t> out;
  std::vector<uint8_t> img = make_header(1, 250, /*format_revision=*/1);
  EXPECT_EQ(hfe_to_scp(img.data(), img.size(), out), HFE_E_UNSUPPORTED);
  EXPECT_TRUE(out.empty());
}

TEST(Hfe, RejectsZeroTracks) {
  std::vector<uint8_t> out;
  std::vector<uint8_t> img = make_header(0, 250);
  EXPECT_EQ(hfe_to_scp(img.data(), img.size(), out), HFE_E_GEOMETRY);
  EXPECT_TRUE(out.empty());
}

TEST(Hfe, RejectsTooManyTracks) {
  std::vector<uint8_t> out;
  std::vector<uint8_t> img = make_header(85, 250);  // > 84 side-0 slots
  EXPECT_EQ(hfe_to_scp(img.data(), img.size(), out), HFE_E_UNSUPPORTED);
  EXPECT_TRUE(out.empty());
}

TEST(Hfe, RejectsNonCpcStandardBitrate) {
  std::vector<uint8_t> out;
  std::vector<uint8_t> img = make_header(1, 500);  // HD rate, not CPC DD
  EXPECT_EQ(hfe_to_scp(img.data(), img.size(), out), HFE_E_UNSUPPORTED);
  EXPECT_TRUE(out.empty());
}

TEST(Hfe, RejectsLutRunningPastBuffer) {
  std::vector<uint8_t> out;
  std::vector<uint8_t> img = make_header(1, 250);  // LUT needs bytes 512..515
  img.resize(514, 0);  // only 2 of the 4 LUT-entry bytes present
  EXPECT_EQ(hfe_to_scp(img.data(), img.size(), out), HFE_E_TRUNCATED);
  EXPECT_TRUE(out.empty());
}

TEST(Hfe, RejectsTrackDataRunningPastBuffer) {
  std::vector<uint8_t> out;
  std::vector<uint8_t> img = make_header(1, 250);
  img.resize(1024, 0);
  put_u16le(img, 512, 2);   // track 0 data at block 2 (byte 1024)
  put_u16le(img, 514, 10);  // claims 10 bytes — buffer ends right there
  EXPECT_EQ(hfe_to_scp(img.data(), img.size(), out), HFE_E_TRUNCATED);
  EXPECT_TRUE(out.empty());
}

// ---- Bit order: LSb-first transmission within each byte ----

TEST(Hfe, BitOrderIsLsbFirstWithinEachByte) {
  // Side-0 byte 0xAA = 0b1010_1010. LSb-first transmission order reads
  // bit0(=0),bit1(=1),bit2(=0),bit3(=1),bit4(=0),bit5(=1),bit6(=0),bit7(=1)
  // -> logical bitcell sequence 0,1,0,1,0,1,0,1: transitions ('1's) at
  // logical cells 1,3,5,7, each 2 cells apart -> four flux intervals of
  // 2*80=160 ticks. (Getting the bit order backwards — MSb-first — would
  // instead read 1,0,1,0,1,0,1,0: transitions at cells 0,2,4,6, giving a
  // FIRST interval of 1*80=80 ticks, not 160 — this test would catch that.)
  std::vector<uint8_t> out;
  const std::vector<uint8_t> img =
      make_hfe_image(/*number_of_track=*/1, /*bit_rate=*/250,
                      {std::vector<uint8_t>{0xAA}});
  ASSERT_EQ(hfe_to_scp(img.data(), img.size(), out), 0);

  const uint32_t toff = le32(out.data() + 0x10);
  ASSERT_NE(toff, 0u);
  const uint32_t words = le32(out.data() + toff + 8);
  const uint32_t doff = le32(out.data() + toff + 12);
  ASSERT_EQ(words, 4u);
  const uint8_t* flux = out.data() + toff + doff;
  for (int w = 0; w < 4; w++)
    EXPECT_EQ(be16(flux + 2 * w), 160u) << "interval " << w;
}

// ---- Full structural transcode: header, TLUT, revolution entry, flux words,
// and an absent (unformatted) second track ----

TEST(Hfe, TranscodesTwoTrackImageIntoAWellFormedScp) {
  // Track 0 fills one complete 512-byte block: 256 bytes side 0, then 256
  // bytes side 1. Side 0 = byte0 0x01, the remaining 255 bytes zero; side 1
  // = 256 bytes of 0xFF filler (deliberately transition-dense so a BROKEN
  // de-interleave that leaked side-1 into the bitstream would explode the
  // word count — this is what proves the 256/256 split works).
  //   Side-0 byte0 0x01 = 0b0000_0001 -> LSb-first logical cells:
  //   1,0,0,0,0,0,0,0; the 255 following zero bytes add 2040 more zero cells.
  //   Net: ONE flux transition at logical cell 0 (gap = 1 cell = 80 ticks),
  //   then a long trailing transition-less gap that scp_from_mfm_tracks's
  //   encoder drops (matches a real capture — see ipf.cpp push_flux_rev).
  //   nbits = 256*8 = 2048; duration = 2048*80 = 163840 ticks.
  // Track 1 is absent (track_len = 0) -> an unformatted/empty TLUT slot.
  std::vector<uint8_t> track0(512, 0x00);
  track0[0] = 0x01;                          // side-0 byte 0
  std::fill(track0.begin() + 256, track0.end(), 0xFF);  // side-1 filler
  const std::vector<uint8_t> img = make_hfe_image(
      /*number_of_track=*/2, /*bit_rate=*/250, {track0, std::vector<uint8_t>{}});

  std::vector<uint8_t> scp;
  ASSERT_EQ(hfe_to_scp(img.data(), img.size(), scp), 0);

  ASSERT_GE(scp.size(), 0x2B0u);
  EXPECT_EQ(std::memcmp(scp.data(), "SCP", 3), 0);
  EXPECT_EQ(scp[0x05], 1) << "one revolution per track (HFE v1: fixed rate)";
  EXPECT_EQ(scp[0x06], 0) << "start track";
  EXPECT_EQ(scp[0x07], 2) << "last side-0 slot = (number_of_track-1)*2";
  EXPECT_EQ(scp[0x09], 16) << "16-bit flux words";
  EXPECT_EQ(scp[0x0A], 0) << "both-heads slot layout (side-0 slots = cyl*2)";
  EXPECT_EQ(scp[0x0B], 0) << "25 ns resolution";

  // Track-lookup slot 0 (cyl 0, side 0) -> a "TRK" data header.
  const uint32_t toff = le32(scp.data() + 0x10);
  ASSERT_NE(toff, 0u);
  ASSERT_LE(static_cast<size_t>(toff) + 4, scp.size());
  EXPECT_EQ(std::memcmp(scp.data() + toff, "TRK", 3), 0);
  EXPECT_EQ(scp[toff + 3], 0) << "slot number 0";

  const uint32_t duration = le32(scp.data() + toff + 4);
  const uint32_t words = le32(scp.data() + toff + 8);
  const uint32_t doff = le32(scp.data() + toff + 12);
  EXPECT_EQ(duration, 2048u * 80u) << "index-to-index ticks = nbits*ticks/cell";
  EXPECT_EQ(words, 1u)
      << "exactly one transition — side-1's 0xFF filler was de-interleaved "
         "out; a broken split would inflate this";
  const uint8_t* flux = scp.data() + toff + doff;
  ASSERT_LE(static_cast<size_t>(toff) + doff + 2u * words, scp.size());
  EXPECT_EQ(be16(flux + 0), 80u) << "1 cell * 80 ticks/cell";

  // Slot 2 (cyl 1, side 0) is absent — track 1 was unformatted.
  EXPECT_EQ(le32(scp.data() + 0x10 + 4 * 2), 0u);
  // Odd (side-1) slots are always absent — this transcoder is side-0 only.
  EXPECT_EQ(le32(scp.data() + 0x10 + 4 * 1), 0u);
  EXPECT_EQ(le32(scp.data() + 0x10 + 4 * 3), 0u);

  uint32_t sum = 0;  // stored checksum covers 0x10..EOF
  for (size_t i = 0x10; i < scp.size(); i++) sum += scp[i];
  EXPECT_EQ(le32(scp.data() + 0x0c), sum);

  // The engine-side parser agrees. flux_scp_cylinders reports the highest
  // PRESENT side-0 cylinder + 1 — track 1 is absent, so only cylinder 0 is
  // present and it returns 1 (not the 2-track LUT span).
  EXPECT_EQ(flux_scp_probe(scp.data(), scp.size()), 1);
  EXPECT_EQ(flux_scp_revolutions(scp.data(), scp.size()), 1);
  EXPECT_EQ(flux_scp_cylinders(scp.data(), scp.size()), 1);
}

TEST(Hfe, DeinterleavesAcrossMultipleBlocks) {
  // A track spanning TWO 512-byte blocks exercises the repeating
  // side0/side1 split loop (256 B side0, skip 256 B side1, next block).
  // Layout (1024 bytes): block0 s0[0]=0x01, block0 s1 = 0xFF filler;
  // block1 s0[0] (file byte 512) = 0x01, block1 s1 = 0xFF filler.
  // De-interleaved side 0 = 512 bytes = 4096 bitcells, with a '1' at
  // logical cell 0 (block0 byte0 bit0) and logical cell 2048 (block1's
  // first side-0 byte, i.e. side-0 byte 256 -> bit 2048). The second gap
  // (2048 cells * 80 = 163840 ticks) also crosses the SCP 16-bit ceiling,
  // so it doubles as an overflow check: 163840 = 2*65536 + 32768 -> two
  // 0x0000 carry words then 32768 (matches ipf.cpp push_flux_rev).
  std::vector<uint8_t> track0(1024, 0x00);
  track0[0] = 0x01;                                    // block0 side-0 byte0
  std::fill(track0.begin() + 256, track0.begin() + 512, 0xFF);  // block0 s1
  track0[512] = 0x01;                                  // block1 side-0 byte0
  std::fill(track0.begin() + 768, track0.end(), 0xFF); // block1 s1
  const std::vector<uint8_t> img =
      make_hfe_image(/*number_of_track=*/1, /*bit_rate=*/250, {track0});

  std::vector<uint8_t> scp;
  ASSERT_EQ(hfe_to_scp(img.data(), img.size(), scp), 0);

  const uint32_t toff = le32(scp.data() + 0x10);
  ASSERT_NE(toff, 0u);
  const uint32_t duration = le32(scp.data() + toff + 4);
  const uint32_t words = le32(scp.data() + toff + 8);
  const uint32_t doff = le32(scp.data() + toff + 12);
  EXPECT_EQ(duration, 4096u * 80u) << "512 de-interleaved bytes = 4096 cells";
  ASSERT_EQ(words, 4u) << "80 tick word + two 0x0000 carries + 32768 remainder";
  const uint8_t* flux = scp.data() + toff + doff;
  ASSERT_LE(static_cast<size_t>(toff) + doff + 2u * words, scp.size());
  EXPECT_EQ(be16(flux + 0), 80u);
  EXPECT_EQ(be16(flux + 2), 0u) << "65536-tick carry 1";
  EXPECT_EQ(be16(flux + 4), 0u) << "65536-tick carry 2";
  EXPECT_EQ(be16(flux + 6), 32768u) << "163840 - 2*65536 remainder";
}

TEST(Hfe, BothCylindersPresentUseEvenSlots) {
  // Two single-block formatted tracks -> side-0 slots 0 and 2 (= cyl*2)
  // both present, odd (side-1) slots absent, flux_scp_cylinders == 2.
  std::vector<uint8_t> blk(512, 0x00);
  blk[0] = 0x01;
  std::fill(blk.begin() + 256, blk.end(), 0xFF);  // side-1 filler
  const std::vector<uint8_t> img =
      make_hfe_image(/*number_of_track=*/2, /*bit_rate=*/250, {blk, blk});

  std::vector<uint8_t> scp;
  ASSERT_EQ(hfe_to_scp(img.data(), img.size(), scp), 0);
  EXPECT_NE(le32(scp.data() + 0x10 + 4 * 0), 0u) << "cyl 0 -> slot 0 present";
  EXPECT_EQ(le32(scp.data() + 0x10 + 4 * 1), 0u) << "slot 1 (side-1) absent";
  EXPECT_NE(le32(scp.data() + 0x10 + 4 * 2), 0u) << "cyl 1 -> slot 2 present";
  EXPECT_EQ(le32(scp.data() + 0x10 + 4 * 3), 0u) << "slot 3 (side-1) absent";
  EXPECT_EQ(flux_scp_probe(scp.data(), scp.size()), 1);
  EXPECT_EQ(flux_scp_cylinders(scp.data(), scp.size()), 2);
}

TEST(Hfe, AllTracksUnformattedIsRejected) {
  // scp_from_mfm_tracks finds zero revolutions anywhere and returns {};
  // hfe_to_scp surfaces that as HFE_E_GEOMETRY rather than an empty "OK".
  std::vector<uint8_t> out;
  const std::vector<uint8_t> img = make_hfe_image(
      /*number_of_track=*/2, /*bit_rate=*/250,
      {std::vector<uint8_t>{}, std::vector<uint8_t>{}});
  EXPECT_EQ(hfe_to_scp(img.data(), img.size(), out), HFE_E_GEOMETRY);
  EXPECT_TRUE(out.empty());
}

// ---- Optional oracle: a real HFE capture, provided out-of-band ----
//
// Set KONCEPCJA_REAL_HFE to its path, or drop it at test/hw/fixtures/
// real.hfe — so no (possibly unlicensed) preservation dump lands in the
// repo. Absent -> SKIP, so CI is unaffected (mirrors IpfMirror.
// MirrorsARealCapsImageWhenProvided / FdcFlux.DecodesARealScpCaptureWhenProvided).
TEST(Hfe, DecodesARealHfeCaptureWhenProvided) {
  std::string path;
  if (const char* env = std::getenv("KONCEPCJA_REAL_HFE")) path = env;
  if (path.empty() && !read_file("test/hw/fixtures/real.hfe").empty())
    path = "test/hw/fixtures/real.hfe";
  if (path.empty() && !read_file("../test/hw/fixtures/real.hfe").empty())
    path = "../test/hw/fixtures/real.hfe";
  if (path.empty())
    GTEST_SKIP() << "no real HFE fixture (set KONCEPCJA_REAL_HFE=<path> to "
                    "an HFE v1 dump of a CPC disc)";

  const std::vector<uint8_t> hfe = read_file(path.c_str());
  ASSERT_FALSE(hfe.empty()) << "could not read " << path;

  std::vector<uint8_t> scp;
  ASSERT_EQ(hfe_to_scp(hfe.data(), hfe.size(), scp), 0)
      << "hfe_to_scp rejected " << path;
  EXPECT_EQ(flux_scp_probe(scp.data(), scp.size()), 1);

  std::vector<uint8_t> dsk(4u << 20);
  const long n = flux_scp_to_dsk(scp.data(), scp.size(), dsk.data(),
                                 dsk.size(), nullptr);
  EXPECT_GT(n, 0x100) << "flux decode returned " << n;
}
