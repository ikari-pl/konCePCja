/* a2r_test.cpp — the Applesauce A2R3 → SCP transcoder (src/hw/a2r). A
 * hand-built minimal A2R with one side-0 timing capture is transcoded, then the
 * emitted SCP is inspected byte-for-byte: correct header (resolution 4 = 125
 * ns/tick, 2 revs), a TLUT slot pointing at a "TRK" data header, and flux
 * intervals that round-trip through A2R's accumulated-byte encoding into SCP
 * big-endian words — including a 0xFF-carry interval. The real-capture decode
 * is exercised by the flux harness in fdc_test.cpp (skip-if-absent). */
#include "hw/a2r.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>  // std::memcmp — libstdc++ does not provide it transitively
#include <vector>

namespace {

void put32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(static_cast<uint8_t>(x));
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x >> 16));
  v.push_back(static_cast<uint8_t>(x >> 24));
}
uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t rd16be(const uint8_t* p) {
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

// A2R3 with one RWCP timing capture at location 0 (side 0), one index, and the
// given accumulated-byte flux.
std::vector<uint8_t> make_a2r(uint32_t index_ticks,
                              const std::vector<uint8_t>& flux) {
  std::vector<uint8_t> rwcp;
  rwcp.push_back(1);               // version
  put32(rwcp, 125000);             // resolution: 125000 ps = 125 ns/tick
  rwcp.insert(rwcp.end(), 11, 0);  // reserved
  rwcp.push_back('C');             // capture mark
  rwcp.push_back(1);               // type: timing
  rwcp.push_back(0);               // location lo (0 = cyl 0, side 0)
  rwcp.push_back(0);               // location hi
  rwcp.push_back(1);               // num_index
  put32(rwcp, index_ticks);        // index[0]
  put32(rwcp, static_cast<uint32_t>(flux.size()));  // data_len
  rwcp.insert(rwcp.end(), flux.begin(), flux.end());
  rwcp.push_back('X');  // terminator

  std::vector<uint8_t> a2r = {'A', '2', 'R', '3', 0xFF, 0x0A, 0x0D, 0x0A};
  const char* info = "INFO";
  a2r.insert(a2r.end(), info, info + 4);
  put32(a2r, 37);
  a2r.push_back(1);                // INFO version
  a2r.insert(a2r.end(), 32, ' ');  // creator
  a2r.push_back(2);                // drive type: 3.5 DS 80trk
  a2r.push_back(0);                // write protected
  a2r.push_back(1);                // synchronized
  a2r.push_back(0);                // hard sector count (INFO body = 37 bytes)
  const char* rw = "RWCP";
  a2r.insert(a2r.end(), rw, rw + 4);
  put32(a2r, static_cast<uint32_t>(rwcp.size()));
  a2r.insert(a2r.end(), rwcp.begin(), rwcp.end());
  return a2r;
}

}  // namespace

TEST(A2r, RejectsNonA2r) {
  std::vector<uint8_t> out;
  const std::vector<uint8_t> junk(64, 0x00);
  EXPECT_EQ(a2r_to_scp(junk.data(), junk.size(), out), A2R_E_NOT_A2R);
  EXPECT_TRUE(out.empty());
}

TEST(A2r, TranscodesFluxIntoAWellFormedScp) {
  // Intervals 16, 24, 32 ticks, then 300 (= 255 + 45, a 0xFF carry in A2R).
  const std::vector<uint8_t> flux = {16, 24, 32, 0xFF, 45};
  const std::vector<uint8_t> a2r = make_a2r(/*index_ticks=*/1000, flux);
  std::vector<uint8_t> scp;
  ASSERT_EQ(a2r_to_scp(a2r.data(), a2r.size(), scp), 0);

  // Header: SCP magic, 2 revolutions, 125 ns/tick (resolution byte 4).
  ASSERT_GE(scp.size(), 0x2B0u);
  EXPECT_EQ(std::memcmp(scp.data(), "SCP", 3), 0);
  EXPECT_EQ(scp[0x05], 2) << "two revolutions per track";
  EXPECT_EQ(scp[0x0B], 4)
      << "resolution 4 => 25*(4+1) = 125 ns/tick, matches A2R";

  // Track-lookup slot 0 (cyl 0, side 0) points at a "TRK" data header.
  const uint32_t toff = rd32(scp.data() + 0x10);
  ASSERT_NE(toff, 0u);
  ASSERT_LE(static_cast<size_t>(toff) + 4, scp.size());
  EXPECT_EQ(std::memcmp(scp.data() + toff, "TRK", 3), 0);
  EXPECT_EQ(scp[toff + 3], 0) << "track number 0";

  // Revolution-0 entry: [index_time, words, data_offset]. Four transitions
  // (16, 24, 32, 300) — no SCP overflow word needed (all < 65536).
  const uint32_t index_time = rd32(scp.data() + toff + 4);
  const uint32_t words = rd32(scp.data() + toff + 8);
  const uint32_t doff = rd32(scp.data() + toff + 12);
  EXPECT_EQ(index_time, 1000u)
      << "revolution length carried from the A2R index";
  EXPECT_EQ(words, 4u);

  const uint8_t* fw = scp.data() + toff + doff;
  ASSERT_LE(static_cast<size_t>(toff) + doff + 2u * words, scp.size());
  EXPECT_EQ(rd16be(fw + 0), 16u);
  EXPECT_EQ(rd16be(fw + 2), 24u);
  EXPECT_EQ(rd16be(fw + 4), 32u);
  EXPECT_EQ(rd16be(fw + 6), 300u) << "255 + 45 accumulated into one interval";

  // Only one capture was supplied, so revolution 1 duplicates revolution 0.
  EXPECT_EQ(rd32(scp.data() + toff + 4 + 12 + 4), words) << "rev1 mirrors rev0";
}
