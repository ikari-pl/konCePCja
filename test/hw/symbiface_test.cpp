/* symbiface_test.cpp — the Symbiface II Device: the &FDxx decode, the IDE
 * register file + IDENTIFY + sector read/write against a caller-owned
 * mutable image (with the dirty flag), the DS12887 host-fed clock + CMOS
 * NVRAM, and the PS/2 mouse FIFO. See docs/hardware/symbiface-device.md. */

#include "hw/symbiface.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "hw/board.h"

namespace {

struct Sf2Rig {
  std::vector<uint8_t> mem = std::vector<uint8_t>(sf2_state_size());
  Board board;
  Device dev;
};

void make_rig(Sf2Rig& rig) {
  rig.dev = sf2_init(rig.mem.data());
  board_init(&rig.board);
  board_add(&rig.board, rig.dev);
  board_reset(&rig.board);
  sf2_set_plugged(&rig.dev, 1);
}

void idle(Sf2Rig& rig) {
  rig.board.bus = bus_resting();
  board_tick(&rig.board);
}

uint8_t io_read(Sf2Rig& rig, uint16_t addr) {
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.iorq = true;
  rig.board.bus.cpu.rd = true;
  rig.board.bus.cpu.addr = addr;
  board_tick(&rig.board);
  const uint8_t v = rig.board.bus.cpu.data;
  idle(rig);
  return v;
}

void io_write(Sf2Rig& rig, uint16_t addr, uint8_t val) {
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.iorq = true;
  rig.board.bus.cpu.wr = true;
  rig.board.bus.cpu.addr = addr;
  rig.board.bus.cpu.data = val;
  board_tick(&rig.board);
  idle(rig);
}

}  // namespace

TEST(Symbiface, IdentifyReportsTheVirtualCf) {
  Sf2Rig rig;
  make_rig(rig);
  std::vector<uint8_t> img(64 * 512, 0);  // 64 sectors
  sf2_ide_attach(&rig.dev, 0, img.data(), img.size());

  io_write(rig, 0xFD0E, 0x00);  // drive/head: master
  io_write(rig, 0xFD0F, 0xEC);  // IDENTIFY DEVICE
  EXPECT_EQ(io_read(rig, 0xFD0F) & 0x08, 0x08) << "DRQ: the identity is ready";

  std::vector<uint8_t> id(512);
  for (int i = 0; i < 512; ++i) id[i] = io_read(rig, 0xFD08);
  // Model string lives at word 27 (byte 54), byte-swapped within words.
  std::string model;
  for (int i = 0; i < 40; i += 2) {
    model += static_cast<char>(id[54 + i + 1]);
    model += static_cast<char>(id[54 + i]);
  }
  EXPECT_NE(model.find("konCePCja Virtual CF"), std::string::npos)
      << "got: [" << model << "]";
}

TEST(Symbiface, SectorWriteRoundTripsAndMarksDirty) {
  Sf2Rig rig;
  make_rig(rig);
  std::vector<uint8_t> img(64 * 512, 0);
  sf2_ide_attach(&rig.dev, 0, img.data(), img.size());
  EXPECT_EQ(sf2_media_dirty(&rig.dev), 0);

  // WRITE SECTORS: 1 sector at LBA 5.
  io_write(rig, 0xFD0E, 0xE0);  // LBA mode, master, LBA[27:24]=0
  io_write(rig, 0xFD0A, 0x01);  // sector count
  io_write(rig, 0xFD0B, 0x05);  // LBA low
  io_write(rig, 0xFD0C, 0x00);
  io_write(rig, 0xFD0D, 0x00);
  io_write(rig, 0xFD0F, 0x30);  // WRITE SECTORS
  ASSERT_EQ(io_read(rig, 0xFD0F) & 0x08, 0x08) << "DRQ: wants the data";
  for (int i = 0; i < 512; ++i)
    io_write(rig, 0xFD08, static_cast<uint8_t>(i * 3 + 1));
  EXPECT_EQ(sf2_media_dirty(&rig.dev), 1) << "the image diverged";
  EXPECT_EQ(img[5 * 512 + 0], 0x01) << "byte 0 landed at LBA 5";
  EXPECT_EQ(img[5 * 512 + 10], static_cast<uint8_t>(10 * 3 + 1));

  // READ SECTORS the same LBA back through the chip.
  io_write(rig, 0xFD0A, 0x01);
  io_write(rig, 0xFD0B, 0x05);
  io_write(rig, 0xFD0F, 0x20);  // READ SECTORS
  ASSERT_EQ(io_read(rig, 0xFD0F) & 0x08, 0x08);
  for (int i = 0; i < 512; ++i)
    EXPECT_EQ(io_read(rig, 0xFD08), static_cast<uint8_t>(i * 3 + 1));
}

// READ SECTORS past the end of the medium: the ATA error register latches IDNF
// (0x10, ID Not Found) and the status ERR bit is set — the µPD-independent ATA
// contract. (Previously only the happy path was covered.)
TEST(Symbiface, ReadPastEndSetsIdnf) {
  Sf2Rig rig;
  make_rig(rig);
  std::vector<uint8_t> img(4 * 512, 0);  // 4 sectors: valid LBAs 0..3
  sf2_ide_attach(&rig.dev, 0, img.data(), img.size());

  io_write(rig, 0xFD0E, 0xE0);  // LBA mode, master
  io_write(rig, 0xFD0A, 0x01);  // sector count
  io_write(rig, 0xFD0B, 100);   // LBA 100 ≫ 3
  io_write(rig, 0xFD0C, 0x00);
  io_write(rig, 0xFD0D, 0x00);
  io_write(rig, 0xFD0F, 0x20);  // READ SECTORS

  EXPECT_EQ(io_read(rig, 0xFD0F) & 0x01, 0x01) << "status ERR bit set";
  EXPECT_EQ(io_read(rig, 0xFD09), 0x10) << "error register = IDNF";
  EXPECT_EQ(io_read(rig, 0xFD0F) & 0x08, 0x00) << "no DRQ: no data to read";
}

