/* flux_save_test.cpp — Stage 4 save-core proof (src/flux_save.{h,cpp}). Drives
 * the save core directly (no ImGui): the live FDC medium of a drive turned into
 * the bytes of a chosen container, and re-decoded to prove correctness.
 *
 *  - a sector-backed disc saves DSK bytes == its image;
 *  - a flux-backed New disc saves a valid SCP that decodes to the DSK sectors;
 *  - the end-to-end proof: WRITE one track through the real µPD765A command
 *    path, Save-As SCP, re-decode — the written track carries the written bytes
 *    and an untouched track keeps its original flux content;
 *  - HFE export decodes back to the expected sectors;
 *  - .scp/.hfe on a sector-backed disc is refused (empty + a clear error).
 *
 * The FDC command rig mirrors fdc_flux_write_test.cpp so the write path is the
 * genuine one, not the attach helper. */

#include "flux_save.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "hw/board.h"
#include "hw/fdc.h"
#include "hw/flux.h"    // flux_scp_probe / flux_decode_track_rev
#include "hfe.h"        // hfe_to_scp
#include "hw/flux_synth.h"  // fluxsynth::amsdos_content / scp_from_sectors

namespace {

using namespace fluxsynth;

constexpr uint16_t kData = 0xFB7F;   // FDC data register
constexpr uint16_t kMotor = 0xFA7E;  // drive motor latch

// ------------------------------------------------------------ the rig -------

struct FluxRig {
  std::vector<uint8_t> mem = std::vector<uint8_t>(fdc_state_size());
  Board board;
  Device dev;
};

void make_rig(FluxRig& rig) {
  rig.dev = fdc_init(rig.mem.data());
  board_init(&rig.board);
  board_add(&rig.board, rig.dev);
  board_reset(&rig.board);
}

void idle(FluxRig& rig) {
  rig.board.bus = bus_resting();
  board_tick(&rig.board);
}
uint8_t io_read(FluxRig& rig, uint16_t addr) {
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.iorq = true;
  rig.board.bus.cpu.rd = true;
  rig.board.bus.cpu.addr = addr;
  board_tick(&rig.board);
  const uint8_t val = rig.board.bus.cpu.data;
  idle(rig);
  return val;
}
void io_write(FluxRig& rig, uint16_t addr, uint8_t val) {
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.iorq = true;
  rig.board.bus.cpu.wr = true;
  rig.board.bus.cpu.addr = addr;
  rig.board.bus.cpu.data = val;
  board_tick(&rig.board);
  idle(rig);
}
void command(FluxRig& rig, std::initializer_list<uint8_t> bytes) {
  for (uint8_t byte : bytes) io_write(rig, kData, byte);
}
void spin(FluxRig& rig, uint64_t count) {
  for (uint64_t i = 0; i < count; ++i) {
    rig.board.bus = bus_resting();
    board_tick(&rig.board);
  }
}

void motor_on_ready(FluxRig& rig) {
  io_write(rig, kMotor, 0x01);
  spin(rig, 8000004);  // kSpinUpCycles + slack
  FdcRegs regs{};
  fdc_peek(&rig.dev, &regs);
  ASSERT_EQ(regs.ready, 1) << "drive ready after the 500 ms spin-up";
}

bool wait_rqm(FluxRig& rig) {
  FdcRegs regs{};
  for (int i = 0; i < 300000; ++i) {
    fdc_peek(&rig.dev, &regs);
    if (regs.msr & 0x80) return true;
    spin(rig, 64);
  }
  ADD_FAILURE() << "RQM never rose (msr stuck)";
  return false;
}

std::vector<uint8_t> read_result(FluxRig& rig, int count) {
  std::vector<uint8_t> out;
  for (int i = 0; i < count; i++) {
    if (!wait_rqm(rig)) break;
    out.push_back(io_read(rig, kData));
  }
  return out;
}
void feed_bytes(FluxRig& rig, const std::vector<uint8_t>& bytes) {
  for (uint8_t byte : bytes) {
    if (!wait_rqm(rig)) return;
    io_write(rig, kData, byte);
  }
}

// Attach a writable flux disc built from `content`; the returned buffers must
// outlive the rig (they are the FDC's live wiring).
struct FluxDisc {
  std::vector<uint8_t> scp;
  std::vector<uint8_t> dsk;
};
FluxDisc make_writable_flux(FluxRig& rig,
                            const std::vector<std::vector<Sector>>& content) {
  FluxDisc disc;
  disc.scp = scp_from_sectors(content);
  disc.dsk.assign(0x100 + 102 * (0x100 + 8192), 0);
  const long dsk_len =
      flux_scp_to_dsk(disc.scp.data(), disc.scp.size(), disc.dsk.data(),
                      disc.dsk.size(), nullptr);
  EXPECT_GT(dsk_len, 0) << "the synthetic flux must synthesize a standard DSK";
  disc.dsk.resize(static_cast<size_t>(dsk_len > 0 ? dsk_len : 0));
  EXPECT_EQ(fdc_attach_flux_writable(&rig.dev, disc.scp.data(), disc.scp.size(),
                                     disc.dsk.data(), disc.dsk.size()),
            0);
  return disc;
}

// The revolution-0 payload of sector `record` on cylinder `cyl`, decoded from a
// flux dump — the oracle for "what does this track read".
std::vector<uint8_t> flux_sector(const std::vector<uint8_t>& scp, uint8_t cyl,
                                 uint8_t record) {
  FluxTrack track{};
  std::vector<uint8_t> pay(8192, 0);
  EXPECT_EQ(flux_decode_track_rev(scp.data(), scp.size(), cyl, 0, &track,
                                  pay.data(), pay.size()),
            0);
  for (int i = 0; i < track.count; i++)
    if (track.sec[i].chrn[2] == record)
      return std::vector<uint8_t>(pay.begin() + track.sec[i].off,
                                  pay.begin() + track.sec[i].off +
                                      track.sec[i].len);
  return {};
}

}  // namespace

