/* plotter_hp7470a_test.cpp — the HP 7470A plotter Device: the parser-parity
 * oracle against the legacy HpglPlotter (acid test 1), the response protocol
 * end-to-end through the RS232 card on the same board (acid 2), XON/XOFF
 * thresholds, and a mid-plot snapshot. See docs/hardware/plotter-device.md. */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>  // std::memcmp — libstdc++ does not provide it transitively
#include <string>
#include <vector>

#include "hw/board.h"
#include "hw/plotter_hp7470a.h"
#include "hw/rs232.h"
#include "plotter.h"  // the legacy oracle

namespace {

constexpr uint32_t BIT_TIME = 128;        // divisor 1
constexpr uint32_t BYTE_TIME = 10 * BIT_TIME;
constexpr uint32_t DRAIN_TIME = 2 * BYTE_TIME;  // the device's service period

// ── plotter-only rig: the test drives serial.txd like the card would ────

struct PlotterRig {
  std::vector<uint8_t> mem;
  Board board;
  Device dev;

  void tick(bool txd_level = true) {
    board.bus.serial.txd = txd_level;
    board_tick(&board);
  }
};

void make_plotter_rig(PlotterRig& rig) {
  rig.mem.assign(plotter_hp7470a_state_size(), 0);
  rig.dev = plotter_hp7470a_init(rig.mem.data());
  board_init(&rig.board);
  board_add(&rig.board, rig.dev);
  board_reset(&rig.board);
  plotter_hp7470a_set_plugged(&rig.dev, 1);
  plotter_hp7470a_set_baud_divisor(&rig.dev, 1);
}

// Clock one 8N1 frame into the plotter's receive line.
void drive_frame(PlotterRig& rig, uint8_t byte) {
  for (uint32_t i = 0; i < BIT_TIME; i++) rig.tick(false);  // start
  for (int bit = 0; bit < 8; bit++) {
    const bool level = (byte >> bit) & 1;
    for (uint32_t i = 0; i < BIT_TIME; i++) rig.tick(level);
  }
  for (uint32_t i = 0; i < BIT_TIME; i++) rig.tick(true);  // stop
}

// Frame + enough idle that the input buffer drains before the next byte.
void drive_byte_paced(PlotterRig& rig, uint8_t byte) {
  drive_frame(rig, byte);
  for (uint32_t i = 0; i < DRAIN_TIME + 64; i++) rig.tick(true);
}

void feed_paced(PlotterRig& rig, const std::string& bytes) {
  for (char c : bytes) drive_byte_paced(rig, static_cast<uint8_t>(c));
  for (uint32_t i = 0; i < 3 * DRAIN_TIME; i++) rig.tick(true);
}

// ── the parity comparison ────────────────────────────────────────────────

void expect_parity(const std::string& hpgl) {
  HpglPlotter oracle;
  oracle.clear();
  for (char c : hpgl) oracle.feed_byte(static_cast<uint8_t>(c));

  PlotterRig rig;
  make_plotter_rig(rig);
  feed_paced(rig, hpgl);

  const PlotSeg* segs = nullptr;
  const size_t n = plotter_hp7470a_segments(&rig.dev, &segs);
  const auto& oseg = oracle.segments();
  ASSERT_EQ(n, oseg.size()) << "segment count diverged for: " << hpgl;
  for (size_t i = 0; i < n; i++) {
    SCOPED_TRACE("segment " + std::to_string(i) + " of: " + hpgl);
    EXPECT_EQ(static_cast<int>(segs[i].type), static_cast<int>(oseg[i].type));
    EXPECT_EQ(static_cast<int>(segs[i].pen), oseg[i].pen);
    EXPECT_EQ(static_cast<int>(segs[i].line_type), oseg[i].line_type);
    EXPECT_FLOAT_EQ(segs[i].x1, oseg[i].x1);
    EXPECT_FLOAT_EQ(segs[i].y1, oseg[i].y1);
    EXPECT_FLOAT_EQ(segs[i].x2, oseg[i].x2);
    EXPECT_FLOAT_EQ(segs[i].y2, oseg[i].y2);
    EXPECT_FLOAT_EQ(segs[i].radius, oseg[i].radius);
    EXPECT_FLOAT_EQ(segs[i].start_angle, oseg[i].start_angle);
    EXPECT_FLOAT_EQ(segs[i].sweep_angle, oseg[i].sweep_angle);
    EXPECT_STREQ(segs[i].text, oseg[i].text.c_str());
  }
}

// ── card + plotter rig for the wire-level response tests ─────────────────

struct DuoRig {
  std::vector<uint8_t> card_mem, plt_mem;
  Board board;
  Device card, plt;

