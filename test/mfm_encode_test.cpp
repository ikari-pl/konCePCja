/* mfm_encode_test.cpp — the IBM System 34 MFM synthesizer (src/mfm_encode).
 * Cell-level unit assertions (A1 sync pattern, byte cell expansion, CRC-CCITT
 * vectors) plus faithfulness round-trips through the flux decoder: what this
 * module writes, src/hw/flux.cpp reads back byte-exact. All inputs synthetic.
 */
#include "mfm_encode.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "flux_encode_util.h"
#include "hw/flux.h"
#include "ipf.h"

// ------------------------------------------------------- cell-level units ----

TEST(MfmEncodeCells, AoneSyncPatternIsExactly4489) {
  EXPECT_EQ(kMfmA1SyncCells, 0x4489u);
  // Sampling the data half-cells of 0x4489 must recover 0xA1.
  uint8_t data = 0;
  for (int k = 0; k < 8; k++) {
    const int bit = (kMfmA1SyncCells >> (15 - ((2 * k) + 1))) & 1;
    data = static_cast<uint8_t>((data << 1) | bit);
  }
  EXPECT_EQ(data, 0xA1u) << "the A1 mark's data half-cells decode to 0xA1";
}

TEST(MfmEncodeCells, ByteExpansionAllOnesAndAllZeros) {
  int prev = 0;
  // 0xFF: every data bit 1 -> clock always 0 -> cells 0101...0101 = 0x5555.
  EXPECT_EQ(mfm_expand_byte(0xFF, prev), 0x5555u);
  EXPECT_EQ(prev, 1);
  // 0x00 following a 1 data bit: first clock 0 (prev=1), rest 1 ->
  // 0010101010101010? prev=1: bit0 clock=(prev0&&cur0)? no ->0,data0; then
  // prev=0: clock=1,data0...
  prev = 1;
  EXPECT_EQ(mfm_expand_byte(0x00, prev), 0x2AAAu)
      << "leading clock suppressed by the preceding 1 data bit";
  EXPECT_EQ(prev, 0);
  // 0x00 following a 0 data bit: every clock 1 -> 1010...1010 = 0xAAAA.
  prev = 0;
  EXPECT_EQ(mfm_expand_byte(0x00, prev), 0xAAAAu);
}

TEST(MfmEncodeCells, DataHalfCellsAlwaysRecoverTheByte) {
  int prev = 0;
  for (int value = 0; value < 256; value++) {
    int local_prev = prev;
    const uint16_t cells =
        mfm_expand_byte(static_cast<uint8_t>(value), local_prev);
    uint8_t decoded = 0;
    for (int k = 0; k < 8; k++)
      decoded = static_cast<uint8_t>((decoded << 1) |
                                     ((cells >> (15 - ((2 * k) + 1))) & 1));
    EXPECT_EQ(decoded, value) << "value " << value;
    prev = local_prev;
  }
}

TEST(MfmEncodeCells, CrcCcittKnownVectors) {
  const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  EXPECT_EQ(mfm_crc_ccitt(check, sizeof(check)), 0x29B1u)
      << "canonical CRC-CCITT/FALSE check value";
  const uint8_t empty[1] = {0};
  EXPECT_EQ(mfm_crc_ccitt(empty, 0), 0xFFFFu) << "init value over no data";
  // A CRC over the three A1 sync bytes + IDAM 0xFE matches the decoder's
  // crc_sync_preset()-then-0xFE chain (implicitly proven by the round-trips).
  const uint8_t idam_pre[] = {0xA1, 0xA1, 0xA1, 0xFE};
  EXPECT_NE(mfm_crc_ccitt(idam_pre, sizeof(idam_pre)), 0u);
}

// ------------------------------------------------- decode faithfulness -------