// ---------------------------------------------------------------- sector ----

// A sector-backed disc saves DSK bytes equal to its live image.
TEST(FluxSave, SectorBackedSavesDskEqualToImage) {
  std::vector<uint8_t> dsk(0x100 + 102 * (0x100 + 8192), 0);
  {
    // Build a real DSK from synthetic flux, then attach it as a plain sector
    // image (the flux is only a convenient DSK generator here).
    const std::vector<uint8_t> scp = scp_from_sectors(amsdos_content(3));
    const long len = flux_scp_to_dsk(scp.data(), scp.size(), dsk.data(),
                                     dsk.size(), nullptr);
    ASSERT_GT(len, 0);
    dsk.resize(static_cast<size_t>(len));
  }
  std::vector<uint8_t> mem(fdc_state_size());
  Device dev = fdc_init(mem.data());
  ASSERT_EQ(fdc_attach_disk(&dev, dsk.data(), dsk.size(), 0), 0);

  const FluxSaveCaps caps = flux_save_caps_dev(&dev, 0);
  EXPECT_TRUE(caps.present);
  EXPECT_TRUE(caps.can_dsk);
  EXPECT_FALSE(caps.can_scp);
  EXPECT_FALSE(caps.can_hfe);

  std::string err;
  const std::vector<uint8_t> out =
      flux_save_bytes_dev(&dev, 0, SaveFormat::Dsk, err);
  EXPECT_TRUE(err.empty()) << err;
  EXPECT_EQ(out, dsk) << "sector-backed DSK save == the live image";
}

