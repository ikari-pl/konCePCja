/* cpct_tap_rig.cpp — boot a given 16 KB lower ROM on the clean-room board
 * (Z80 + Gate Array + CRTC + PPI + PSG + memory) with a CPCT bus tap attached,
 * run N master cycles, write the trace. The other half of the comparison is
 * the CoPyCat standalone RTL bench running the same ROM (copycat/sim/tap).
 *
 *   ./cpct_tap_rig --rom ROM16K --out TRACE.cpct [options]
 *
 * Options (--cycles, --key, --upper, --screen, --garegs, --capacity,
 * --expansion, --dump-frames) are listed in docs/cpct-tap.md.
 *
 * Exit status: 0 ok, 1 I/O failure, 2 bad arguments or input, 3 the trace is
 * not usable (records were dropped, or no access was recorded).
 */
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <system_error>
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
#include "rig_args.h"
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

using rig::parse_u64;

// "ROW,COLS": row 0..15, columns byte 0..255 (0 bits = pressed).
static bool parse_key(const char* s, int& row, int& cols) {
  const char* comma = s ? std::strchr(s, ',') : nullptr;
  if (!comma) return false;
  const std::string r(s, comma);
  unsigned long long rv = 0, cv = 0;
  if (!parse_u64(r.c_str(), 0, 15, rv) || !parse_u64(comma + 1, 0, 255, cv))
    return false;
  row = static_cast<int>(rv);
  cols = static_cast<int>(cv);
  return true;
}