  void tick() { board_tick(&board); }
};

void make_duo_rig(DuoRig& rig) {
  rig.card_mem.assign(rs232_state_size(), 0);
  rig.plt_mem.assign(plotter_hp7470a_state_size(), 0);
  rig.card = rs232_init(rig.card_mem.data());
  rig.plt = plotter_hp7470a_init(rig.plt_mem.data());
  board_init(&rig.board);
  board_add(&rig.board, rig.card);
  board_add(&rig.board, rig.plt);
  board_reset(&rig.board);
  rs232_set_plugged(&rig.card, 1);
  plotter_hp7470a_set_plugged(&rig.plt, 1);
  plotter_hp7470a_set_baud_divisor(&rig.plt, 1);
}

void duo_io_write(DuoRig& rig, uint16_t addr, uint8_t val) {
  rig.board.bus.cpu.iorq = true;
  rig.board.bus.cpu.wr = true;
  rig.board.bus.cpu.addr = addr;
  rig.board.bus.cpu.data = val;
  rig.tick();
  rig.tick();  // deassert
}

uint8_t duo_io_read(DuoRig& rig, uint16_t addr) {
  rig.board.bus.cpu.iorq = true;
  rig.board.bus.cpu.rd = true;
  rig.board.bus.cpu.addr = addr;
  rig.tick();
  const uint8_t val = rig.board.bus.cpu.data;
  rig.tick();  // deassert
  return val;
}

void duo_set_card_divisor(DuoRig& rig, uint16_t divisor) {
  duo_io_write(rig, 0xFBDF, 0x36);  // counter 0, LSB/MSB, mode 3
  duo_io_write(rig, 0xFBDC, static_cast<uint8_t>(divisor & 0xFF));
  duo_io_write(rig, 0xFBDC, static_cast<uint8_t>(divisor >> 8));
}

// Send one byte through the DART, waiting for TX-buffer-empty first.
void dart_send(DuoRig& rig, uint8_t byte) {
  for (int guard = 0; guard < 40; guard++) {
    if (duo_io_read(rig, 0xFADE) & 0x08) break;  // RR0 TX buffer empty
    for (int i = 0; i < 200; i++) rig.tick();
  }
  duo_io_write(rig, 0xFADC, byte);
}

// Collect the plotter's reply through the DART until CR (or timeout).
std::string dart_recv_line(DuoRig& rig) {
  std::string line;
  for (int guard = 0; guard < 4000; guard++) {
    if (duo_io_read(rig, 0xFADE) & 0x01) {  // RR0 RX available
      const char c = static_cast<char>(duo_io_read(rig, 0xFADC));
      line += c;
      if (c == '\r') return line;
    }
    for (int i = 0; i < 200; i++) rig.tick();
  }
  return line;
}

std::string query(DuoRig& rig, const std::string& q) {
  for (char c : q) dart_send(rig, static_cast<uint8_t>(c));
  return dart_recv_line(rig);
}

// ── a UART sniffer for the plotter's TX line (flow-control test) ─────────

struct Sniffer {
  uint8_t phase = 0, shift = 0, bits = 0;
  uint32_t cycle = 0;
  uint8_t prev = 1;
  std::vector<uint8_t> bytes;

  void sample(uint8_t line) {
    switch (phase) {
      case 0:
        if (prev && !line) {
          phase = 1;
          cycle = 0;
        }
        break;
      case 1:
        if (++cycle >= BIT_TIME / 2) {
          phase = line ? 0 : 2;
          cycle = 0;
          shift = 0;
          bits = 0;
        }
        break;
      case 2:
        if (++cycle >= BIT_TIME) {
          cycle = 0;
          shift = static_cast<uint8_t>((shift >> 1) | (line ? 0x80u : 0u));
          if (++bits == 8) phase = 3;
        }
        break;
      default:
        if (++cycle >= BIT_TIME) {
          if (line) bytes.push_back(shift);
          phase = 0;
          cycle = 0;
        }
        break;
    }
    prev = line;
  }
};

}  // namespace

