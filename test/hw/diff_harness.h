/* diff_harness.h — the Gate B3 differential-harness ENGINE (beads-t907).
 *
 * Two builds, same seed + input, flag any divergence in OBSERVATIONS so a speed
 * change that alters what the emulated program sees fails CI on landing. Today
 * there is one faithful simulator, so the first consumer is a self-differential
 * determinism gate (two independent Machines must agree frame-for-frame). When
 * the fast modes (B4-B7) land, the SAME engine compares mode-A vs mode-B — only
 * the second machine's build flag changes.
 *
 * Two tiers, matching what each can catch:
 *   - cheap  (fb_hash):    FNV-1a over the 768x272 RGB24 framebuffer — the pure
 *                          OBSERVATION. Works today; needs nothing from B1.
 *   - deep   (state_hash): FNV-1a over every board Device's save() blob — the
 *                          logical machine state. Buildable because B1 (yjpy)
 *                          made save() deterministic + pointer-free across all
 *                          18 devices (device.h contract: logical, versioned).
 *
 * SHARED SEAM (cv2 invariant #1): the framebuffer this hashes is the one
 * caller-owned RGB24 buffer attached via Machine::attach_framebuffer — the SAME
 * buffer cv2 Phase 4 shmem-publishes. One extraction seam, two consumers; if
 * its layout/format changes, both change together.
 */
#ifndef TEST_HW_DIFF_HARNESS_H_
#define TEST_HW_DIFF_HARNESS_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "hw/device.h"
#include "subcycle/machine.h"

namespace diffharness {

// FNV-1a 64-bit — an order-sensitive rolling hash. Seed with the basis and fold
// bytes in; chain calls to hash several buffers into one digest.
constexpr uint64_t kFnvBasis = 1469598103934665603ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

inline uint64_t fnv1a(const uint8_t* bytes, size_t len,
                      uint64_t seed = kFnvBasis) {
  uint64_t hash = seed;
  for (size_t i = 0; i < len; ++i) {
    hash ^= bytes[i];
    hash *= kFnvPrime;
  }
  return hash;
}

// Cheap tier: hash the pure OBSERVATION (the displayed framebuffer).
inline uint64_t fb_hash(const uint8_t* pixels, size_t len) {
  return fnv1a(pixels, len);
}

// One board device paired with a stable label, for per-device diffing.
struct NamedDevice {
  const char* name;
  const Device* dev;
};

// The board's 18 devices in a FIXED order (both compared machines enumerate
// identically, so a plain rolling FNV over them is deterministic without an
// order-independent combiner). All are hashed, fitted or not: an unfitted
// peripheral sits at a deterministic construction default, so it costs nothing
// and adds coverage.
inline std::vector<NamedDevice> board_devices(
    const subcycle::Machine& machine) {
  return {
      {"z80", machine.z80()},
      {"gate_array", machine.gate_array()},
      {"crtc", machine.crtc()},
      {"ppi", machine.ppi()},
      {"psg", machine.psg()},
      {"memory", machine.memory()},
      {"video", machine.video()},
      {"fdc", machine.fdc()},
      {"tape", machine.tape()},
      {"printer", machine.printer()},
      {"amdrum", machine.amdrum()},
      {"mf2", machine.mf2()},
      {"m4", machine.m4()},
      {"asic", machine.asic()},
      {"amx", machine.amx()},
      {"smartwatch", machine.smartwatch()},
      {"symbiface", machine.symbiface()},
      {"probe", machine.probe()},
  };
}

// Deep tier, per device: the save() blob (versioned, pointer-free per device.h)
// hashed on its own. When the folded state_hash diverges, comparing these two
// vectors names the culprit device instead of leaving an opaque digest.
inline std::vector<uint64_t> per_device_state_hashes(
    const subcycle::Machine& machine) {
  std::vector<uint64_t> out;
  std::vector<uint8_t> blob;
  for (const NamedDevice& item : board_devices(machine)) {
    const size_t size = item.dev->state_size(item.dev->self);
    blob.resize(size);
    item.dev->save(item.dev->self, blob.data());
    out.push_back(fnv1a(blob.data(), size));
  }
  return out;
}

// Deep tier: fold every board device's per-device hash into one digest.
inline uint64_t machine_state_hash(const subcycle::Machine& machine) {
  uint64_t acc = kFnvBasis;
  for (uint64_t part : per_device_state_hashes(machine)) {
    acc = fnv1a(reinterpret_cast<const uint8_t*>(&part), sizeof(part), acc);
  }
  return acc;
}

struct FrameHashes {
  uint64_t fb;     // cheap tier: the displayed framebuffer
  uint64_t state;  // deep tier: all-device logical state
  bool operator==(const FrameHashes& other) const {
    return fb == other.fb && state == other.state;
  }
};

// Advance one frame and snapshot both tiers. `pixels` must be the buffer
// currently attached to `machine` — the harness never owns it (one seam).
inline FrameHashes step_and_hash(subcycle::Machine& machine,
                                 const uint8_t* pixels, size_t pixels_len) {
  machine.run_frame();
  return {fb_hash(pixels, pixels_len), machine_state_hash(machine)};
}

}  // namespace diffharness

#endif  // TEST_HW_DIFF_HARNESS_H_
