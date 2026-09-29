/* cpct_tap_rig.cpp — boot a given 16 KB lower ROM on the clean-room board
 * (Z80 + Gate Array + CRTC + PPI + PSG + memory) with a CPCT bus tap attached,
 * run N master cycles, write the trace. The other half of the comparison is
 * the CoPyCat standalone RTL bench running the same ROM (copycat/sim/tap).
 *
 *   ./cpct_tap_rig --rom stub.bin --out trace.cpct [--cycles N]
 *                  [--key ROW,COLUMNS]...   (PSG keyboard: columns 0 = pressed;
 * repeatable)
 *                  [--upper basic.bin]   (16 KB upper ROM 0, e.g. BASIC)
 *                  [--screen ram_c000.bin] (dump RAM C000-FFFF at the end)
 *                  [--garegs ga.txt]     (dump the Gate Array's mode and inks)
 *                  [--capacity N]        (tap record buffer, default 1<<24)
 *                  [--expansion KB]      (attach a dk'tronics-style RAM
 * expansion, 64..512)
 *                  [--dump-frames DIR]   (the video Device's framebuffer as
 *                                         DIR/frame_NNNNN.ppm at every
 * completed frame: the shadow twin's oracle)
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "hw/board.h"
#include "hw/cpct_tap.h"
#include "hw/crtc.h"
#include "hw/gate_array.h"
#include "hw/memory.h"
#include "hw/ppi.h"
#include "hw/psg.h"
#include "hw/video.h"
#include "hw/z80.h"
#include "subcycle/machine.h"

static std::vector<uint8_t> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
}

static bool write_ppm(const std::string& path, const std::vector<uint8_t>& fb,
                      int w, int h) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  bool ok = std::fprintf(f, "P6\n%d %d\n255\n", w, h) > 0;
  ok = ok && std::fwrite(fb.data(), 1, fb.size(), f) == fb.size();
  return std::fclose(f) == 0 && ok;
}

int main(int argc, char** argv) {
  const char* rom_path = nullptr;
  const char* out_path = nullptr;
  long cycles = 2000000;
  int key_rows[16], key_cols[16], nkeys = 0;
  const char* upper_path = nullptr;
  const char* screen_path = nullptr;
  const char* garegs_path = nullptr;
  size_t capacity = size_t(1) << 24;
  long expansion_kb = 0;
  const char* frames_dir = nullptr;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--rom") && i + 1 < argc)
      rom_path = argv[++i];
    else if (!std::strcmp(argv[i], "--out") && i + 1 < argc)
      out_path = argv[++i];
    else if (!std::strcmp(argv[i], "--cycles") && i + 1 < argc)
      cycles = std::atol(argv[++i]);
    else if (!std::strcmp(argv[i], "--upper") && i + 1 < argc)
      upper_path = argv[++i];
    else if (!std::strcmp(argv[i], "--screen") && i + 1 < argc)
      screen_path = argv[++i];
    else if (!std::strcmp(argv[i], "--garegs") && i + 1 < argc)
      garegs_path = argv[++i];
    else if (!std::strcmp(argv[i], "--capacity") && i + 1 < argc)
      capacity = std::strtoul(argv[++i], nullptr, 0);
    else if (!std::strcmp(argv[i], "--expansion") && i + 1 < argc)
      expansion_kb = std::atol(argv[++i]);
    else if (!std::strcmp(argv[i], "--dump-frames") && i + 1 < argc)
      frames_dir = argv[++i];
    else if (!std::strcmp(argv[i], "--key") && i + 1 < argc && nkeys < 16) {
      std::sscanf(argv[++i], "%d,%i", &key_rows[nkeys], &key_cols[nkeys]);
      nkeys++;
    } else {
      std::fprintf(stderr,
                   "usage: %s --rom ROM16K --out TRACE.cpct [--cycles N] "
                   "[--key ROW,COLS]... [--upper ROM16K]\n"
                   "       [--screen FILE] [--garegs FILE] [--capacity N] "
                   "[--expansion KB] [--dump-frames DIR]\n",
                   argv[0]);
      return 2;
    }
  }
  if (!rom_path || !out_path) {
    std::fprintf(stderr, "cpct_tap_rig: --rom and --out are required\n");
    return 2;
  }
  std::vector<uint8_t> rom = read_file(rom_path);
  if (rom.size() < 0x4000) {
    std::fprintf(stderr, "cpct_tap_rig: %s is not a 16 KB ROM (%zu bytes)\n",
                 rom_path, rom.size());
    return 2;
  }

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
  std::vector<uint8_t> zmem(z80_state_size());
  Device zdev = z80_init(zmem.data());
  std::vector<uint8_t> tmem(cpct_tap_state_size());
  Device tdev = cpct_tap_init(tmem.data());
  std::vector<uint8_t> vmem(video_state_size());
  Device vdev = video_init(vmem.data());

  Board board;
  board_init(&board);
  board_add(&board, gdev);
  board_add(&board, cdev);
  board_add(&board, pdev);
  board_add(&board, sdev);
  board_add(&board, mdev);
  board_add(&board, zdev);
  board_add(&board, tdev);
  // The video Device only listens (RAM fetch bus + CRTC timing), so the CPU
  // bus -- and with it the trace -- is the same with or without it.
  if (frames_dir) board_add(&board, vdev);
  board_reset(&board);
  const int fb_w = subcycle::kFbWidth, fb_h = subcycle::kFbHeight;
  std::vector<uint8_t> fb;
  if (frames_dir) {
    std::error_code ec;
    std::filesystem::create_directories(frames_dir, ec);
    if (ec) {
      std::fprintf(stderr, "cpct_tap_rig: cannot create %s: %s\n", frames_dir,
                   ec.message().c_str());
      return 1;
    }
    fb.assign(static_cast<size_t>(fb_w) * fb_h * 3, 0);
    video_attach(&vdev, &gdev, fb.data(), fb_w, fb_h);
  }
  mem_load_lower_rom(&mdev, rom.data(), 0x4000);
  std::vector<uint8_t> upper;
  if (upper_path) {
    upper = read_file(upper_path);
    if (upper.size() < 0x4000) {
      std::fprintf(stderr, "cpct_tap_rig: %s is not a 16 KB ROM\n", upper_path);
      return 2;
    }
    mem_load_upper_rom(&mdev, upper.data(), 0x4000);
  }
  std::vector<uint8_t> xmem;
  if (expansion_kb > 0) {
    xmem.assign(static_cast<size_t>(expansion_kb) * 1024, 0);
    mem_attach_expansion(&mdev, xmem.data(), xmem.size());
  }
  cpct_tap_set_capacity(&tdev, capacity);
  for (int k = 0; k < nkeys; k++)
    psg_set_key_row(&sdev, static_cast<uint8_t>(key_rows[k]),
                    static_cast<uint8_t>(key_cols[k]));

  uint32_t frames_seen = 0, frames_written = 0;
  for (long i = 0; i < cycles; i++) {
    board_tick(&board);
    if (!frames_dir) continue;
    VideoRegs v{};
    video_peek(&vdev, &v);
    if (v.frames == frames_seen) continue;
    frames_seen = v.frames;
    char name[32];
    std::snprintf(name, sizeof name, "frame_%05u.ppm", frames_seen);
    const std::string path =
        (std::filesystem::path(frames_dir) / name).string();
    if (!write_ppm(path, fb, fb_w, fb_h)) {
      std::fprintf(stderr, "cpct_tap_rig: cannot write %s\n", path.c_str());
      return 1;
    }
    frames_written++;
  }

  size_t n = 0, dropped = 0;
  cpct_tap_records(&tdev, &n, &dropped);
  if (cpct_tap_write(&tdev, out_path, 0, 2) != 0) {
    std::fprintf(stderr, "cpct_tap_rig: cannot write %s\n", out_path);
    return 1;
  }
  if (screen_path) {
    FILE* f = std::fopen(screen_path, "wb");
    if (!f) {
      std::fprintf(stderr, "cpct_tap_rig: cannot write %s\n", screen_path);
      return 1;
    }
    uint8_t scr[0x4000];
    for (unsigned a = 0xC000; a < 0x10000; a++)
      scr[a - 0xC000] = mem_peek_ram(&mdev, static_cast<uint16_t>(a));
    const bool ok = std::fwrite(scr, 1, sizeof scr, f) == sizeof scr;
    if (std::fclose(f) != 0 || !ok) {
      std::fprintf(stderr, "cpct_tap_rig: cannot write %s\n", screen_path);
      return 1;
    }
  }
  if (garegs_path) {
    GateArrayRegs g{};
    ga_peek(&gdev, &g);
    FILE* f = std::fopen(garegs_path, "w");
    if (!f) {
      std::fprintf(stderr, "cpct_tap_rig: cannot write %s\n", garegs_path);
      return 1;
    }
    std::fprintf(f, "mode=%u\n", g.mode);
    for (int k = 0; k < 17; k++) std::fprintf(f, "ink%d=%u\n", k, g.ink[k]);
    if (std::fclose(f) != 0) {
      std::fprintf(stderr, "cpct_tap_rig: cannot write %s\n", garegs_path);
      return 1;
    }
  }
  Z80Regs z{};
  z80_peek(&zdev, &z);
  std::fprintf(stderr,
               "cpct_tap_rig: %ld master cycles, %zu records (%zu dropped), "
               "PC=%04X, wrote %s\n",
               cycles, n, dropped, z.pc, out_path);
  if (frames_dir)
    std::fprintf(stderr, "cpct_tap_rig: %u frames written to %s\n",
                 frames_written, frames_dir);
  return 0;
}
