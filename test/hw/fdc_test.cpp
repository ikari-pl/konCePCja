/* fdc_test.cpp — the µPD765A FDC Device: MSR handshake, motor latch, seeks +
 * SENSE INTERRUPT, READ ID, READ DATA (single and multi-sector), the V1 stubs,
 * DSK/EDSK parsing, and the integration acid test: the real CPC6128 firmware +
 * AMSDOS cataloguing an in-memory disc. See docs/hardware/fdc-device.md. */

#include "hw/fdc.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "hw/board.h"
#include "hw/crtc.h"
#include "hw/gate_array.h"
#include "hw/memory.h"
#include "hw/ppi.h"
#include "hw/psg.h"
#include "hw/video.h"
#include "hw/z80.h"

namespace {

constexpr uint16_t kMsr = 0xFB7E;    // main status register (read)
constexpr uint16_t kData = 0xFB7F;   // data register (read/write)
constexpr uint16_t kMotor = 0xFA7E;  // drive motor latch (write)

// ------------------------------------------------------------ test discs ----

// A minimal standard DSK: single-sided, 9×512-byte sectors per track in the
// AMSDOS DATA format (&C1..&C9), every byte 0xE5, with an AMSDOS directory in
// track 0 holding HELLO.BAS and README.TXT (1K each).
std::vector<uint8_t> build_dsk(int tracks = 40) {
  const int spt = 9, ssz = 512;
  const int block = 0x100 + spt * ssz;
  std::vector<uint8_t> d(0x100 + static_cast<size_t>(tracks) * block, 0);
  std::memcpy(d.data(), "MV - CPCEMU Disk-File\r\nDisk-Info\r\n", 34);
  d[0x30] = static_cast<uint8_t>(tracks);
  d[0x31] = 1;  // sides
  d[0x32] = static_cast<uint8_t>(block & 0xFF);
  d[0x33] = static_cast<uint8_t>(block >> 8);
  for (int t = 0; t < tracks; t++) {
    const size_t base = 0x100 + static_cast<size_t>(t) * block;
    std::memcpy(&d[base], "Track-Info\r\n", 12);
    d[base + 0x10] = static_cast<uint8_t>(t);  // track
    d[base + 0x11] = 0;                        // side
    d[base + 0x14] = 2;                        // size code: 512 bytes
    d[base + 0x15] = spt;
    d[base + 0x16] = 0x4E;  // GAP3
    d[base + 0x17] = 0xE5;  // filler
    for (int s = 0; s < spt; s++) {
      const size_t si = base + 0x18 + 8 * static_cast<size_t>(s);
      d[si + 0] = static_cast<uint8_t>(t);         // C
      d[si + 1] = 0;                               // H
      d[si + 2] = static_cast<uint8_t>(0xC1 + s);  // R (DATA format)
      d[si + 3] = 2;                               // N
    }
    std::memset(&d[base + 0x100], 0xE5, static_cast<size_t>(spt) * ssz);
  }
  // AMSDOS directory (track 0, sectors C1..C4 = the first 2K). One extent per
  // file: user 0, 8+3 name, EX/S1/S2, RC = 128-byte records, allocation blocks
  // (block 0/1 = the directory itself, files start at block 2).
  auto dirent = [&](int idx, const char* name, const char* ext, uint8_t blk) {
    const size_t e = 0x100 + 0x100 + static_cast<size_t>(idx) * 32;
    const size_t nl = std::strlen(name), el = std::strlen(ext);
    d[e] = 0;  // user 0
    for (size_t i = 0; i < 8; i++)
      d[e + 1 + i] = static_cast<uint8_t>(i < nl ? name[i] : ' ');
    for (size_t i = 0; i < 3; i++)
      d[e + 9 + i] = static_cast<uint8_t>(i < el ? ext[i] : ' ');
    d[e + 12] = d[e + 13] = d[e + 14] = 0;  // EX, S1, S2
    d[e + 15] = 8;                          // RC: 8 records = 1K
    std::memset(&d[e + 16], 0, 16);
    d[e + 16] = blk;
  };
  dirent(0, "HELLO", "BAS", 2);
  dirent(1, "README", "TXT", 3);
  return d;
}

// A tiny extended DSK: 2 tracks, 1 side, 2×256-byte sectors per track (R =
// 1,2), sector data filled with 0x10*track + sector.
std::vector<uint8_t> build_edsk() {
  const int tracks = 2, spt = 2, ssz = 256;
  const int block = 0x100 + spt * ssz;
  std::vector<uint8_t> d(0x100 + static_cast<size_t>(tracks) * block, 0);
  std::memcpy(d.data(), "EXTENDED CPC DSK File\r\nDisk-Info\r\n", 34);
  d[0x30] = tracks;
  d[0x31] = 1;
  d[0x34] = block >> 8;  // per-track size table (includes the header)
  d[0x35] = block >> 8;
  for (int t = 0; t < tracks; t++) {
    const size_t base = 0x100 + static_cast<size_t>(t) * block;
    std::memcpy(&d[base], "Track-Info\r\n", 12);
    d[base + 0x10] = static_cast<uint8_t>(t);
    d[base + 0x14] = 1;  // size code: 256
    d[base + 0x15] = spt;
    for (int s = 0; s < spt; s++) {
      const size_t si = base + 0x18 + 8 * static_cast<size_t>(s);
      d[si + 0] = static_cast<uint8_t>(t);
      d[si + 2] = static_cast<uint8_t>(1 + s);  // R
      d[si + 3] = 1;                            // N
      d[si + 6] = 0x00;                         // stored length lo
      d[si + 7] = 0x01;                         // stored length hi (256)
      std::memset(&d[base + 0x100 + static_cast<size_t>(s) * ssz], 0x10 * t + s,
                  ssz);
    }
  }
  return d;
}

// Overwrite every sector's data field with a marker byte (track/sector headers
// left intact), so two otherwise-identical images become byte-distinguishable.
// Used by the dual-drive spec: drive A and drive B must serve their OWN data.
void paint_sectors(std::vector<uint8_t>& dsk, uint8_t marker) {
  const int tracks = dsk[0x30];
  const size_t block = static_cast<size_t>(dsk[0x32]) | (dsk[0x33] << 8);
  for (int t = 0; t < tracks; t++) {
    const size_t base = 0x100 + static_cast<size_t>(t) * block;
    std::memset(&dsk[base + 0x100], marker, block - 0x100);
  }
}

// ------------------------------------------------------------ the rig -------

struct FdcRig {
  std::vector<uint8_t> mem = std::vector<uint8_t>(fdc_state_size());
  Board board;
  Device dev;
};

void make_fdc(FdcRig& rig) {
  rig.dev = fdc_init(rig.mem.data());
  board_init(&rig.board);
  board_add(&rig.board, rig.dev);
  board_reset(&rig.board);
}

// One idle cycle deasserts the access, so the next access has a fresh edge
// (data-register transfers are one byte per ACCESS, not per master cycle).
void idle(FdcRig& rig) {
  rig.board.bus = bus_resting();
  board_tick(&rig.board);
}
uint8_t io_read(FdcRig& rig, uint16_t addr) {
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.iorq = true;
  rig.board.bus.cpu.rd = true;
  rig.board.bus.cpu.addr = addr;
  board_tick(&rig.board);
  const uint8_t val = rig.board.bus.cpu.data;
  idle(rig);
  return val;
}
void io_write(FdcRig& rig, uint16_t addr, uint8_t val) {
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.iorq = true;
  rig.board.bus.cpu.wr = true;
  rig.board.bus.cpu.addr = addr;
  rig.board.bus.cpu.data = val;
  board_tick(&rig.board);
  idle(rig);
}

void command(FdcRig& rig, std::initializer_list<uint8_t> bytes) {
  for (uint8_t b : bytes) io_write(rig, kData, b);
}

// --- Stage 2 (rotating medium) helpers: the mechanics take real time now ----

// Advance the board `n` idle master cycles.
void spin(FdcRig& rig, uint64_t n) {
  for (uint64_t i = 0; i < n; ++i) {
    rig.board.bus = bus_resting();
    board_tick(&rig.board);
  }
}

// Motor on, then wait out the 500 ms spin-up so the drive reports ready.
void motor_on_ready(FdcRig& rig) {
  io_write(rig, kMotor, 0x01);
  spin(rig, 8000004);  // kSpinUpCycles + slack
  FdcRegs f{};
  fdc_peek(&rig.dev, &f);
  EXPECT_EQ(f.ready, 1) << "drive ready after the 500 ms spin-up";
}

// Wait (bounded to well over two revolutions) until RQM rises — covers seeks,
// rotational latency, the 2-index-pass No-Data bound, and 32 µs byte pacing.
bool wait_rqm(FdcRig& rig) {
  FdcRegs f{};
  for (int i = 0; i < 300000; ++i) {  // bound ≈ 19.2M cycles ≈ 6 revolutions
    fdc_peek(&rig.dev, &f);
    if (f.msr & 0x80) return true;
    spin(rig, 64);
  }
  ADD_FAILURE() << "RQM never rose (msr stuck at 0x" << std::hex << int(f.msr)
                << ")";
  return false;
}

std::vector<uint8_t> read_result(FdcRig& rig, int n) {
  std::vector<uint8_t> r;
  for (int i = 0; i < n; i++) {
    if (!wait_rqm(rig)) break;  // rotation / pacing: the byte must arrive first
    r.push_back(io_read(rig, kData));
  }
  return r;
}

// Milliseconds of board time (seek steps are (16-SRT)*2 ms; SRT=0 → 32 ms).
void spin_ms(FdcRig& rig, uint64_t ms) { spin(rig, ms * 16000 + 64); }

}  // namespace

