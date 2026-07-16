/* hfe_write_test.cpp — the HFE v1 WRITE-back (src/hfe_write). Proves the
 * byte-exact inverse property against the hfe_to_scp decoder, plus a
 * disk-level New-flux-disk sanity decode. All inputs synthetic. */
#include "hfe_write.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "flux_encode_util.h"
#include "hfe.h"
#include "hw/flux.h"
#include "ipf.h"
#include "mfm_encode.h"

namespace {

// A one-track single-revolution bitcell capture with `nsec` sectors.
t_mfm_track make_track(int track, int nsec) {
  MfmTrackDesc desc;
  for (int sector = 0; sector < nsec; sector++) {
    MfmSector sec;
    sec.cyl = static_cast<uint8_t>(track);
    sec.sec_id = static_cast<uint8_t>(0xC1 + sector);
    sec.size_code = 2;
    sec.payload = fluxtest::make_payload(track, sector, 512);
    desc.sectors.push_back(std::move(sec));
  }
  t_mfm_track trk;
  trk.push_back(mfm_encode_track(desc));
  return trk;
}

}  // namespace

TEST(HfeWrite, RoundTripsThroughDecoderBitForBit) {
  std::vector<t_mfm_track> cyls;
  cyls.push_back(make_track(0, 4));
  cyls.push_back(make_track(1, 3));

  std::vector<uint8_t> hfe;
  ASSERT_EQ(hfe_from_mfm_tracks(cyls, hfe), 0);
  EXPECT_EQ(std::memcmp(hfe.data(), "HXCPICFE", 8), 0);

  std::vector<uint8_t> scp_via_hfe;
  ASSERT_EQ(hfe_to_scp(hfe.data(), hfe.size(), scp_via_hfe), 0);

  const std::vector<uint8_t> scp_direct = scp_from_mfm_tracks(cyls);
  ASSERT_FALSE(scp_direct.empty());
  EXPECT_EQ(scp_via_hfe, scp_direct)
      << "hfe_to_scp(hfe_from_mfm_tracks(x)) must equal scp_from_mfm_tracks(x)";
}

TEST(HfeWrite, RoundTripsWithAnAbsentMiddleCylinder) {
  std::vector<t_mfm_track> cyls;
  cyls.push_back(make_track(0, 2));
  cyls.emplace_back();  // cylinder 1 unformatted -> LUT {0,0}
  cyls.push_back(make_track(2, 2));

  std::vector<uint8_t> hfe;
  ASSERT_EQ(hfe_from_mfm_tracks(cyls, hfe), 0);
  std::vector<uint8_t> scp_via_hfe;
  ASSERT_EQ(hfe_to_scp(hfe.data(), hfe.size(), scp_via_hfe), 0);
  EXPECT_EQ(scp_via_hfe, scp_from_mfm_tracks(cyls));
  EXPECT_EQ(flux_scp_cylinders(scp_via_hfe.data(), scp_via_hfe.size()), 3);
}

TEST(HfeWrite, RejectsEmptyAndAllUnformatted) {
  std::vector<uint8_t> out;
  EXPECT_EQ(hfe_from_mfm_tracks({}, out), HFE_E_GEOMETRY);
  EXPECT_TRUE(out.empty());

  const std::vector<t_mfm_track> empties(2);  // two unformatted cylinders
  EXPECT_EQ(hfe_from_mfm_tracks(empties, out), HFE_E_GEOMETRY);
  EXPECT_TRUE(out.empty());
}

TEST(HfeWrite, RejectsTooManyCylinders) {
  std::vector<t_mfm_track> cyls(85);  // > 84 side-0 slots
  cyls[0] = make_track(0, 1);
  std::vector<uint8_t> out;
  EXPECT_EQ(hfe_from_mfm_tracks(cyls, out), HFE_E_UNSUPPORTED);
  EXPECT_TRUE(out.empty());
}

TEST(HfeWrite, DiskLevelNewDiscDecodesBackToDskSectors) {
  std::vector<std::vector<fluxtest::SectorSpec>> tracks;
  for (int track = 0; track < 2; track++) {
    std::vector<fluxtest::SectorSpec> sectors;
    for (int sector = 0; sector < 4; sector++) {
      fluxtest::SectorSpec spec;
      spec.chrn[0] = static_cast<uint8_t>(track);
      spec.chrn[2] = static_cast<uint8_t>(0xC1 + sector);
      spec.chrn[3] = 2;
      spec.payload = fluxtest::make_payload(track, sector, 512);
      sectors.push_back(std::move(spec));
    }
    tracks.push_back(std::move(sectors));
  }
  const std::vector<uint8_t> dsk = fluxtest::build_standard_dsk(tracks);

  std::vector<uint8_t> hfe;
  ASSERT_EQ(hfe_from_disk(nullptr, 0, dsk.data(), dsk.size(), nullptr, 0, hfe),
            0);

  std::vector<uint8_t> scp;
  ASSERT_EQ(hfe_to_scp(hfe.data(), hfe.size(), scp), 0);
  std::vector<uint8_t> dsk_out(1u << 20, 0);
  const long size = flux_scp_to_dsk(scp.data(), scp.size(), dsk_out.data(),
                                    dsk_out.size(), nullptr);
  ASSERT_GT(size, 0x100);

  const auto original = fluxtest::read_dsk(dsk.data(), dsk.size());
  const auto restored =
      fluxtest::read_dsk(dsk_out.data(), static_cast<std::size_t>(size));
  ASSERT_EQ(restored.size(), original.size());
  for (std::size_t track = 0; track < original.size(); track++) {
    ASSERT_EQ(restored[track].size(), original[track].size());
    for (std::size_t sector = 0; sector < original[track].size(); sector++)
      EXPECT_EQ(restored[track][sector].payload,
                original[track][sector].payload)
          << "track " << track << " sector " << sector;
  }
}
