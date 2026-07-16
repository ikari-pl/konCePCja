/* amx_test.cpp — the AMX mouse Device: the row-9 monostable protocol (one
 * mickey per deselect/reselect), the wired-AND external column lines, the
 * plugged gate, and the full-machine drain through the firmware's own
 * keyboard scan. See docs/hardware/amx-mouse-device.md. */

#include "hw/amx.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <vector>

#include "hw/board.h"
#include "subcycle/machine.h"

namespace {

struct AmxRig {
  std::vector<uint8_t> mem = std::vector<uint8_t>(amx_state_size());
  Board board;
  Device dev;
};

void make_rig(AmxRig& rig) {
  rig.dev = amx_init(rig.mem.data());
  board_init(&rig.board);
  board_add(&rig.board, rig.dev);
  board_reset(&rig.board);
}

// Drive the row-select lines for a few ticks (the PPI stand-in) and return
// the committed external column lines while the row was held.
uint8_t select_row(AmxRig& rig, uint8_t row) {
  uint8_t lines = 0xFF;
  for (int i = 0; i < 4; ++i) {
    Bus b = bus_resting();
    b.ay.kbd_row = row;
    rig.board.bus = b;
    board_tick(&rig.board);
    lines = rig.board.bus.ay.row_ext;
  }
  return lines;
}

std::vector<uint8_t> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
}

}  // namespace

TEST(AmxMouse, DrivesRow9LinesOnlyWhilePluggedAndSelected) {
  AmxRig rig;
  make_rig(rig);
  amx_feed(&rig.dev, 1, 0, 0x01);  // one mickey right + left button

  EXPECT_EQ(select_row(rig, 9), 0xFF) << "unplugged: the connector is empty";
  amx_set_plugged(&rig.dev, 1);
  EXPECT_EQ(select_row(rig, 5), 0xFF) << "another row: our columns idle";
  const uint8_t lines = select_row(rig, 9);
  EXPECT_EQ(lines & (1 << 3), 0) << "right direction bit LOW";
  EXPECT_EQ(lines & (1 << 4), 0) << "left button -> Fire2 LOW";
  EXPECT_NE(lines & (1 << 0), 0) << "no vertical motion";
}

TEST(AmxMouse, OneMickeyPerDeselectReselectCycle) {
  AmxRig rig;
  make_rig(rig);
  amx_set_plugged(&rig.dev, 1);
  amx_feed(&rig.dev, 3, -2, 0);  // 3 right, 2 up

  int right_reads = 0, up_reads = 0;
  for (int scan = 0; scan < 6; ++scan) {  // scan = select 9, then deselect
    const uint8_t lines = select_row(rig, 9);
    if ((lines & (1 << 3)) == 0) right_reads++;
    if ((lines & (1 << 0)) == 0) up_reads++;
    select_row(rig, 0);  // the rest of the matrix scan (deselect = arm)
  }
  EXPECT_EQ(right_reads, 3) << "each mickey yields exactly one LOW read";
  EXPECT_EQ(up_reads, 2);
  AmxRegs r{};
  amx_peek(&rig.dev, &r);
  EXPECT_EQ(r.mickeys_x, 0);
  EXPECT_EQ(r.mickeys_y, 0);
}

TEST(AmxMouse, ResetClearsMotionButKeepsThePlug) {
  AmxRig rig;
  make_rig(rig);
  amx_set_plugged(&rig.dev, 1);
  amx_feed(&rig.dev, 5, 5, 0x07);
  board_reset(&rig.board);
  AmxRegs r{};
  amx_peek(&rig.dev, &r);
  EXPECT_EQ(r.mickeys_x, 0);
  EXPECT_EQ(r.buttons, 0);
  EXPECT_EQ(r.plugged, 1);
}

TEST(AmxMouse, FirmwareScanDrainsTheMickeysOnTheFullMachine) {
  std::vector<uint8_t> rom = read_file("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read_file("../rom/cpc6128.rom");
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";

  subcycle::Machine m;
  ASSERT_TRUE(m.build(rom.data(), rom.size()));
  m.set_amx_mouse(true);
  for (int i = 0; i < 120; ++i) m.run_frame();  // to the Ready screen

  m.amx_mouse_feed(3, 0, 0);
  AmxRegs r{};
  amx_peek(m.amx(), &r);
  ASSERT_EQ(r.mickeys_x, 3);
  // The firmware scans the whole matrix once per frame — row 9 included —
  // so each frame's deselect/reselect cycle consumes one mickey.
  for (int i = 0; i < 8; ++i) m.run_frame();
  amx_peek(m.amx(), &r);
  EXPECT_EQ(r.mickeys_x, 0) << "the real scan loop consumed the motion";
}