// ------------------------------------------------------------ unit tests ----

TEST(Fdc, MsrHandshakeAndNotReadyRead) {
  FdcRig rig;
  make_fdc(rig);
  EXPECT_EQ(io_read(rig, kMsr), 0x80) << "idle: RQM set, direction CPU->FDC";
  io_write(rig, kData, 0x46);  // READ DATA opcode, parameters outstanding
  EXPECT_EQ(io_read(rig, kMsr), 0x90) << "mid-command: RQM + CB";
  command(rig, {0x00, 0x00, 0x00, 0xC1, 0x02, 0xC1, 0x2A, 0xFF});
  EXPECT_EQ(io_read(rig, kMsr), 0xD0)
      << "no disc: straight to the result phase";
  const std::vector<uint8_t> r = read_result(rig, 7);
  EXPECT_EQ(r[0] & 0xC8, 0x48) << "ST0: abnormal termination + not ready";
  EXPECT_EQ(io_read(rig, kMsr), 0x80)
      << "result drained: back to command phase";
}

TEST(Fdc, MotorLatch) {
  FdcRig rig;
  make_fdc(rig);
  FdcRegs f{};
  io_write(rig, kMotor, 0x01);
  fdc_peek(&rig.dev, &f);
  EXPECT_EQ(f.motor, 1);
  EXPECT_EQ(io_read(rig, kMotor), 0xFF) << "the motor latch is write-only";
  io_write(rig, kMotor, 0x00);
  fdc_peek(&rig.dev, &f);
  EXPECT_EQ(f.motor, 0);
}

// fdc_peek reports which drive the last command selected (US0 = bit 0 of the
// command's second byte). The sub-cycle engine's drive-activity LED routes on
// this (subcycle_bridge_disk_leds) — the legacy FDC.led surface is dormant
// under engine=1, so a wrong/missing unit would light the wrong drive's LED.
TEST(Fdc, PeekReportsSelectedUnit) {
  FdcRig rig;
  make_fdc(rig);
  FdcRegs f{};
  command(rig, {0x07, 0x00});  // RECALIBRATE, unit 0 (drive A)
  fdc_peek(&rig.dev, &f);
  EXPECT_EQ(f.unit, 0) << "drive A selected";
  command(rig, {0x07, 0x01});  // RECALIBRATE, unit 1 (drive B)
  fdc_peek(&rig.dev, &f);
  EXPECT_EQ(f.unit, 1) << "drive B selected";
}

// The mechanical event ring the drive-sound overlay drains (DriveSoundOverlay
// in subcycle_bridge.cpp): the motor latch emits MOTOR_ON on the rising edge
// and MOTOR_OFF when cleared. Under engine=1 this ring is the ONLY source of
// drive audio — the legacy io_register_fdc_motor_hook path never runs.
TEST(Fdc, MotorLatchEmitsDriveSoundEvents) {
  FdcRig rig;
  make_fdc(rig);
  FdcEvent ev[16];
  fdc_drain_events(&rig.dev, ev, 16);  // discard any startup events

  io_write(rig, kMotor, 0x01);  // motor on
  int n = fdc_drain_events(&rig.dev, ev, 16);
  bool got_on = false;
  for (int i = 0; i < n; ++i)
    if (ev[i].type == FDC_EV_MOTOR_ON) got_on = true;
  EXPECT_TRUE(got_on) << "motor-on latch emits FDC_EV_MOTOR_ON";

  io_write(rig, kMotor, 0x00);  // motor off
  n = fdc_drain_events(&rig.dev, ev, 16);
  bool got_off = false;
  for (int i = 0; i < n; ++i)
    if (ev[i].type == FDC_EV_MOTOR_OFF) got_off = true;
  EXPECT_TRUE(got_off) << "motor-off latch emits FDC_EV_MOTOR_OFF";
}

TEST(Fdc, SeekRecalibrateAndSenseInterrupt) {
  FdcRig rig;
  make_fdc(rig);
  std::vector<uint8_t> dsk = build_dsk();
  ASSERT_EQ(fdc_attach_disk(&rig.dev, dsk.data(), dsk.size()), 0);
  motor_on_ready(rig);

  // The spin-up READY edge flagged a status change on both units — drain them.
  command(rig, {0x08});
  std::vector<uint8_t> r = read_result(rig, 2);
  EXPECT_EQ(r[0], 0xC0) << "ST0: ready changed, unit 0 (now ready)";
  command(rig, {0x08});
  r = read_result(rig, 2);
  EXPECT_EQ(r[0], 0xC9) << "ST0: ready changed, unit 1, not ready";

  command(rig, {0x0F, 0x00, 0x05});  // SEEK unit 0 to track 5 — no result phase
  EXPECT_EQ(io_read(rig, kMsr), 0x81) << "RQM + the unit-0 seek-busy bit (D0B)";
  FdcRegs f{};
  fdc_peek(&rig.dev, &f);
  EXPECT_EQ(f.track[0], 0) << "the head has not arrived yet";
  command(rig, {0x08});  // SENSE INTERRUPT mid-seek: nothing to report yet
  EXPECT_EQ(read_result(rig, 1)[0], 0x80) << "seek still in flight: invalid";

  spin_ms(rig, 5 * 32 + 1);  // no SPECIFY sent → SRT=0 → 32 ms/step, 5 steps
  fdc_peek(&rig.dev, &f);
  EXPECT_EQ(f.track[0], 5) << "the head arrived at the SRT-timed ETA";
  EXPECT_EQ(io_read(rig, kMsr) & 0x01, 0) << "seek-busy bit dropped";

  command(rig, {0x08});  // SENSE INTERRUPT: the seek-end report
  r = read_result(rig, 2);
  EXPECT_EQ(r[0], 0x20) << "ST0: Seek End, unit 0, normal";
  EXPECT_EQ(r[1], 5) << "PCN = present cylinder";

  command(rig, {0x07, 0x00});  // RECALIBRATE → track 0
  spin_ms(rig, 5 * 32 + 1);
  command(rig, {0x08});
  r = read_result(rig, 2);
  EXPECT_EQ(r[0], 0x20);
  EXPECT_EQ(r[1], 0);

  command(rig, {0x08});
  EXPECT_EQ(read_result(rig, 1)[0], 0x80) << "nothing pending: invalid command";
  EXPECT_EQ(io_read(rig, kMsr), 0x80);
}

