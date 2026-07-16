/* flux_test.cpp — the SCP flux → DSK converter (src/hw/flux). The oracle is a
 * round trip built here: an MFM ENCODER (sector map → IBM System 34 track
 * bitstream → flux intervals → SCP container) feeds flux_scp_to_dsk, and the
 * resulting DSK must be byte-exact vs the source sectors and acceptable to
 * fdc_attach_disk. Covers: clean/jittered/off-speed PLL decoding, data-CRC
 * corruption, 2-revolution weak-sector detection, 0x0000 overflow words,
 * mixed sector sizes (extended DSK), error codes, and the end-to-end boot:
 * AMSDOS catalogues a flux-sourced disc. See docs/hardware/flux-media.md. */

#include "hw/flux.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "flux_synth.h"
#include "hw/board.h"
#include "hw/crtc.h"
#include "hw/fdc.h"
#include "hw/gate_array.h"
#include "hw/memory.h"
#include "hw/ppi.h"
#include "hw/psg.h"
#include "hw/video.h"
#include "hw/z80.h"

namespace {

using namespace fluxsynth;  // the MFM/flux/SCP synthesis toolkit

// ------------------------------------------------------------ DSK checks ----

// Locate track t's Track-Info block in a converted DSK (standard or extended).
const uint8_t* dsk_track(const std::vector<uint8_t>& dsk, int t) {
  size_t off = 0x100;
  if (std::memcmp(dsk.data(), "MV - CPC", 8) == 0) {
    const size_t block = dsk[0x32] | (static_cast<size_t>(dsk[0x33]) << 8);
    off += block * static_cast<size_t>(t);
  } else {
    for (int i = 0; i < t; i++) off += static_cast<size_t>(dsk[0x34 + i]) << 8;
  }
  return dsk.data() + off;
}

// Assert the DSK's sectors are byte-exact vs the source content.
void expect_round_trip(const std::vector<uint8_t>& dsk,
                       const std::vector<std::vector<Sector>>& src) {
  ASSERT_EQ(static_cast<size_t>(dsk[0x30]), src.size());
  ASSERT_EQ(dsk[0x31], 1);
  for (size_t t = 0; t < src.size(); t++) {
    const uint8_t* th = dsk_track(dsk, static_cast<int>(t));
    ASSERT_EQ(std::memcmp(th, "Track-Info", 10), 0) << "track " << t;
    ASSERT_EQ(static_cast<size_t>(th[0x15]), src[t].size())
        << "sector count, track " << t;
    size_t data = 0x100;
    for (size_t s = 0; s < src[t].size(); s++) {
      const uint8_t* si = th + 0x18 + 8 * s;
      EXPECT_EQ(si[0], src[t][s].c);
      EXPECT_EQ(si[1], src[t][s].h);
      EXPECT_EQ(si[2], src[t][s].r);
      EXPECT_EQ(si[3], src[t][s].n);
      EXPECT_EQ(
          std::memcmp(th + data, src[t][s].data.data(), src[t][s].data.size()),
          0)
          << "payload mismatch: track " << t << " sector " << s;
      data += src[t][s].data.size();
    }
  }
}

long convert(const std::vector<uint8_t>& scp, std::vector<uint8_t>& dsk,
             FluxWeakReport* weak = nullptr) {
  dsk.assign(512 * 1024, 0);
  const long sz =
      flux_scp_to_dsk(scp.data(), scp.size(), dsk.data(), dsk.size(), weak);
  if (sz > 0) dsk.resize(static_cast<size_t>(sz));
  return sz;
}

}  // namespace

// ------------------------------------------------------------ unit tests ----

TEST(Flux, ProbeAcceptsScpRejectsGarbage) {
  const std::vector<uint8_t> scp = scp_from_sectors(amsdos_content(1));
  EXPECT_EQ(flux_scp_probe(scp.data(), scp.size()), 1);
  EXPECT_EQ(flux_scp_probe(scp.data(), 0x20), 0)
      << "shorter than the offset table";
  const std::vector<uint8_t> junk(0x400, 0xAA);
  EXPECT_EQ(flux_scp_probe(junk.data(), junk.size()), 0);
  std::vector<uint8_t> bad = scp;
  bad[0x09] = 8;  // unsupported bitcell width
  EXPECT_EQ(flux_scp_probe(bad.data(), bad.size()), 0);
  bad = scp;
  bad[0x0A] = 2;  // side-1-only dump
  EXPECT_EQ(flux_scp_probe(bad.data(), bad.size()), 0);
}

