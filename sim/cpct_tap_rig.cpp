/* cpct_tap_rig.cpp — boot a given 16 KB lower ROM on the clean-room board
 * (Z80 + Gate Array + CRTC + PPI + PSG + memory) with a CPCT bus tap attached,
 * run N master cycles, write the trace. The other half of the comparison is
 * the CoPyCat standalone RTL bench running the same ROM (copycat/sim/tap).
 *
 *   ./cpct_tap_rig --rom stub.bin --out trace.cpct [--cycles N]
 *                  [--key ROW,COLUMNS]   (PSG keyboard: columns 0 = pressed)
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "hw/board.h"
#include "hw/cpct_tap.h"
#include "hw/crtc.h"
#include "hw/gate_array.h"
#include "hw/memory.h"
#include "hw/ppi.h"
#include "hw/psg.h"
#include "hw/z80.h"

static std::vector<uint8_t> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
  const char* rom_path = nullptr;
  const char* out_path = nullptr;
  long cycles = 2000000;
  int key_row = -1, key_cols = 0xFF;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--rom") && i + 1 < argc) rom_path = argv[++i];
    else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
    else if (!std::strcmp(argv[i], "--cycles") && i + 1 < argc) cycles = std::atol(argv[++i]);
    else if (!std::strcmp(argv[i], "--key") && i + 1 < argc) {
      std::sscanf(argv[++i], "%d,%i", &key_row, &key_cols);
    } else {
      std::fprintf(stderr, "usage: %s --rom ROM16K --out TRACE.cpct [--cycles N] [--key ROW,COLS]\n", argv[0]);
      return 2;
    }
  }
  if (!rom_path || !out_path) { std::fprintf(stderr, "cpct_tap_rig: --rom and --out are required\n"); return 2; }
  std::vector<uint8_t> rom = read_file(rom_path);
  if (rom.size() < 0x4000) { std::fprintf(stderr, "cpct_tap_rig: %s is not a 16 KB ROM (%zu bytes)\n", rom_path, rom.size()); return 2; }

  std::vector<uint8_t> gmem(ga_state_size());   Device gdev = ga_init(gmem.data());
  std::vector<uint8_t> cmem(crtc_state_size()); Device cdev = crtc_init(cmem.data());
  std::vector<uint8_t> pmem(ppi_state_size());  Device pdev = ppi_init(pmem.data());
  std::vector<uint8_t> smem(psg_state_size());  Device sdev = psg_init(smem.data());
  std::vector<uint8_t> mmem(mem_state_size());  Device mdev = mem_init(mmem.data());
  std::vector<uint8_t> zmem(z80_state_size());  Device zdev = z80_init(zmem.data());
  std::vector<uint8_t> tmem(cpct_tap_state_size()); Device tdev = cpct_tap_init(tmem.data());

  Board board;
  board_init(&board);
  board_add(&board, gdev);
  board_add(&board, cdev);
  board_add(&board, pdev);
  board_add(&board, sdev);
  board_add(&board, mdev);
  board_add(&board, zdev);
  board_add(&board, tdev);
  board_reset(&board);
  mem_load_lower_rom(&mdev, rom.data(), 0x4000);
  if (key_row >= 0) psg_set_key_row(&sdev, static_cast<uint8_t>(key_row), static_cast<uint8_t>(key_cols));

  for (long i = 0; i < cycles; i++) board_tick(&board);

  size_t n = 0, dropped = 0;
  cpct_tap_records(&tdev, &n, &dropped);
  if (cpct_tap_write(&tdev, out_path, 2) != 0) { std::fprintf(stderr, "cpct_tap_rig: cannot write %s\n", out_path); return 1; }
  Z80Regs z{}; z80_peek(&zdev, &z);
  std::fprintf(stderr, "cpct_tap_rig: %ld master cycles, %zu records (%zu dropped), PC=%04X, wrote %s\n",
               cycles, n, dropped, z.pc, out_path);
  return 0;
}