TEST(Fdc, ReadIdWalksTheSectorIds) {
  FdcRig rig;
  make_fdc(rig);
  std::vector<uint8_t> dsk = build_dsk();
  ASSERT_EQ(fdc_attach_disk(&rig.dev, dsk.data(), dsk.size()), 0);
  motor_on_ready(rig);

  command(rig, {0x4A, 0x00});  // READ ID (MFM), unit 0
  std::vector<uint8_t> r = read_result(rig, 7);
  EXPECT_EQ(r[0], 0x00) << "ST0 normal";
  EXPECT_EQ(r[3], 0x00);  // C
  EXPECT_EQ(r[4], 0x00);  // H
  // With a spinning disc the FIRST ID is whichever header passes next — any of
  // C1..C9 depending on the angle the platter happens to be at.
  const uint8_t first = r[5];
  EXPECT_GE(first, 0xC1);
  EXPECT_LE(first, 0xC9);
  EXPECT_EQ(r[6], 0x02);  // N

  command(rig, {0x4A, 0x00});  // the head rotates on to the NEXT header
  r = read_result(rig, 7);
  const uint8_t expect_next =
      static_cast<uint8_t>(0xC1 + ((first - 0xC1 + 1) % 9));
  EXPECT_EQ(r[5], expect_next)
      << "consecutive READ IDs walk the track angularly";
}

TEST(Fdc, ReadDataDeliversSectorAndResult) {
  FdcRig rig;
  make_fdc(rig);
  std::vector<uint8_t> dsk = build_dsk();
  ASSERT_EQ(fdc_attach_disk(&rig.dev, dsk.data(), dsk.size()), 0);
  motor_on_ready(rig);

  command(rig, {0x46, 0x00, 0x00, 0x00, 0xC1, 0x02, 0xC1, 0x2A, 0xFF});
  EXPECT_EQ(io_read(rig, kMsr) & 0x10, 0x10) << "busy from dispatch onward";
  ASSERT_TRUE(wait_rqm(rig));  // rotational latency until C1's data field
  EXPECT_EQ(io_read(rig, kMsr), 0xF0) << "execution: RQM + DIO + EXM + CB";
  std::vector<uint8_t> data = read_result(rig, 512);
  // Sector C1 of track 0 = the first directory sector: HELLO.BAS's entry.
  EXPECT_EQ(data[0], 0x00) << "user 0";
  EXPECT_EQ(std::memcmp(&data[1], "HELLO   BAS", 11), 0);
  EXPECT_EQ(data[32], 0x00);
  EXPECT_EQ(std::memcmp(&data[33], "README  TXT", 11), 0);
  EXPECT_EQ(data[64], 0xE5) << "unused directory entries";

  EXPECT_EQ(io_read(rig, kMsr), 0xD0) << "transfer done: result phase";
  const std::vector<uint8_t> r = read_result(rig, 7);
  EXPECT_EQ(r[0], 0x40)
      << "ST0: abnormal termination (ran to EOT — the CPC norm)";
  EXPECT_EQ(r[1], 0x80) << "ST1: End of Cylinder";
  EXPECT_EQ(r[2], 0x00);
  EXPECT_EQ(r[3], 0x00);  // C
  EXPECT_EQ(r[5], 0xC1)   // R = EOT — GOLDEN-MASTER DIVERGENCE A (docs §4b): the
                          // datasheet's Table-2 rollover is for TC-terminated
                          // reads; the CPC's EN path is unspecified + AMSDOS
                          // never reads this R, so we keep the oracle's R=EOT.
      << "R stays = EOT (divergence A, docs/hardware/fdc-device.md §4b)";
  EXPECT_EQ(r[6], 0x02);  // N
  EXPECT_EQ(io_read(rig, kMsr), 0x80);

  FdcRegs f{};
  fdc_peek(&rig.dev, &f);
  EXPECT_EQ(f.sectors_read, 1u);
}

// ---------------------------------------------------- drive B (unit 1) ------
// Drive B is a full peer of drive A (beads-xsdc): fdc_attach_disk(..., unit=1)
// stores its image in `media1`, and the whole command path — drive_ready,
// head_track, cmd_drive_status, the READ/WRITE byte serves, do_format — routes
// by the selected unit (sel_media / track_pos[unit]). These started as RED
// specs and now guard that wiring; the `unit=0` control assertions guard
// against a broken harness masquerading as the feature. (Flux capture stays
// drive-A-only — fdc_attach_flux only fills `media`.)

TEST(Fdc, DualDriveB_Unit1ReportsReadyWhenDiskAttached) {
  FdcRig rig;
  make_fdc(rig);
  std::vector<uint8_t> dskA = build_dsk(2);
  std::vector<uint8_t> dskB = build_dsk(2);
  ASSERT_EQ(fdc_attach_disk(&rig.dev, dskA.data(), dskA.size(), 0), 0);
  ASSERT_EQ(fdc_attach_disk(&rig.dev, dskB.data(), dskB.size(), 1), 0);
  motor_on_ready(rig);  // one motor line; drive A ready confirms spin-up

  // SENSE DRIVE STATUS drive A → ST3 with RY (0x20). Control: the wired path.
  command(rig, {0x04, 0x00});
  const uint8_t st3a = read_result(rig, 1)[0];
  EXPECT_EQ(st3a & 0x20, 0x20) << "drive A reports ready (control)";

  // SENSE DRIVE STATUS drive B. A disk is attached to unit 1, the motor is up,
  // so ST3.RY must be set — exactly as for unit 0. FAILS until drive B wired.
  command(rig, {0x04, 0x01});
  const uint8_t st3b = read_result(rig, 1)[0];
  EXPECT_EQ(st3b & 0x20, 0x20)
      << "drive B: attached disk on unit 1 must report ready (RED until the "
         "dual-drive read path is wired)";
}

TEST(Fdc, DualDriveB_Unit1ReadsItsOwnDistinctSectorData) {
  FdcRig rig;
  make_fdc(rig);
  std::vector<uint8_t> dskA = build_dsk(2);
  std::vector<uint8_t> dskB = build_dsk(2);
  paint_sectors(dskA, 0xAA);  // drive A: every sector byte 0xAA
  paint_sectors(dskB, 0xBB);  // drive B: every sector byte 0xBB
  ASSERT_EQ(fdc_attach_disk(&rig.dev, dskA.data(), dskA.size(), 0), 0);
  ASSERT_EQ(fdc_attach_disk(&rig.dev, dskB.data(), dskB.size(), 1), 0);
  motor_on_ready(rig);

  // Control: READ DATA track 0, sector C1 from unit 0 delivers drive A's 0xAA.
  command(rig, {0x46, 0x00, 0x00, 0x00, 0xC1, 0x02, 0xC1, 0x2A, 0xFF});
  ASSERT_TRUE(wait_rqm(rig));
  const std::vector<uint8_t> a = read_result(rig, 512);
  EXPECT_EQ(a[0], 0xAA) << "unit 0 serves drive A's data (control)";
  read_result(rig, 7);  // drain the result phase

  // SPEC: the same read on unit 1 must deliver drive B's 0xBB, from media1 —
  // not unit 0's image, and not an aborted not-ready result. FAILS until the
  // dual-drive read path (drive_ready/head_track/cmd_drive_status) is wired.
  command(rig, {0x46, 0x01, 0x00, 0x00, 0xC1, 0x02, 0xC1, 0x2A, 0xFF});
  ASSERT_TRUE(wait_rqm(rig));
  const std::vector<uint8_t> b = read_result(rig, 512);
  EXPECT_EQ(b[0], 0xBB)
      << "unit 1 must serve drive B's own sector data (RED until wired)";
  EXPECT_EQ(b[256], 0xBB) << "mid-sector byte also from drive B";
}

