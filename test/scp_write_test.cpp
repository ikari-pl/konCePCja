/* scp_write_test.cpp — the SCP flux WRITE-back (src/scp_write). Verbatim copy
 * of clean tracks vs re-synthesis of dirty tracks, and the New-flux-disk path.
 * All inputs synthetic; every check decodes the output back through
 * src/hw/flux.cpp. */
#include "scp_write.h"

#include <gtest/gtest.h>

#include <vector>

#include "flux_encode_util.h"
#include "hw/flux.h"
#include "ipf.h"
#include "mfm_encode.h"

namespace {

// A 3-track single-sided DSK; each sector's payload is salted so a re-synth
// from a different DSK is distinguishable from a verbatim flux copy.
std::vector<uint8_t> make_dsk(uint8_t salt) {
  std::vector<std::vector<fluxtest::SectorSpec>> tracks;
  for (int track = 0; track < 3; track++) {
    std::vector<fluxtest::SectorSpec> sectors;
    for (int sector = 0; sector < 4; sector++) {
      fluxtest::SectorSpec spec;
      spec.chrn[0] = static_cast<uint8_t>(track);
      spec.chrn[2] = static_cast<uint8_t>(0xC1 + sector);
      spec.chrn[3] = 2;
      spec.payload = fluxtest::make_payload(track, sector, 512, salt);
      sectors.push_back(std::move(spec));
    }
    tracks.push_back(std::move(sectors));
  }
  return fluxtest::build_standard_dsk(tracks);
}

std::vector<uint8_t> scp_of(const std::vector<uint8_t>& dsk) {
  return scp_from_mfm_tracks(mfm_tracks_from_dsk(dsk.data(), dsk.size()));
}

// Decode the whole output SCP to a DSK and read its sectors.
std::vector<std::vector<fluxtest::DecodedSector>> decode_scp(
    const std::vector<uint8_t>& scp) {
  std::vector<uint8_t> dsk(1u << 20, 0);
  const long size = flux_scp_to_dsk(scp.data(), scp.size(), dsk.data(),
                                    dsk.size(), nullptr);
  if (size <= 0x100) return {};
  return fluxtest::read_dsk(dsk.data(), static_cast<std::size_t>(size));
}

}  // namespace

TEST(ScpWrite, NoDirtyTrackCopiesOriginalByteForByte) {
  const std::vector<uint8_t> dsk = make_dsk(0);
  const std::vector<uint8_t> orig = scp_of(dsk);
  ASSERT_FALSE(orig.empty());

  const bool dirty[3] = {false, false, false};
  const std::vector<uint8_t> out =
      scp_from_disk(orig.data(), orig.size(), dsk.data(), dsk.size(), dirty, 3);
  EXPECT_EQ(out, orig) << "nothing dirty + original present -> exact copy";
}

TEST(ScpWrite, NewFluxDiscSynthesizesEveryTrack) {
  const std::vector<uint8_t> dsk = make_dsk(7);

  const std::vector<uint8_t> out =
      scp_from_disk(nullptr, 0, dsk.data(), dsk.size(), nullptr, 0);
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(flux_scp_probe(out.data(), out.size()), 1);
  EXPECT_EQ(flux_scp_cylinders(out.data(), out.size()), 3);

  const auto restored = decode_scp(out);
  const auto original = fluxtest::read_dsk(dsk.data(), dsk.size());
  ASSERT_EQ(restored.size(), original.size());
  for (std::size_t track = 0; track < original.size(); track++) {
    ASSERT_EQ(restored[track].size(), original[track].size());
    for (std::size_t sector = 0; sector < original[track].size(); sector++)
      EXPECT_EQ(restored[track][sector].payload, original[track][sector].payload)
          << "track " << track << " sector " << sector;
  }
}

TEST(ScpWrite, DirtyTrackResynthesizedCleanTrackPreservedVerbatim) {
  // Original flux holds DSK-A. The DSK handed to the writer is DSK-B (every
  // payload changed). Only track 1 is marked dirty: it must read back DSK-B's
  // sectors, while clean tracks 0 and 2 must still read back DSK-A's — proving
  // clean tracks are copied from the original flux, not re-synthesized.
  const std::vector<uint8_t> dsk_a = make_dsk(0);
  const std::vector<uint8_t> dsk_b = make_dsk(0x55);
  const std::vector<uint8_t> orig = scp_of(dsk_a);
  ASSERT_FALSE(orig.empty());

  const bool dirty[3] = {false, true, false};
  const std::vector<uint8_t> out = scp_from_disk(
      orig.data(), orig.size(), dsk_b.data(), dsk_b.size(), dirty, 3);
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(flux_scp_probe(out.data(), out.size()), 1);
  EXPECT_EQ(flux_scp_cylinders(out.data(), out.size()), 3);

  const auto restored = decode_scp(out);
  const auto from_a = fluxtest::read_dsk(dsk_a.data(), dsk_a.size());
  const auto from_b = fluxtest::read_dsk(dsk_b.data(), dsk_b.size());
  ASSERT_EQ(restored.size(), 3u);

  for (std::size_t track = 0; track < 3; track++) {
    const auto& expect = (track == 1) ? from_b[track] : from_a[track];
    ASSERT_EQ(restored[track].size(), expect.size()) << "track " << track;
    for (std::size_t sector = 0; sector < expect.size(); sector++)
      EXPECT_EQ(restored[track][sector].payload, expect[sector].payload)
          << (track == 1 ? "dirty track must be DSK-B " : "clean track must be DSK-A ")
          << "track " << track << " sector " << sector;
  }
  // And the two clean tracks must differ from what a DSK-B re-synth would give.
  EXPECT_NE(restored[0][0].payload, from_b[0][0].payload)
      << "clean track 0 is the original flux, not the modified DSK";
}
