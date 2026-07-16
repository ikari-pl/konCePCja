/* flux_ingest_test.cpp — the unified content-sniffing flux dispatcher
 * (src/flux_ingest). Exercises flux::sniff container identification (magic
 * wins, the ".raw" IPF-vs-KryoFlux collision, garbage → Unknown) and
 * flux::to_scp end-to-end transcode/pass-through for each supported container,
 * asserting the emitted bytes satisfy the real flux decoder's probe. Inputs
 * are synthetic (the same minimal headers the per-format tests build), so
 * every case runs in CI with no external capture files. */
#include "flux_ingest.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "hw/flux.h"  // flux_scp_probe — proves the emitted SCP is well-formed
}

namespace {

void put_u16le(std::vector<uint8_t>& v, size_t at, uint16_t x) {
  v[at] = static_cast<uint8_t>(x & 0xFF);
  v[at + 1] = static_cast<uint8_t>((x >> 8) & 0xFF);
}

// ---- HFE v1 (magic "HXCPICFE") -------------------------------------------
// A minimal single-track CPC-DD HFE image: 512-byte header + 512-byte LUT +
// one 512-byte data block. Track 0's side-0 byte 0 = 0x01 (one transition),
// side-1 = 0xFF filler (de-interleaved out). Mirrors hfe_test.cpp.
std::vector<uint8_t> make_hfe() {
  std::vector<uint8_t> h(512, 0xFF);
  std::memcpy(h.data(), "HXCPICFE", 8);
  h[0x08] = 0;     // format_revision (v1)
  h[0x09] = 1;     // number_of_track
  h[0x0A] = 2;     // number_of_side
  h[0x0B] = 0x00;  // track_encoding
  put_u16le(h, 0x0C, 250);  // bitRate: CPC DD (250 kbit/s -> 2 us cells)
  put_u16le(h, 0x0E, 300);  // floppyRPM
  h[0x10] = 0x06;           // CPC_DD_FLOPPYMODE
  put_u16le(h, 0x12, 1);    // track_list_offset (block 1)

  std::vector<uint8_t> img = std::move(h);
  img.resize(1024, 0);            // header block (0) + LUT block (1)
  put_u16le(img, 512, 2);         // track 0 data at block 2 (byte 1024)
  put_u16le(img, 514, 512);       // track 0 length: one 512-byte block
  img.resize(1024 + 512, 0);
  img[1024] = 0x01;               // side-0 byte 0
  std::fill(img.begin() + 1024 + 256, img.end(), 0xFF);  // side-1 filler
  return img;
}

// ---- KryoFlux STREAM (no magic — identified by the ".raw" ext) -----------
// Injects a KFInfo sck=40000000 (identity clock), four flux bytes, and three
// index pulses -> two revolutions. Mirrors kryoflux_stream_test.cpp.
std::vector<uint8_t> make_kryoflux_stream() {
  std::vector<uint8_t> s;
  auto put_kfinfo = [&](const std::string& body) {
    s.push_back(0x0D);
    s.push_back(0x04);
    s.push_back(static_cast<uint8_t>(body.size() & 0xFF));
    s.push_back(static_cast<uint8_t>((body.size() >> 8) & 0xFF));
    for (char c : body) s.push_back(static_cast<uint8_t>(c));
  };
  auto put_index = [&](uint32_t spos) {
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
    put32(spos);
    put32(0);  // SampleCounter
    put32(0);  // IndexCounter
  };
  put_kfinfo("sck=40000000");
  for (int i = 0; i < 4; i++) s.push_back(0x40);
  put_index(0);
  put_index(2);  // rev0 = flux[0..1]
  put_index(4);  // rev1 = flux[2..3]
  s.push_back(0x0D);  // EOF OOB
  s.push_back(0x0D);
  s.push_back(0x0D);
  s.push_back(0x0D);
  return s;
}

// ---- A2R3 (magic "A2R3") -------------------------------------------------
// One side-0 timing capture with a handful of flux intervals. Mirrors
// a2r_test.cpp.
std::vector<uint8_t> make_a2r() {
  auto put32 = [](std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 24));
  };
  const std::vector<uint8_t> flux = {16, 24, 32, 0xFF, 45};
  std::vector<uint8_t> rwcp;
  rwcp.push_back(1);             // version
  put32(rwcp, 125000);          // resolution: 125 ns/tick
  rwcp.insert(rwcp.end(), 11, 0);
  rwcp.push_back('C');          // capture mark
  rwcp.push_back(1);            // type: timing
  rwcp.push_back(0);            // location lo (cyl 0, side 0)
  rwcp.push_back(0);            // location hi
  rwcp.push_back(1);            // num_index
  put32(rwcp, 1000);           // index[0]
  put32(rwcp, static_cast<uint32_t>(flux.size()));
  rwcp.insert(rwcp.end(), flux.begin(), flux.end());
  rwcp.push_back('X');          // terminator

  std::vector<uint8_t> a2r = {'A', '2', 'R', '3', 0xFF, 0x0A, 0x0D, 0x0A};
  const char* info = "INFO";
  a2r.insert(a2r.end(), info, info + 4);
  put32(a2r, 37);
  a2r.push_back(1);                    // INFO version
  a2r.insert(a2r.end(), 32, ' ');      // creator
  a2r.push_back(2);                    // drive type
  a2r.push_back(0);                    // write protected
  a2r.push_back(1);                    // synchronized
  a2r.push_back(0);                    // hard sector count
  const char* rw = "RWCP";
  a2r.insert(a2r.end(), rw, rw + 4);
  put32(a2r, static_cast<uint32_t>(rwcp.size()));
  a2r.insert(a2r.end(), rwcp.begin(), rwcp.end());
  return a2r;
}

