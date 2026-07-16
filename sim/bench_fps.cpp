/* bench_fps.cpp — headless FPS benchmark for the sub-cycle chip simulation.
 *
 * Purpose (beads-lcfa / plan §10-B4, risk #5): give the PGO 2-phase flow a
 * FIXED, deterministic headless trace to (a) train the profile on and (b)
 * measure frames-per-second on, so the shipping DUAL-PATH build's PGO number is
 * measured instead of quoted from the monomorphic SOLDERED=1 build.
 *
 * It cold-boots a 6128 from rom/cpc6128.rom, attaches a caller-owned RGB24
 * framebuffer (the one extraction seam, same as diff_harness.h), releases every
 * key each frame (the GUI's per-frame "no keys" feed), and runs run_frame() in a
 * loop. NO SDL, NO legacy loop — only the src/hw Devices + subcycle::Machine, so
 * the number reflects exactly the engine that ships.
 *
 * Dispatch path is a BUILD property, not a runtime flag:
 *   - default build       → Machine::run_frame() calls board_tick (pluggable,
 *                           fn-pointer array — the shipping default path).
 *   - built with SOLDERED → run_frame() calls tick_soldered (measurement-only
 *                           direct dispatch). Same chip logic, different
 *                           scheduler; see machine.cpp.
 * So `bench SOLDERED` vs `bench` (default) measures the two dispatchers; PGO is
 * layered on either by the `make pgo` flow.
 *
 * Usage: koncepcja_bench [--rom PATH] [--warmup N] [--frames N] [--quiet]
 *                        [--nocksum]
 *   Prints a human line to stderr and a machine-readable line to stdout:
 *     FPS=<f> MS_PER_FRAME=<ms> FRAMES=<n> WALL_MS=<ms> DISPATCH=<soldered|pluggable> CKSUM=<hex>
 *   --nocksum skips the per-frame framebuffer hash (44% of wall time at
 *   fast-tier speeds — F8 profile) and prints CKSUM=skipped: the FPS-iteration
 *   mode. Canonical checksum runs must NOT pass it.
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "subcycle/machine.h"

namespace {

constexpr int kW = subcycle::kFbWidth, kH = subcycle::kFbHeight;
constexpr size_t kFbLen = static_cast<size_t>(kW) * kH * 3;

std::vector<uint8_t> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
}

// FNV-1a over the framebuffer — printed so the optimizer cannot dead-code the
// per-frame rendering work, and so a broken build (black/frozen frame) is
// visible as a suspicious checksum rather than a fast-but-empty run.
uint64_t fb_cksum(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
  return h;
}

void feed_no_keys(subcycle::Machine& m) {
  for (int row = 0; row < 10; ++row) m.set_key_row(static_cast<uint8_t>(row), 0xFF);
}

// Minimal CPR (RIFF/AMS!) parser — same as plus_cart_boot_test's: 12-byte
// header, then cbNN chunks placed at 16K bank NN. Flat 512K image, empty on
// failure. Lets the bench measure a real full-screen graphics/sprite workload (a
// Plus cartridge) instead of the near-blank 6128 boot, where the pixel path
// barely runs.
std::vector<uint8_t> parse_cpr(const std::vector<uint8_t>& raw) {
  constexpr size_t kBank = 0x4000, kBanks = 32;
  std::vector<uint8_t> flat(kBank * kBanks, 0);
  if (raw.size() < 12 || std::memcmp(raw.data(), "RIFF", 4) != 0 ||
      std::memcmp(raw.data() + 8, "AMS!", 4) != 0)
    return {};
  size_t off = 12;
  int placed = 0;
  while (off + 8 <= raw.size()) {
    const uint32_t sz = static_cast<uint32_t>(raw[off + 4]) |
                        (static_cast<uint32_t>(raw[off + 5]) << 8) |
                        (static_cast<uint32_t>(raw[off + 6]) << 16) |
                        (static_cast<uint32_t>(raw[off + 7]) << 24);
    if (raw[off] == 'c' && raw[off + 1] == 'b') {
      const int bank = (raw[off + 2] - '0') * 10 + (raw[off + 3] - '0');
      const size_t keep = sz < kBank ? sz : kBank;
      if (bank >= 0 && bank < static_cast<int>(kBanks) &&
          off + 8 + keep <= raw.size()) {
        std::memcpy(&flat[static_cast<size_t>(bank) * kBank], &raw[off + 8], keep);
        placed++;
      }
    }
    off += 8 + ((sz + 1) & ~1u);  // chunks are word-padded
  }
  return placed ? flat : std::vector<uint8_t>{};
}

}  // namespace

int main(int argc, char** argv) {
  const char* rom_path = "rom/cpc6128.rom";
  const char* cpr_path = nullptr;             // --cpr → Plus cartridge workload
  const char* amsdos_path = "rom/amsdos.rom";  // AMSDOS ROM for the cartridge
  int warmup = 200;    // boot past the banner; excluded from the timed window
  int frames = 2000;   // ~40 s of CPC time — long enough to be noise-stable
  bool quiet = false;
  bool nocksum = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--rom") && i + 1 < argc) rom_path = argv[++i];
    else if (!std::strcmp(argv[i], "--cpr") && i + 1 < argc) cpr_path = argv[++i];
    else if (!std::strcmp(argv[i], "--amsdos") && i + 1 < argc) amsdos_path = argv[++i];
    else if (!std::strcmp(argv[i], "--warmup") && i + 1 < argc) warmup = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--quiet")) quiet = true;
    else if (!std::strcmp(argv[i], "--nocksum")) nocksum = true;
  }
  if (frames < 1) frames = 1;

  // These back the machine's ROM/cartridge for its whole lifetime — attach_*
  // keeps pointers into them, so they must outlive `machine`.
  subcycle::Machine machine;
  std::vector<uint8_t> rom, cart, amsdos;
  if (cpr_path) {  // Plus cartridge: full-screen graphics + sprites — the real
                   // pixel-path workload the stock 6128 boot never exercises.
    std::vector<uint8_t> raw = read_file(cpr_path);
    if (raw.size() < 0x4000)
      raw = read_file((std::string("../") + cpr_path).c_str());
    cart = parse_cpr(raw);
    if (cart.empty()) {
      std::fprintf(stderr, "bench: %s is not a valid RIFF/AMS! cartridge\n",
                   cpr_path);
      return 1;
    }
    if (!machine.build(cart.data(), 0x8000)) {
      std::fprintf(stderr, "bench: Machine::build (cartridge) failed\n");
      return 1;
    }
    machine.attach_cartridge(cart.data(), cart.size());
    machine.set_asic(true);  // model 3: the ASIC is on the board
    amsdos = read_file(amsdos_path);
    if (amsdos.size() < 0x4000)
      amsdos = read_file((std::string("../") + amsdos_path).c_str());
    if (amsdos.size() >= 0x4000)
      machine.attach_amsdos(amsdos.data(), amsdos.size());
  } else {  // classic: cold-boot a 6128 from its system ROM
    rom = read_file(rom_path);
    if (rom.size() < 0x8000)
      rom = read_file((std::string("../") + rom_path).c_str());
    if (rom.size() < 0x8000) {
      std::fprintf(stderr,
                   "bench: %s not found / too small (run from project root)\n",
                   rom_path);
      return 1;
    }
    if (!machine.build(rom.data(), rom.size())) {
      std::fprintf(stderr, "bench: Machine::build failed\n");
      return 1;
    }
  }
  std::vector<uint8_t> fb(kFbLen, 0);
  machine.attach_framebuffer(fb.data(), kW, kH);

#ifdef SOLDERED
  const char* dispatch = "soldered";
#else
  const char* dispatch = "pluggable";
#endif

  for (int f = 0; f < warmup; ++f) { feed_no_keys(machine); machine.run_frame(); }

  const auto t0 = std::chrono::steady_clock::now();
  uint64_t cksum = 0;
  for (int f = 0; f < frames; ++f) {
    feed_no_keys(machine);
    machine.run_frame();
    // Rolling combine (not XOR: identical static post-boot frames would cancel
    // on even counts and read as a false "empty" run). Printed so the optimizer
    // cannot dead-code the per-frame render work.
    if (!nocksum)
      cksum = fb_cksum(fb.data(), kFbLen) ^ (cksum * 1099511628211ULL);
  }
  const auto t1 = std::chrono::steady_clock::now();

  const double wall_ms =
      std::chrono::duration<double, std::milli>(t1 - t0).count();
  const double ms_per_frame = wall_ms / frames;
  const double fps = 1000.0 / ms_per_frame;

  if (!quiet) {
    std::fprintf(stderr,
                 "bench[%s]: %d frames (%d warmup) in %.2f ms  ->  %.2f ms/frame  "
                 "%.1f FPS  (%.0f%% of 50Hz realtime)  cksum=%016llx\n",
                 dispatch, frames, warmup, wall_ms, ms_per_frame, fps,
                 fps / 50.0 * 100.0, static_cast<unsigned long long>(cksum));
  }
  if (nocksum) {
    std::printf(
        "FPS=%.2f MS_PER_FRAME=%.4f FRAMES=%d WALL_MS=%.2f DISPATCH=%s "
        "CKSUM=skipped\n",
        fps, ms_per_frame, frames, wall_ms, dispatch);
  } else {
    std::printf(
        "FPS=%.2f MS_PER_FRAME=%.4f FRAMES=%d WALL_MS=%.2f DISPATCH=%s "
        "CKSUM=%016llx\n",
        fps, ms_per_frame, frames, wall_ms, dispatch,
        static_cast<unsigned long long>(cksum));
  }
  return 0;
}