// Multi-sector chaining: a 2-sector WRITE feeds 1024 bytes across two LBAs, the
// count decrements and the LBA auto-advances, then a 2-sector READ streams both
// back — the golden-master chaining, previously untested beyond one sector.
TEST(Symbiface, MultiSectorWriteAndReadChain) {
  Sf2Rig rig;
  make_rig(rig);
  std::vector<uint8_t> img(64 * 512, 0);
  sf2_ide_attach(&rig.dev, 0, img.data(), img.size());

  io_write(rig, 0xFD0E, 0xE0);  // LBA mode, master
  io_write(rig, 0xFD0A, 0x02);  // TWO sectors
  io_write(rig, 0xFD0B, 0x07);  // starting LBA 7
  io_write(rig, 0xFD0C, 0x00);
  io_write(rig, 0xFD0D, 0x00);
  io_write(rig, 0xFD0F, 0x30);  // WRITE SECTORS
  ASSERT_EQ(io_read(rig, 0xFD0F) & 0x08, 0x08) << "DRQ for sector 1";
  for (int i = 0; i < 512; ++i)
    io_write(rig, 0xFD08, static_cast<uint8_t>(i));  // sector 7 = i
  ASSERT_EQ(io_read(rig, 0xFD0F) & 0x08, 0x08) << "chained: DRQ for sector 2";
  for (int i = 0; i < 512; ++i)
    io_write(rig, 0xFD08, static_cast<uint8_t>(255 - i));  // sector 8 = 255-i
  EXPECT_EQ(io_read(rig, 0xFD0F) & 0x08, 0x00)
      << "both sectors done: DRQ clear";
  EXPECT_EQ(img[7 * 512 + 3], 3) << "sector 7 landed at LBA 7";
  EXPECT_EQ(img[8 * 512 + 3], static_cast<uint8_t>(252))
      << "sector 8 auto-advanced to LBA 8";

  io_write(rig, 0xFD0A, 0x02);  // read the pair back
  io_write(rig, 0xFD0B, 0x07);
  io_write(rig, 0xFD0F, 0x20);  // READ SECTORS
  ASSERT_EQ(io_read(rig, 0xFD0F) & 0x08, 0x08);
  for (int i = 0; i < 512; ++i)
    EXPECT_EQ(io_read(rig, 0xFD08), static_cast<uint8_t>(i));
  for (int i = 0; i < 512; ++i)
    EXPECT_EQ(io_read(rig, 0xFD08), static_cast<uint8_t>(255 - i));
}

TEST(Symbiface, RtcServesFedTimeAndCmosNvram) {
  Sf2Rig rig;
  make_rig(rig);
  const uint8_t regs[10] = {0x34, 0, 0x12, 0, 0x23, 0, 0x05, 0x28, 0x06, 0x26};
  sf2_rtc_set_time(&rig.dev, regs);

  io_write(rig, 0xFD15, 0x00);  // index seconds
  EXPECT_EQ(io_read(rig, 0xFD14), 0x34);
  io_write(rig, 0xFD15, 0x04);  // hours
  EXPECT_EQ(io_read(rig, 0xFD14), 0x23);
  io_write(rig, 0xFD15, 0x0B);  // register B: 24h + BCD
  EXPECT_EQ(io_read(rig, 0xFD14), 0x02);

  // CMOS NVRAM (register 20): writable, and time regs are read-only.
  io_write(rig, 0xFD15, 20);
  io_write(rig, 0xFD14, 0xAB);
  EXPECT_EQ(io_read(rig, 0xFD14), 0xAB) << "CMOS holds the write";
  io_write(rig, 0xFD15, 0x00);
  io_write(rig, 0xFD14, 0x99);  // must be ignored (time is read-only)
  EXPECT_EQ(io_read(rig, 0xFD14), 0x34) << "seconds unchanged";
}

TEST(Symbiface, MouseFifoDeliversMultiplexedPackets) {
  Sf2Rig rig;
  make_rig(rig);
  sf2_mouse_feed(&rig.dev, 3, -2, 0x01);  // right 3, up 2 (host), left button

  const uint8_t x = io_read(rig, 0xFD10);
  EXPECT_EQ(x >> 6, 0x01) << "mode 01: X offset";
  EXPECT_EQ(x & 0x3F, 3);
  const uint8_t y = io_read(rig, 0xFD18);  // the alias port
  EXPECT_EQ(y >> 6, 0x02) << "mode 10: Y offset";
  EXPECT_EQ(static_cast<int8_t>(y << 2) >> 2, 2) << "host up -> SF2 +Y";
  const uint8_t b = io_read(rig, 0xFD10);
  EXPECT_EQ(b >> 6, 0x03) << "mode 11: buttons";
  EXPECT_EQ(b & 0x01, 0x01) << "left button";
  EXPECT_EQ(io_read(rig, 0xFD10) >> 6, 0x00) << "drained: mode 00";
}