// Drive B (unit 1) is sector-only and sourced from the FDC's media1 image, not
// the legacy driveB struct.
TEST(FluxSave, DriveBSectorImageFromLiveMedium) {
  std::vector<uint8_t> dsk(0x100 + 102 * (0x100 + 8192), 0);
  const std::vector<uint8_t> scp = scp_from_sectors(amsdos_content(2));
  const long len =
      flux_scp_to_dsk(scp.data(), scp.size(), dsk.data(), dsk.size(), nullptr);
  ASSERT_GT(len, 0);
  dsk.resize(static_cast<size_t>(len));

  std::vector<uint8_t> mem(fdc_state_size());
  Device dev = fdc_init(mem.data());
  ASSERT_EQ(fdc_attach_disk(&dev, dsk.data(), dsk.size(), 1), 0);

  const FluxSaveCaps caps = flux_save_caps_dev(&dev, 1);
  EXPECT_TRUE(caps.can_dsk);
  EXPECT_FALSE(caps.can_scp) << "flux is drive-A-only";

  std::string err;
  const std::vector<uint8_t> out =
      flux_save_bytes_dev(&dev, 1, SaveFormat::Dsk, err);
  EXPECT_TRUE(err.empty()) << err;
  EXPECT_EQ(out, dsk);
}

// .scp / .hfe on a sector-backed disc is refused with a clear error, no crash.
TEST(FluxSave, SectorBackedRejectsFluxFormats) {
  std::vector<uint8_t> dsk(0x100 + 102 * (0x100 + 8192), 0);
  const std::vector<uint8_t> scp = scp_from_sectors(amsdos_content(1));
  const long len =
      flux_scp_to_dsk(scp.data(), scp.size(), dsk.data(), dsk.size(), nullptr);
  ASSERT_GT(len, 0);
  dsk.resize(static_cast<size_t>(len));

  std::vector<uint8_t> mem(fdc_state_size());
  Device dev = fdc_init(mem.data());
  ASSERT_EQ(fdc_attach_disk(&dev, dsk.data(), dsk.size(), 0), 0);

  for (SaveFormat fmt : {SaveFormat::Scp, SaveFormat::Hfe}) {
    std::string err;
    const std::vector<uint8_t> out = flux_save_bytes_dev(&dev, 0, fmt, err);
    EXPECT_TRUE(out.empty());
    EXPECT_FALSE(err.empty()) << "an unsupported combo must explain itself";
  }
}

// ------------------------------------------------------------------ flux ----

// A flux-backed New disc (no writes) saves a valid SCP that decodes back to the
// DSK's sectors.
TEST(FluxSave, FluxNewDiscSavesDecodableScp) {
  FluxRig rig;
  make_rig(rig);
  const std::vector<std::vector<Sector>> src = amsdos_content(3);
  const FluxDisc disc = make_writable_flux(rig, src);

  const FluxSaveCaps caps = flux_save_caps_dev(&rig.dev, 0);
  EXPECT_TRUE(caps.can_dsk);
  EXPECT_TRUE(caps.can_scp);
  EXPECT_TRUE(caps.can_hfe);

  std::string err;
  const std::vector<uint8_t> out =
      flux_save_bytes_dev(&rig.dev, 0, SaveFormat::Scp, err);
  ASSERT_TRUE(err.empty()) << err;
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(flux_scp_probe(out.data(), out.size()), 1) << "a usable SCP";

  // Every sector of every source track decodes back to its content.
  for (uint8_t cyl = 0; cyl < src.size(); cyl++)
    for (const Sector& sec : src[cyl]) {
      const std::vector<uint8_t> got = flux_sector(out, cyl, sec.r);
      ASSERT_EQ(got, sec.data)
          << "cyl " << int(cyl) << " sector " << std::hex << int(sec.r);
    }
}