// A valid SCP image, produced by round-tripping the HFE transcoder through the
// dispatcher itself (the transcoder is proven separately in hfe_test.cpp).
std::vector<uint8_t> make_scp() {
  const std::vector<uint8_t> hfe = make_hfe();
  return flux::to_scp(hfe.data(), hfe.size(), ".hfe");
}

}  // namespace

// ===========================================================================
// sniff — magic identifies each container.
// ===========================================================================
TEST(FluxIngestSniff, IdentifiesEachContainerByMagic) {
  const std::vector<uint8_t> hfe = make_hfe();
  const std::vector<uint8_t> a2r = make_a2r();
  const std::vector<uint8_t> scp = make_scp();
  ASSERT_FALSE(scp.empty()) << "HFE->SCP setup failed";

  EXPECT_EQ(flux::sniff(hfe.data(), hfe.size(), ".hfe"), flux::Container::Hfe);
  EXPECT_EQ(flux::sniff(a2r.data(), a2r.size(), ".a2r"), flux::Container::A2R);
  EXPECT_EQ(flux::sniff(scp.data(), scp.size(), ".scp"), flux::Container::Scp);

  // The ext hint is advisory only — magic still wins even with a wrong ext.
  EXPECT_EQ(flux::sniff(hfe.data(), hfe.size(), ".dsk"), flux::Container::Hfe);

  // HFE v3 magic also classifies as Hfe (to_scp then rejects it).
  std::vector<uint8_t> hfe3 = hfe;
  std::memcpy(hfe3.data(), "HXCHFEV3", 8);
  EXPECT_EQ(flux::sniff(hfe3.data(), hfe3.size(), ".hfe"),
            flux::Container::Hfe);

  // A2R2 magic classifies as A2R.
  std::vector<uint8_t> a2r2 = a2r;
  a2r2[3] = '2';
  EXPECT_EQ(flux::sniff(a2r2.data(), a2r2.size(), ".a2r"),
            flux::Container::A2R);
}

TEST(FluxIngestSniff, CapsMagicIsIpf) {
  const uint8_t caps[8] = {'C', 'A', 'P', 'S', 0, 0, 0, 12};
  EXPECT_EQ(flux::sniff(caps, sizeof(caps), ".ipf"), flux::Container::Ipf);
}

// ===========================================================================
// sniff — the ".raw" collision: magic wins, magic-less falls to KryoFlux.
// ===========================================================================
TEST(FluxIngestSniff, RawExtensionCollisionResolvedByMagic) {
  // "CAPS" magic + ".raw" ext -> Ipf (an IPF CTRAW dump), NOT KryoFlux.
  const uint8_t caps_raw[8] = {'C', 'A', 'P', 'S', 0, 0, 0, 12};
  EXPECT_EQ(flux::sniff(caps_raw, sizeof(caps_raw), ".raw"),
            flux::Container::Ipf);

  // No known magic + ".raw" -> KryoFluxStream (the real thing has no magic).
  const std::vector<uint8_t> stream = make_kryoflux_stream();
  EXPECT_EQ(flux::sniff(stream.data(), stream.size(), ".raw"),
            flux::Container::KryoFluxStream);

  // Any magic-less bytes with ".raw" resolve to KryoFluxStream by the tie rule.
  const uint8_t junk[16] = {0};
  EXPECT_EQ(flux::sniff(junk, sizeof(junk), ".raw"),
            flux::Container::KryoFluxStream);
}