TEST(Fdc, ReadDataMultiSector) {
  FdcRig rig;
  make_fdc(rig);
  std::vector<uint8_t> dsk = build_dsk();
  ASSERT_EQ(fdc_attach_disk(&rig.dev, dsk.data(), dsk.size()), 0);
  motor_on_ready(rig);

  // R = C1, EOT = C2: two sectors stream without a new command.
  command(rig, {0x46, 0x00, 0x00, 0x00, 0xC1, 0x02, 0xC2, 0x2A, 0xFF});
  const std::vector<uint8_t> data = read_result(rig, 1024);
  EXPECT_EQ(std::memcmp(&data[1], "HELLO   BAS", 11), 0);
  EXPECT_EQ(data[512], 0xE5) << "sector C2: empty directory entries";
  const std::vector<uint8_t> r = read_result(rig, 7);
  EXPECT_EQ(r[0], 0x40);
  EXPECT_EQ(r[1], 0x80);
  EXPECT_EQ(r[5], 0xC2);
  FdcRegs f{};
  fdc_peek(&rig.dev, &f);
  EXPECT_EQ(f.sectors_read, 2u);
}

TEST(Fdc, ReadDataMissingSectorIsNoData) {
  FdcRig rig;
  make_fdc(rig);
  std::vector<uint8_t> dsk = build_dsk();
  ASSERT_EQ(fdc_attach_disk(&rig.dev, dsk.data(), dsk.size()), 0);
  motor_on_ready(rig);

  command(rig, {0x46, 0x00, 0x00, 0x00, 0xF7, 0x02, 0xF7, 0x2A, 0xFF});
  EXPECT_EQ(io_read(rig, kMsr) & 0x90, 0x10)
      << "searching: busy, RQM low (the miss takes two index passes)";
  ASSERT_TRUE(wait_rqm(rig));  // ≈ 2 revolutions of fruitless searching
  EXPECT_EQ(io_read(rig, kMsr), 0xD0) << "then the result phase, no execution";
  const std::vector<uint8_t> r = read_result(rig, 7);
  EXPECT_EQ(r[0] & 0x40, 0x40) << "ST0: abnormal termination";
  EXPECT_EQ(r[1], 0x04) << "ST1: No Data";
}

TEST(Fdc, MotorSpinUpGatesReadiness) {
  FdcRig rig;
  make_fdc(rig);
  std::vector<uint8_t> dsk = build_dsk();
  ASSERT_EQ(fdc_attach_disk(&rig.dev, dsk.data(), dsk.size()), 0);

  io_write(rig, kMotor, 0x01);
  FdcRegs f{};
  fdc_peek(&rig.dev, &f);
  EXPECT_EQ(f.motor, 1);
  EXPECT_EQ(f.ready, 0) << "latch on, but the platter is still spinning up";

  // A read issued during spin-up terminates Not-Ready, exactly like no motor.
  command(rig, {0x46, 0x00, 0x00, 0x00, 0xC1, 0x02, 0xC1, 0x2A, 0xFF});
  const std::vector<uint8_t> r = read_result(rig, 7);
  EXPECT_EQ(r[0] & 0x48, 0x48)
      << "AT + Not Ready before the drive is up to speed";

  spin(rig, 4000000);  // 250 ms in: still below speed
  fdc_peek(&rig.dev, &f);
  EXPECT_EQ(f.ready, 0);
  spin(rig, 4000100);  // past the 500 ms spin-up
  fdc_peek(&rig.dev, &f);
  EXPECT_EQ(f.ready, 1) << "ready exactly after the spin-up time";
}

TEST(Fdc, SlowPollingOverruns) {
  FdcRig rig;
  make_fdc(rig);
  std::vector<uint8_t> dsk = build_dsk();
  ASSERT_EQ(fdc_attach_disk(&rig.dev, dsk.data(), dsk.size()), 0);
  motor_on_ready(rig);

  command(rig, {0x46, 0x00, 0x00, 0x00, 0xC1, 0x02, 0xC1, 0x2A, 0xFF});
  ASSERT_TRUE(wait_rqm(rig));  // rotational latency to C1's data field
  (void)io_read(rig, kData);   // take byte 0...
  spin(rig, 2000);             // ...then ignore RQM for ~4 byte cells

  FdcRegs f{};
  fdc_peek(&rig.dev, &f);
  EXPECT_EQ(f.phase, 2)
      << "the missed 32 µs window aborted to the result phase";
  const std::vector<uint8_t> r = read_result(rig, 7);
  EXPECT_EQ(r[0] & 0x40, 0x40) << "ST0: abnormal termination";
  EXPECT_EQ(r[1] & 0x10, 0x10) << "ST1: Overrun — the CPU polled too slowly";
}

TEST(Fdc, MechanicalEventRingTimestampsThePhysics) {
  FdcRig rig;
  make_fdc(rig);
  std::vector<uint8_t> dsk = build_dsk();
  ASSERT_EQ(fdc_attach_disk(&rig.dev, dsk.data(), dsk.size()), 0);

  FdcEvent ev[64];
  io_write(rig, kMotor, 0x01);
  int n = fdc_drain_events(&rig.dev, ev, 64);
  ASSERT_GE(n, 1);
  EXPECT_EQ(ev[0].type, FDC_EV_MOTOR_ON);

  // Spin-up window: two index passes (3.2M, 6.4M) then READY at 8M cycles.
  spin(rig, 8000004);
  n = fdc_drain_events(&rig.dev, ev, 64);
  ASSERT_EQ(n, 3);
  EXPECT_EQ(ev[0].type, FDC_EV_INDEX);
  EXPECT_EQ(ev[1].type, FDC_EV_INDEX);
  EXPECT_EQ(ev[2].type, FDC_EV_MOTOR_READY);
  EXPECT_EQ(ev[1].cycle - ev[0].cycle, 3200000u)
      << "index pulses one revolution apart";

  // AMSDOS-style SPECIFY (SRT = 0xA -> 12 ms), then a 5-track seek: five STEP
  // events, 12 ms apart, arg = the new physical track each time.
  command(rig, {0x03, 0xA1, 0x03});
  command(rig, {0x0F, 0x00, 0x05});
  spin_ms(rig, 5 * 12 + 2);
  n = fdc_drain_events(&rig.dev, ev, 64);
  int steps = 0;
  uint64_t prev = 0;
  for (int i = 0; i < n; i++) {
    if (ev[i].type != FDC_EV_STEP) continue;  // index pulses interleave freely
    steps++;
    EXPECT_EQ(ev[i].arg, steps) << "arg carries the new track";
    if (steps > 1)
      EXPECT_EQ(ev[i].cycle - prev, 12u * 16000u) << "steps at the SRT period";
    prev = ev[i].cycle;
  }
  EXPECT_EQ(steps, 5);

  io_write(rig, kMotor, 0x00);
  n = fdc_drain_events(&rig.dev, ev, 64);
  ASSERT_GE(n, 1);
  EXPECT_EQ(ev[n - 1].type, FDC_EV_MOTOR_OFF);
}

