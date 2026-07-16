/* record_replay.cpp — see record_replay.h. Deterministic input record/replay
 * for the sub-cycle CPC (Gate B2, beads-icz0). */

#include "record_replay.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace recordreplay {
namespace {

// Little-endian byte pushers/readers — explicit so the trace is byte-identical
// on every CI target regardless of host endianness or struct padding.
// NOLINTNEXTLINE(readability-non-const-parameter): pointer written through a cast or passed to a non-const callee
void put_u16(std::vector<uint8_t> *out, uint16_t v) {
  out->push_back(static_cast<uint8_t>(v & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

// NOLINTNEXTLINE(readability-non-const-parameter): pointer written through a cast or passed to a non-const callee
void put_u32(std::vector<uint8_t> *out, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    out->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

// NOLINTNEXTLINE(readability-non-const-parameter): pointer written through a cast or passed to a non-const callee
void put_u64(std::vector<uint8_t> *out, uint64_t v) {
  for (int i = 0; i < 8; ++i)
    out->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

uint16_t get_u16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t get_u32(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
  return v;
}

uint64_t get_u64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

bool valid_kind(uint8_t k) {
  return k <= static_cast<uint8_t>(EventKind::kSymMouse);
}

}  // namespace

void Recorder::key_row(uint64_t cycle, uint8_t row, uint8_t columns) {
  events_.push_back(InputEvent{cycle, EventKind::kKeyRow, row, columns, 0, 0});
}

void Recorder::key(uint64_t cycle, uint8_t code, bool down) {
  events_.push_back(InputEvent{cycle, EventKind::kKey, code,
                               static_cast<uint8_t>(down ? 1 : 0), 0, 0});
}

void Recorder::amx_mouse(uint64_t cycle, int dx, int dy, uint8_t buttons) {
  events_.push_back(InputEvent{cycle, EventKind::kAmxMouse, 0, buttons,
                               static_cast<int16_t>(dx),
                               static_cast<int16_t>(dy)});
}

void Recorder::sym_mouse(uint64_t cycle, uint8_t packet) {
  events_.push_back(InputEvent{cycle, EventKind::kSymMouse, 0, packet, 0, 0});
}

std::vector<uint8_t> Recorder::serialize() const {
  std::vector<uint8_t> out;
  out.reserve(kHeaderBytes + (events_.size() * kRecordBytes));
  out.insert(out.end(), kMagic, kMagic + 4);
  put_u32(&out, kFormatVersion);
  put_u32(&out, static_cast<uint32_t>(events_.size()));
  for (const InputEvent& ev : events_) {
    put_u64(&out, ev.cycle);
    out.push_back(static_cast<uint8_t>(ev.kind));
    out.push_back(ev.a);
    out.push_back(ev.b);
    put_u16(&out, static_cast<uint16_t>(ev.dx));
    put_u16(&out, static_cast<uint16_t>(ev.dy));
  }
  return out;
}

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other translation units/tests; internal linkage would break the link
bool deserialize(const uint8_t *data, size_t len,
                 // NOLINTNEXTLINE(readability-non-const-parameter): pointer written through a cast or passed to a non-const callee
                 std::vector<InputEvent> *out) {
  if (out == nullptr || data == nullptr || len < kHeaderBytes) return false;
  if (data[0] != kMagic[0] || data[1] != kMagic[1] || data[2] != kMagic[2] ||
      data[3] != kMagic[3])
    return false;
  if (get_u32(data + 4) != kFormatVersion) return false;
  const uint32_t count = get_u32(data + 8);
  if (len < kHeaderBytes + (static_cast<size_t>(count) * kRecordBytes))
    return false;

  std::vector<InputEvent> parsed;
  parsed.reserve(count);
  const uint8_t* p = data + kHeaderBytes;
  for (uint32_t i = 0; i < count; ++i, p += kRecordBytes) {
    if (!valid_kind(p[8])) return false;
    InputEvent ev;
    ev.cycle = get_u64(p);
    ev.kind = static_cast<EventKind>(p[8]);
    ev.a = p[9];
    ev.b = p[10];
    ev.dx = static_cast<int16_t>(get_u16(p + 11));
    ev.dy = static_cast<int16_t>(get_u16(p + 13));
    parsed.push_back(ev);
  }
  out->swap(parsed);
  return true;
}

bool save_trace(const char* path, const Recorder& recorder) {
  if (path == nullptr) return false;
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) return false;
  const std::vector<uint8_t> blob = recorder.serialize();
  if (!blob.empty())
    file.write(reinterpret_cast<const char*>(blob.data()),
               static_cast<std::streamsize>(blob.size()));
  file.flush();
  return static_cast<bool>(file);  // false if any write/flush failed
}

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other translation units/tests; internal linkage would break the link
bool load_trace(const char* path, std::vector<InputEvent>* out) {
  if (path == nullptr || out == nullptr) return false;
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
  return deserialize(blob.data(), blob.size(), out);
}

Player::Player(std::vector<InputEvent> events) : events_(std::move(events)) {
  // Stable-sort by tag so events apply in cycle order even if the recorder
  // appended out of order; equal-cycle events keep their recorded order.
  std::stable_sort(events_.begin(), events_.end(),
                   [](const InputEvent& l, const InputEvent& r) {
                     return l.cycle < r.cycle;
                   });
}

void Player::trampoline(void* ctx, uint64_t master_cycle) {
  auto* self = static_cast<Player*>(ctx);
  if (self->machine_ != nullptr)
    self->apply_pending(*self->machine_, master_cycle);
}

void Player::attach(subcycle::Machine& machine) {
  machine_ = &machine;
  cursor_ = 0;
  machine.set_cycle_hook(&Player::trampoline, this);
}

void Player::apply_pending(subcycle::Machine& machine, uint64_t master_cycle) {
  while (cursor_ < events_.size() && events_[cursor_].cycle <= master_cycle) {
    const InputEvent& ev = events_[cursor_++];
    switch (ev.kind) {
      case EventKind::kKeyRow:
        machine.set_key_row(ev.a, ev.b);
        break;
      case EventKind::kKey:
        machine.key(ev.a, ev.b != 0);
        break;
      case EventKind::kAmxMouse:
        machine.amx_mouse_feed(ev.dx, ev.dy, ev.b);
        break;
      case EventKind::kSymMouse:
        machine.symbiface_mouse_packet(ev.b);
        break;
    }
  }
}

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other translation units/tests; internal linkage would break the link
void apply_deterministic_device_set(subcycle::Machine& machine) {
  // Unplug the host-coupled peripherals (kHostCoupledDevices) so a replay is
  // reproducible from ROM + trace alone. Inert peripherals still hash from a
  // fixed construction default, so this costs nothing and adds coverage.
  machine.set_smartwatch(false);
  machine.set_symbiface(false);
  machine.set_m4(false);
}

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other translation units/tests; internal linkage would break the link
bool run_corpus(subcycle::Machine& machine, const uint8_t* rom, size_t rom_len,
                uint8_t* fb, int w, int h, Player& player, int frames) {
  if (!machine.build(rom, rom_len)) return false;
  apply_deterministic_device_set(machine);
  machine.attach_framebuffer(fb, w, h);
  player.rewind();
  player.attach(machine);
  for (int f = 0; f < frames; ++f) machine.run_frame();
  return true;
}

}  // namespace recordreplay