TEST(Flux, CleanRoundTripIsByteExactAndStandardDsk) {
  const std::vector<std::vector<Sector>> src = amsdos_content(3);
  const std::vector<uint8_t> scp = scp_from_sectors(src);
  std::vector<uint8_t> dsk;
  FluxWeakReport weak;
  ASSERT_GT(convert(scp, dsk, &weak), 0);
  EXPECT_EQ(weak.count, 0);
  EXPECT_EQ(std::memcmp(dsk.data(), "MV - CPC", 8), 0)
      << "uniform disc → standard DSK";
  expect_round_trip(dsk, src);
  // The unchanged FDC accepts the result.
  std::vector<uint8_t> fmem(fdc_state_size());
  Device fdev = fdc_init(fmem.data());
  EXPECT_EQ(fdc_attach_disk(&fdev, dsk.data(), dsk.size()), 0);
}

TEST(Flux, JitteredFluxStillDecodesByteExact) {
  const std::vector<std::vector<Sector>> src = amsdos_content(3);
  const std::vector<uint8_t> scp = scp_from_sectors(src, 80.0, 0.10, 0xC0FFEE);
  std::vector<uint8_t> dsk;
  FluxWeakReport weak;
  ASSERT_GT(convert(scp, dsk, &weak), 0);
  EXPECT_EQ(weak.count, 0)
      << "±10% per-interval jitter must not corrupt sectors";
  expect_round_trip(dsk, src);
}

TEST(Flux, OffSpeedSpindleStillDecodesByteExact) {
  const std::vector<std::vector<Sector>> src = amsdos_content(3);
  // +2% spindle-speed error (every cell long) plus mild jitter.
  const std::vector<uint8_t> scp =
      scp_from_sectors(src, 80.0 * 1.02, 0.05, 0xF00D);
  std::vector<uint8_t> dsk;
  FluxWeakReport weak;
  ASSERT_GT(convert(scp, dsk, &weak), 0);
  EXPECT_EQ(weak.count, 0);
  expect_round_trip(dsk, src);
}

TEST(Flux, DataCrcErrorMarksTheSector) {
  std::vector<std::vector<Sector>> src = amsdos_content(1);
  src[0][2].corrupt_data =
      true;  // sector &C3: one payload byte flipped on the wire
  const std::vector<uint8_t> scp = scp_from_sectors(src);
  std::vector<uint8_t> dsk;
  FluxWeakReport weak;
  ASSERT_GT(convert(scp, dsk, &weak), 0);
  ASSERT_EQ(weak.count, 1);
  EXPECT_EQ(weak.sec[0].cyl, 0);
  EXPECT_EQ(weak.sec[0].sector_id, 0xC3);
  EXPECT_EQ(weak.sec[0].reason, FLUX_WEAK_CRC);
  const uint8_t* th = dsk_track(dsk, 0);
  ASSERT_EQ(th[0x15], 9) << "the bad sector is kept, only marked";
  EXPECT_EQ(th[0x18 + 8 * 2 + 4], 0x20) << "ST1 Data Error";
  EXPECT_EQ(th[0x18 + 8 * 2 + 5], 0x20) << "ST2 Data Error in Data Field";
  EXPECT_EQ(th[0x18 + 8 * 1 + 4], 0x00) << "neighbours unmarked";
  // The suspect payload (with the flipped byte) is what the DSK carries.
  EXPECT_EQ(th[0x100 + 2 * 512 + 10], 0xE5 ^ 0xFF);
}

TEST(Flux, WeakSectorDetectedAcrossRevolutions) {
  const std::vector<std::vector<Sector>> src = amsdos_content(1);
  std::vector<std::vector<Sector>> rev1 = src;
  std::memset(rev1[0][4].data.data() + 100, 0x55,
              16);  // sector &C5 reads differently
  Prng rng(0xBEEF);
  const std::vector<std::vector<std::vector<uint32_t>>> tracks = {
      {bits_to_flux(track_bits(src[0]), 80.0, 0.0, &rng),
       bits_to_flux(track_bits(rev1[0]), 80.0, 0.0, &rng)}};
  const std::vector<uint8_t> scp = build_scp(tracks);
  std::vector<uint8_t> dsk;
  FluxWeakReport weak;
  ASSERT_GT(convert(scp, dsk, &weak), 0);
  ASSERT_EQ(weak.count, 1) << "exactly the one differing sector";
  EXPECT_EQ(weak.sec[0].cyl, 0);
  EXPECT_EQ(weak.sec[0].side, 0);
  EXPECT_EQ(weak.sec[0].sector_id, 0xC5);
  EXPECT_EQ(weak.sec[0].reason, FLUX_WEAK_DIFFER);
  expect_round_trip(dsk, src);  // the DSK carries the revolution-0 data
}