// Feed `bytes` to the data register at the execution phase's 32 µs pacing.
void feed_bytes(FdcRig& rig, const std::vector<uint8_t>& bytes) {
  for (uint8_t b : bytes) {
    if (!wait_rqm(rig)) return;
    io_write(rig, kData, b);
  }
}

TEST(Fdc, WriteDataRoundTripMutatesTheImage) {
  FdcRig rig;
  make_fdc(rig);
  std::vector<uint8_t> dsk = build_dsk();
  ASSERT_EQ(fdc_attach_disk(&rig.dev, dsk.data(), dsk.size()), 0);
  motor_on_ready(rig);
  EXPECT_EQ(fdc_media_dirty(&rig.dev), 0) << "fresh attach is clean";

  // WRITE DATA C1..C2 on track 0 (two sectors chained via EOT).
  command(rig, {0x45, 0x00, 0x00, 0x00, 0xC1, 0x02, 0xC2, 0x2A, 0xFF});
  std::vector<uint8_t> payload(1024);
  for (size_t i = 0; i < payload.size(); ++i)
    payload[i] = static_cast<uint8_t>(i * 7 + 3);
  feed_bytes(rig, payload);
  std::vector<uint8_t> r = read_result(rig, 7);
  ASSERT_EQ(r.size(), 7u);
  EXPECT_EQ(r[0] & 0x40, 0x40) << "ST0: AT (normal EOT termination)";
  EXPECT_EQ(r[1], 0x80) << "ST1: End of Cylinder only";
  EXPECT_EQ(fdc_media_dirty(&rig.dev), 1) << "the image diverged";

  // The bytes must be readable back through the chip...
  command(rig, {0x46, 0x00, 0x00, 0x00, 0xC1, 0x02, 0xC2, 0x2A, 0xFF});
  std::vector<uint8_t> back = read_result(rig, 1024);
  ASSERT_EQ(back.size(), 1024u);
  EXPECT_EQ(back, payload) << "round trip through WRITE then READ";
  read_result(rig, 7);
  // ...and must live in the caller's own buffer (in-place mutation, §10).
  EXPECT_EQ(dsk[0x200], payload[0]) << "sector C1 data landed in the image";

  fdc_media_mark_clean(&rig.dev);
  EXPECT_EQ(fdc_media_dirty(&rig.dev), 0);
}

TEST(Fdc, WriteUnderfeedOverruns) {
  FdcRig rig;
  make_fdc(rig);
  std::vector<uint8_t> dsk = build_dsk();
  ASSERT_EQ(fdc_attach_disk(&rig.dev, dsk.data(), dsk.size()), 0);
  motor_on_ready(rig);

  command(rig, {0x45, 0x00, 0x00, 0x00, 0xC1, 0x02, 0xC1, 0x2A, 0xFF});
  feed_bytes(rig, {0xAA, 0xBB, 0xCC});  // then stop feeding
  spin(rig, 4096);                      // several byte cells pass unfed
  const std::vector<uint8_t> r = read_result(rig, 7);
  ASSERT_EQ(r.size(), 7u);
  EXPECT_EQ(r[0] & 0x40, 0x40) << "ST0: abnormal termination";
  EXPECT_EQ(r[1] & 0x10, 0x10) << "ST1: Overrun — the CPU missed the cell";
}

TEST(Fdc, WriteDeletedSetsControlMark) {
  FdcRig rig;
  make_fdc(rig);
  std::vector<uint8_t> dsk = build_dsk();
  ASSERT_EQ(fdc_attach_disk(&rig.dev, dsk.data(), dsk.size()), 0);
  motor_on_ready(rig);

  command(rig, {0x49, 0x00, 0x00, 0x00, 0xC3, 0x02, 0xC3, 0x2A, 0xFF});
  feed_bytes(rig, std::vector<uint8_t>(512, 0x5D));
  read_result(rig, 7);

  // A plain READ of that sector now reports the Control Mark.
  command(rig, {0x46, 0x00, 0x00, 0x00, 0xC3, 0x02, 0xC3, 0x2A, 0xFF});
  read_result(rig, 512);
  const std::vector<uint8_t> r = read_result(rig, 7);
  ASSERT_EQ(r.size(), 7u);
  EXPECT_EQ(r[2] & 0x40, 0x40) << "ST2: Control Mark from the deleted write";
  // And the mark is durable: it lives in the image's own Track-Info entry.
  EXPECT_EQ(dsk[0x100 + 0x18 + 8 * 2 + 5] & 0x40, 0x40);
}

TEST(Fdc, FormatTrackRewritesTheTrackInPlace) {
  FdcRig rig;
  make_fdc(rig);
  std::vector<uint8_t> dsk = build_dsk();
  ASSERT_EQ(fdc_attach_disk(&rig.dev, dsk.data(), dsk.size()), 0);
  motor_on_ready(rig);

  // FORMAT track 0: N=2, SC=9, GPL, filler 0x77 — new sector IDs D1..D9.
  command(rig, {0x4D, 0x00, 0x02, 0x09, 0x2A, 0x77});
  std::vector<uint8_t> ids;
  for (int sec = 0; sec < 9; ++sec) {
    ids.push_back(0x00);                              // C
    ids.push_back(0x00);                              // H
    ids.push_back(static_cast<uint8_t>(0xD1 + sec));  // R
    ids.push_back(0x02);                              // N
  }
  feed_bytes(rig, ids);
  std::vector<uint8_t> r = read_result(rig, 7);
  ASSERT_EQ(r.size(), 7u);
  EXPECT_EQ(r[0] & 0xC0, 0x00) << "ST0: normal termination";
  EXPECT_EQ(fdc_media_dirty(&rig.dev), 1);

  // READ ID must now serve the fresh IDs; the data must be the filler.
  command(rig, {0x0A, 0x00});
  r = read_result(rig, 7);
  ASSERT_EQ(r.size(), 7u);
  EXPECT_GE(r[5], 0xD1) << "R of a freshly formatted sector";
  EXPECT_LE(r[5], 0xD9);
  command(rig, {0x46, 0x00, 0x00, 0x00, 0xD4, 0x02, 0xD4, 0x2A, 0xFF});
  const std::vector<uint8_t> data = read_result(rig, 512);
  ASSERT_EQ(data.size(), 512u);
  for (uint8_t b : data) ASSERT_EQ(b, 0x77) << "filler byte from the format";
  read_result(rig, 7);
}

TEST(Fdc, FormatThatCannotFitTheDskBlockIsNotWritable) {
  FdcRig rig;
  make_fdc(rig);
  std::vector<uint8_t> dsk = build_dsk();
  const std::vector<uint8_t> before = dsk;
  ASSERT_EQ(fdc_attach_disk(&rig.dev, dsk.data(), dsk.size()), 0);
  motor_on_ready(rig);

  // 9 × 1024-byte sectors cannot fit a 9 × 512 track block — and N=3 breaks
  // the standard image's shared size code anyway (docs §10).
  command(rig, {0x4D, 0x00, 0x03, 0x09, 0x2A, 0xE5});
  std::vector<uint8_t> ids;
  for (int sec = 0; sec < 9; ++sec) {
    ids.push_back(0x00);
    ids.push_back(0x00);
    ids.push_back(static_cast<uint8_t>(0xD1 + sec));
    ids.push_back(0x03);
  }
  feed_bytes(rig, ids);
  const std::vector<uint8_t> r = read_result(rig, 7);
  ASSERT_EQ(r.size(), 7u);
  EXPECT_EQ(r[1] & 0x02, 0x02) << "ST1: NW — the container cannot hold it";
  EXPECT_EQ(dsk, before) << "a rejected format leaves the image untouched";
  EXPECT_EQ(fdc_media_dirty(&rig.dev), 0);
}

