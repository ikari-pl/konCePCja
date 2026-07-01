/* z80_fuse_test.cpp — runs the FUSE Z80 conformance corpus against our Device.
 *
 * The FUSE test suite (z80/tests/tests.in + tests.expected, by Ian Collier /
 * Philip Kendall, GPL) is the reference oracle for Z80 CPU behaviour: 1356
 * single-instruction tests, each pinning the FULL final architectural state —
 * every register including the alternate set, IX/IY, SP, PC, the undocumented
 * flag bits in F, MEMPTR (WZ), I/R, the IFF flip-flops, HALT, the exact T-state
 * total, and memory. Passing it is the definitive accuracy proof the code review
 * flagged as not-yet-wired.
 *
 * We ignore the per-T-state bus-event trace in tests.expected (it encodes ZX
 * Spectrum memory contention, irrelevant to a generic Z80 / the CPC) and compare
 * the resulting state + memory + T-states, which is a complete CPU conformance
 * check.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "hw/board.h"
#include "hw/z80.h"

namespace {

struct Ram {
  uint8_t cells[0x10000];
};
void fram_tick(void* self, const Bus* in, Bus* out) {
  Ram* ram = static_cast<Ram*>(self);
  if (in->cpu.mreq && in->cpu.wr) {
    ram->cells[in->cpu.addr] = in->cpu.data;
  } else if (in->cpu.mreq && in->cpu.rd) {
    out->cpu.data = ram->cells[in->cpu.addr];
  }
}
void fram_reset(void*) {}
size_t fram_size(const void*) { return sizeof(Ram); }
void fram_save(const void* s, void* b) { std::memcpy(b, s, sizeof(Ram)); }
void fram_load(void* s, const void* b) { std::memcpy(s, b, sizeof(Ram)); }
Device fram_device(Ram* s) {
  return Device{s, "ram", fram_tick, fram_reset, fram_size, fram_save, fram_load};
}

// FUSE's test readport() convention: an unconnected port returns the high byte
// of the port address (B for IN r,(C), etc.). Model it so input ops match.
void fio_tick(void* self, const Bus* in, Bus* out) {
  (void)self;
  if (in->cpu.iorq && in->cpu.rd) out->cpu.data = static_cast<uint8_t>(in->cpu.addr >> 8);
}
void fio_reset(void*) {}
size_t fio_size(const void*) { return 1; }
void fio_save(const void*, void* b) { *static_cast<uint8_t*>(b) = 0; }
void fio_load(void*, const void*) {}
Device fio_device() {
  static uint8_t dummy = 0;
  return Device{&dummy, "io", fio_tick, fio_reset, fio_size, fio_save, fio_load};
}

void fclk_tick(void*, const Bus*, Bus* out) { out->clk.cpu = true; }
void fclk_reset(void*) {}
size_t fclk_size(const void*) { return 1; }
void fclk_save(const void*, void* b) { *static_cast<uint8_t*>(b) = 0; }
void fclk_load(void*, const void*) {}
Device fclk_device() {
  static uint8_t dummy = 0;
  return Device{&dummy, "clk", fclk_tick, fclk_reset, fclk_size, fclk_save, fclk_load};
}

struct MemBlock {
  uint16_t addr;
  std::vector<uint8_t> bytes;
};

struct FuseTest {
  std::string name;
  Z80Regs regs{};        // initial state (or final, for the expected side)
  uint64_t end_tstates;  // run until tstates >= this (FUSE semantics)
  std::vector<MemBlock> mem;
  bool valid = false;
};

std::vector<std::string> split_ws(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream iss(s);
  std::string tok;
  while (iss >> tok) out.push_back(tok);
  return out;
}
uint32_t hx(const std::string& s) { return static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 16)); }

// Read one .in test starting at line index i; advances i. Returns valid=false at EOF.
FuseTest parse_in(const std::vector<std::string>& lines, size_t& i) {
  FuseTest t;
  while (i < lines.size() && split_ws(lines[i]).empty()) i++;  // skip blanks
  if (i >= lines.size()) return t;
  t.name = lines[i++];
  auto w = split_ws(lines[i++]);  // 13 words
  if (w.size() < 13) return t;
  t.regs.af = static_cast<uint16_t>(hx(w[0]));  t.regs.bc = static_cast<uint16_t>(hx(w[1]));
  t.regs.de = static_cast<uint16_t>(hx(w[2]));  t.regs.hl = static_cast<uint16_t>(hx(w[3]));
  t.regs.af_ = static_cast<uint16_t>(hx(w[4])); t.regs.bc_ = static_cast<uint16_t>(hx(w[5]));
  t.regs.de_ = static_cast<uint16_t>(hx(w[6])); t.regs.hl_ = static_cast<uint16_t>(hx(w[7]));
  t.regs.ix = static_cast<uint16_t>(hx(w[8]));  t.regs.iy = static_cast<uint16_t>(hx(w[9]));
  t.regs.sp = static_cast<uint16_t>(hx(w[10])); t.regs.pc = static_cast<uint16_t>(hx(w[11]));
  t.regs.wz = static_cast<uint16_t>(hx(w[12]));
  auto s = split_ws(lines[i++]);  // I R IFF1 IFF2 IM halted end_tstates
  if (s.size() < 7) return t;
  t.regs.i = static_cast<uint8_t>(hx(s[0])); t.regs.r = static_cast<uint8_t>(hx(s[1]));
  t.regs.iff1 = static_cast<uint8_t>(std::atoi(s[2].c_str()));
  t.regs.iff2 = static_cast<uint8_t>(std::atoi(s[3].c_str()));
  t.regs.im = static_cast<uint8_t>(std::atoi(s[4].c_str()));
  t.regs.halted = static_cast<uint8_t>(std::atoi(s[5].c_str()));
  t.end_tstates = std::strtoull(s[6].c_str(), nullptr, 10);
  // memory lines: "addr b b ... -1"; section ends at a lone "-1"
  while (i < lines.size()) {
    auto m = split_ws(lines[i]);
    if (m.empty()) break;
    if (m[0] == "-1") { i++; break; }
    i++;
    MemBlock mb;
    mb.addr = static_cast<uint16_t>(hx(m[0]));
    for (size_t k = 1; k < m.size(); ++k) {
      if (m[k] == "-1") break;
      mb.bytes.push_back(static_cast<uint8_t>(hx(m[k])));
    }
    t.mem.push_back(mb);
  }
  t.valid = true;
  return t;
}

// Read one .expected test; skips the leading bus-event trace (indented lines).
FuseTest parse_expected(const std::vector<std::string>& lines, size_t& i) {
  FuseTest t;
  while (i < lines.size() && split_ws(lines[i]).empty()) i++;
  if (i >= lines.size()) return t;
  t.name = lines[i++];
  // Skip event lines: they begin with whitespace ("    4 MR 0000 00").
  while (i < lines.size() && !lines[i].empty() && std::isspace((unsigned char)lines[i][0])) i++;
  if (i >= lines.size()) return t;
  auto w = split_ws(lines[i++]);
  if (w.size() < 13) return t;
  t.regs.af = static_cast<uint16_t>(hx(w[0]));  t.regs.bc = static_cast<uint16_t>(hx(w[1]));
  t.regs.de = static_cast<uint16_t>(hx(w[2]));  t.regs.hl = static_cast<uint16_t>(hx(w[3]));
  t.regs.af_ = static_cast<uint16_t>(hx(w[4])); t.regs.bc_ = static_cast<uint16_t>(hx(w[5]));
  t.regs.de_ = static_cast<uint16_t>(hx(w[6])); t.regs.hl_ = static_cast<uint16_t>(hx(w[7]));
  t.regs.ix = static_cast<uint16_t>(hx(w[8]));  t.regs.iy = static_cast<uint16_t>(hx(w[9]));
  t.regs.sp = static_cast<uint16_t>(hx(w[10])); t.regs.pc = static_cast<uint16_t>(hx(w[11]));
  t.regs.wz = static_cast<uint16_t>(hx(w[12]));
  auto s = split_ws(lines[i++]);
  if (s.size() < 7) return t;
  t.regs.i = static_cast<uint8_t>(hx(s[0])); t.regs.r = static_cast<uint8_t>(hx(s[1]));
  t.regs.iff1 = static_cast<uint8_t>(std::atoi(s[2].c_str()));
  t.regs.iff2 = static_cast<uint8_t>(std::atoi(s[3].c_str()));
  t.regs.im = static_cast<uint8_t>(std::atoi(s[4].c_str()));
  t.regs.halted = static_cast<uint8_t>(std::atoi(s[5].c_str()));
  t.end_tstates = std::strtoull(s[6].c_str(), nullptr, 10);
  while (i < lines.size()) {
    auto m = split_ws(lines[i]);
    if (m.empty()) break;
    if (m[0] == "-1") { i++; break; }
    i++;
    MemBlock mb;
    mb.addr = static_cast<uint16_t>(hx(m[0]));
    for (size_t k = 1; k < m.size(); ++k) {
      if (m[k] == "-1") break;
      mb.bytes.push_back(static_cast<uint8_t>(hx(m[k])));
    }
    t.mem.push_back(mb);
  }
  t.valid = true;
  return t;
}

std::vector<std::string> read_lines(const std::string& path) {
  std::vector<std::string> lines;
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
  }
  return lines;
}

// Run one instruction group per FUSE semantics: execute until tstates reach the
// target, completing the in-flight instruction. Returns the final state.
Z80Regs run_fuse(const FuseTest& in, Ram* ram, Board* board, Device* zdev) {
  std::memset(ram->cells, 0, sizeof(Ram::cells));
  for (const auto& mb : in.mem)
    for (size_t k = 0; k < mb.bytes.size(); ++k)
      ram->cells[static_cast<uint16_t>(mb.addr + k)] = mb.bytes[k];
  z80_poke(zdev, &in.regs);

  Z80Regs r{};
  // FUSE runs COMPLETE opcodes until tstates >= end_tstates. We T-step, so tick
  // until an instruction just completed (instr_count advanced this tick) with
  // tstates >= end — never mid-instruction. A halted CPU burns T-states in place.
  const long cap = 2000000;
  uint64_t prev_ic = 0;
  for (long tick = 0; tick < cap; ++tick) {
    board_tick(board);
    z80_peek(zdev, &r);
    const bool just_done = r.instr_count > prev_ic;
    prev_ic = r.instr_count;
    if (r.tstates >= in.end_tstates && (just_done || r.halted)) break;
  }
  return r;
}

// Compare, returning a human-readable diff (empty == match). Undocumented flag
// bits in F are included — that is the whole point.
std::string diff_state(const Z80Regs& got, const FuseTest& exp, Ram* ram) {
  std::string d;
  auto cmp16 = [&](const char* n, uint16_t g, uint16_t e) {
    if (g != e) { char b[64]; std::snprintf(b, sizeof(b), " %s=%04X!=%04X", n, g, e); d += b; }
  };
  auto cmp8 = [&](const char* n, uint8_t g, uint8_t e) {
    if (g != e) { char b[64]; std::snprintf(b, sizeof(b), " %s=%02X!=%02X", n, g, e); d += b; }
  };
  cmp16("AF", got.af, exp.regs.af);  cmp16("BC", got.bc, exp.regs.bc);
  cmp16("DE", got.de, exp.regs.de);  cmp16("HL", got.hl, exp.regs.hl);
  cmp16("AF'", got.af_, exp.regs.af_); cmp16("BC'", got.bc_, exp.regs.bc_);
  cmp16("DE'", got.de_, exp.regs.de_); cmp16("HL'", got.hl_, exp.regs.hl_);
  cmp16("IX", got.ix, exp.regs.ix);  cmp16("IY", got.iy, exp.regs.iy);
  cmp16("SP", got.sp, exp.regs.sp);  cmp16("PC", got.pc, exp.regs.pc);
  cmp16("WZ", got.wz, exp.regs.wz);
  cmp8("I", got.i, exp.regs.i);      cmp8("R", got.r, exp.regs.r);
  cmp8("IFF1", got.iff1, exp.regs.iff1); cmp8("IFF2", got.iff2, exp.regs.iff2);
  cmp8("IM", got.im, exp.regs.im);   cmp8("HALT", got.halted, exp.regs.halted);
  if (got.tstates != exp.end_tstates) {
    char b[64]; std::snprintf(b, sizeof(b), " T=%llu!=%llu",
                              (unsigned long long)got.tstates, (unsigned long long)exp.end_tstates);
    d += b;
  }
  for (const auto& mb : exp.mem)
    for (size_t k = 0; k < mb.bytes.size(); ++k) {
      const uint16_t a = static_cast<uint16_t>(mb.addr + k);
      if (ram->cells[a] != mb.bytes[k]) {
        char b[64]; std::snprintf(b, sizeof(b), " [%04X]=%02X!=%02X", a, ram->cells[a], mb.bytes[k]);
        d += b;
      }
    }
  return d;
}

}  // namespace

TEST(Z80Fuse, ConformanceCorpus) {
  const std::vector<std::string> in_paths = {"test/hw/fuse/tests.in",
                                             "../test/hw/fuse/tests.in"};
  std::vector<std::string> in_lines, exp_lines;
  std::string used;
  for (const auto& p : in_paths) {
    in_lines = read_lines(p);
    if (!in_lines.empty()) { used = p; break; }
  }
  if (in_lines.empty()) {
    GTEST_SKIP() << "FUSE corpus not found (test/hw/fuse/tests.in) — run from project root";
  }
  std::string exp_path = used.substr(0, used.size() - std::string("tests.in").size()) + "tests.expected";
  exp_lines = read_lines(exp_path);
  ASSERT_FALSE(exp_lines.empty()) << "missing " << exp_path;

  auto ram = std::make_unique<Ram>();
  Board board;
  std::vector<uint8_t> z80mem(z80_state_size());
  Device zdev = z80_init(z80mem.data());
  board_init(&board);
  board_add(&board, fclk_device());
  board_add(&board, fram_device(ram.get()));
  board_add(&board, fio_device());
  board_add(&board, zdev);
  board_reset(&board);

  size_t ii = 0, ei = 0;
  int total = 0, passed = 0, shown = 0;
  for (;;) {
    FuseTest tin = parse_in(in_lines, ii);
    if (!tin.valid) break;
    FuseTest texp = parse_expected(exp_lines, ei);
    if (!texp.valid) break;
    ASSERT_EQ(tin.name, texp.name) << "in/expected desync at test " << total;
    total++;
    Z80Regs got = run_fuse(tin, ram.get(), &board, &zdev);
    std::string d = diff_state(got, texp, ram.get());
    if (d.empty()) {
      passed++;
    } else if (shown < 30) {
      std::fprintf(stderr, "FUSE FAIL %-10s:%s\n", tin.name.c_str(), d.c_str());
      shown++;
    }
  }
  std::fprintf(stderr, "\nFUSE conformance: %d/%d passed (%.1f%%)\n", passed, total,
               total ? 100.0 * passed / total : 0.0);
  EXPECT_EQ(passed, total) << "FUSE conformance failures (see stderr for the first 30)";
}
