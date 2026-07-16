/* crtc_types_test.cpp — the program-visible differences between CRTC types 0-3:
 * register readability, the type-1 status register, R3 sync widths, and R8
 * DISPTMG skew. These are the behaviours real detection routines and demos rely
 * on. See docs/hardware/crtc-device.md §5. */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "hw/board.h"
#include "hw/crtc.h"

namespace {

// 1 MHz clock stub: every master cycle is a character cycle.
void cclk_tick(void*, const Bus*, Bus* out) { out->clk.crtc = true; }
size_t cone_size(const void*) { return 1; }
Device cclk_device() {
  static uint8_t d = 0;
  return Device{&d,
                "clk",
                cclk_tick,
                [](void*) {},
                cone_size,
                [](const void*, void*) {},
                [](void*, const void*) {}};
}

struct Rig {
  std::vector<uint8_t> mem = std::vector<uint8_t>(crtc_state_size());
  Board board;
  Device dev;
};

void make_crtc(Rig& rig, uint8_t type, bool with_clock = false) {
  rig.dev = crtc_init(rig.mem.data());
  crtc_set_type(&rig.dev, type);
  board_init(&rig.board);
  if (with_clock) board_add(&rig.board, cclk_device());
  board_add(&rig.board, rig.dev);
  board_reset(&rig.board);
}

void out_port(Rig& rig, uint16_t port, uint8_t val) {
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.iorq = true;
  rig.board.bus.cpu.wr = true;
  rig.board.bus.cpu.addr = port;
  rig.board.bus.cpu.data = val;
  board_tick(&rig.board);
}
uint8_t in_port(Rig& rig, uint16_t port) {
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.iorq = true;
  rig.board.bus.cpu.rd = true;
  rig.board.bus.cpu.addr = port;
  board_tick(&rig.board);
  return rig.board.bus.cpu.data;
}
uint8_t read_crtc_reg(Rig& rig, uint8_t reg, uint16_t read_port = 0xBF00) {
  out_port(rig, 0xBC00, reg);
  return in_port(rig, read_port);
}

// Program the standard CPC screen, then run and measure sync durations.
void std_screen(Rig& rig) {
  const uint8_t regs[10] = {63, 40, 46, 0x8E, 38, 0, 25, 30, 0, 7};
  for (uint8_t i = 0; i < 10; ++i) {
    out_port(rig, 0xBC00, i);
    out_port(rig, 0xBD00, regs[i]);
  }
}
struct SyncStats {
  int hsync_run;
  int vsync_lines;
  bool any_hsync;
};
SyncStats measure(Rig& rig, int ticks = 90000) {
  SyncStats st{0, 0, false};
  CrtcRegs r{};
  bool h_prev = false, v_prev = false;
  int hrun = 0, vlines = 0;
  for (int i = 0; i < ticks; ++i) {
    board_tick(&rig.board);
    crtc_peek(&rig.dev, &r);
    if (r.hsync) {
      st.any_hsync = true;
      hrun++;
    } else if (h_prev) {
      st.hsync_run = hrun;
      hrun = 0;
    }
    // count scanlines inside VSYNC via HSYNC starts while vsync high
    if (r.vsync && r.hsync && !h_prev) vlines++;
    if (!r.vsync && v_prev && vlines) {
      st.vsync_lines = vlines;
      vlines = 0;
    }
    h_prev = r.hsync != 0;
    v_prev = r.vsync != 0;
  }
  return st;
}

}  // namespace

TEST(CrtcTypes, RegisterReadabilityPerType) {
  for (uint8_t type = 0; type <= 3; ++type) {
    Rig rig;
    make_crtc(rig, type);
    out_port(rig, 0xBC00, 12);
    out_port(rig, 0xBD00, 0x30);  // R12 = 0x30
    // R14/R15 (cursor) readable on every type:
    out_port(rig, 0xBC00, 14);
    out_port(rig, 0xBD00, 0x12);
    EXPECT_EQ(read_crtc_reg(rig, 14), 0x12)
        << "R14 readable on type " << int(type);
    // R12 readable only on types 0/3:
    const uint8_t r12 = read_crtc_reg(rig, 12);
    if (type == 0 || type == 3)
      EXPECT_EQ(r12, 0x30) << "type " << int(type);
    else
      EXPECT_EQ(r12, 0x00) << "R12 write-only on type " << int(type);
    // R0 readable on no type:
    out_port(rig, 0xBC00, 0);
    out_port(rig, 0xBD00, 63);
    EXPECT_EQ(read_crtc_reg(rig, 0), 0x00)
        << "R0 never readable, type " << int(type);
  }
}