TEST(FluxIngestSniff, GarbageAndEmptyAreUnknown) {
  const uint8_t junk[16] = {0x11, 0x22, 0x33, 0x44};
  EXPECT_EQ(flux::sniff(junk, sizeof(junk), ""), flux::Container::Unknown);
  EXPECT_EQ(flux::sniff(junk, sizeof(junk), ".dsk"), flux::Container::Unknown);
  EXPECT_EQ(flux::sniff(nullptr, 0, ".scp"), flux::Container::Unknown);
  const uint8_t empty[1] = {0};
  EXPECT_EQ(flux::sniff(empty, 0, ".raw"), flux::Container::Unknown);
}

// ===========================================================================
// to_scp — each transcoder/pass-through emits a well-formed SCP.
// ===========================================================================
TEST(FluxIngest, ScpPassThroughReturnsBytesVerbatim) {
  const std::vector<uint8_t> scp = make_scp();
  ASSERT_FALSE(scp.empty());
  ASSERT_EQ(flux_scp_probe(scp.data(), scp.size()), 1);

  const std::vector<uint8_t> out = flux::to_scp(scp.data(), scp.size(), ".scp");
  EXPECT_EQ(out, scp) << "valid SCP must pass through byte-for-byte";
  EXPECT_EQ(flux_scp_probe(out.data(), out.size()), 1);
}

TEST(FluxIngest, HfeTranscodesToValidScp) {
  const std::vector<uint8_t> hfe = make_hfe();
  const std::vector<uint8_t> out = flux::to_scp(hfe.data(), hfe.size(), ".hfe");
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(std::memcmp(out.data(), "SCP", 3), 0);
  EXPECT_EQ(flux_scp_probe(out.data(), out.size()), 1);
}

TEST(FluxIngest, KryoFluxStreamTranscodesToValidScp) {
  const std::vector<uint8_t> stream = make_kryoflux_stream();
  // Routed purely by the ".raw" ext (the stream carries no magic).
  const std::vector<uint8_t> out =
      flux::to_scp(stream.data(), stream.size(), ".raw");
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(std::memcmp(out.data(), "SCP", 3), 0);
  EXPECT_EQ(flux_scp_probe(out.data(), out.size()), 1);
}

TEST(FluxIngest, A2rTranscodesToValidScp) {
  const std::vector<uint8_t> a2r = make_a2r();
  const std::vector<uint8_t> out = flux::to_scp(a2r.data(), a2r.size(), ".a2r");
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(std::memcmp(out.data(), "SCP", 3), 0);
  EXPECT_EQ(flux_scp_probe(out.data(), out.size()), 1);
}

// ===========================================================================
// to_scp — failure paths return an empty vector, no crash / no OOB.
// ===========================================================================
TEST(FluxIngest, GarbageAndEmptyReturnEmpty) {
  const uint8_t junk[16] = {0x11, 0x22, 0x33, 0x44};
  EXPECT_TRUE(flux::to_scp(junk, sizeof(junk), "").empty());
  EXPECT_TRUE(flux::to_scp(junk, sizeof(junk), ".dsk").empty());
  EXPECT_TRUE(flux::to_scp(nullptr, 0, ".scp").empty());
}

TEST(FluxIngest, InvalidScpIsRejected) {
  // "SCP" magic but far too short for a valid header (< 0x2B0) -> {}.
  std::vector<uint8_t> bad(16, 0);
  std::memcpy(bad.data(), "SCP", 3);
  EXPECT_TRUE(flux::to_scp(bad.data(), bad.size(), ".scp").empty());
}

TEST(FluxIngest, MalformedIpfReturnsEmpty) {
  // "CAPS" magic but no valid record structure -> ipf::Image::open fails,
  // to_scp returns {} (open() rejects it before any encoder-type check).
  const uint8_t caps[8] = {'C', 'A', 'P', 'S', 0, 0, 0, 12};
  EXPECT_TRUE(flux::to_scp(caps, sizeof(caps), ".ipf").empty());
  EXPECT_TRUE(flux::to_scp(caps, sizeof(caps), ".raw").empty());
}

TEST(FluxIngest, HfeV3IsRejected) {
  // Classified as Hfe by magic, but hfe_to_scp rejects v3 -> {}.
  std::vector<uint8_t> hfe3 = make_hfe();
  std::memcpy(hfe3.data(), "HXCHFEV3", 8);
  EXPECT_TRUE(flux::to_scp(hfe3.data(), hfe3.size(), ".hfe").empty());
}
