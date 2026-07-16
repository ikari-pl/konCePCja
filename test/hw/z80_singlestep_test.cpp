/* z80_singlestep_test.cpp — runs the SingleStepTests (jsmoo) Z80 corpus against
 * our Device. Where FUSE is single-instruction and carries no Q register and no
 * incoming-Q, SingleStepTests pins the FULL post-instruction state INCLUDING the
 * Q latch and sweeps the *incoming* Q — so it is the oracle for two things FUSE
 * cannot reach (beads-yjql):
 *   - the per-iteration undocumented flags of the repeating block-I/O ops
 *     (INIR/INDR/OTIR/OTDR — each SingleStep case is one iteration, i.e. exactly
 *     the mid-loop state an interrupt would sample), and
 *   - SCF/CCF's dependence on the *previous* instruction's Q (§5).
 *
 * Corpus format (per opcode file, an array of cases): each case has `initial`
 * and `final` full-state objects (a,f,b,c,...,ix,iy,sp,pc,wz,i,r,im,iff1,iff2,q,
 * ram[[addr,val]]) and `ports` [[addr,val,"r"|"w"]]. We ignore the jsmoo-internal
 * `p` latch and per-T-state `cycles` (stripped from the vendored slice) and
 * compare architectural state + memory + port writes — a complete check.
 *
 * Corpus lives in test/hw/sst/ as .json files (a trimmed, cycles-stripped
 * slice, MIT).
 * scripts/fetch-z80-sst.sh fetches the full set. Skipped if the dir is absent.
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "hw/board.h"
#include "hw/z80.h"

namespace {

// ------------------------------------------------------- minimal JSON -------
struct JVal {
  enum Type { Null, Num, Str, Arr, Obj } type = Null;
  double num = 0;
  std::string str;
  std::vector<JVal> arr;
  std::map<std::string, JVal> obj;
  const JVal* find(const char* k) const {
    if (type != Obj) return nullptr;
    auto it = obj.find(k);
    return it == obj.end() ? nullptr : &it->second;
  }
  long i(const char* k) const {
    const JVal* v = find(k);
    return v && v->type == Num ? static_cast<long>(v->num) : 0;
  }
};

struct JParser {
  const char* p;
  const char* e;
  void ws() {
    while (p < e && static_cast<unsigned char>(*p) <= ' ') p++;
  }
  std::string str() {
    std::string s;
    if (p < e && *p == '"') p++;
    while (p < e && *p != '"') {
      if (*p == '\\' && p + 1 < e) {
        p++;
        s.push_back(*p == 'n' ? '\n' : *p);
      } else {
        s.push_back(*p);
      }
      p++;
    }
    if (p < e) p++;  // closing quote
    return s;
  }
  JVal value() {
    ws();
    JVal v;
    if (p >= e) return v;
    const char c = *p;
    if (c == '{') return object();
    if (c == '[') return array();
    if (c == '"') {
      v.type = JVal::Str;
      v.str = str();
      return v;
    }
    if (c == 'n') {  // null
      p += 4;
      return v;
    }
    if (c == 't') {
      p += 4;
      v.type = JVal::Num;
      v.num = 1;
      return v;
    }
    if (c == 'f') {
      p += 5;
      v.type = JVal::Num;
      v.num = 0;
      return v;
    }
    char* endp = nullptr;
    v.type = JVal::Num;
    v.num = std::strtod(p, &endp);
    p = endp;
    return v;
  }
  JVal array() {
    JVal v;
    v.type = JVal::Arr;
    p++;  // [
    ws();
    while (p < e && *p != ']') {
      v.arr.push_back(value());
      ws();
      if (p < e && *p == ',') p++;
      ws();
    }
    if (p < e) p++;  // ]
    return v;
  }
  JVal object() {
    JVal v;
    v.type = JVal::Obj;
    p++;  // {
    ws();
    while (p < e && *p != '}') {
      ws();
      std::string k = str();
      ws();
      if (p < e && *p == ':') p++;
      v.obj.emplace(std::move(k), value());
      ws();
      if (p < e && *p == ',') p++;
      ws();
    }
    if (p < e) p++;  // }
    return v;
  }
};

// ----------------------------------------------------------- devices --------
struct Ram {
  uint8_t cells[0x10000];
};
void ram_tick(void* self, const Bus* in, Bus* out) {
  Ram* ram = static_cast<Ram*>(self);
  if (in->cpu.mreq && in->cpu.wr)
    ram->cells[in->cpu.addr] = in->cpu.data;
  else if (in->cpu.mreq && in->cpu.rd)
    out->cpu.data = ram->cells[in->cpu.addr];
}
size_t ram_size(const void*) { return sizeof(Ram); }
void ram_save(const void* s, void* b) { std::memcpy(b, s, sizeof(Ram)); }
void ram_load(void* s, const void* b) { std::memcpy(s, b, sizeof(Ram)); }
Device ram_device(Ram* s) {
  return Device{s, "ram", ram_tick, [](void*) {}, ram_size, ram_save, ram_load};
}

struct Port {
  uint16_t addr;
  uint8_t val;
};
// Programmable I/O. An IN cycle holds iorq+rd for several T-states, and the CPU
// samples on the last, so the read value must be stable for the whole cycle —
// we return it every tick (a single-instruction case does at most one IN, so a
// single stored value suffices). Writes are recorded once, on the wr edge.
struct Pio {
  std::vector<Port> reads;
  std::vector<Port> writes;
  bool wr_prev = false;
};
void pio_tick(void* self, const Bus* in, Bus* out) {
  Pio* io = static_cast<Pio*>(self);
  if (in->cpu.iorq && in->cpu.rd && !io->reads.empty())
    out->cpu.data = io->reads.front().val;
  const bool wr = in->cpu.iorq && in->cpu.wr;
  if (wr && !io->wr_prev) io->writes.push_back({in->cpu.addr, in->cpu.data});
  io->wr_prev = wr;
}
size_t pio_size(const void*) { return 1; }
void pio_save(const void*, void* b) { *static_cast<uint8_t*>(b) = 0; }
Device pio_device(Pio* s) {
  return Device{s,        "io",     pio_tick, [](void*) {},
                pio_size, pio_save, [](void*, const void*) {}};
}

void clk_tick(void*, const Bus*, Bus* out) { out->clk.cpu = true; }
size_t clk_size(const void*) { return 1; }
void clk_save(const void*, void* b) { *static_cast<uint8_t*>(b) = 0; }
Device clk_device() {
  static uint8_t d = 0;
  return Device{&d,       "clk",    clk_tick, [](void*) {},
                clk_size, clk_save, [](void*, const void*) {}};
}

// -------------------------------------------------------------- runner ------
Z80Regs regs_from(const JVal& s) {
  Z80Regs r{};
  const uint16_t a = s.i("a"), f = s.i("f"), b = s.i("b"), c = s.i("c");
  const uint16_t d = s.i("d"), e = s.i("e"), h = s.i("h"), l = s.i("l");
  r.af = (a << 8) | f;
  r.bc = (b << 8) | c;
  r.de = (d << 8) | e;
  r.hl = (h << 8) | l;
  r.af_ = s.i("af_");
  r.bc_ = s.i("bc_");
  r.de_ = s.i("de_");
  r.hl_ = s.i("hl_");
  r.ix = s.i("ix");
  r.iy = s.i("iy");
  r.sp = s.i("sp");
  r.pc = s.i("pc");
  r.wz = s.i("wz");
  r.i = s.i("i");
  r.r = s.i("r");
  r.im = s.i("im");
  r.iff1 = s.i("iff1");
  r.iff2 = s.i("iff2");
  r.q = s.i("q");
  return r;
}

std::string diff(const Z80Regs& g, const JVal& fin, Ram* ram) {
  const Z80Regs e = regs_from(fin);
  std::string d;
  auto c16 = [&](const char* n, uint16_t gg, uint16_t ee) {
    if (gg != ee) {
      char b[48];
      std::snprintf(b, sizeof(b), " %s=%04X!=%04X", n, gg, ee);
      d += b;
    }
  };
  auto c8 = [&](const char* n, uint8_t gg, uint8_t ee) {
    if (gg != ee) {
      char b[48];
      std::snprintf(b, sizeof(b), " %s=%02X!=%02X", n, gg, ee);
      d += b;
    }
  };
  c16("AF", g.af, e.af);
  c16("BC", g.bc, e.bc);
  c16("DE", g.de, e.de);
  c16("HL", g.hl, e.hl);
  c16("AF'", g.af_, e.af_);
  c16("BC'", g.bc_, e.bc_);
  c16("DE'", g.de_, e.de_);
  c16("HL'", g.hl_, e.hl_);
  c16("IX", g.ix, e.ix);
  c16("IY", g.iy, e.iy);
  c16("SP", g.sp, e.sp);
  c16("PC", g.pc, e.pc);
  c16("WZ", g.wz, e.wz);
  c8("I", g.i, e.i);
  c8("R", g.r, e.r);
  c8("IFF1", g.iff1, e.iff1);
  c8("IFF2", g.iff2, e.iff2);
  c8("IM", g.im, e.im);
  c8("Q", g.q, static_cast<uint8_t>(fin.i("q")));
  if (const JVal* rm = fin.find("ram"))
    for (const JVal& cell : rm->arr) {
      const uint16_t a = static_cast<uint16_t>(cell.arr[0].num);
      const uint8_t v = static_cast<uint8_t>(cell.arr[1].num);
      if (ram->cells[a] != v) {
        char b[48];
        std::snprintf(b, sizeof(b), " [%04X]=%02X!=%02X", a, ram->cells[a], v);
        d += b;
      }
    }
  return d;  // port writes are compared by the caller
}

struct Result {
  int total = 0, passed = 0;
};

Result run_file(const std::filesystem::path& path, Ram* ram, Board* board,
                Device* zdev, Pio* pio, int& shown) {
  Result res;
  std::ifstream f(path, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
  if (text.empty()) return res;
  JParser jp{text.data(), text.data() + text.size()};
  const JVal root = jp.value();
  if (root.type != JVal::Arr) return res;

  for (const JVal& tc : root.arr) {
    const JVal* init = tc.find("initial");
    const JVal* fin = tc.find("final");
    if (init == nullptr || fin == nullptr) continue;
    res.total++;

    std::memset(ram->cells, 0, sizeof(Ram::cells));
    if (const JVal* rm = init->find("ram"))
      for (const JVal& cell : rm->arr)
        ram->cells[static_cast<uint16_t>(cell.arr[0].num)] =
            static_cast<uint8_t>(cell.arr[1].num);

    pio->reads.clear();
    pio->writes.clear();
    pio->wr_prev = false;
    std::vector<Port> exp_writes;
    if (const JVal* pl = tc.find("ports"))
      for (const JVal& pe : pl->arr) {
        const Port pt{static_cast<uint16_t>(pe.arr[0].num),
                      static_cast<uint8_t>(pe.arr[1].num)};
        if (pe.arr[2].str == "r")
          pio->reads.push_back(pt);
        else
          exp_writes.push_back(pt);
      }

    const Z80Regs in = regs_from(*init);
    z80_poke(zdev, &in);
    Z80Regs got{};
    for (long tick = 0; tick < 4000; ++tick) {
      board_tick(board);
      z80_peek(zdev, &got);
      if (got.instr_count >= 1) break;
    }

    std::string d = diff(got, *fin, ram);
    // Compare port writes (addr + data).
    if (pio->writes.size() != exp_writes.size()) {
      char b[64];
      std::snprintf(b, sizeof(b), " writes=%zu!=%zu", pio->writes.size(),
                    exp_writes.size());
      d += b;
    } else {
      for (size_t k = 0; k < exp_writes.size(); ++k)
        if (pio->writes[k].addr != exp_writes[k].addr ||
            pio->writes[k].val != exp_writes[k].val) {
          char b[64];
          std::snprintf(b, sizeof(b), " OUT[%04X]=%02X!=[%04X]%02X",
                        pio->writes[k].addr, pio->writes[k].val,
                        exp_writes[k].addr, exp_writes[k].val);
          d += b;
        }
    }

    if (d.empty()) {
      res.passed++;
    } else if (shown < 25) {
      const JVal* nm = tc.find("name");
      std::fprintf(stderr, "SST FAIL %-12s:%s\n",
                   nm ? nm->str.c_str() : "?", d.c_str());
      shown++;
    }
  }
  return res;
}

std::filesystem::path corpus_dir() {
  for (const char* c : {"test/hw/sst", "../test/hw/sst"})
    if (std::filesystem::is_directory(c)) return c;
  return {};
}

}  // namespace

TEST(Z80SingleStep, BlockOpsAndScfCcfQCorpus) {
  const std::filesystem::path dir = corpus_dir();
  if (dir.empty())
    GTEST_SKIP() << "SingleStepTests corpus not found (test/hw/sst/*.json) — run "
                    "scripts/fetch-z80-sst.sh";

  std::vector<std::filesystem::path> files;
  for (const auto& ent : std::filesystem::directory_iterator(dir))
    if (ent.path().extension() == ".json") files.push_back(ent.path());
  std::sort(files.begin(), files.end());
  ASSERT_FALSE(files.empty()) << "corpus dir is empty";

  auto ram = std::make_unique<Ram>();
  auto pio = std::make_unique<Pio>();
  Board board;
  std::vector<uint8_t> z80mem(z80_state_size());
  Device zdev = z80_init(z80mem.data());
  board_init(&board);
  board_add(&board, clk_device());
  board_add(&board, ram_device(ram.get()));
  board_add(&board, pio_device(pio.get()));
  board_add(&board, zdev);
  board_reset(&board);

  int total = 0, passed = 0, shown = 0;
  for (const auto& path : files) {
    Result r = run_file(path, ram.get(), &board, &zdev, pio.get(), shown);
    total += r.total;
    passed += r.passed;
    std::fprintf(stderr, "%-14s %4d/%4d\n", path.filename().string().c_str(),
                 r.passed, r.total);
  }
  std::fprintf(stderr, "\nSingleStepTests: %d/%d passed (%.2f%%)\n", passed,
               total, total ? 100.0 * passed / total : 0.0);
  ASSERT_GT(total, 0);
  EXPECT_EQ(passed, total)
      << "SingleStepTests conformance failures (see stderr for the first 25)";
}
