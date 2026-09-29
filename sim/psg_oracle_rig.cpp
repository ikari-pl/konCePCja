/* psg_oracle_rig.cpp — drive the PSG Device through the AY bus with a scripted
 * register program and dump its generators once per 1 MHz PSG clock, so the
 * CoPyCat cpc_psg.v can be measured against it (copycat/sim/psg/).
 *
 *   ./psg_oracle_rig --script prog.txt --out dump.txt --us N
 *
 * Script: lines "t_us reg val" (decimal, register 0..15, value hex or dec),
 * sorted by time. At t_us the driver latches the register number (BDIR=1,
 * BC1=1, DA=reg) for one microsecond, rests one, writes the value (BDIR=1,
 * BC1=0, DA=val) for one, rests one -- the phases the PPI's port C produces.
 * Dump: one line per PSG clock: "t tone noise env la lb lc" with tone as a
 * 3-bit mask (bit k = channel k), env 0..31, levels 0..31 (konCePCja's
 * 5-bit scale: fixed level L reads 2L+1).
 *
 * Exit status: 0 ok, 1 I/O failure, 2 bad arguments or script, 3 the dump is
 * not usable (no PSG clock was dumped).
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "hw/board.h"
#include "hw/crtc.h"
#include "hw/gate_array.h"
#include "hw/psg.h"
#include "rig_args.h"

struct Ev {
  uint64_t us;
  uint8_t reg, val;
};

struct driver_state {
  std::vector<Ev> evs;
  size_t next = 0;
  uint64_t cycle = 0;  // master cycles
  // phase: 0 idle, then 4 x 16 master cycles of latch / rest / write / rest
  int phase = 0;
  uint64_t phase_start = 0;
  Ev cur{};
};

static void drv_tick(void* self, const Bus* in, Bus* out) {
  (void)in;
  driver_state* d = static_cast<driver_state*>(self);
  const uint64_t us = d->cycle / 16;
  if (d->phase == 0 && d->next < d->evs.size() && us >= d->evs[d->next].us) {
    d->cur = d->evs[d->next++];
    d->phase = 1;
    d->phase_start = d->cycle;
  }
  if (d->phase) {
    const uint64_t k =
        (d->cycle - d->phase_start) / 16;  // which microsecond of the op
    if (k == 0) {
      out->ay.bdir = true;
      out->ay.bc1 = true;
      out->ay.da = d->cur.reg;
    } else if (k == 1) {
      out->ay.bdir = false;
      out->ay.bc1 = false;
    } else if (k == 2) {
      out->ay.bdir = true;
      out->ay.bc1 = false;
      out->ay.da = d->cur.val;
    } else if (k == 3) {
      out->ay.bdir = false;
      out->ay.bc1 = false;
    } else
      d->phase = 0;
  }
  d->cycle++;
}
static void drv_reset(void*) {}
static size_t drv_size(const void*) { return 1; }
static void drv_save(const void*, void* b) { static_cast<uint8_t*>(b)[0] = 1; }
static void drv_load(void*, const void*) {}

int main(int argc, char** argv) {
  const char* script = nullptr;
  const char* outp = nullptr;
  unsigned long long us = 20000;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--script") && i + 1 < argc)
      script = argv[++i];
    else if (!std::strcmp(argv[i], "--out") && i + 1 < argc)
      outp = argv[++i];
    else if (!std::strcmp(argv[i], "--us") && i + 1 < argc) {
      if (!rig::parse_u64(argv[++i], 1, 100000000ULL, us))
        return rig::bad_arg("psg_oracle_rig", "--us", argv[i]);
    } else {
      std::fprintf(stderr,
                   "usage: %s --script prog.txt --out dump.txt [--us N]\n",
                   argv[0]);
      return 2;
    }
  }
  if (!script || !outp) {
    std::fprintf(stderr, "psg_oracle_rig: --script and --out are required\n");
    return 2;
  }
  driver_state drv;
  if (FILE* f = std::fopen(script, "r")) {
    char line[256];
    unsigned long lineno = 0;
    while (std::fgets(line, sizeof line, f)) {
      lineno++;
      // A line that does not scan is a typo in the register program, not
      // something to drop: a silently skipped line yields a flat dump that
      // still exits 0.
      const char* p = line + std::strspn(line, " \t\r\n");
      if (*p == '\0' || *p == '#') continue;  // blank line or comment
      unsigned long t;
      unsigned r;
      int v;
      if (std::sscanf(line, "%lu %u %i", &t, &r, &v) != 3 || r > 15 || v < 0 ||
          v > 255) {
        std::fprintf(stderr, "psg_oracle_rig: %s:%lu is not \"t_us reg val\"\n",
                     script, lineno);
        std::fclose(f);
        return 2;
      }
      drv.evs.push_back(
          Ev{t, static_cast<uint8_t>(r), static_cast<uint8_t>(v)});
    }
    std::fclose(f);
  } else {
    std::fprintf(stderr, "psg_oracle_rig: cannot read %s\n", script);
    return 2;
  }

  std::vector<uint8_t> gmem(ga_state_size());
  Device gdev = ga_init(gmem.data());
  std::vector<uint8_t> cmem(crtc_state_size());
  Device cdev = crtc_init(cmem.data());
  std::vector<uint8_t> smem(psg_state_size());
  Device sdev = psg_init(smem.data());
  Device ddev = {};
  ddev.self = &drv;
  ddev.name = "ay-driver";
  ddev.tick = drv_tick;
  ddev.reset = drv_reset;
  ddev.state_size = drv_size;
  ddev.save = drv_save;
  ddev.load = drv_load;
  Board board;
  board_init(&board);
  board_add(&board, gdev);
  board_add(&board, cdev);
  board_add(&board, sdev);
  board_add(&board, ddev);
  board_reset(&board);
  FILE* fo = std::fopen(outp, "w");
  if (!fo) {
    std::fprintf(stderr, "psg_oracle_rig: cannot write %s\n", outp);
    return 1;
  }
  unsigned long long t = 0;
  bool ok = true;
  for (unsigned long long c = 0; ok && c < us * 16; c++) {
    board_tick(&board);
    if (board.bus.clk.psg) {
      PsgRegs p{};
      psg_peek(&sdev, &p);
      ok = std::fprintf(fo, "%llu %u %u %u %u %u %u\n", t, p.tone_out & 7,
                        p.noise_out & 1, p.env_level, p.chan_level[0],
                        p.chan_level[1], p.chan_level[2]) > 0;
      t++;
    }
  }
  if (std::fclose(fo) != 0 || !ok) {
    std::fprintf(stderr, "psg_oracle_rig: cannot write %s\n", outp);
    return 1;
  }
  std::fprintf(stderr, "psg_oracle_rig: %llu PSG clocks dumped to %s\n", t,
               outp);
  // An empty dump is not a successful comparison input: say so in the status.
  if (t == 0) {
    std::fprintf(stderr, "psg_oracle_rig: no PSG clock was dumped\n");
    return 3;
  }
  return 0;
}
