/* record_replay.h — deterministic input record/replay for the sub-cycle CPC
 * (Gate B2, beads-icz0).
 *
 * WHY: the differential harness (test/hw/diff_harness.h) needs to replay a
 * frozen input trace as a CI corpus — "apply input event at cycle N", exactly,
 * reproducibly. This module is the trace format plus the record/replay engine.
 *
 * MODEL: every host input the Machine accepts (keyboard/joystick matrix rows,
 * single-key taps, AMX mouse, Symbiface mouse) is an InputEvent tagged with the
 * ABSOLUTE master cycle (Machine::master_cycle()) it applies at. On replay the
 * Player rides Machine's per-cycle hook (Machine::set_cycle_hook) and applies
 * each event the master cycle the machine reaches its tag — so two independent
 * machines fed the same trace observe identical state, cycle for cycle.
 *
 * DETERMINISM IS NOT UNIVERSAL: smartwatch + Symbiface RTC read host wall-clock
 * and Symbiface IDE + the M4 read the host filesystem. A reproducible corpus
 * MUST exclude those (apply_deterministic_device_set) and feed ALL host input
 * from the trace — which is the whole point of this file.
 *
 * This layer stays test-framework-free: it produces a reproducible run; the
 * caller (a harness test) hashes the framebuffer / device state with
 * diff_harness.h. */
#ifndef KONCPC_SUBCYCLE_RECORD_REPLAY_H
#define KONCPC_SUBCYCLE_RECORD_REPLAY_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "subcycle/machine.h"

namespace recordreplay {

// The host input surfaces the Machine exposes, in the trace's tagged form.
enum class EventKind : uint8_t {
  kKeyRow = 0,    // set_key_row(row, columns) — keyboard AND joystick matrix
  kKey = 1,       // key(code, down) — packed (row << 4 | bit)
  kAmxMouse = 2,  // amx_mouse_feed(dx, dy, buttons)
  kSymMouse = 3,  // symbiface_mouse_packet(pkt)
};

// One input event tagged with the absolute master cycle it applies at. A single
// flat record covers every kind (no union / no heap): the fields are reused per
// kind as documented, and unused fields stay value-initialised to zero so the
// wire form is canonical.
struct InputEvent {
  uint64_t cycle = 0;  // Machine::master_cycle() at which this applies
  EventKind kind = EventKind::kKeyRow;
  uint8_t a = 0;   // kKeyRow: row · kKey: code · mouse: unused
  uint8_t b = 0;   // kKeyRow: columns · kKey: down(0/1) · kAmxMouse: buttons ·
                   // kSymMouse: packet
  int16_t dx = 0;  // kAmxMouse: dx
  int16_t dy = 0;  // kAmxMouse: dy

  bool operator==(const InputEvent& o) const {
    return cycle == o.cycle && kind == o.kind && a == o.a && b == o.b &&
           dx == o.dx && dy == o.dy;
  }
  bool operator!=(const InputEvent& o) const { return !(*this == o); }
};

// Wire format: a 12-byte header then N fixed 15-byte little-endian records.
// Endian-explicit byte packing (no struct memcpy / no padding) so a trace is
// portable across the CI's MINGW32/MINGW64/macOS builds.
constexpr uint8_t kMagic[4] = {'K', 'R', 'P', 'L'};  // konCePCja RePLay
constexpr uint32_t kFormatVersion = 1;
constexpr size_t kHeaderBytes = 12;  // magic(4) + version(4) + count(4)
constexpr size_t kRecordBytes = 15;  // cycle(8) + kind(1) + a(1) + b(1)
                                     //   + dx(2 LE) + dy(2 LE)

// Recorder: collect events during a live/scripted session, in tag order, then
// serialise to the frozen trace blob.
class Recorder {
 public:
  void key_row(uint64_t cycle, uint8_t row, uint8_t columns);
  void key(uint64_t cycle, uint8_t code, bool down);
  void amx_mouse(uint64_t cycle, int dx, int dy, uint8_t buttons);
  void sym_mouse(uint64_t cycle, uint8_t packet);
  void add(const InputEvent& ev) { events_.push_back(ev); }