TEST(Fdc, InvalidCommandReturnsSt0_80) {
  FdcRig rig;
  make_fdc(rig);
  io_write(rig, kData, 0x18);  // no such opcode
  EXPECT_EQ(io_read(rig, kMsr), 0xD0);
  EXPECT_EQ(read_result(rig, 1)[0], 0x80);
  EXPECT_EQ(io_read(rig, kMsr), 0x80);
}

TEST(Fdc, ExtendedDskParsesAndReads) {
  FdcRig rig;
  make_fdc(rig);
  std::vector<uint8_t> dsk = build_edsk();
  ASSERT_EQ(fdc_attach_disk(&rig.dev, dsk.data(), dsk.size()), 0);
  motor_on_ready(rig);

  command(rig, {0x0F, 0x00, 0x01});  // SEEK to track 1
  spin_ms(rig, 32 + 1);              // one 32 ms step (SRT = 0)
  command(rig, {0x08});
  read_result(rig, 2);

  command(rig, {0x46, 0x00, 0x01, 0x00, 0x02, 0x01, 0x02, 0x2A, 0xFF});
  const std::vector<uint8_t> data = read_result(rig, 256);
  for (uint8_t b : data) ASSERT_EQ(b, 0x11) << "track 1, sector 2 fill byte";
  const std::vector<uint8_t> r = read_result(rig, 7);
  EXPECT_EQ(r[0], 0x40);
  EXPECT_EQ(r[1], 0x80);
}

TEST(Fdc, AttachRejectsGarbage) {
  FdcRig rig;
  make_fdc(rig);
  std::vector<uint8_t> junk(0x400, 0xAA);
  EXPECT_EQ(fdc_attach_disk(&rig.dev, junk.data(), junk.size()), -1);
  EXPECT_EQ(fdc_attach_disk(&rig.dev, junk.data(), 0x10), -1) << "too short";
}

TEST(Fdc, SaveLoadRoundTripsRegistersAndKeepsMedia) {
  FdcRig rig;
  make_fdc(rig);
  std::vector<uint8_t> dsk = build_dsk();
  ASSERT_EQ(fdc_attach_disk(&rig.dev, dsk.data(), dsk.size()), 0);
  motor_on_ready(rig);
  command(rig, {0x0F, 0x00, 0x03});  // SEEK to 3
  spin_ms(rig, 3 * 32 + 1);          // let the head arrive before the snapshot

  std::vector<uint8_t> blob(rig.dev.state_size(rig.dev.self));
  rig.dev.save(rig.dev.self, blob.data());
  command(rig, {0x0F, 0x00, 0x07});  // SEEK to 7
  spin_ms(rig, 4 * 32 + 1);
  rig.dev.load(rig.dev.self, blob.data());

  FdcRegs f{};
  fdc_peek(&rig.dev, &f);
  EXPECT_EQ(f.track[0], 3) << "registers restored";
  command(rig, {0x4A, 0x00});  // media attachment survived the load
  const std::vector<uint8_t> r = read_result(rig, 7);
  EXPECT_EQ(r[3], 3) << "READ ID on the restored track works: C = 3";
}

// ---------------------------------------------------- integration: cat ------

namespace {

std::vector<uint8_t> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
}

}  // namespace