TEST(MfmEncode, SingleTrackRoundTripsThroughFluxDecoder) {
  MfmTrackDesc desc;
  for (int sector = 1; sector <= 4; sector++) {
    MfmSector sec;
    sec.cyl = 5;
    sec.head = 0;
    sec.sec_id = static_cast<uint8_t>(0xC0 + sector);  // typical CPC ids
    sec.size_code = 2;
    sec.payload = fluxtest::make_payload(0, sector, 512);
    desc.sectors.push_back(std::move(sec));
  }

  const t_mfm_rev rev = mfm_encode_track(desc);
  ASSERT_GT(rev.nbits, 0u);

  std::vector<t_mfm_track> cyls(1);
  cyls[0].push_back(rev);
  const std::vector<uint8_t> scp = scp_from_mfm_tracks(cyls);
  ASSERT_FALSE(scp.empty());
  ASSERT_EQ(flux_scp_probe(scp.data(), scp.size()), 1);

  FluxTrack track;
  std::vector<uint8_t> payload(64u * 1024u, 0);
  ASSERT_EQ(flux_decode_track_rev(scp.data(), scp.size(), 0, 0, &track,
                                  payload.data(), payload.size()),
            0);
  ASSERT_EQ(track.count, 4);
  for (int i = 0; i < track.count; i++) {
    const FluxSector& sec = track.sec[i];
    EXPECT_EQ(sec.chrn[0], 5u);
    EXPECT_EQ(sec.chrn[1], 0u);
    EXPECT_EQ(sec.chrn[2], static_cast<uint8_t>(0xC0 + (i + 1)));
    EXPECT_EQ(sec.chrn[3], 2u);
    EXPECT_EQ(sec.st1, 0u) << "no data error: CRC valid";
    ASSERT_EQ(sec.len, 512u);
    const std::vector<uint8_t> expect = fluxtest::make_payload(0, i + 1, 512);
    EXPECT_EQ(std::memcmp(payload.data() + sec.off, expect.data(), 512), 0)
        << "sector " << i << " payload";
  }
}

TEST(MfmEncode, DeletedDataMarkSurfacesAsControlMark) {
  MfmTrackDesc desc;
  MfmSector sec;
  sec.sec_id = 0x41;
  sec.size_code = 1;  // 256 bytes
  sec.dam = 0xF8;     // deleted data
  sec.payload = fluxtest::make_payload(0, 1, 256);
  desc.sectors.push_back(std::move(sec));

  const t_mfm_rev rev = mfm_encode_track(desc);
  std::vector<t_mfm_track> cyls(1);
  cyls[0].push_back(rev);
  const std::vector<uint8_t> scp = scp_from_mfm_tracks(cyls);

  FluxTrack track;
  std::vector<uint8_t> payload(8192, 0);
  ASSERT_EQ(flux_decode_track_rev(scp.data(), scp.size(), 0, 0, &track,
                                  payload.data(), payload.size()),
            0);
  ASSERT_EQ(track.count, 1);
  EXPECT_EQ(track.sec[0].chrn[2], 0x41u);
  EXPECT_EQ(track.sec[0].st2 & 0x40, 0x40) << "0xF8 -> ST2 Control Mark";
}

// -------------------------------------------------- whole-DSK round trip -----

TEST(MfmEncode, StandardDskRoundTripsSectorForSector) {
  std::vector<std::vector<fluxtest::SectorSpec>> tracks;
  for (int track = 0; track < 3; track++) {
    std::vector<fluxtest::SectorSpec> sectors;
    for (int sector = 0; sector < 5; sector++) {
      fluxtest::SectorSpec spec;
      spec.chrn[0] = static_cast<uint8_t>(track);
      spec.chrn[1] = 0;
      spec.chrn[2] = static_cast<uint8_t>(0xC1 + sector);
      spec.chrn[3] = 2;
      spec.payload = fluxtest::make_payload(track, sector, 512);
      sectors.push_back(std::move(spec));
    }
    tracks.push_back(std::move(sectors));
  }
  const std::vector<uint8_t> dsk = fluxtest::build_standard_dsk(tracks);

  const std::vector<t_mfm_track> cyls =
      mfm_tracks_from_dsk(dsk.data(), dsk.size());
  ASSERT_EQ(cyls.size(), 3u);
  const std::vector<uint8_t> scp = scp_from_mfm_tracks(cyls);
  ASSERT_FALSE(scp.empty());

  std::vector<uint8_t> dsk_out(1u << 20, 0);
  const long size = flux_scp_to_dsk(scp.data(), scp.size(), dsk_out.data(),
                                    dsk_out.size(), nullptr);
  ASSERT_GT(size, 0x100);

  const auto original = fluxtest::read_dsk(dsk.data(), dsk.size());
  const auto restored =
      fluxtest::read_dsk(dsk_out.data(), static_cast<std::size_t>(size));
  ASSERT_EQ(restored.size(), original.size());
  for (std::size_t track = 0; track < original.size(); track++) {
    ASSERT_EQ(restored[track].size(), original[track].size())
        << "track " << track << " sector count";
    for (std::size_t sector = 0; sector < original[track].size(); sector++) {
      EXPECT_EQ(std::memcmp(restored[track][sector].chrn,
                            original[track][sector].chrn, 4),
                0)
          << "track " << track << " sector " << sector << " CHRN";
      EXPECT_EQ(restored[track][sector].payload,
                original[track][sector].payload)
          << "track " << track << " sector " << sector << " payload";
    }
  }
}