// ── acid 1: the parser-parity oracle ─────────────────────────────────────

TEST(PlotterHp7470a, ParityFullCommandSet) {
  expect_parity(
      "IN;SP1;PA100,100;PD;PA500,100,500,500;PU;CI250;AA400,400,90;"
      "EA600,600;LT2;PA0,0;PR50,50;LBHello\x03"
      "DT*;SP1;PD;LBWorld*PU;SC0,100,0,100;IP100,100,900,900;"
      "IW0,0,5000,5000;DF;AR100,100,45;ER200,200;SI0.5,0.7;DI0,1;"
      "SP2;PD;PA1000,1000;PU;");
}

TEST(PlotterHp7470a, ParityLowercaseAndWhitespace) {
  expect_parity(
      "in;sp1;pa 200 , 300;pd;pa4000,4000;pu;PD;PA-100,-100;PU;"
      "sp0;PD;PA50,50;CI99;SP1;LT;CI42;PA;PD;PU;");
}

TEST(PlotterHp7470a, ParitySpaceSeparatedPairLosesSecondValue) {
  // "PA700 800" is ONE comma token; the prefix float parse yields 700 only,
  // so no move happens — on the oracle and on the Device alike.
  expect_parity("IN;SP1;PD;PA10,10;PA700 800;PA20,20;PU;");
}

TEST(PlotterHp7470a, ParityEscSequencesShedTheirEsc) {
  // The GSX init preamble: ESC is a dropped control char on the oracle; the
  // printable remainder parses as unknown mnemonics and is ignored.
  expect_parity(
      "\x1b.I81;;17:\x1b.N;19:IN;SP1;PD;PA10,10;PU;");
}

TEST(PlotterHp7470a, ParityScalingAndRelativeMoves) {
  expect_parity(
      "IN;SC0,10,0,10;IP0,0,1000,1000;SP1;PD;PA5,5;PR1,1,2,0;PU;CI3;"
      "AA5,5,180;SC;PD;PA100,100;PU;");
}

TEST(PlotterHp7470a, ParityLabelsAndTerminators) {
  expect_parity(
      "IN;SP1;LB\x03"                       // empty label: no segment
      "PA100,200;LBabc def\x03"             // spaces inside label text
      "DT#;SI1,1;DI1,0.5;LBwith#SP2;LBpen2\x03;PU;");
}

TEST(PlotterHp7470a, ParityCrLfAsWhitespace) {
  expect_parity("IN;\r\nSP1;\rPD;\nPA1,1;\r\nPA2,2;PU;");
}

// ── acid 2: the response protocol, end-to-end through the card ───────────

TEST(PlotterHp7470a, RespondsToStatusQuery) {
  DuoRig rig;
  make_duo_rig(rig);
  duo_set_card_divisor(rig, 1);
  EXPECT_EQ(query(rig, "OS;"), "16\r");
}

TEST(PlotterHp7470a, RespondsToDimensionAndIdQueries) {
  DuoRig rig;
  make_duo_rig(rig);
  duo_set_card_divisor(rig, 1);
  EXPECT_EQ(query(rig, "OD;"), "0,0,10300,7650\r");
  EXPECT_EQ(query(rig, "OI;"), "7470A\r");
}

TEST(PlotterHp7470a, EnqReportsBufferSpace) {
  DuoRig rig;
  make_duo_rig(rig);
  duo_set_card_divisor(rig, 1);
  EXPECT_EQ(query(rig, "\x05"), "128\r");  // idle buffer: the oracle constant
}

// ── flow control ─────────────────────────────────────────────────────────