TEST(Flux, OverflowTickWordsDecode) {
  const std::vector<std::vector<Sector>> src = amsdos_content(1);
  Prng rng(1);
  std::vector<uint32_t> flux =
      bits_to_flux(track_bits(src[0]), 80.0, 0.0, &rng);
  // A >16-bit interval at the very start (a long unformatted splice before
  // gap 4a): the SCP encoding is a 0x0000 word + the remainder.
  flux.insert(flux.begin(), 0x10000 + 160);
  const std::vector<uint8_t> scp = build_scp({{flux}});
  ASSERT_EQ(scp[scp.size() - flux.size() * 2 - 2], 0)  // fixture sanity: a zero
      << "the overflow word must actually be present";
  std::vector<uint8_t> dsk;
  ASSERT_GT(convert(scp, dsk, nullptr), 0);
  expect_round_trip(dsk, src);
}

TEST(Flux, MixedSectorSizesEmitExtendedDsk) {
  std::vector<std::vector<Sector>> src = amsdos_content(2);
  src[1].clear();  // track 1: 4×256-byte sectors (N=1) instead
  for (int s = 0; s < 4; s++)
    src[1].push_back({1, 0, static_cast<uint8_t>(0x01 + s), 1,
                      std::vector<uint8_t>(256, static_cast<uint8_t>(0x10 + s)),
                      false});
  const std::vector<uint8_t> scp = scp_from_sectors(src);
  std::vector<uint8_t> dsk;
  ASSERT_GT(convert(scp, dsk, nullptr), 0);
  EXPECT_EQ(std::memcmp(dsk.data(), "EXTENDED", 8), 0);
  expect_round_trip(dsk, src);
  std::vector<uint8_t> fmem(fdc_state_size());
  Device fdev = fdc_init(fmem.data());
  EXPECT_EQ(fdc_attach_disk(&fdev, dsk.data(), dsk.size()), 0);
}

TEST(Flux, RejectsBrokenInputsLoudly) {
  const std::vector<uint8_t> scp = scp_from_sectors(amsdos_content(1));
  std::vector<uint8_t> dsk(512 * 1024, 0);
  const std::vector<uint8_t> junk(0x400, 0xAA);
  EXPECT_EQ(flux_scp_to_dsk(junk.data(), junk.size(), dsk.data(), dsk.size(),
                            nullptr),
            FLUX_E_NOT_SCP);
  EXPECT_EQ(flux_scp_to_dsk(scp.data(), scp.size() / 2, dsk.data(), dsk.size(),
                            nullptr),
            FLUX_E_TRUNCATED)
      << "flux data cut short";
  EXPECT_EQ(flux_scp_to_dsk(scp.data(), scp.size(), dsk.data(), 0x800, nullptr),
            FLUX_E_DSK_OVERFLOW)
      << "output capacity exhausted must be an error, not a truncated DSK";
  std::vector<uint8_t> bad = scp;
  bad[0x05] = 0;  // zero revolutions
  EXPECT_EQ(
      flux_scp_to_dsk(bad.data(), bad.size(), dsk.data(), dsk.size(), nullptr),
      FLUX_E_GEOMETRY);
}

// ---------------------------------------------------- integration: cat ------

namespace {

std::vector<uint8_t> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
}

}  // namespace

