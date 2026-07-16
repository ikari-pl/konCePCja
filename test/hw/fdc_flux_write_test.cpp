/* fdc_flux_write_test.cpp — Stage 2/3 write-proof for the WRITABLE flux medium
 * (docs/hardware/flux-media.md §7). A flux-backed disc gets a synthesized DSK
 * overlay: a CLEAN (unwritten) track keeps serving the rotating flux cache
 * (weak/protection bits intact), a track the FDC WRITES promotes to the overlay
 * (per-track dirty map) and reads back the written data — exactly as a real
 * drive replaces a track's flux fuzz with fresh MFM when it writes it.
 *
 * The whole sequence runs through the real µPD765A command path (io_write /
 * io_read over the board bus), so it proves promotion + image-vs-flux routing
 * end to end, not just the attach helper. The final case closes the loop:
 * export (scp_from_disk over the dirty map) then re-decode the flux — the
 * written track decodes to the written bytes, the clean track to the original.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "flux_synth.h"  // fluxsynth::amsdos_content / scp_from_sectors
#include "hw/board.h"
#include "hw/fdc.h"
#include "hw/flux.h"    // flux_scp_to_dsk / flux_decode_track_rev
#include "scp_write.h"  // scp_from_disk (Stage-1 export encoder)

namespace {

using namespace fluxsynth;

constexpr uint16_t kMsr = 0xFB7E;    // main status register (read)
constexpr uint16_t kData = 0xFB7F;   // data register (read/write)
constexpr uint16_t kMotor = 0xFA7E;  // drive motor latch (write)

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

// One idle cycle deasserts the access so the next has a fresh edge (transfers
// are one byte per ACCESS, not per master cycle).
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
  for (uint8_t b : bytes) io_write(rig, kData, b);
}
void spin(FluxRig& rig, uint64_t n) {
  for (uint64_t i = 0; i < n; ++i) {
    rig.board.bus = bus_resting();
    board_tick(&rig.board);
  }
}
void spin_ms(FluxRig& rig, uint64_t ms) { spin(rig, ms * 16000 + 64); }

void motor_on_ready(FluxRig& rig) {
  io_write(rig, kMotor, 0x01);
  spin(rig, 8000004);  // kSpinUpCycles + slack
  FdcRegs f{};
  fdc_peek(&rig.dev, &f);
  ASSERT_EQ(f.ready, 1) << "drive ready after the 500 ms spin-up";
}

bool wait_rqm(FluxRig& rig) {
  FdcRegs f{};
  for (int i = 0; i < 300000; ++i) {  // ≈ 6 revolutions
    fdc_peek(&rig.dev, &f);
    if (f.msr & 0x80) return true;
    spin(rig, 64);
  }
  ADD_FAILURE() << "RQM never rose (msr stuck at 0x" << std::hex << int(f.msr)
                << ")";
  return false;
}

std::vector<uint8_t> read_result(FluxRig& rig, int n) {
  std::vector<uint8_t> r;
  for (int i = 0; i < n; i++) {
    if (!wait_rqm(rig)) break;
    r.push_back(io_read(rig, kData));
  }
  return r;
}
void feed_bytes(FluxRig& rig, const std::vector<uint8_t>& bytes) {
  for (uint8_t b : bytes) {
    if (!wait_rqm(rig)) return;
    io_write(rig, kData, b);
  }
}

// Seek unit 0 to `track` and drain the SENSE INTERRUPT seek-end (no SPECIFY →
// SRT = 0 → 32 ms/step). A no-op distance still needs the SENSE drain.
void seek_to(FluxRig& rig, uint8_t track, uint8_t from) {
  command(rig, {0x0F, 0x00, track});
  const uint8_t steps = track > from ? track - from : from - track;
  spin_ms(rig, static_cast<uint64_t>(steps) * 32 + 1);
  command(rig, {0x08});  // SENSE INTERRUPT
  read_result(rig, 2);
}

// Attach a writable flux disc built from `content`, returning the pristine SCP
// and the synthesized DSK overlay (both must outlive the rig).
struct FluxDisc {
  std::vector<uint8_t> scp;
  std::vector<uint8_t> dsk;
};
FluxDisc make_writable_flux(FluxRig& rig,
                            const std::vector<std::vector<Sector>>& content) {
  FluxDisc d;
  d.scp = scp_from_sectors(content);
  d.dsk.assign(0x100 + 102 * (0x100 + 8192), 0);
  const long dsk_len = flux_scp_to_dsk(d.scp.data(), d.scp.size(), d.dsk.data(),
                                       d.dsk.size(), nullptr);
  EXPECT_GT(dsk_len, 0) << "the synthetic flux must synthesize a standard DSK";
  d.dsk.resize(static_cast<size_t>(dsk_len > 0 ? dsk_len : 0));
  EXPECT_EQ(fdc_attach_flux_writable(&rig.dev, d.scp.data(), d.scp.size(),
                                     d.dsk.data(), d.dsk.size()),
            0);
  return d;
}

// The revolution-0 payload of sector `r` on cylinder `cyl`, decoded straight
// from a flux dump (the oracle for "what the clean track still reads").
std::vector<uint8_t> flux_sector(const std::vector<uint8_t>& scp, uint8_t cyl,
                                 uint8_t r) {
  FluxTrack ft{};
  std::vector<uint8_t> pay(8192, 0);
  EXPECT_EQ(flux_decode_track_rev(scp.data(), scp.size(), cyl, 0, &ft,
                                  pay.data(), pay.size()),
            0);
  for (int i = 0; i < ft.count; i++)
    if (ft.sec[i].chrn[2] == r)
      return std::vector<uint8_t>(pay.begin() + ft.sec[i].off,
                                  pay.begin() + ft.sec[i].off + ft.sec[i].len);
  return {};
}

}  // namespace

// A written track serves the overlay; an untouched track still serves the flux;
// only the written track is marked dirty. The whole point of the hybrid.
TEST(FdcFluxWrite, WriteOneTrackServesImageOthersStayFlux) {
  FluxRig rig;
  make_rig(rig);
  const std::vector<std::vector<Sector>> src = amsdos_content(2);
  const FluxDisc disc = make_writable_flux(rig, src);
  motor_on_ready(rig);

  // A flux disc with an overlay is NOT write-protected (WP clear in ST3).
  command(rig, {0x04, 0x00});  // SENSE DRIVE STATUS unit 0
  const uint8_t st3 = read_result(rig, 1)[0];
  EXPECT_EQ(st3 & 0x40, 0x00) << "writable flux: WP clear";
  EXPECT_EQ(st3 & 0x20, 0x20) << "ready";

  // WRITE DATA track 0, sector C1 — a distinctive 512-byte pattern.
  std::vector<uint8_t> payload(512);
  for (size_t i = 0; i < payload.size(); ++i)
    payload[i] = static_cast<uint8_t>(0x5A ^ (i * 3 + 1));
  command(rig, {0x45, 0x00, 0x00, 0x00, 0xC1, 0x02, 0xC1, 0x2A, 0xFF});
  feed_bytes(rig, payload);
  const std::vector<uint8_t> wr = read_result(rig, 7);
  ASSERT_EQ(wr.size(), 7u);
  EXPECT_EQ(wr[0] & 0x40, 0x40) << "ST0: AT (normal single-sector EOT)";
  EXPECT_EQ(wr[1] & 0x02, 0x00) << "ST1: NOT Not-Writable — the write went in";

  // The written track now reads the written bytes back (dirty → overlay).
  command(rig, {0x46, 0x00, 0x00, 0x00, 0xC1, 0x02, 0xC1, 0x2A, 0xFF});
  const std::vector<uint8_t> back = read_result(rig, 512);
  read_result(rig, 7);
  ASSERT_EQ(back.size(), 512u);
  EXPECT_EQ(back, payload) << "written track serves the DSK overlay";

  // An UNTOUCHED track still reads its ORIGINAL flux content (clean → cache).
  seek_to(rig, 1, 0);
  command(rig, {0x46, 0x00, 0x01, 0x00, 0xC1, 0x02, 0xC1, 0x2A, 0xFF});
  const std::vector<uint8_t> t1 = read_result(rig, 512);
  read_result(rig, 7);
  const std::vector<uint8_t> t1_orig = flux_sector(disc.scp, 1, 0xC1);
  ASSERT_EQ(t1.size(), 512u);
  ASSERT_EQ(t1_orig.size(), 512u);
  EXPECT_EQ(t1, t1_orig) << "clean track 1 still served from the rotating flux";

  // The dirty map: only the written track flipped.
  int ntracks = 0;
  const bool* dirty = fdc_media_track_dirty(&rig.dev, ntracks);
  ASSERT_NE(dirty, nullptr);
  ASSERT_GE(ntracks, 2);
  EXPECT_TRUE(dirty[0]) << "track 0 was written → dirty";
  EXPECT_FALSE(dirty[1]) << "track 1 untouched → clean";
}

// A read-only flux dump (no overlay) stays Not-Writable, byte-for-byte with the
// pre-Stage-2 behaviour — the regression guard for the write-gate change.
TEST(FdcFluxWrite, ReadOnlyFluxStaysNotWritable) {
  FluxRig rig;
  make_rig(rig);
  const std::vector<std::vector<Sector>> src = amsdos_content(1);
  const std::vector<uint8_t> scp = scp_from_sectors(src);
  ASSERT_EQ(fdc_attach_flux(&rig.dev, scp.data(), scp.size()), 0);
  motor_on_ready(rig);

  // No overlay → the accessor reports no image, and SENSE DRIVE STATUS says WP.
  size_t img_len = 123;
  EXPECT_EQ(fdc_media_image(&rig.dev, img_len), nullptr);
  command(rig, {0x04, 0x00});
  EXPECT_EQ(read_result(rig, 1)[0] & 0x40, 0x40) << "read-only flux: WP set";

  command(rig, {0x45, 0x00, 0x00, 0x00, 0xC1, 0x02, 0xC1, 0x2A, 0xFF});
  const std::vector<uint8_t> r = read_result(rig, 7);
  ASSERT_EQ(r.size(), 7u);
  EXPECT_EQ(r[1] & 0x02, 0x02) << "ST1: Not Writable (no overlay)";

  int ntracks = 7;
  EXPECT_EQ(fdc_media_track_dirty(&rig.dev, ntracks), nullptr)
      << "read-only flux exposes no dirty map for export";
}

// FORMAT TRACK promotes and rewrites a flux track through the overlay.
TEST(FdcFluxWrite, FormatPromotesFluxTrack) {
  FluxRig rig;
  make_rig(rig);
  const FluxDisc disc = make_writable_flux(rig, amsdos_content(2));
  (void)disc;
  motor_on_ready(rig);

  // FORMAT track 0: N=2, SC=9, filler 0x6B, fresh IDs E1..E9.
  command(rig, {0x4D, 0x00, 0x02, 0x09, 0x2A, 0x6B});
  std::vector<uint8_t> ids;
  for (int s = 0; s < 9; ++s) {
    ids.push_back(0x00);
    ids.push_back(0x00);
    ids.push_back(static_cast<uint8_t>(0xE1 + s));
    ids.push_back(0x02);
  }
  feed_bytes(rig, ids);
  const std::vector<uint8_t> r = read_result(rig, 7);
  ASSERT_EQ(r.size(), 7u);
  EXPECT_EQ(r[0] & 0xC0, 0x00) << "ST0: normal termination — format wrote";

  int ntracks = 0;
  const bool* dirty = fdc_media_track_dirty(&rig.dev, ntracks);
  ASSERT_NE(dirty, nullptr);
  EXPECT_TRUE(dirty[0]) << "formatted track is dirty";
  EXPECT_FALSE(dirty[1]) << "the other track untouched";

  // The freshly formatted sector reads the filler back (overlay), not flux.
  command(rig, {0x46, 0x00, 0x00, 0x00, 0xE4, 0x02, 0xE4, 0x2A, 0xFF});
  const std::vector<uint8_t> data = read_result(rig, 512);
  read_result(rig, 7);
  ASSERT_EQ(data.size(), 512u);
  for (uint8_t b : data) ASSERT_EQ(b, 0x6B) << "formatted filler from overlay";
}

// The full write → export loop: after writing one track, scp_from_disk over the
// dirty map re-encodes the disc; decoding the flux shows the written track
// carries the written bytes and the clean track its original content.
TEST(FdcFluxWrite, ExportRoundTripsWrittenAndCleanTracks) {
  FluxRig rig;
  make_rig(rig);
  const std::vector<std::vector<Sector>> src = amsdos_content(2);
  const FluxDisc disc = make_writable_flux(rig, src);
  motor_on_ready(rig);

  std::vector<uint8_t> payload(512);
  for (size_t i = 0; i < payload.size(); ++i)
    payload[i] = static_cast<uint8_t>(i + 0x11);
  command(rig, {0x45, 0x00, 0x00, 0x00, 0xC3, 0x02, 0xC3, 0x2A, 0xFF});
  feed_bytes(rig, payload);
  read_result(rig, 7);

  // Export via the Stage-1 encoder, fed by the medium's own accessors.
  int ntracks = 0;
  const bool* dirty = fdc_media_track_dirty(&rig.dev, ntracks);
  ASSERT_NE(dirty, nullptr);
  size_t img_len = 0;
  const uint8_t* image = fdc_media_image(&rig.dev, img_len);
  ASSERT_NE(image, nullptr);
  size_t pristine_len = 0;
  const uint8_t* pristine = fdc_media_flux_scp(&rig.dev, pristine_len);
  ASSERT_NE(pristine, nullptr);

  const std::vector<uint8_t> out =
      scp_from_disk(pristine, pristine_len, image, img_len, dirty, ntracks);
  ASSERT_FALSE(out.empty());

  // Written track 0 sector C3 decodes to the written bytes.
  const std::vector<uint8_t> exp0 = flux_sector(out, 0, 0xC3);
  ASSERT_EQ(exp0.size(), payload.size());
  EXPECT_EQ(exp0, payload)
      << "exported flux carries the write on the dirty track";

  // Clean track 1 sector C1 decodes to its original content (verbatim splice).
  const std::vector<uint8_t> exp1 = flux_sector(out, 1, 0xC1);
  const std::vector<uint8_t> orig1 = flux_sector(disc.scp, 1, 0xC1);
  ASSERT_EQ(exp1.size(), orig1.size());
  EXPECT_EQ(exp1, orig1)
      << "clean track exported verbatim from the pristine flux";
}