int main(int argc, char** argv) {
  const char* rom_path = nullptr;
  const char* out_path = nullptr;
  unsigned long long cycles = 2000000;
  int key_rows[16] = {}, key_cols[16] = {}, nkeys = 0;
  const char* upper_path = nullptr;
  const char* screen_path = nullptr;
  const char* garegs_path = nullptr;
  size_t capacity = size_t(1) << 24;
  unsigned long long expansion_kb = 0;
  const char* frames_dir = nullptr;
  auto bad_arg = [](const char* opt, const char* val) {
    return rig::bad_arg("cpct_tap_rig", opt, val);
  };
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--rom") && i + 1 < argc)
      rom_path = argv[++i];
    else if (!std::strcmp(argv[i], "--out") && i + 1 < argc)
      out_path = argv[++i];
    else if (!std::strcmp(argv[i], "--cycles") && i + 1 < argc) {
      if (!parse_u64(argv[++i], 1, ~0ULL, cycles))
        return bad_arg("--cycles", argv[i]);
    } else if (!std::strcmp(argv[i], "--upper") && i + 1 < argc)
      upper_path = argv[++i];
    else if (!std::strcmp(argv[i], "--screen") && i + 1 < argc)
      screen_path = argv[++i];
    else if (!std::strcmp(argv[i], "--garegs") && i + 1 < argc)
      garegs_path = argv[++i];
    else if (!std::strcmp(argv[i], "--capacity") && i + 1 < argc) {
      unsigned long long c = 0;
      if (!parse_u64(argv[++i], 0, SIZE_MAX, c))
        return bad_arg("--capacity", argv[i]);
      capacity = static_cast<size_t>(c);
    } else if (!std::strcmp(argv[i], "--expansion") && i + 1 < argc) {
      // Expansion RAM comes in 64 KB banks; anything else is a typo.
      if (!parse_u64(argv[++i], 64, 512, expansion_kb) || expansion_kb % 64)
        return bad_arg("--expansion", argv[i]);
    } else if (!std::strcmp(argv[i], "--dump-frames") && i + 1 < argc)
      frames_dir = argv[++i];
    else if (!std::strcmp(argv[i], "--key") && i + 1 < argc) {
      if (nkeys == 16) {
        std::fprintf(stderr, "cpct_tap_rig: at most 16 --key options\n");
        return 2;
      }
      if (!parse_key(argv[++i], key_rows[nkeys], key_cols[nkeys]))
        return bad_arg("--key", argv[i]);
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
  // The trace is the deliverable: fail on an unwritable path now, not after a
  // run that can take minutes. cpct_tap_write reopens with "wb" and truncates,
  // so the empty probe file is harmless.
  if (FILE* probe = std::fopen(out_path, "wb")) {
    std::fclose(probe);
  } else {
    std::fprintf(stderr, "cpct_tap_rig: cannot write %s: %s\n", out_path,
                 std::strerror(errno));
    return 1;
  }
  if (frames_dir) {
    std::error_code ec;
    std::filesystem::create_directories(frames_dir, ec);
    if (ec) {
      std::fprintf(stderr, "cpct_tap_rig: cannot create %s: %s\n", frames_dir,
                   ec.message().c_str());
      return 1;
    }
    // Frame names restart at frame_00001.ppm every run: a shorter second run
    // into the same directory would leave the tail of the first one behind and
    // a consumer globbing frame_*.ppm would mix two captures.
    for (const auto& e : std::filesystem::directory_iterator(frames_dir, ec)) {
      if (e.path().filename().string().rfind("frame_", 0) == 0) {
        std::fprintf(stderr,
                     "cpct_tap_rig: %s already contains frames; use an empty "
                     "directory\n",
                     frames_dir);
        return 2;
      }
    }
  }
  std::vector<uint8_t> rom = read_file(rom_path);
  // 16 KB is the lower ROM. A 32 KB image is the usual OS+BASIC pair: split
  // it, so a 6128 ROM set boots with BASIC instead of silently running with an
  // empty upper socket.
  if (rom.size() != 0x4000 && rom.size() != 0x8000) {
    std::fprintf(stderr,
                 "cpct_tap_rig: %s is not a 16 KB or 32 KB ROM (%zu bytes)\n",
                 rom_path, rom.size());
    return 2;
  }
  if (rom.size() == 0x8000 && upper_path) {
    std::fprintf(stderr,
                 "cpct_tap_rig: %s is a 32 KB OS+BASIC pair; --upper would "
                 "contradict its second half\n",
                 rom_path);
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
  // The tap owns a heap buffer: destroy it on every return.
  CpctTapOwner tap_owner(&tdev);
  // Before the board reset, so even its power-on RESET record is bounded.
  cpct_tap_set_capacity(&tdev, capacity);
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
    fb.assign(static_cast<size_t>(fb_w) * fb_h * 3, 0);
    video_attach(&vdev, &gdev, fb.data(), fb_w, fb_h);
  }
  mem_load_lower_rom(&mdev, rom.data(), 0x4000);
  std::vector<uint8_t> upper;
  if (rom.size() == 0x8000) {
    mem_load_upper_rom(&mdev, rom.data() + 0x4000, 0x4000);
  } else if (upper_path) {
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
  for (int k = 0; k < nkeys; k++)
    psg_set_key_row(&sdev, static_cast<uint8_t>(key_rows[k]),
                    static_cast<uint8_t>(key_cols[k]));

  uint32_t frames_seen = 0, frames_written = 0;
  bool frames_ok = true;
  try {
    for (unsigned long long i = 0; frames_ok && i < cycles; i++) {
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
        // Stop the run, but still write the trace below: the records captured
        // so far are intact and are the expensive half of the result.
        std::fprintf(stderr, "cpct_tap_rig: cannot write %s\n", path.c_str());
        frames_ok = false;
        break;
      }
      frames_written++;
    }
  } catch (const std::bad_alloc&) {
    std::fprintf(stderr,
                 "cpct_tap_rig: out of memory recording the trace; lower "
                 "--capacity\n");
    return 1;
  }

  size_t n = 0, dropped = 0;
  const CpctRecord* recs = cpct_tap_records(&tdev, &n, &dropped);
  size_t accesses = 0;
  for (size_t k = 0; k < n; k++)
    if (recs[k].flags &
        (CPCT_M1 | CPCT_MEM_RD | CPCT_MEM_WR | CPCT_IO_RD | CPCT_IO_WR))
      accesses++;
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
    bool ok = std::fprintf(f, "mode=%u\n", g.mode) > 0;
    for (int k = 0; k < 17; k++)
      ok = ok && std::fprintf(f, "ink%d=%u\n", k, g.ink[k]) > 0;
    if (std::fclose(f) != 0 || !ok) {
      std::fprintf(stderr, "cpct_tap_rig: cannot write %s\n", garegs_path);
      return 1;
    }
  }
  Z80Regs z{};
  z80_peek(&zdev, &z);
  std::fprintf(stderr,
               "cpct_tap_rig: %llu master cycles, %zu records (%zu dropped), "
               "PC=%04X, wrote %s\n",
               cycles, n, dropped, z.pc, out_path);
  if (frames_dir)
    std::fprintf(stderr, "cpct_tap_rig: %u frames written to %s\n",
                 frames_written, frames_dir);
  // The trace is on disk either way; a failed frame dump is still a failure.
  if (!frames_ok) return 1;
  // A trace that lost records or caught no access is not a usable result:
  // the file is written for inspection, but the exit status says so.
  if (dropped) {
    std::fprintf(stderr,
                 "cpct_tap_rig: trace truncated (%zu records lost, see the GAP "
                 "record); raise --capacity\n",
                 dropped);
    return 3;
  }
  if (accesses == 0) {
    std::fprintf(stderr, "cpct_tap_rig: no bus access was recorded\n");
    return 3;
  }
  return 0;
}