TEST(PlotterHp7470a, XoffWhenBufferFillsXonWhenDrained) {
  PlotterRig rig;
  make_plotter_rig(rig);
  Sniffer sniff;

  // Back-to-back frames outpace the 2-byte-time service rate: the buffer
  // climbs at one byte per two frames. 520 bytes crosses the XOFF line
  // (free < 32, i.e. fill > 223) with margin.
  for (int i = 0; i < 520; i++) {
    const uint8_t byte = 'A';
    for (uint32_t k = 0; k < BIT_TIME; k++) {
      rig.tick(false);
      sniff.sample(rig.board.bus.serial.rxd);
    }
    for (int bit = 0; bit < 8; bit++) {
      const bool level = (byte >> bit) & 1;
      for (uint32_t k = 0; k < BIT_TIME; k++) {
        rig.tick(level);
        sniff.sample(rig.board.bus.serial.rxd);
      }
    }
    for (uint32_t k = 0; k < BIT_TIME; k++) {
      rig.tick(true);
      sniff.sample(rig.board.bus.serial.rxd);
    }
  }
  PlotterRegs regs{};
  plotter_hp7470a_peek(&rig.dev, &regs);
  EXPECT_GT(regs.buffer_fill, 200);
  EXPECT_EQ(regs.flow_stopped, 1);
  ASSERT_FALSE(sniff.bytes.empty());
  bool saw_xoff = false;
  for (uint8_t b : sniff.bytes) saw_xoff |= (b == 0x13);
  EXPECT_TRUE(saw_xoff) << "XOFF never transmitted";

  // Stop sending; the buffer drains at one byte per two byte-times and XON
  // follows once free space recovers past 128.
  for (uint32_t k = 0; k < 300 * DRAIN_TIME; k++) {
    rig.tick(true);
    sniff.sample(rig.board.bus.serial.rxd);
  }
  plotter_hp7470a_peek(&rig.dev, &regs);
  EXPECT_EQ(regs.buffer_fill, 0);
  EXPECT_EQ(regs.flow_stopped, 0);
  bool saw_xon = false;
  for (uint8_t b : sniff.bytes) saw_xon |= (b == 0x11);
  EXPECT_TRUE(saw_xon) << "XON never transmitted";
}

// ── snapshot mid-plot ────────────────────────────────────────────────────

TEST(PlotterHp7470a, SnapshotMidPlotResumesIdentically) {
  PlotterRig a;
  make_plotter_rig(a);
  feed_paced(a, "IN;SP1;PA0,0;PD;PA100,100;");

  std::vector<uint8_t> blob(a.dev.state_size(a.dev.self));
  a.dev.save(a.dev.self, blob.data());

  PlotterRig b;
  make_plotter_rig(b);
  b.dev.load(b.dev.self, blob.data());

  const std::string rest = "PA200,50;PU;CI40;";
  feed_paced(a, rest);
  feed_paced(b, rest);

  const PlotSeg *sa = nullptr, *sb = nullptr;
  const size_t na = plotter_hp7470a_segments(&a.dev, &sa);
  const size_t nb = plotter_hp7470a_segments(&b.dev, &sb);
  ASSERT_EQ(na, nb);
  EXPECT_EQ(std::memcmp(sa, sb, na * sizeof(PlotSeg)), 0);
}

// ── page management ──────────────────────────────────────────────────────

TEST(PlotterHp7470a, ClearPageTearsTheSheetOff) {
  PlotterRig rig;
  make_plotter_rig(rig);
  feed_paced(rig, "IN;SP1;PD;PA100,100;PU;");
  PlotterRegs regs{};
  plotter_hp7470a_peek(&rig.dev, &regs);
  const uint32_t rev_before = regs.page_rev;
  const PlotSeg* segs = nullptr;
  ASSERT_EQ(plotter_hp7470a_segments(&rig.dev, &segs), 1u);

  plotter_hp7470a_clear_page(&rig.dev);
  EXPECT_EQ(plotter_hp7470a_segments(&rig.dev, &segs), 0u);
  plotter_hp7470a_peek(&rig.dev, &regs);
  EXPECT_GT(regs.page_rev, rev_before);
}

TEST(PlotterHp7470a, UnpluggedIgnoresTheWire) {
  PlotterRig rig;
  make_plotter_rig(rig);
  plotter_hp7470a_set_plugged(&rig.dev, 0);
  feed_paced(rig, "IN;SP1;PD;PA100,100;PU;");
  const PlotSeg* segs = nullptr;
  EXPECT_EQ(plotter_hp7470a_segments(&rig.dev, &segs), 0u);
  EXPECT_TRUE(rig.board.bus.serial.rxd) << "wire rests at mark";
}