// The full pipeline on the full machine: synthesize a 40-track AMSDOS flux
// dump, convert it, attach the DSK to the FDC on the real boot board, type
// `cat` — AMSDOS must read the directory and print the catalogue (the same
// acceptance as FdcBoot.AmsdosCataloguesTheDisc).
TEST(FluxBoot, AmsdosCataloguesAFluxSourcedDisc) {
  std::vector<uint8_t> rom = read_file("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read_file("../rom/cpc6128.rom");
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";
  std::vector<uint8_t> amsdos = read_file("rom/amsdos.rom");
  if (amsdos.size() < 0x4000) amsdos = read_file("../rom/amsdos.rom");
  if (amsdos.size() < 0x4000) GTEST_SKIP() << "rom/amsdos.rom not found";

  const std::vector<std::vector<Sector>> src = amsdos_content(40);
  const std::vector<uint8_t> scp = scp_from_sectors(src, 80.0, 0.05, 0xCAFE);
  ASSERT_EQ(flux_scp_probe(scp.data(), scp.size()), 1);
  std::vector<uint8_t> dsk;
  ASSERT_GT(convert(scp, dsk, nullptr), 0);
  expect_round_trip(dsk, src);

  std::vector<uint8_t> gmem(ga_state_size());
  Device gdev = ga_init(gmem.data());
  std::vector<uint8_t> cmem(crtc_state_size());
  Device cdev = crtc_init(cmem.data());
  std::vector<uint8_t> pmem(ppi_state_size());
  Device pdev = ppi_init(pmem.data());
  std::vector<uint8_t> smem(psg_state_size());
  Device sdev = psg_init(smem.data());
  std::vector<uint8_t> mmem(mem_state_size());
  Device mdev = mem_init(mmem.data());
  std::vector<uint8_t> vmem(video_state_size());
  Device vdev = video_init(vmem.data());
  std::vector<uint8_t> zmem(z80_state_size());
  Device zdev = z80_init(zmem.data());
  std::vector<uint8_t> fmem(fdc_state_size());
  Device fdev = fdc_init(fmem.data());

  Board board;
  board_init(&board);
  board_add(&board, gdev);
  board_add(&board, cdev);
  board_add(&board, pdev);
  board_add(&board, sdev);
  board_add(&board, mdev);
  board_add(&board, vdev);
  board_add(&board, zdev);
  board_add(&board, fdev);
  board_reset(&board);

  mem_load_lower_rom(&mdev, rom.data(), 0x4000);
  mem_load_upper_rom(&mdev, rom.data() + 0x4000, 0x4000);
  std::vector<uint8_t> xmem(0x10000, 0);
  mem_attach_expansion(&mdev, xmem.data(), xmem.size());
  mem_attach_rom(&mdev, 7, amsdos.data());  // AMSDOS at the disc ROM slot

  ASSERT_EQ(fdc_attach_disk(&fdev, dsk.data(), dsk.size()), 0);

  const int w = 768, h = 272;
  std::vector<uint8_t> fb(static_cast<size_t>(w) * h * 3, 0);
  video_attach(&vdev, &gdev, fb.data(), w, h);

  VideoRegs vr{};
  auto run_frames = [&](unsigned n) {
    video_peek(&vdev, &vr);
    const uint32_t target = vr.frames + n;
    for (long tick = 0; tick < 400000L * n && vr.frames < target; ++tick) {
      board_tick(&board);
      video_peek(&vdev, &vr);
    }
  };
  auto tap = [&](uint8_t code) {  // code = row << 4 | bit
    const uint8_t row = code >> 4, bit = code & 0x0F;
    psg_set_key_row(&sdev, row, static_cast<uint8_t>(0xFF & ~(1u << bit)));
    run_frames(4);
    psg_set_key_row(&sdev, row, 0xFF);
    run_frames(4);
  };

  run_frames(120);  // to the Ready screen
  const std::vector<uint8_t> before = fb;

  tap(0x76);  // c
  tap(0x85);  // a
  tap(0x63);  // t
  tap(0x22);  // RETURN
  run_frames(
      600);  // real mechanics: 500 ms spin-up + seeks + rotation + printing

  FdcRegs f{};
  fdc_peek(&fdev, &f);
  EXPECT_GE(f.sectors_read, 4u) << "AMSDOS read the four directory sectors";
  EXPECT_EQ(f.track[0], 0) << "the directory lives on track 0";
  EXPECT_NE(before, fb) << "the screen changed: the catalogue was printed";
}

// ---- Stage 3: per-track / per-revolution decode with angular positions -----
// (The substrate for the rotating-flux FDC: docs/hardware/flux-media.md §7.)

TEST(Flux, Stage3PerRevolutionDecodeMatchesSource) {
  const std::vector<std::vector<Sector>> src = amsdos_content(2);
  const std::vector<uint8_t> scp = scp_from_sectors(src);
  EXPECT_EQ(flux_scp_revolutions(scp.data(), scp.size()), 1);
  EXPECT_EQ(flux_scp_cylinders(scp.data(), scp.size()), 2);

  FluxTrack trk;
  std::vector<uint8_t> payload(8192);
  ASSERT_EQ(flux_decode_track_rev(scp.data(), scp.size(), 0, 0, &trk,
                                  payload.data(), payload.size()),
            0);
  ASSERT_EQ(trk.count, static_cast<int>(src[0].size()));
  for (int i = 0; i < trk.count; i++) {  // scan order = encoding order
    const FluxSector& fs = trk.sec[i];
    const Sector& s = src[0][static_cast<size_t>(i)];
    EXPECT_EQ(fs.chrn[2], s.r) << "sector " << i;
    ASSERT_EQ(fs.len, s.data.size());
    EXPECT_EQ(std::memcmp(payload.data() + fs.off, s.data.data(), fs.len), 0)
        << "payload byte-exact, sector " << i;
  }
  // A cylinder past the dump: nothing under the head, and that is success.
  ASSERT_EQ(flux_decode_track_rev(scp.data(), scp.size(), 9, 0, &trk,
                                  payload.data(), payload.size()),
            0);
  EXPECT_EQ(trk.count, 0);
}

TEST(Flux, Stage3AngularPositionsMeasureTheRealLayout) {
  const std::vector<std::vector<Sector>> src = amsdos_content(1);
  const std::vector<uint8_t> scp = scp_from_sectors(src);
  FluxTrack trk;
  std::vector<uint8_t> payload(8192);
  ASSERT_EQ(flux_decode_track_rev(scp.data(), scp.size(), 0, 0, &trk,
                                  payload.data(), payload.size()),
            0);
  ASSERT_GE(trk.count, 3);
  // ID fields arrive at strictly increasing angles; each data field trails its
  // ID by the System 34 GAP2 + sync + DAM ≈ 38 byte cells — measured from the
  // real bitstream, not synthesized.
  for (int i = 0; i < trk.count; i++) {
    const FluxSector& fs = trk.sec[i];
    if (i > 0)
      EXPECT_GT(fs.idam_cell, trk.sec[i - 1].idam_cell) << "sector " << i;
    const int lead =
        static_cast<int>(fs.data_cell) - static_cast<int>(fs.idam_cell);
    EXPECT_GE(lead, 34) << "sector " << i;
    EXPECT_LE(lead, 42) << "sector " << i;
  }
  // Equal-size sectors → uniform angular pitch between consecutive ID fields.
  const int pitch = trk.sec[1].idam_cell - trk.sec[0].idam_cell;
  for (int i = 2; i < trk.count; i++)
    EXPECT_NEAR(trk.sec[i].idam_cell - trk.sec[i - 1].idam_cell, pitch, 2)
        << "sector " << i;
}

TEST(Flux, Stage3RevolutionsCarryTheirOwnData) {
  // Two captured revolutions with one sector reading differently: the physical
  // substrate of weak-bit emulation — the FDC will serve whichever revolution
  // is passing the head.
  const std::vector<std::vector<Sector>> src = amsdos_content(1);
  std::vector<std::vector<Sector>> rev1 = src;
  std::memset(rev1[0][4].data.data() + 100, 0x55, 16);  // &C5 reads differently
  Prng rng(0xBEEF);
  const std::vector<std::vector<std::vector<uint32_t>>> tracks = {
      {bits_to_flux(track_bits(src[0]), 80.0, 0.0, &rng),
       bits_to_flux(track_bits(rev1[0]), 80.0, 0.0, &rng)}};
  const std::vector<uint8_t> scp = build_scp(tracks);
  EXPECT_EQ(flux_scp_revolutions(scp.data(), scp.size()), 2);

  FluxTrack t0, t1, t2;
  std::vector<uint8_t> p0(8192), p1(8192), p2(8192);
  ASSERT_EQ(flux_decode_track_rev(scp.data(), scp.size(), 0, 0, &t0, p0.data(),
                                  p0.size()),
            0);
  ASSERT_EQ(flux_decode_track_rev(scp.data(), scp.size(), 0, 1, &t1, p1.data(),
                                  p1.size()),
            0);
  ASSERT_EQ(t0.count, t1.count);
  int differing = 0;
  for (int i = 0; i < t0.count; i++) {
    ASSERT_EQ(t0.sec[i].chrn[2], t1.sec[i].chrn[2]) << "same map, sector " << i;
    if (std::memcmp(p0.data() + t0.sec[i].off, p1.data() + t1.sec[i].off,
                    t0.sec[i].len) != 0) {
      differing++;
      EXPECT_EQ(t0.sec[i].chrn[2], 0xC5) << "only the weak sector differs";
    }
  }
  EXPECT_EQ(differing, 1);
  // The platter keeps turning: rev 2 wraps back to capture 0.
  ASSERT_EQ(flux_decode_track_rev(scp.data(), scp.size(), 0, 2, &t2, p2.data(),
                                  p2.size()),
            0);
  ASSERT_EQ(t2.count, t0.count);
  EXPECT_EQ(std::memcmp(p2.data() + t2.sec[4].off, p0.data() + t0.sec[4].off,
                        t0.sec[4].len),
            0)
      << "rev 2 = capture 0 again";
}