// Boot the REAL CPC6128 firmware + AMSDOS with the in-memory test disc attached
// and type `cat`. Asserted: the FDC actually served the directory (all four
// directory sectors of track 0 via READ DATA) and the framebuffer changed from
// the Ready screen (the catalogue was printed). We do not OCR the pixels — the
// FDC trace plus a changed frame is the documented acceptance (see the task's
// note in docs/hardware/fdc-device.md §7 context).
TEST(FdcBoot, AmsdosCataloguesTheDisc) {
  std::vector<uint8_t> rom = read_file("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read_file("../rom/cpc6128.rom");
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";
  std::vector<uint8_t> amsdos = read_file("rom/amsdos.rom");
  if (amsdos.size() < 0x4000) amsdos = read_file("../rom/amsdos.rom");
  if (amsdos.size() < 0x4000) GTEST_SKIP() << "rom/amsdos.rom not found";

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

  std::vector<uint8_t> dsk = build_dsk();
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
  // Type through the PSG key matrix the way koncepcja_sim's type_text does:
  // hold each key across several scans, release with a gap.
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
  // Real mechanics now: 500 ms spin-up + AMSDOS's own motor ticker + seeks +
  // rotational latency per directory sector + printing → give it ~12 s.
  run_frames(600);

  FdcRegs f{};
  fdc_peek(&fdev, &f);
  std::fprintf(stderr,
               "FDC CAT: phase=%u msr=0x%02X motor=%u last_cmd=0x%02X track=%u "
               "st0=0x%02X st1=0x%02X st2=0x%02X sectors_read=%u\n",
               f.phase, f.msr, f.motor, f.last_cmd, f.track[0], f.st0, f.st1,
               f.st2, f.sectors_read);

  EXPECT_GE(f.sectors_read, 4u) << "AMSDOS read the four directory sectors";
  EXPECT_EQ(f.track[0], 0) << "the directory lives on track 0";
  EXPECT_NE(before, fb) << "the screen changed: the catalogue was printed";

  if (FILE* out = std::fopen("/tmp/fdc_cat_test.ppm", "wb")) {
    std::fprintf(out, "P6\n%d %d\n255\n", w, h);
    std::fwrite(fb.data(), 1, fb.size(), out);
    std::fclose(out);
  }
}

// ---- Stage 3: the FDC on a raw flux medium (docs/hardware/flux-media.md §7)
// --

#include "flux_synth.h"
#include "hw/a2r.h"
#include "hw/flux.h"

namespace {
using namespace fluxsynth;
}  // namespace

TEST(FdcFlux, ReadsDirectlyFromFluxAndSeeksAcrossTracks) {
  FdcRig rig;
  make_fdc(rig);
  const std::vector<std::vector<Sector>> src = amsdos_content(2);
  const std::vector<uint8_t> scp = scp_from_sectors(src);
  ASSERT_EQ(fdc_attach_flux(&rig.dev, scp.data(), scp.size()), 0);
  motor_on_ready(rig);

  // READ ID on track 0 — served from the on-demand flux decode.
  command(rig, {0x4A, 0x00});
  std::vector<uint8_t> r = read_result(rig, 7);
  EXPECT_EQ(r[0], 0x00) << "ST0 normal from a flux-backed track";
  EXPECT_GE(r[5], 0xC1);
  EXPECT_LE(r[5], 0xC9);

  // READ DATA C1: payload must be byte-exact vs the encoded source.
  command(rig, {0x46, 0x00, 0x00, 0x00, 0xC1, 0x02, 0xC1, 0x2A, 0xFF});
  const std::vector<uint8_t> data = read_result(rig, 512);
  const Sector* c1 = nullptr;
  for (const Sector& s : src[0])
    if (s.r == 0xC1) c1 = &s;
  ASSERT_NE(c1, nullptr);
  ASSERT_EQ(data.size(), c1->data.size());
  EXPECT_EQ(std::memcmp(data.data(), c1->data.data(), data.size()), 0);
  read_result(rig, 7);  // drain the result phase

  // Seek to cylinder 1: the cache must refresh for the new track.
  command(rig, {0x0F, 0x00, 0x01});
  spin_ms(rig, 32 + 1);
  command(rig, {0x08});
  read_result(rig, 2);
  command(rig, {0x46, 0x00, 0x01, 0x00, 0xC1, 0x02, 0xC1, 0x2A, 0xFF});
  const std::vector<uint8_t> d1 = read_result(rig, 512);
  const Sector* t1c1 = nullptr;
  for (const Sector& s : src[1])
    if (s.r == 0xC1) t1c1 = &s;
  ASSERT_NE(t1c1, nullptr);
  EXPECT_EQ(std::memcmp(d1.data(), t1c1->data.data(), d1.size()), 0)
      << "track 1 read after the head moved: cache refreshed";
  read_result(rig, 7);
}

TEST(FdcFlux, WeakSectorAlternatesAcrossReadsPhysically) {
  // Two captured revolutions; sector &C5 reads differently on each. The FDC
  // serves whichever capture is passing the head, so repeated reads must
  // observe BOTH variants — the protection-check behaviour of real hardware.
  FdcRig rig;
  make_fdc(rig);
  const std::vector<std::vector<Sector>> src = amsdos_content(1);
  std::vector<std::vector<Sector>> rev1 = src;
  std::memset(rev1[0][4].data.data() + 100, 0x55, 16);
  Prng rng(0xBEEF);
  const std::vector<std::vector<std::vector<uint32_t>>> tracks = {
      {bits_to_flux(track_bits(src[0]), 80.0, 0.0, &rng),
       bits_to_flux(track_bits(rev1[0]), 80.0, 0.0, &rng)}};
  const std::vector<uint8_t> scp = build_scp(tracks);
  ASSERT_EQ(fdc_attach_flux(&rig.dev, scp.data(), scp.size()), 0);
  motor_on_ready(rig);

  const uint8_t orig = src[0][4].data[100];
  int saw_orig = 0, saw_weak = 0;
  for (int pass = 0; pass < 6; ++pass) {
    command(rig, {0x46, 0x00, 0x00, 0x00, 0xC5, 0x02, 0xC5, 0x2A, 0xFF});
    const std::vector<uint8_t> data = read_result(rig, 512);
    ASSERT_EQ(data.size(), 512u);
    if (data[100] == orig)
      saw_orig++;
    else if (data[100] == 0x55)
      saw_weak++;
    read_result(rig, 7);
    spin_ms(rig, 73);  // decorrelate the next read from the revolution parity
  }
  EXPECT_GT(saw_orig, 0) << "some reads served capture 0";
  EXPECT_GT(saw_weak, 0) << "some reads served capture 1 — the weak bits LIVE";
  EXPECT_EQ(saw_orig + saw_weak, 6)
      << "every read returned one of the captures";
}

// beads-mwpg: validate the flux decoder against a REAL SuperCard Pro capture,
// not our own synth encoder (the FdcFlux tests above round-trip through
// scp_from_sectors / build_scp, so an encoder+decoder shared bug is invisible).
// The capture is provided out-of-band — set KONCEPCJA_REAL_SCP to its path, or
// drop it at test/hw/fixtures/real.scp — so no (possibly unlicensed / large)
// binary lands in the repo. Absent → SKIP, so CI is unaffected. The assertions
// hold for ANY double-density IBM/CPC-format disc (they do not assume C1..C9):
// the SCP parses, the pipeline yields a DSK, and at least one track decodes
// CRC-clean sectors with sane geometry.
TEST(FdcFlux, DecodesARealScpCaptureWhenProvided) {
  std::vector<uint8_t> scp;
  if (const char* env = std::getenv("KONCEPCJA_REAL_SCP")) scp = read_file(env);
  if (scp.empty()) scp = read_file("test/hw/fixtures/real.scp");
  if (scp.empty()) scp = read_file("test/hw/fixtures/real.a2r");
  if (scp.empty()) scp = read_file("../test/hw/fixtures/real.scp");
  if (scp.empty()) scp = read_file("../test/hw/fixtures/real.a2r");
  if (scp.size() < 8)
    GTEST_SKIP() << "no real flux fixture (set KONCEPCJA_REAL_SCP=<path> to a "
                    "SuperCard Pro .scp or Applesauce .a2r of a DD CPC/IBM disc)";

  // Accept an Applesauce A2R flux capture: transcode it to SCP in memory so the
  // rest of the harness (and the whole flux pipeline) is unchanged.
  if (std::memcmp(scp.data(), "A2R", 3) == 0) {
    std::vector<uint8_t> conv;
    const int rc = a2r_to_scp(scp.data(), scp.size(), conv);
    ASSERT_EQ(rc, 0) << "A2R->SCP transcode failed with code " << rc;
    scp.swap(conv);
  }
  if (scp.size() < 0x2B0) GTEST_SKIP() << "flux fixture too small to be an SCP";

  ASSERT_EQ(flux_scp_probe(scp.data(), scp.size()), 1)
      << "the fixture is a valid SCP with sane geometry";
  EXPECT_GE(flux_scp_revolutions(scp.data(), scp.size()), 1);
  const int cyls = flux_scp_cylinders(scp.data(), scp.size());
  ASSERT_GE(cyls, 1);

  // Measure the capture's physical format from track-0, revolution-0 timing
  // (SCP TDH per rev: [index_time, flux_words, data_off], 25ns*(res+1) ticks).
  // This decoder targets 300 RPM / 250 kbit/s DD (a 2 us half-cell). An HD disc
  // — e.g. PC-98 2HD at 360 RPM / 500 kbit/s — parses as a valid SCP but decodes
  // to zero sectors: a SCOPE mismatch, not a decoder bug. Measure so an
  // out-of-scope disc SKIPs while a genuine DD-decode regression still FAILS.
  auto rd32le = [](const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
  };
  const double tick_ns = 25.0 * (static_cast<double>(scp[0x0B]) + 1.0);
  const uint32_t toff0 = rd32le(scp.data() + 0x10);  // side-0 track-0 TDH
  double rpm = 0.0, ticks_per_flux = 0.0;
  if (toff0 != 0 && static_cast<size_t>(toff0) + 16 <= scp.size() &&
      std::memcmp(scp.data() + toff0, "TRK", 3) == 0) {
    const uint32_t index_time = rd32le(scp.data() + toff0 + 4);
    const uint32_t words = rd32le(scp.data() + toff0 + 8);
    if (index_time != 0 && words != 0) {
      rpm = 60.0 / (index_time * tick_ns * 1e-9);
      ticks_per_flux = static_cast<double>(index_time) / words;
    }
  }
  const bool looks_dd = rpm >= 270.0 && rpm <= 330.0 && ticks_per_flux >= 120.0;

  // Whole-pipeline decode to a DSK: proves the PLL+MFM+CRC chain handled real
  // flux end to end.
  std::vector<uint8_t> dsk(1u << 21);
  const long dsk_len =
      flux_scp_to_dsk(scp.data(), scp.size(), dsk.data(), dsk.size(), nullptr);
  if (dsk_len == FLUX_E_NO_SECTORS && !looks_dd)
    GTEST_SKIP() << "out of scope for the DD decoder: measured " << rpm
                 << " RPM, " << ticks_per_flux
                 << " ticks/flux. This decoder targets ~300 RPM / 250 kbit/s DD "
                    "(CPC 3\", PC/ST 720K, PC-98 2DD); HD captures (e.g. PC-98 "
                    "2HD @ 360 RPM / 500 kbit/s) parse but decode to no sectors.";
  ASSERT_GT(dsk_len, 0) << "the real capture decoded into a DSK (measured " << rpm
                        << " RPM, " << ticks_per_flux << " ticks/flux)";
  EXPECT_TRUE(std::memcmp(dsk.data(), "MV - CPC", 8) == 0 ||
              std::memcmp(dsk.data(), "EXTENDED", 8) == 0)
      << "a well-formed DSK header";

  // Find the first track with CRC-clean sectors and sanity-check the geometry.
  std::vector<uint8_t> payload(1u << 16);
  int total = 0, first_cyl = -1;
  FluxTrack tk{};
  for (int c = 0; c < cyls && first_cyl < 0; ++c) {
    FluxTrack t{};
    if (flux_decode_track_rev(scp.data(), scp.size(), static_cast<uint8_t>(c), 0,
                              &t, payload.data(), payload.size()) == 0 &&
        t.count > 0) {
      first_cyl = c;
      tk = t;
    }
    total += t.count;
  }
  ASSERT_GE(first_cyl, 0) << "at least one cylinder decoded real sectors";
  EXPECT_GT(total, 0);
  for (int s = 0; s < tk.count; ++s) {
    EXPECT_LE(tk.sec[s].chrn[3], 6) << "N is a sane size code (128<<N)";
    EXPECT_EQ(tk.sec[s].len, 128u << tk.sec[s].chrn[3]);
  }
  int clean = 0;
  for (int s = 0; s < tk.count; ++s)
    if ((tk.sec[s].st1 & 0x20) == 0) clean++;  // ST1 Data Error clear
  EXPECT_GT(clean, 0) << "a real CRC-clean sector was recovered from flux";

  // And the FDC serves it: READ ID on that cylinder returns a normal header.
  FdcRig rig;
  make_fdc(rig);
  ASSERT_EQ(fdc_attach_flux(&rig.dev, scp.data(), scp.size()), 0);
  motor_on_ready(rig);
  if (first_cyl > 0) {
    command(rig, {0x0F, 0x00, static_cast<uint8_t>(first_cyl)});  // SEEK
    spin_ms(rig, static_cast<uint64_t>(first_cyl) * 32 + 1);
    command(rig, {0x08});
    read_result(rig, 2);
  }
  command(rig, {0x4A, 0x00});  // READ ID
  const std::vector<uint8_t> r = read_result(rig, 7);
  ASSERT_EQ(r.size(), 7u);
  EXPECT_EQ(r[0] & 0xC0, 0x00) << "ST0 normal: a header decoded from real flux";
  EXPECT_LE(r[6], 6) << "N sane";
}

// SENSE DRIVE STATUS (0x04) → ST3. Datasheet-faithful FDD-line semantics, NOT
// the legacy "WP + two-sided when no disc" fabrication (docs §4, beads-7mo4):
// FT=0 (fault line unwired), WP=media-not-writable, RY=ready, T0=track-0,
// TS=0 (single-sided 3" drive), and HD/US echoed. Read ST3 in three media
// states. (Placed after the flux helpers so the read-only-media case is real.)
TEST(Fdc, SenseDriveStatusReportsRealFddLines) {
  auto st3 = [](FdcRig& rig) {
    command(rig, {0x04, 0x00});  // SENSE DRIVE STATUS, unit 0 head 0
    return read_result(rig, 1)[0];
  };

  {  // No disc, head at track 0: WP (nothing to write) + T0. Never TS or FT.
    FdcRig rig;
    make_fdc(rig);
    const uint8_t s = st3(rig);
    EXPECT_EQ(s, 0x50) << "no disc: WP(0x40) | T0(0x10)";
    EXPECT_EQ(s & 0x08, 0x00) << "TS never set — the 3\" drive is single-sided";
    EXPECT_EQ(s & 0x80, 0x00) << "FT never set — the fault line is unwired";
  }
  {  // Writable DSK, spun up, track 0: RY + T0, WP clear (it IS writable now).
    FdcRig rig;
    make_fdc(rig);
    std::vector<uint8_t> dsk = build_dsk();
    ASSERT_EQ(fdc_attach_disk(&rig.dev, dsk.data(), dsk.size()), 0);
    motor_on_ready(rig);
    EXPECT_EQ(st3(rig), 0x30) << "writable disc ready: RY(0x20) | T0(0x10)";
  }
  {  // Flux-backed (read-only) media, spun up, track 0: WP + RY + T0.
    FdcRig rig;
    make_fdc(rig);
    const std::vector<uint8_t> scp = scp_from_sectors(amsdos_content(1));
    ASSERT_EQ(fdc_attach_flux(&rig.dev, scp.data(), scp.size()), 0);
    motor_on_ready(rig);
    EXPECT_EQ(st3(rig), 0x70)
        << "read-only flux media: WP(0x40) | RY(0x20) | T0(0x10)";
  }
}

// ---------------------------------------------------------------------------
// The write-path acid test (docs §10): the real firmware SAVEs a BASIC
// program through AMSDOS onto the sub-cycle machine's disc, and the file
// materializes in the caller's own DSK buffer with the dirty flag raised.
#include "subcycle/machine.h"

TEST(FdcBoot, AmsdosSavesABasicProgramToTheDisc) {
  std::vector<uint8_t> rom = read_file("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read_file("../rom/cpc6128.rom");
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";
  std::vector<uint8_t> amsdos = read_file("rom/amsdos.rom");
  if (amsdos.size() < 0x4000) amsdos = read_file("../rom/amsdos.rom");
  if (amsdos.size() < 0x4000) GTEST_SKIP() << "rom/amsdos.rom not found";

  subcycle::Machine m;
  ASSERT_TRUE(m.build(rom.data(), rom.size()));
  m.attach_amsdos(amsdos.data(), amsdos.size());
  std::vector<uint8_t> dsk = build_dsk();
  ASSERT_TRUE(m.insert_disk(dsk.data(), dsk.size()));

  auto tap = [&m](uint8_t code) {  // packed row << 4 | bit
    m.key(code, true);
    for (int i = 0; i < 4; ++i) m.run_frame();
    m.key(code, false);
    for (int i = 0; i < 4; ++i) m.run_frame();
  };
  for (int i = 0; i < 150; ++i) m.run_frame();  // to the AMSDOS Ready screen

  // 1 CLS <RETURN> — the smallest program worth saving.
  for (uint8_t code : {0x80, 0x57, 0x76, 0x44, 0x74, 0x22}) tap(code);
  EXPECT_FALSE(m.disk_dirty()) << "typing alone must not touch the disc";

  // SAVE"T <RETURN>
  for (uint8_t code : {0x74, 0x85, 0x67, 0x72}) tap(code);  // save
  m.key(0x25, true);                                        // SHIFT …
  for (int i = 0; i < 2; ++i) m.run_frame();
  tap(0x81);  // … + 2 = "
  m.key(0x25, false);
  for (int i = 0; i < 2; ++i) m.run_frame();
  tap(0x63);  // t
  tap(0x22);  // RETURN

  // Motor spin-up (500 ms) + directory + data writes: give it real time, but
  // stop as soon as the write-back reaches the image.
  bool dirty = false;
  for (int i = 0; i < 500 && !dirty; ++i) {
    m.run_frame();
    dirty = m.disk_dirty();
  }
  EXPECT_TRUE(dirty) << "SAVE\" must mutate the DSK buffer";
  for (int i = 0; i < 100; ++i) m.run_frame();  // let AMSDOS finish cleanly

  // The AMSDOS directory entry lives in the caller's own image: user 0,
  // "T" padded to 8, extension "BAS" — 11 contiguous name bytes.
  const uint8_t name[11] = {'T', ' ', ' ', ' ', ' ', ' ',
                            ' ', ' ', 'B', 'A', 'S'};
  bool found = false;
  for (size_t off = 0x200; off + 11 <= dsk.size() && !found; ++off)
    found = std::memcmp(&dsk[off], name, 11) == 0;
  EXPECT_TRUE(found) << "T.BAS appears in the image's AMSDOS directory";
}