  const std::vector<InputEvent>& events() const { return events_; }
  bool empty() const { return events_.empty(); }
  void clear() { events_.clear(); }

  // The frozen trace blob (header + records). Deterministic for given events.
  std::vector<uint8_t> serialize() const;

 private:
  std::vector<InputEvent> events_;
};

// Parse a serialised trace. Returns false on a truncated/mis-magic/bad-version
// blob (never throws, never partial-fills). On success `out` holds the events.
bool deserialize(const uint8_t* data, size_t len, std::vector<InputEvent>* out);

// Persist / load a trace file. Portable std::fstream (no POSIX); return false
// and touch nothing further on any I/O error. Handy for building a CI corpus of
// trace files on disk.
bool save_trace(const char* path, const Recorder& recorder);
bool load_trace(const char* path, std::vector<InputEvent>* out);

// Player: replay a frozen trace against a Machine, applying each event EXACTLY
// at its tagged master cycle. attach() wires it as the machine's per-cycle
// input source; from then on the machine's own run_frame drives it.
class Player {
 public:
  Player() = default;
  explicit Player(std::vector<InputEvent> events);

  // Install as `machine`'s cycle hook and bind to it for the trampoline. The
  // Player must outlive the machine's use of it.
  void attach(subcycle::Machine& machine);

  // Apply every not-yet-applied event whose tag <= master_cycle (the hook path;
  // also callable directly by a manual driver).
  void apply_pending(subcycle::Machine& machine, uint64_t master_cycle);

  void rewind() { cursor_ = 0; }
  bool done() const { return cursor_ >= events_.size(); }
  size_t applied() const { return cursor_; }
  const std::vector<InputEvent>& events() const { return events_; }

 private:
  static void trampoline(void* ctx, uint64_t master_cycle);

  std::vector<InputEvent> events_;  // sorted by cycle (stable) at construction
  subcycle::Machine* machine_ = nullptr;  // bound at attach() for the hook
  size_t cursor_ = 0;
};

// --- Deterministic corpus ---------------------------------------------------

// The host-coupled devices a deterministic corpus MUST exclude — their state is
// driven by wall-clock or the host filesystem, so no trace could reproduce it:
//   - "smartwatch": Dobbertin DS1216 phantom RTC → host system time
//   - "symbiface":  DS12887 RTC (host time) + IDE images (host filesystem)
//   - "m4":         virtual SD filesystem + wall-clock → host FS + time
// (The ASIC/AmDrum/AMX/MF2 peripherals are deterministic — pure logic fed only
// from the trace — so they may stay; only these three read the host.)
inline constexpr const char* kHostCoupledDevices[] = {"smartwatch", "symbiface",
                                                      "m4"};
inline constexpr size_t kHostCoupledCount =
    sizeof(kHostCoupledDevices) / sizeof(kHostCoupledDevices[0]);

// Force a Machine into the deterministic device set: unplug every host-coupled
// peripheral so the run depends only on ROM + trace. Idempotent (the unplugged
// state is also the construction default; this makes the exclusion explicit and
// robust to a caller that plugged one).
void apply_deterministic_device_set(subcycle::Machine& machine);

// One reproducible corpus run: build a 6128 from `rom`, force the deterministic
// device set, attach the caller-owned framebuffer `fb` (w*h*3), replay `player`
// applying each event at its exact cycle, and advance `frames` frames. Returns
// false if the ROM is too short to build. The caller hashes `fb` / device state
// (diff_harness.h) — this stays test-framework-free.
bool run_corpus(subcycle::Machine& machine, const uint8_t* rom, size_t rom_len,
                uint8_t* fb, int w, int h, Player& player, int frames);

}  // namespace recordreplay

#endif  // KONCPC_SUBCYCLE_RECORD_REPLAY_H
