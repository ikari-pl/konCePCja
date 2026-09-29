// CPCT v0 bus tap (src/hw/cpct_tap.h): the byte layout against the spec
// (~/src/cpc/cpcien/docs/trace-format.md), the recording rule on hand-driven
// bus cycles, and a real board running a tiny ROM with the tap attached.

#include "hw/cpct_tap.h"

#include <gtest/gtest.h>

#ifndef _WIN32
#include <sys/resource.h>
#endif

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "hw/board.h"
#include "hw/crtc.h"
#include "hw/gate_array.h"
#include "hw/memory.h"
#include "hw/ppi.h"
#include "hw/psg.h"
#include "hw/z80.h"

namespace {

struct Tap {
  std::vector<uint8_t> mem;
  Device dev;
  CpctTapOwner owner;
  Tap()
      : mem(cpct_tap_state_size()),
        dev(cpct_tap_init(mem.data())),
        owner(&dev) {
    dev.reset(dev.self);
  }
  Tap(const Tap&) = delete;
  Tap& operator=(const Tap&) = delete;

  // Hold `bus` on the committed side for `n` master cycles.
  void hold(const Bus& bus, int n) {
    Bus out{};
    for (int i = 0; i < n; i++) dev.tick(dev.self, &bus, &out);
  }
  std::vector<CpctRecord> records(size_t* dropped = nullptr) const {
    size_t n = 0;
    const CpctRecord* r = cpct_tap_records(&dev, &n, dropped);
    return std::vector<CpctRecord>(r, r + n);
  }
};

Bus idle() {
  Bus b{};
  b.cpu.data = 0xFF;
  return b;
}
Bus fetch(uint16_t addr, uint8_t op) {
  Bus b = idle();
  b.cpu.addr = addr;
  b.cpu.data = op;
  b.cpu.m1 = b.cpu.mreq = b.cpu.rd = true;
  return b;
}

// A temp file removed on scope exit, even when an ASSERT returns early.
struct TempFile {
  std::filesystem::path path;
  explicit TempFile(const char* name)
      : path(std::filesystem::temp_directory_path() / name) {}
  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
};

Bus with_irq(Bus b, bool irq) {
  b.cpu.irq = irq;
  return b;
}
Bus with_reset(Bus b, bool reset) {
  b.cpu.reset = reset;
  return b;
}

std::vector<uint8_t> slurp(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST(CpctTap, HeaderAndRecordMatchTheV0LayoutByteForByte) {
  uint8_t h[CPCT_HEADER_SIZE];
  cpct_encode_header(h, 1, 3, 0x0102030405060708ULL);
  const uint8_t want_h[32] = {
      'C',  'P',  'C',  'T',                           // magic
      0x00, 0x00,                                      // version 0
      0x01,                                            // crtc_type
      0x03,                                            // machine 6128+
      0x00, 0x09, 0x3D, 0x00,                          // crystal_hz 4000000 LE
      0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,  // start_cycle LE
      0,    0,    0,    0,    0,    0,    0,    0,    0, 0, 0, 0,  // reserved
  };
  EXPECT_EQ(0, std::memcmp(h, want_h, sizeof h));

  const CpctRecord r{0xAABBCCDD, CPCT_M1 | CPCT_MEM_RD | CPCT_HALT, 0x76,
                     0x1234};
  uint8_t b[CPCT_RECORD_SIZE];
  cpct_encode_record(b, &r);
  const uint8_t want_r[8] = {0xDD, 0xCC, 0xBB, 0xAA, 0xC4, 0x76, 0x34, 0x12};
  EXPECT_EQ(0, std::memcmp(b, want_r, sizeof b));
}

TEST(CpctTap, PowerOnStartsWithAResetMarker) {
  Tap t;
  const auto r = t.records();
  ASSERT_EQ(1u, r.size());
  EXPECT_EQ(0u, r[0].cycle);
  EXPECT_EQ(CPCT_RESET, r[0].flags);
}

TEST(CpctTap, OneRecordPerStrobeStampedAtItsStartIdleOmitted) {
  Tap t;
  t.hold(idle(), 8);               // 8 idle master cycles: no record
  t.hold(fetch(0x4000, 0x3E), 6);  // opcode fetch starting at master 8 = T 2
  t.hold(idle(), 10);
  Bus wr = idle();
  wr.cpu.addr = 0x8000;
  wr.cpu.data = 0x55;
  wr.cpu.mreq = wr.cpu.wr = true;
  t.hold(wr, 4);  // master 24 = T 6
  Bus rf = idle();
  rf.cpu.mreq = rf.cpu.rfsh = true;  // refresh: not an access
  t.hold(rf, 4);
  Bus ack = idle();
  ack.cpu.m1 = ack.cpu.iorq = ack.cpu.rd = true;  // /INT acknowledge
  t.hold(ack, 4);
  Bus io = idle();
  io.cpu.addr = 0x7F10;
  io.cpu.data = 0x8D;
  io.cpu.iorq = io.cpu.wr = true;
  t.hold(io, 4);  // master 36 = T 9
  Bus in = idle();
  in.cpu.addr = 0xF5FF;
  in.cpu.data = 0x1E;
  in.cpu.iorq = in.cpu.rd = true;
  t.hold(in, 4);  // master 40 = T 10
  t.hold(idle(), 1);

  const auto r = t.records();
  ASSERT_EQ(5u, r.size());
  EXPECT_EQ(CPCT_M1 | CPCT_MEM_RD, r[1].flags);
  EXPECT_EQ(2u, r[1].cycle);
  EXPECT_EQ(0x4000, r[1].addr);
  EXPECT_EQ(0x3E, r[1].data);
  EXPECT_EQ(CPCT_MEM_WR, r[2].flags);
  EXPECT_EQ(6u, r[2].cycle);
  EXPECT_EQ(0x8000, r[2].addr);
  EXPECT_EQ(0x55, r[2].data);
  EXPECT_EQ(CPCT_IO_WR, r[3].flags);
  EXPECT_EQ(9u, r[3].cycle);
  EXPECT_EQ(0x7F10, r[3].addr);
  EXPECT_EQ(CPCT_IO_RD, r[4].flags);
  EXPECT_EQ(10u, r[4].cycle);
  EXPECT_EQ(0x1E, r[4].data);
}

TEST(CpctTap, IntEdgeInsideAnAccessFollowsItsRecordAndCarriesHalt) {
  Tap t;
  Bus f = fetch(0x0007, 0x76);
  f.cpu.halt = true;
  t.hold(f, 4);  // T 0..1, access opens at master 0
  f.cpu.irq = true;
  t.hold(f, 4);  // /INT falls at master 4 = T 1, inside the fetch
  Bus i = idle();
  i.cpu.irq = true;  // level stays low: no second edge
  i.cpu.halt = true;
  t.hold(i, 8);

  const auto r = t.records();
  ASSERT_EQ(3u, r.size());
  EXPECT_EQ(CPCT_M1 | CPCT_MEM_RD | CPCT_HALT, r[1].flags);
  EXPECT_EQ(0u, r[1].cycle);
  EXPECT_EQ(CPCT_INT | CPCT_HALT, r[2].flags);
  EXPECT_EQ(1u, r[2].cycle);
  EXPECT_EQ(0, r[2].addr);
  EXPECT_EQ(0, r[2].data);
}

TEST(CpctTap, OverflowIsCountedAndWrittenAsAGapRecord) {
  Tap t;
  cpct_tap_set_capacity(&t.dev, 2);  // the RESET marker + one access
  for (int k = 0; k < 3; k++) {
    t.hold(fetch(static_cast<uint16_t>(k), 0), 4);
    t.hold(idle(), 4);
  }
  size_t dropped = 0;
  EXPECT_EQ(2u, t.records(&dropped).size());
  EXPECT_EQ(2u, dropped);

  const auto path =
      std::filesystem::temp_directory_path() / "cpct_tap_test_gap.cpct";
  ASSERT_EQ(0, cpct_tap_write(&t.dev, path.string().c_str(), 0, 2));
  const auto bytes = slurp(path);
  std::filesystem::remove(path);
  ASSERT_EQ(32u + 3 * 8, bytes.size());
  const uint8_t* gap = bytes.data() + 32 + 2 * 8;
  // The spec's "first cycle after the gap": the first dropped record's own
  // cycle, the second fetch, which opened at master 8 = T 2.
  EXPECT_EQ(2, gap[0]);
  EXPECT_EQ(CPCT_GAP, gap[4]);
  EXPECT_EQ(0, gap[5]);
  EXPECT_EQ(2, gap[6] | (gap[7] << 8));
}

// The drop and the write are far apart here, so a write-time stamp cannot
// masquerade as the gap-start stamp.
TEST(CpctTap, GapCycleIsWhereTheLossBeganNotWhereTheWriteHappened) {
  Tap t;
  cpct_tap_set_capacity(&t.dev, 2);  // the RESET marker + one access
  t.hold(fetch(0x1000, 0), 4);       // stored: opens at master 0 = T 0
  t.hold(idle(), 4);
  t.hold(fetch(0x2000, 0), 4);  // dropped: opens at master 8 = T 2
  t.hold(idle(), 4);
  t.hold(idle(), 40000);  // 10000 T-states pass before the trace is written

  size_t dropped = 0;
  ASSERT_EQ(2u, t.records(&dropped).size());
  ASSERT_EQ(1u, dropped);

  TempFile tmp("cpct_tap_test_gap_cycle.cpct");
  ASSERT_EQ(0, cpct_tap_write(&t.dev, tmp.path.string().c_str(), 0, 2));
  const auto bytes = slurp(tmp.path);
  ASSERT_EQ(32u + 3 * 8, bytes.size());
  const uint8_t* gap = bytes.data() + 32 + 2 * 8;
  const uint32_t cycle = gap[0] | (gap[1] << 8) | (gap[2] << 16) |
                         (static_cast<uint32_t>(gap[3]) << 24);
  EXPECT_EQ(CPCT_GAP, gap[4]);
  EXPECT_EQ(2u, cycle);  // not 10004, the tap's cycle at write time
}

// A real board: Z80 + GA + CRTC + PPI + PSG + memory, a 16 KB lower ROM that
// programs the CRTC, enables IM 1 interrupts, touches memory and I/O, then
// HALTs in a loop.
TEST(CpctTap, BoardRunRecordsFetchesIoAndInterruptEdges) {
  std::vector<uint8_t> rom(0x4000, 0x00);
  const uint8_t prog[] = {
      0xF3,              // 0000 DI
      0x31, 0x00, 0xC0,  // 0001 LD SP,C000
      0xED, 0x56,        // 0004 IM 1
      0x21, 0x40, 0x00,  // 0006 LD HL,0040   CRTC table
      0xAF,              // 0009 XOR A
      0x01, 0x00, 0xBC,  // 000A LD BC,BC00   loop: select register A
      0xED, 0x79,        // 000D OUT (C),A
      0x04,              // 000F INC B
      0x56,              // 0010 LD D,(HL)
      0xED, 0x51,        // 0011 OUT (C),D    write it
      0x23,              // 0013 INC HL
      0x3C,              // 0014 INC A
      0xFE, 0x10,        // 0015 CP 16
      0x20, 0xF1,        // 0017 JR NZ,000A
      0x01, 0x00, 0x7F,  // 0019 LD BC,7F00
      0xED, 0x49,        // 001C OUT (C),C
      0x06, 0xF5,        // 001E LD B,F5
      0xED, 0x78,        // 0020 IN A,(C)
      0x32, 0x00, 0x80,  // 0022 LD (8000),A
      0xFB,              // 0025 EI
      0x76,              // 0026 HALT
      0x18, 0xFD,        // 0027 JR 0026
  };
  std::copy(std::begin(prog), std::end(prog), rom.begin());
  rom[0x38] = 0xFB;  // EI
  rom[0x39] = 0xC9;  // RET
  // The firmware's CRTC set: without HSYNC the Gate Array never raises /INT.
  const uint8_t crtc[16] = {63, 40, 46, 0x8E, 38,   0, 25, 30,
                            0,  7,  0,  0,    0x30, 0, 0,  0};
  std::copy(std::begin(crtc), std::end(crtc), rom.begin() + 0x40);

  std::vector<uint8_t> gm(ga_state_size()), cm(crtc_state_size()),
      pm(ppi_state_size()), sm(psg_state_size()), mm(mem_state_size()),
      zm(z80_state_size()), tm(cpct_tap_state_size());
  Device g = ga_init(gm.data()), c = crtc_init(cm.data()),
         p = ppi_init(pm.data()), s = psg_init(sm.data()),
         m = mem_init(mm.data()), z = z80_init(zm.data()),
         tap = cpct_tap_init(tm.data());
  CpctTapOwner tap_owner(&tap);
  Board board;
  board_init(&board);
  for (const Device& d : {g, c, p, s, m, z, tap}) board_add(&board, d);
  board_reset(&board);
  mem_load_lower_rom(&m, rom.data(), rom.size());

  constexpr long kFrame = 16 * 20000;  // master cycles per 50 Hz frame
  for (long i = 0; i < 3 * kFrame; i++) board_tick(&board);

  size_t n = 0, dropped = 0;
  const CpctRecord* r = cpct_tap_records(&tap, &n, &dropped);
  ASSERT_GT(n, 10u);
  EXPECT_EQ(0u, dropped);
  EXPECT_EQ(CPCT_RESET, r[0].flags);

  // Idle cycles omitted: far fewer records than T-states elapsed, and none
  // without a flag.
  EXPECT_LT(n, static_cast<size_t>(3 * kFrame / 4 / 2));
  int ints = 0, halt_accesses = 0, halt_markers = 0, isr_fetches = 0;
  std::vector<uint32_t> int_at;
  bool out7f = false, inf5 = false, wr8000 = false;
  for (size_t i = 0; i < n; i++) {
    if (i) {
      EXPECT_LE(r[i - 1].cycle, r[i].cycle) << "record " << i;
    }
    EXPECT_NE(0, r[i].flags) << "record " << i;
    if (r[i].flags & CPCT_INT) {
      int_at.push_back(r[i].cycle);
      ints++;
    }
    if (r[i].flags & CPCT_HALT) {
      if (r[i].flags & 0xF8)
        halt_accesses++;
      else
        halt_markers++;
    }
    if ((r[i].flags & CPCT_M1) && r[i].addr == 0x0038) isr_fetches++;
    if (r[i].flags == CPCT_IO_WR && r[i].addr == 0x7F00) out7f = true;
    if (r[i].flags == CPCT_IO_RD && (r[i].addr >> 8) == 0xF5) inf5 = true;
    if (r[i].flags == CPCT_MEM_WR && r[i].addr == 0x8000) wr8000 = true;
  }
  // Opcode fetches carry M1|MEM_RD with the opcode byte.
  EXPECT_EQ(CPCT_M1 | CPCT_MEM_RD, r[1].flags);
  EXPECT_EQ(0x0000, r[1].addr);
  EXPECT_EQ(0xF3, r[1].data);
  EXPECT_TRUE(out7f);
  EXPECT_TRUE(inf5);
  EXPECT_TRUE(wr8000);
  // The GA raises /INT every 52 scanlines: 300 per second, 6 per frame. The
  // first intervals after the CRTC is programmed are irregular (the VSYNC
  // resync of the GA's line counter); the steady state is 52 x 64 us exactly.
  EXPECT_GE(ints, 14);
  EXPECT_LE(ints, 19);
  ASSERT_GE(int_at.size(), 8u);
  for (size_t k = int_at.size() - 6; k < int_at.size(); k++)
    EXPECT_EQ(13312u, int_at[k] - int_at[k - 1]) << "interval " << k;
  EXPECT_GE(isr_fetches, ints - 1);  // every edge but maybe the last is taken
  // This Z80 runs no bus cycles while halted (the known artefact in
  // cpct_tap.h), so HALT is never on an access: it rides the /INT markers
  // that end each halt -- every one but possibly the first, which can fall
  // before the program reaches its HALT.
  EXPECT_EQ(0, halt_accesses);
  EXPECT_GE(halt_markers, ints - 1);
}

TEST(CpctTap, ResetFallIsOneLineEventPerEdge) {
  Tap t;
  t.hold(idle(), 8);
  t.hold(with_reset(idle(), true), 8);  // /RESET falls at master 8 = T 2
  t.hold(idle(), 4);
  Bus h = with_reset(idle(), true);
  h.cpu.halt = true;
  t.hold(h, 4);  // falls again at master 20 = T 5, with /HALT low

  const auto r = t.records();
  ASSERT_EQ(3u, r.size());
  EXPECT_EQ(CPCT_RESET, r[1].flags);
  EXPECT_EQ(2u, r[1].cycle);
  EXPECT_EQ(CPCT_RESET | CPCT_HALT, r[2].flags);
  EXPECT_EQ(5u, r[2].cycle);
}

TEST(CpctTap, EdgeOnTheCycleAnAccessOpensFollowsItsRecord) {
  Tap t;
  t.hold(idle(), 4);
  t.hold(with_irq(fetch(0x0100, 0x00), true), 4);  // both start at master 4
  t.hold(with_irq(idle(), true), 4);

  const auto r = t.records();
  ASSERT_EQ(3u, r.size());
  EXPECT_EQ(CPCT_M1 | CPCT_MEM_RD, r[1].flags);
  EXPECT_EQ(1u, r[1].cycle);
  EXPECT_EQ(CPCT_INT, r[2].flags);
  EXPECT_EQ(1u, r[2].cycle);
}

TEST(CpctTap, MoreThanFourEdgesInOneAccessMergeIntoTheLastSlot) {
  Tap t;
  const Bus f = fetch(0x0200, 0x00);
  // Five edges while one fetch stays open: INT@4, RESET@8, INT@16,
  // RESET@24, INT@32 (master cycles).
  t.hold(f, 4);
  t.hold(with_irq(f, true), 4);
  t.hold(with_reset(with_irq(f, true), true), 4);
  t.hold(f, 4);
  t.hold(with_irq(f, true), 4);
  t.hold(f, 4);
  t.hold(with_reset(f, true), 4);
  t.hold(f, 4);
  t.hold(with_irq(f, true), 4);
  t.hold(idle(), 4);

  const auto r = t.records();
  ASSERT_EQ(6u, r.size());  // RESET, the fetch, four markers
  EXPECT_EQ(CPCT_M1 | CPCT_MEM_RD, r[1].flags);
  EXPECT_EQ(CPCT_INT, r[2].flags);
  EXPECT_EQ(1u, r[2].cycle);
  EXPECT_EQ(CPCT_RESET, r[3].flags);
  EXPECT_EQ(2u, r[3].cycle);
  EXPECT_EQ(CPCT_INT, r[4].flags);
  EXPECT_EQ(4u, r[4].cycle);
  // The fifth edge folds its flag into the fourth slot, keeping its stamp.
  EXPECT_EQ(CPCT_RESET | CPCT_INT, r[5].flags);
  EXPECT_EQ(6u, r[5].cycle);
}

TEST(CpctTap, DeviceResetAfterActivityStartsAFreshStream) {
  Tap t;
  cpct_tap_set_capacity(&t.dev, 3);
  for (int k = 0; k < 4; k++) {
    t.hold(fetch(static_cast<uint16_t>(k), 0), 4);
    t.hold(idle(), 4);
  }
  t.hold(fetch(0x10, 0), 2);                  // an access left open ...
  t.hold(with_irq(fetch(0x10, 0), true), 2);  // ... with a marker behind it
  size_t dropped = 0;
  ASSERT_GT(t.records(&dropped).size(), 1u);
  ASSERT_GT(dropped, 0u);

  t.dev.reset(t.dev.self);
  auto r = t.records(&dropped);
  ASSERT_EQ(1u, r.size());
  EXPECT_EQ(CPCT_RESET, r[0].flags);
  EXPECT_EQ(0u, r[0].cycle);
  EXPECT_EQ(0u, dropped);

  // No stale open access, pending marker or /INT level survives the reset.
  t.hold(idle(), 4);
  t.hold(fetch(0x20, 0x3E), 4);
  t.hold(idle(), 1);
  r = t.records();
  ASSERT_EQ(2u, r.size());
  EXPECT_EQ(CPCT_M1 | CPCT_MEM_RD, r[1].flags);
  EXPECT_EQ(1u, r[1].cycle);
  EXPECT_EQ(0x20, r[1].addr);
}

TEST(CpctTap, GapLostCountSaturatesAtFfff) {
  Tap t;
  cpct_tap_set_capacity(&t.dev, 1);  // only the RESET marker fits
  for (int k = 0; k < 70000; k++) {
    t.hold(fetch(0, 0), 4);
    t.hold(idle(), 4);
  }
  size_t dropped = 0;
  t.records(&dropped);
  ASSERT_EQ(70000u, dropped);

  TempFile tmp("cpct_tap_test_saturate.cpct");
  ASSERT_EQ(0, cpct_tap_write(&t.dev, tmp.path.string().c_str(), 0, 2));
  const auto bytes = slurp(tmp.path);
  ASSERT_EQ(32u + 2 * 8, bytes.size());
  const uint8_t* gap = bytes.data() + 32 + 8;
  EXPECT_EQ(CPCT_GAP, gap[4]);
  EXPECT_EQ(0xFFFF, gap[6] | (gap[7] << 8));
}

TEST(CpctTap, WriteWithoutLossIsHeaderPlusRecordsExactly) {
  Tap t;
  t.hold(fetch(0x1234, 0xAB), 4);
  t.hold(idle(), 4);
  const auto r = t.records();
  ASSERT_EQ(2u, r.size());

  TempFile tmp("cpct_tap_test_exact.cpct");
  ASSERT_EQ(0, cpct_tap_write(&t.dev, tmp.path.string().c_str(), 2, 1));
  const auto bytes = slurp(tmp.path);
  ASSERT_EQ(32u + 2 * 8, bytes.size());  // no GAP record
  uint8_t want[CPCT_HEADER_SIZE];
  cpct_encode_header(want, 2, 1, 0);
  EXPECT_EQ(0, std::memcmp(bytes.data(), want, sizeof want));
  for (size_t i = 0; i < r.size(); i++) {
    uint8_t rec[CPCT_RECORD_SIZE];
    cpct_encode_record(rec, &r[i]);
    EXPECT_EQ(0, std::memcmp(bytes.data() + 32 + 8 * i, rec, sizeof rec))
        << "record " << i;
  }
}

TEST(CpctTap, WriteToAnUnopenablePathFails) {
  Tap t;
  const auto path = std::filesystem::temp_directory_path() /
                    "cpct_tap_test_no_such_dir" / "trace.cpct";
  EXPECT_EQ(-1, cpct_tap_write(&t.dev, path.string().c_str(), 0, 2));
}

// A CPCT v0 file has no record count or trailer, so a truncated one parses as
// a shorter but valid capture: a failed write must leave nothing behind.
TEST(CpctTap, AFailedWriteLeavesNoPartialTraceOnDisk) {
#ifdef _WIN32
  GTEST_SKIP() << "no portable way to make a write fail mid-file on Windows";
#else
  Tap t;
  for (int k = 0; k < 8; k++) {
    t.hold(fetch(static_cast<uint16_t>(k), 0), 4);
    t.hold(idle(), 4);
  }
  TempFile tmp("cpct_tap_test_partial.cpct");

  // A 16-byte file size limit: the 32-byte header alone cannot land.
  struct rlimit old_limit{};
  ASSERT_EQ(0, getrlimit(RLIMIT_FSIZE, &old_limit));
  struct rlimit small = old_limit;
  small.rlim_cur = 16;
  ASSERT_EQ(0, setrlimit(RLIMIT_FSIZE, &small));
  // Exceeding the limit also raises SIGXFSZ, whose default action is fatal.
  void (*old_handler)(int) = std::signal(SIGXFSZ, SIG_IGN);

  const int rc = cpct_tap_write(&t.dev, tmp.path.string().c_str(), 0, 2);

  std::signal(SIGXFSZ, old_handler);
  ASSERT_EQ(0, setrlimit(RLIMIT_FSIZE, &old_limit));

  EXPECT_EQ(-1, rc);
  EXPECT_FALSE(std::filesystem::exists(tmp.path))
      << "a partial trace was left at " << tmp.path;
#endif
}

TEST(CpctTap, WriteIncludesMarkersQueuedBehindAnOpenAccess) {
  Tap t;
  t.hold(fetch(0x0300, 0x76), 4);  // opens at master 0, never closes
  t.hold(with_irq(fetch(0x0300, 0x76), true), 4);  // /INT falls at T 1

  TempFile tmp("cpct_tap_test_pending.cpct");
  ASSERT_EQ(0, cpct_tap_write(&t.dev, tmp.path.string().c_str(), 0, 2));
  const auto bytes = slurp(tmp.path);
  // RESET, then the INT marker; the unfinished fetch has no record yet.
  ASSERT_EQ(32u + 2 * 8, bytes.size());
  const uint8_t* m = bytes.data() + 32 + 8;
  EXPECT_EQ(1, m[0]);
  EXPECT_EQ(CPCT_INT, m[4]);
}