// The end-to-end proof: WRITE one track through the real command path, Save-As
// SCP, re-decode — the written track carries the written bytes, an untouched
// track keeps its original flux content.
TEST(FluxSave, FluxWriteThenSaveAsScpRoundTrips) {
  FluxRig rig;
  make_rig(rig);
  const std::vector<std::vector<Sector>> src = amsdos_content(2);
  const FluxDisc disc = make_writable_flux(rig, src);
  motor_on_ready(rig);

  std::vector<uint8_t> payload(512);
  for (size_t i = 0; i < payload.size(); ++i)
    payload[i] = static_cast<uint8_t>(0x5A ^ (i * 7 + 3));
  // WRITE DATA track 0, sector C3.
  command(rig, {0x45, 0x00, 0x00, 0x00, 0xC3, 0x02, 0xC3, 0x2A, 0xFF});
  feed_bytes(rig, payload);
  const std::vector<uint8_t> result = read_result(rig, 7);
  ASSERT_EQ(result.size(), 7u);
  ASSERT_EQ(result[1] & 0x02, 0x00) << "ST1: the write went in";

  std::string err;
  const std::vector<uint8_t> out =
      flux_save_bytes_dev(&rig.dev, 0, SaveFormat::Scp, err);
  ASSERT_TRUE(err.empty()) << err;
  ASSERT_EQ(flux_scp_probe(out.data(), out.size()), 1);

  // Written track 0 sector C3 → the written bytes (dirty → overlay → resynth).
  const std::vector<uint8_t> written = flux_sector(out, 0, 0xC3);
  ASSERT_EQ(written.size(), payload.size());
  EXPECT_EQ(written, payload) << "exported flux carries the write";

  // Untouched track 1 sector C1 → its original flux content (verbatim splice).
  const std::vector<uint8_t> clean = flux_sector(out, 1, 0xC1);
  const std::vector<uint8_t> orig = flux_sector(disc.scp, 1, 0xC1);
  ASSERT_FALSE(orig.empty());
  EXPECT_EQ(clean, orig) << "clean track exported verbatim from the pristine flux";
}

// HFE export: the bytes decode (hfe_to_scp) back to the expected sectors.
TEST(FluxSave, FluxSavesDecodableHfe) {
  FluxRig rig;
  make_rig(rig);
  const std::vector<std::vector<Sector>> src = amsdos_content(2);
  const FluxDisc disc = make_writable_flux(rig, src);

  std::string err;
  const std::vector<uint8_t> hfe =
      flux_save_bytes_dev(&rig.dev, 0, SaveFormat::Hfe, err);
  ASSERT_TRUE(err.empty()) << err;
  ASSERT_FALSE(hfe.empty());

  std::vector<uint8_t> scp;
  ASSERT_EQ(hfe_to_scp(hfe.data(), hfe.size(), scp), 0) << "hfe_to_scp accepts";

  for (uint8_t cyl = 0; cyl < src.size(); cyl++)
    for (const Sector& sec : src[cyl]) {
      const std::vector<uint8_t> got = flux_sector(scp, cyl, sec.r);
      ASSERT_EQ(got, sec.data)
          << "HFE cyl " << int(cyl) << " sector " << std::hex << int(sec.r);
    }
}

// -------------------------------------------------------------- pure core ---

// The pure core classifies by the raw buffers: a null flux SCP means sector-
// backed, so .dsk works and .scp/.hfe are refused.
TEST(FluxSave, PureCoreSectorAndUnsupported) {
  const std::vector<uint8_t> image = {1, 2, 3, 4, 5};
  std::string err;
  const std::vector<uint8_t> dsk = flux_save_bytes_from_medium(
      nullptr, 0, image.data(), image.size(), nullptr, 0, SaveFormat::Dsk, err);
  EXPECT_TRUE(err.empty());
  EXPECT_EQ(dsk, image);

  const std::vector<uint8_t> scp = flux_save_bytes_from_medium(
      nullptr, 0, image.data(), image.size(), nullptr, 0, SaveFormat::Scp, err);
  EXPECT_TRUE(scp.empty());
  EXPECT_FALSE(err.empty());
}

// A null Device / inactive engine is handled without a crash.
TEST(FluxSave, NullDeviceReportsError) {
  std::string err;
  const std::vector<uint8_t> out =
      flux_save_bytes_dev(nullptr, 0, SaveFormat::Dsk, err);
  EXPECT_TRUE(out.empty());
  EXPECT_FALSE(err.empty());

  const FluxSaveCaps caps = flux_save_caps_dev(nullptr, 0);
  EXPECT_FALSE(caps.present);
}
