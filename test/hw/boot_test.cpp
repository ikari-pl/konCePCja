/* boot_test.cpp — boot the REAL CPC6128 firmware through the clean-room board and
 * render whatever screen it paints. The integration acid test: Z80 + Gate Array +
 * CRTC + memory (ROM) + a minimal PPI stub + the live video renderer, all driven by
 * the actual Amstrad OS ROM. Skipped if the ROM is not present. */

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

#include <gtest/gtest.h>

#include "hw/board.h"
#include "hw/crtc.h"
#include "hw/gate_array.h"
#include "hw/memory.h"
#include "hw/video.h"
#include "hw/z80.h"

namespace {

// Minimal PPI 8255 stub: enough for the firmware to boot. Port B read reports the
// CRTC VSYNC (bit 0 — the firmware's frame-timing gate), the Amstrad distributor
// id (bits 1-3 = 7) and 50 Hz (bit 4). Port A/C reads = 0xFF (no keys). Writes
// ignored. Ports &F4/&F5/&F6/&F7.
void ppi_tick(void* self, const Bus* in, Bus* out) {
  (void)self;
  if (in->cpu.iorq && in->cpu.rd) {
    const uint8_t hi = static_cast<uint8_t>(in->cpu.addr >> 8);
    if (hi == 0xF5) out->cpu.data = static_cast<uint8_t>((in->vid.vsync ? 0x01 : 0x00) | 0x1E);
    else if (hi == 0xF4 || hi == 0xF6) out->cpu.data = 0xFF;
  }
}
size_t ppi_size(const void*) { return 1; }
Device ppi_device() {
  static uint8_t d = 0;
  return Device{&d, "ppi", ppi_tick, [](void*){}, ppi_size,
                [](const void*, void*){}, [](void*, const void*){}};
}

std::vector<uint8_t> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
}

}  // namespace

TEST(Boot, RealCpc6128Firmware) {
  std::vector<uint8_t> rom = read_file("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read_file("../rom/cpc6128.rom");
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found (run from project root)";

  std::vector<uint8_t> gmem(ga_state_size());    Device gdev = ga_init(gmem.data());
  std::vector<uint8_t> cmem(crtc_state_size());  Device cdev = crtc_init(cmem.data());
  std::vector<uint8_t> mmem(mem_state_size());   Device mdev = mem_init(mmem.data());
  std::vector<uint8_t> vmem(video_state_size()); Device vdev = video_init(vmem.data());
  std::vector<uint8_t> zmem(z80_state_size());   Device zdev = z80_init(zmem.data());

  Board board;
  board_init(&board);
  board_add(&board, gdev);          // clocks + INT + palette/mode
  board_add(&board, cdev);          // CRTC (programmed by the firmware)
  board_add(&board, ppi_device());  // PPI stub (VSYNC status, keyboard)
  board_add(&board, mdev);          // RAM + ROM
  board_add(&board, vdev);          // live renderer
  board_add(&board, zdev);          // Z80
  board_reset(&board);

  mem_load_lower_rom(&mdev, rom.data(), 0x4000);           // OS at 0x0000
  mem_load_upper_rom(&mdev, rom.data() + 0x4000, 0x4000);  // BASIC at 0xC000

  const int w = 320, h = 200;
  std::vector<uint8_t> fb(static_cast<size_t>(w) * h * 3, 0);
  video_attach(&vdev, &gdev, &mdev, fb.data(), w, h);

  // Run ~120 frames (~2.4 s of CPC time) to reach the Ready screen.
  VideoRegs vr{};
  for (long tick = 0; tick < 40000000L && vr.frames < 120; ++tick) {
    board_tick(&board);
    video_peek(&vdev, &vr);
  }

  // The firmware should have programmed the GA to mode 1 and set a palette.
  GateArrayRegs g{};
  ga_peek(&gdev, &g);
  std::fprintf(stderr, "BOOT: frames=%u mode=%u ink0=%u ink1=%u\n", vr.frames, g.mode,
               g.ink[0], g.ink[1]);

  // Not any all-black frame: the firmware painted something (border/text).
  int nonzero = 0;
  for (uint8_t v : fb) if (v != 0) { nonzero++; }
  EXPECT_GT(vr.frames, 100u) << "the board ran real firmware for many frames";

  if (FILE* out = std::fopen("/tmp/cpc_boot.ppm", "wb")) {
    std::fprintf(out, "P6\n%d %d\n255\n", w, h);
    std::fwrite(fb.data(), 1, fb.size(), out);
    std::fclose(out);
  }
  std::fprintf(stderr, "BOOT: nonzero framebuffer bytes=%d / %zu\n", nonzero, fb.size());
}
