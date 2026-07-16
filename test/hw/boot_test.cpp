/* boot_test.cpp — boot the REAL CPC6128 firmware through the clean-room board
 * and render whatever screen it paints. The integration acid test: Z80 + Gate
 * Array + CRTC + memory (ROM) + a minimal PPI stub + the live video renderer,
 * all driven by the actual Amstrad OS ROM. Skipped if the ROM is not present.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

#include "hw/board.h"
#include "hw/crtc.h"
#include "hw/gate_array.h"
#include "hw/memory.h"
#include "hw/ppi.h"
#include "hw/psg.h"
#include "hw/video.h"
#include "hw/z80.h"

namespace {

std::vector<uint8_t> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
}

}  // namespace

TEST(Boot, RealCpc6128Firmware) {
  std::vector<uint8_t> rom = read_file("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read_file("../rom/cpc6128.rom");
  if (rom.size() < 0x8000)
    GTEST_SKIP() << "rom/cpc6128.rom not found (run from project root)";

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
  std::vector<uint8_t> vmem(video_state_size());
  Device vdev = video_init(vmem.data());
  std::vector<uint8_t> zmem(z80_state_size());
  Device zdev = z80_init(zmem.data());

  Board board;
  board_init(&board);
  board_add(&board, gdev);  // clocks + INT + palette/mode
  board_add(&board, cdev);  // CRTC (programmed by the firmware)
  board_add(&board,
            pdev);  // real PPI 8255 (VSYNC status, keyboard, AY control)
  board_add(&board,
            sdev);  // real AY-3-8912 PSG (reached through the PPI's AY bus)
  board_add(&board, mdev);  // RAM + ROM
  board_add(&board, vdev);  // live renderer
  board_add(&board, zdev);  // Z80
  board_reset(&board);

  mem_load_lower_rom(&mdev, rom.data(), 0x4000);           // OS at 0x0000
  mem_load_upper_rom(&mdev, rom.data() + 0x4000, 0x4000);  // BASIC at 0xC000
  std::vector<uint8_t> xmem(0x10000, 0);  // 64K expansion → true 128K 6128
  mem_attach_expansion(&mdev, xmem.data(), xmem.size());
  std::vector<uint8_t> amsdos = read_file("rom/amsdos.rom");
  if (amsdos.size() < 0x4000) amsdos = read_file("../rom/amsdos.rom");
  if (amsdos.size() >= 0x4000)
    mem_attach_rom(&mdev, 7, amsdos.data());  // disc ROM

  const int w = 768,
            h = 272;  // full monitor window (48 chars * 16 px, 272 lines)
  std::vector<uint8_t> fb(static_cast<size_t>(w) * h * 3, 0);
  video_attach(&vdev, &gdev, fb.data(), w, h);

  // Run ~120 frames (~2.4 s of CPC time) to reach the Ready screen.
  VideoRegs vr{};
  for (long tick = 0; tick < 40000000L && vr.frames < 120; ++tick) {
    board_tick(&board);
    video_peek(&vdev, &vr);
  }

  // The firmware should have programmed the GA to mode 1 and set a palette.
  GateArrayRegs g{};
  ga_peek(&gdev, &g);
  std::fprintf(stderr, "BOOT: frames=%u mode=%u ink0=%u ink1=%u\n", vr.frames,
               g.mode, g.ink[0], g.ink[1]);

  // Not any all-black frame: the firmware painted something (border/text).
  int nonzero = 0;
  for (uint8_t v : fb)
    if (v != 0) {
      nonzero++;
    }
  EXPECT_GT(vr.frames, 100u) << "the board ran real firmware for many frames";
  EXPECT_GT(nonzero, 0)
      << "the firmware painted something — not an all-black framebuffer";

  if (FILE* out = std::fopen("/tmp/cpc_boot.ppm", "wb")) {
    std::fprintf(out, "P6\n%d %d\n255\n", w, h);
    std::fwrite(fb.data(), 1, fb.size(), out);
    std::fclose(out);
  }
  std::fprintf(stderr, "BOOT: nonzero framebuffer bytes=%d / %zu\n", nonzero,
               fb.size());
}