TEST(CrtcTypes, Type1QuirksStatusRegisterAndR31) {
  Rig rig;
  make_crtc(rig, 1);
  std_screen(rig);
  // vcc = 0 < R6 = 25 → not blanking → bit 5 clear.
  EXPECT_EQ(in_port(rig, 0xBE00), 0x00)
      << "type 1 status: in the displayed area";
  out_port(rig, 0xBC00, 6);
  out_port(rig, 0xBD00, 0);  // R6 = 0 → always blanking
  EXPECT_EQ(in_port(rig, 0xBE00), 0x20)
      << "type 1 status: vertical blanking bit";
  EXPECT_EQ(read_crtc_reg(rig, 31), 0xFF) << "type 1 answers 0xFF for R31";
}

TEST(CrtcTypes, StatusPortFloatsOnTypes0And2ReadsRegsOnType3) {
  for (uint8_t type : {uint8_t{0}, uint8_t{2}}) {
    Rig rig;
    make_crtc(rig, type);
    EXPECT_EQ(in_port(rig, 0xBE00), 0xFF) << "&BE floats on type " << int(type);
  }
  Rig rig;
  make_crtc(rig, 3);
  out_port(rig, 0xBC00, 12);
  out_port(rig, 0xBD00, 0x30);
  EXPECT_EQ(read_crtc_reg(rig, 12, /*read_port=*/0xBE00), 0x30)
      << "&BE is a second register-read port on type 3";
}

TEST(CrtcTypes, VsyncWidthFixed16OnTypes1And2) {
  for (uint8_t type = 0; type <= 3; ++type) {
    Rig rig;
    make_crtc(rig, type, /*with_clock=*/true);
    std_screen(rig);  // R3 = 0x8E → programmed VSYNC width 8
    const SyncStats st = measure(rig);
    const int expect = (type == 1 || type == 2) ? 16 : 8;
    EXPECT_EQ(st.vsync_lines, expect)
        << "type " << int(type) << ": VSYNC scanlines (R3 high nibble = 8)";
  }
}

TEST(CrtcTypes, HsyncWidthZeroMeansOffOn0And1But16On2And3) {
  for (uint8_t type = 0; type <= 3; ++type) {
    Rig rig;
    make_crtc(rig, type, /*with_clock=*/true);
    std_screen(rig);
    out_port(rig, 0xBC00, 3);
    out_port(rig, 0xBD00, 0x80);  // HSYNC width = 0
    const SyncStats st = measure(rig, 4000);
    if (type == 2 || type == 3) {
      EXPECT_TRUE(st.any_hsync) << "type " << int(type);
      EXPECT_EQ(st.hsync_run, 16) << "type " << int(type) << ": width 0 = 16";
    } else {
      EXPECT_FALSE(st.any_hsync)
          << "type " << int(type) << ": width 0 = no HSYNC";
    }
  }
}

TEST(CrtcTypes, R8SkewOnTypes0And3IgnoredOn1And2) {
  for (uint8_t type = 0; type <= 3; ++type) {
    Rig rig;
    make_crtc(rig, type, /*with_clock=*/true);
    std_screen(rig);
    out_port(rig, 0xBC00, 8);
    out_port(rig, 0xBD00, 0x30);  // skew bits = 3
    bool any_disp = false;
    CrtcRegs r{};
    for (int i = 0; i < 90000; ++i) {
      board_tick(&rig.board);
      crtc_peek(&rig.dev, &r);
      if (r.dispen) {
        any_disp = true;
        break;
      }
    }
    if (type == 0 || type == 3)
      EXPECT_FALSE(any_disp)
          << "type " << int(type) << ": skew 3 blanks the display";
    else
      EXPECT_TRUE(any_disp) << "type " << int(type) << ": R8 skew bits ignored";
  }
}
