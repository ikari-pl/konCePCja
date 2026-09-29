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
  long us = 20000;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--script") && i + 1 < argc)
      script = argv[++i];
    else if (!std::strcmp(argv[i], "--out") && i + 1 < argc)
      outp = argv[++i];
    else if (!std::strcmp(argv[i], "--us") && i + 1 < argc)
      us = std::atol(argv[++i]);
    else {
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
    while (std::fgets(line, sizeof line, f)) {
      unsigned long t;
      unsigned r;
      int v;
      if (std::sscanf(line, "%lu %u %i", &t, &r, &v) == 3)
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
  long t = 0;
  for (long c = 0; c < us * 16; c++) {
    board_tick(&board);
    if (board.bus.clk.psg) {
      PsgRegs p{};
      psg_peek(&sdev, &p);
      std::fprintf(fo, "%ld %u %u %u %u %u %u\n", t, p.tone_out & 7,
                   p.noise_out & 1, p.env_level, p.chan_level[0],
                   p.chan_level[1], p.chan_level[2]);
      t++;
    }
  }
  std::fclose(fo);
  std::fprintf(stderr, "psg_oracle_rig: %ld PSG clocks dumped to %s\n", t,
               outp);
  return 0;
}
