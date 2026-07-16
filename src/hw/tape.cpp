/* tape.cpp — cassette deck Device. THE SPEC: docs/hardware/tape-device.md. */

#include "tape.h"

#include <cstring>
#include <new>

#include "buses.h"

namespace {

// CDT durations are 3.5 MHz T-states; 3.5/16 = 7/32 exactly: pulses load as
// t-states * 32 sub-units, the deck consumes 7 sub-units per master cycle.
constexpr int32_t kSubPerCycle = 7;
constexpr int32_t kSubPerTs = 32;
constexpr uint32_t kCyclesPerMs = 16000;

enum Phase : uint8_t {
  PH_IDLE = 0,  // between blocks / not playing
  PH_PILOT,
  PH_SYNC1,
  PH_SYNC2,
  PH_DATA,
  PH_PULSESEQ,  // 0x13: a list of explicit pulses
  PH_DIRECT,    // 0x15: each data BIT is the line level for t_bit0 t-states
  PH_PAUSE,
};

struct tape_state {
  // Playback cursor (serialized).
  uint32_t pos = 0;    // byte offset of the CURRENT block header
  uint32_t block = 0;  // block ordinal
  uint8_t phase = PH_IDLE;
  uint8_t level = 0;
  uint8_t playing = 0;
  uint8_t line_mode = 0;
  uint8_t line_level = 0;
  uint8_t motor_seen = 0;
  uint8_t error = 0;
  // Current-block decode state.
  uint32_t data_off = 0;  // offset of the data bytes (0x10/0x11/0x14)
  uint32_t data_len = 0;
  uint32_t byte_idx = 0;
  uint8_t bit_idx = 0;       // 0..7, MSB first
  uint8_t last_used = 8;     // used bits in the final byte
  uint8_t pulse_half = 0;    // which half of the bit's pulse pair
  uint16_t pulses_left = 0;  // pilot / pure-tone / pulse-seq counter
  uint16_t seq_idx = 0;
  // Timings for the block in flight (t-states), pause in ms.
  uint16_t t_pilot = 2168, t_sync1 = 667, t_sync2 = 735, t_bit0 = 855,
           t_bit1 = 1710;
  uint16_t pause_ms = 0;
  int32_t sub_remaining = 0;  // current pulse, in 1/32-t-state sub-units
  uint32_t pause_cycles = 0;  // pause countdown, master cycles

  // Live wiring (never serialized) — keep LAST.
  const uint8_t* cdt = nullptr;
  size_t len = 0;

  // Decoded-bit observation ring for the host tape scope's BITS view. Placed
  // AFTER cdt/len — past the offsetof(cdt) save cut — so it is never serialized
  // (pure host visualization). Each data bit the deck emits is recorded here
  // and drained by tape_drain_bits. 256-entry; uint8_t indices wrap mod 256 and
  // a full ring drops the oldest bit.
  uint8_t bit_ring[256] = {};
  uint8_t bit_wr = 0;  // write index
  uint8_t bit_rd = 0;  // drain index
};

tape_state* self_of(void* self) { return static_cast<tape_state*>(self); }

uint8_t rd8(const tape_state* t, uint32_t off) {
  return off < t->len ? t->cdt[off] : 0;
}
uint16_t rd16(const tape_state* t, uint32_t off) {
  return static_cast<uint16_t>(rd8(t, off) | (rd8(t, off + 1) << 8));
}
uint32_t rd24(const tape_state* t, uint32_t off) {
  return rd8(t, off) | (rd8(t, off + 1) << 8) | (rd8(t, off + 2) << 16);
}

// Byte length of the CDT block at `pos` (0 = unsizable → stop walking). The
// host block table (tape_scan_blocks) walks the same sizes via
// tape_cdt_block_len, so a block ordinal here matches the UI's.
uint32_t block_len(const tape_state* t, uint32_t pos) {
  return tape_cdt_block_len(t->cdt, static_cast<uint32_t>(t->len), pos);
}

void load_pulse(tape_state* t, uint16_t ts) {
  t->sub_remaining = static_cast<int32_t>(ts) * kSubPerTs;
}

// Record one decoded data bit for the host scope's BITS view (drained by
// tape_drain_bits). uint8_t indices wrap mod 256; if the ring is full the
// oldest unread bit is dropped so recent history always wins.
void push_bit(tape_state* t, uint8_t bit) {
  t->bit_ring[t->bit_wr++] = bit ? 1 : 0;
  if (t->bit_wr == t->bit_rd) t->bit_rd++;
}

void stop(tape_state* t) {
  t->playing = 0;
  t->phase = PH_IDLE;
}

// Enter the block at t->pos; skip metadata; set up the pulse machinery.
void start_block(tape_state* t) {
  if (t->cdt == nullptr) {
    stop(t);
    return;
  }
  for (;;) {
    if (t->pos >= t->len) {
      stop(t);
      return;  // end of tape
    }
    const uint8_t id = rd8(t, t->pos);
    const uint32_t body = t->pos + 1;
    switch (id) {
      case 0x10:  // standard speed data
        t->pause_ms = rd16(t, body);
        t->data_len = rd16(t, body + 2);
        t->data_off = body + 4;
        t->t_pilot = 2168;
        t->t_sync1 = 667;
        t->t_sync2 = 735;
        t->t_bit0 = 855;
        t->t_bit1 = 1710;
        t->last_used = 8;
        t->pulses_left = rd8(t, t->data_off) < 0x80 ? 8063 : 3223;
        t->phase = PH_PILOT;
        load_pulse(t, t->t_pilot);
        t->byte_idx = 0;
        t->bit_idx = 0;
        t->pulse_half = 0;
        t->pos = t->data_off + t->data_len;
        return;
      case 0x11:  // turbo speed data
        t->t_pilot = rd16(t, body);
        t->t_sync1 = rd16(t, body + 2);
        t->t_sync2 = rd16(t, body + 4);
        t->t_bit0 = rd16(t, body + 6);
        t->t_bit1 = rd16(t, body + 8);
        t->pulses_left = rd16(t, body + 10);
        t->last_used = rd8(t, body + 12);
        t->pause_ms = rd16(t, body + 13);
        t->data_len = rd24(t, body + 15);
        t->data_off = body + 18;
        t->phase = PH_PILOT;
        load_pulse(t, t->t_pilot);
        t->byte_idx = 0;
        t->bit_idx = 0;
        t->pulse_half = 0;
        t->pos = t->data_off + t->data_len;
        return;
      case 0x12:  // pure tone
        t->t_pilot = rd16(t, body);
        t->pulses_left = rd16(t, body + 2);
        t->phase = PH_PILOT;
        // Pure tone ends without sync: reuse PILOT with a "no data" marker.
        t->data_len = 0;
        t->data_off = 0;
        t->pause_ms = 0;
        load_pulse(t, t->t_pilot);
        t->pos = body + 4;
        return;
      case 0x13:  // pulse sequence
        t->pulses_left = rd8(t, body);
        t->seq_idx = 0;
        t->data_off = body + 1;  // pulse table
        t->phase = PH_PULSESEQ;
        load_pulse(t, rd16(t, t->data_off));
        t->pos = body + 1 + (static_cast<uint32_t>(t->pulses_left) * 2);
        return;
      case 0x14:  // pure data
        t->t_bit0 = rd16(t, body);
        t->t_bit1 = rd16(t, body + 2);
        t->last_used = rd8(t, body + 4);
        t->pause_ms = rd16(t, body + 5);
        t->data_len = rd24(t, body + 7);
        t->data_off = body + 10;
        t->phase = PH_DATA;
        t->byte_idx = 0;
        t->bit_idx = 0;
        t->pulse_half = 0;
        push_bit(t, (rd8(t, t->data_off) & 0x80) ? 1 : 0);  // first data bit
        load_pulse(t, (rd8(t, t->data_off) & 0x80) ? t->t_bit1 : t->t_bit0);
        t->pos = t->data_off + t->data_len;
        return;
      case 0x15: {  // direct recording: each bit IS the level, no pulse pairs
        t->t_bit0 = rd16(t, body);  // t-states per sample
        t->pause_ms = rd16(t, body + 2);
        t->last_used = rd8(t, body + 4);
        t->data_len = rd24(t, body + 5);
        t->data_off = body + 8;
        t->pos = t->data_off + t->data_len;
        if (t->data_len == 0) {  // degenerate: nothing to play
          t->block++;
          continue;
        }
        t->phase = PH_DIRECT;
        t->byte_idx = 0;
        t->bit_idx = 0;
        t->level = (rd8(t, t->data_off) & 0x80) ? 1 : 0;
        push_bit(t, t->level);
        load_pulse(t, t->t_bit0);
        return;
      }
      case 0x20: {  // pause / stop
        const uint16_t ms = rd16(t, body);
        t->pos = body + 2;
        if (ms == 0) {
          stop(t);
          return;
        }
        t->phase = PH_PAUSE;
        t->pause_cycles = ms * kCyclesPerMs;
        t->level = 0;
        return;
      }
      // Metadata: skip by the block's own length rule.
      case 0x21:
        t->pos = body + 1 + rd8(t, body);
        t->block++;
        continue;
      case 0x22:
        t->pos = body;
        t->block++;
        continue;
      case 0x30:
        t->pos = body + 1 + rd8(t, body);
        t->block++;
        continue;
      case 0x31:
        t->pos = body + 2 + rd8(t, body + 1);
        t->block++;
        continue;
      case 0x32:
        t->pos = body + 2 + rd16(t, body);
        t->block++;
        continue;
      case 0x33:
        t->pos = body + 1 + (rd8(t, body) * 3u);
        t->block++;
        continue;
      default:
        t->error = 1;  // never guess a length (spec §3)
        stop(t);
        return;
    }
  }
}

void next_block(tape_state* t) {
  t->block++;
  if (t->pause_ms) {
    t->phase = PH_PAUSE;
    t->pause_cycles = t->pause_ms * kCyclesPerMs;
    t->pause_ms = 0;
    t->level = 0;
    return;
  }
  start_block(t);
}

// One pulse boundary reached: toggle and load the next pulse (or advance).
void pulse_done(tape_state* t) {
  t->level ^= 1;
  switch (t->phase) {
    case PH_PILOT:
      if (--t->pulses_left > 0) {
        load_pulse(t, t->t_pilot);
        return;
      }
      if (t->data_off == 0 && t->data_len == 0) {  // pure tone: no sync/data
        next_block(t);
        return;
      }
      t->phase = PH_SYNC1;
      load_pulse(t, t->t_sync1);
      return;
    case PH_SYNC1:
      t->phase = PH_SYNC2;
      load_pulse(t, t->t_sync2);
      return;
    case PH_SYNC2:
      t->phase = PH_DATA;
      t->pulse_half = 0;
      push_bit(t, (rd8(t, t->data_off) & 0x80) ? 1 : 0);  // first data bit
      load_pulse(t, (rd8(t, t->data_off) & 0x80) ? t->t_bit1 : t->t_bit0);
      return;
    case PH_DATA: {
      if (t->pulse_half == 0) {  // second half of the same bit
        t->pulse_half = 1;
        const uint8_t byte = rd8(t, t->data_off + t->byte_idx);
        const uint8_t bit = (byte >> (7 - t->bit_idx)) & 1;
        load_pulse(t, bit ? t->t_bit1 : t->t_bit0);
        return;
      }
      t->pulse_half = 0;
      t->bit_idx++;
      const bool last_byte = (t->byte_idx + 1 == t->data_len);
      const uint8_t bits_in_byte = last_byte ? t->last_used : 8;
      if (t->bit_idx >= bits_in_byte) {
        t->bit_idx = 0;
        t->byte_idx++;
      }
      if (t->byte_idx >= t->data_len) {
        next_block(t);
        return;
      }
      const uint8_t byte = rd8(t, t->data_off + t->byte_idx);
      const uint8_t bit = (byte >> (7 - t->bit_idx)) & 1;
      push_bit(t, bit);  // next data bit
      load_pulse(t, bit ? t->t_bit1 : t->t_bit0);
      return;
    }
    case PH_PULSESEQ:
      if (--t->pulses_left > 0) {
        t->seq_idx++;
        load_pulse(t, rd16(t, t->data_off + (t->seq_idx * 2u)));
        return;
      }
      next_block(t);
      return;
    case PH_DIRECT: {
      t->level ^= 1;  // undo the entry toggle: each sample SETS the level
                      // from its own bit; the last one persists at block end
      t->bit_idx++;
      const bool last_byte = (t->byte_idx + 1 == t->data_len);
      const uint8_t bits_in_byte = last_byte ? t->last_used : 8;
      if (t->bit_idx >= bits_in_byte) {
        t->bit_idx = 0;
        t->byte_idx++;
      }
      if (t->byte_idx >= t->data_len) {
        next_block(t);
        return;
      }
      const uint8_t bit =
          (rd8(t, t->data_off + t->byte_idx) >> (7 - t->bit_idx)) & 1;
      t->level = bit;
      push_bit(t, bit);
      load_pulse(t, t->t_bit0);
      return;
    }
    default:
      return;
  }
}

void tape_tick(void* self, const Bus* __restrict in, Bus* __restrict out) {
  tape_state* t = self_of(self);
  t->motor_seen = in->tape.motor ? 1 : 0;

  if (t->line_mode) {  // live deck on the DIN socket: follow the host level
    t->level = (in->tape.motor && t->line_level) ? 1 : 0;
    out->tape.rdata = t->level != 0;
    return;
  }

  if (t->playing && in->tape.motor && t->phase != PH_IDLE) {
    if (t->phase == PH_PAUSE) {
      if (t->pause_cycles > 0) {
        t->pause_cycles--;
      } else {
        start_block(t);
      }
    } else {
      t->sub_remaining -= kSubPerCycle;
      if (t->sub_remaining <= 0) pulse_done(t);
    }
  }
  out->tape.rdata = t->level != 0;
}

void tape_reset(void* self) {
  // A CPC reset does not touch the deck: media, PLAY and position persist
  // (the cassette recorder is a separate appliance).
  (void)self;
}

size_t tape_dev_state_size(const void* /*unused*/) {
  return offsetof(tape_state, cdt) + 1;
}
void tape_save(const void* self, void* buf) {
  uint8_t* b = static_cast<uint8_t*>(buf);
  b[0] = 1;
  std::memcpy(b + 1, self, offsetof(tape_state, cdt));
}
void tape_load(void* self, const void* buf) {
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  if (b[0] != 1) return;
  std::memcpy(self, b + 1, offsetof(tape_state, cdt));
}

}  // namespace

extern "C" {

size_t tape_state_size(void) { return sizeof(tape_state); }

uint32_t tape_cdt_block_len(const uint8_t* cdt, uint32_t len, uint32_t pos) {
  const auto b8 = [&](uint32_t off) -> uint8_t {
    return (cdt != nullptr && off < len) ? cdt[off] : 0;
  };
  const auto b16 = [&](uint32_t off) -> uint32_t {
    return b8(off) | (static_cast<uint32_t>(b8(off + 1)) << 8);
  };
  const auto b24 = [&](uint32_t off) -> uint32_t {
    return b16(off) | (static_cast<uint32_t>(b8(off + 2)) << 16);
  };
  const auto b32 = [&](uint32_t off) -> uint32_t {
    return b24(off) | (static_cast<uint32_t>(b8(off + 3)) << 24);
  };
  switch (b8(pos)) {
    case 0x10:
      return b16(pos + 0x03) + 0x04u + 1u;
    case 0x11:
      return b24(pos + 0x10) + 0x12u + 1u;
    case 0x12:
      return 4u + 1u;
    case 0x13:
      return (b8(pos + 0x01) * 2u) + 1u + 1u;
    case 0x14:
      return b24(pos + 0x08) + 0x0au + 1u;
    case 0x15:
      return b24(pos + 0x06) + 0x08u + 1u;
    case 0x20:
      return 2u + 1u;
    case 0x21:
      return b8(pos + 0x01) + 1u + 1u;
    case 0x22:
      return 1u;
    case 0x30:
      return b8(pos + 0x01) + 1u + 1u;
    case 0x31:
      return b8(pos + 0x02) + 2u + 1u;
    case 0x32:
      return b16(pos + 0x01) + 2u + 1u;
    case 0x33:
      return (b8(pos + 0x01) * 3u) + 1u + 1u;
    case 0x34:
      return 8u + 1u;
    case 0x35:
      return b32(pos + 0x11) + 0x14u + 1u;
    case 0x40:
      return b24(pos + 0x02) + 0x04u + 1u;
    case 0x5A:
      return 9u + 1u;
    default:
      return b32(pos + 0x01) + 4u + 1u;  // unknown: 4-byte length
  }
}

Device tape_init(void* storage) {
  // NOLINTNEXTLINE(misc-const-correctness): pointer is stored in Device::self
  // (void*), cannot be const
  tape_state* t = new (storage) tape_state();
  Device dev = {};
  dev.self = t;
  dev.name = "tape";
  dev.tick = tape_tick;
  dev.reset = tape_reset;
  dev.state_size = tape_dev_state_size;
  dev.save = tape_save;
  dev.load = tape_load;
  return dev;
}

void tape_peek(const Device* dev, TapeRegs* out) {
  const tape_state* t = static_cast<const tape_state*>(dev->self);
  out->attached = t->cdt != nullptr;
  out->playing = t->playing;
  out->motor = t->motor_seen;
  out->level = t->level;
  out->line_mode = t->line_mode;
  out->error = t->error;
  out->block = t->block;
  out->pos = t->pos;
}

int tape_attach_cdt(const Device* dev, const uint8_t* data, size_t len) {
  tape_state* t = self_of(dev->self);
  if (data == nullptr || len < 10 || std::memcmp(data, "ZXTape!\x1a", 8) != 0)
    return -1;
  t->cdt = data;
  t->len = len;
  tape_rewind(dev);
  return 0;
}

void tape_eject(const Device* dev) {
  tape_state* t = self_of(dev->self);
  t->cdt = nullptr;
  t->len = 0;
  stop(t);
  t->error = 0;
}

void tape_play(const Device* dev, int on) {
  tape_state* t = self_of(dev->self);
  if (on && !t->playing && t->cdt != nullptr) {
    t->playing = 1;
    if (t->phase == PH_IDLE) start_block(t);
  } else if (!on) {
    t->playing = 0;
  }
}

void tape_rewind(const Device* dev) {
  tape_state* t = self_of(dev->self);
  const uint8_t was_playing = t->playing;
  t->pos = 10;  // past "ZXTape!\x1A" + version
  t->block = 0;
  t->phase = PH_IDLE;
  t->level = 0;
  t->error = 0;
  t->bit_wr = t->bit_rd = 0;  // clear the decoded-bit scope ring
  t->playing = 0;
  if (was_playing) tape_play(dev, 1);
}

void tape_seek(const Device* dev, uint32_t block_ordinal) {
  tape_state* t = self_of(dev->self);
  if (t->cdt == nullptr) return;
  // Walk the deck's OWN cdt from the first block, skipping `block_ordinal`
  // blocks by size (layout-independent: the legacy pbTapeImage strips the
  // 10-byte header, so a byte offset taken from it lands 10 bytes short here).
  uint32_t pos = 10, blk = 0;  // 10 = past "ZXTape!\x1A" + version
  for (; blk < block_ordinal; ++blk) {
    if (pos >= t->len) break;
    const uint32_t sz = block_len(t, pos);
    if (sz == 0 || pos + sz <= pos) break;  // unsizable / overflow guard
    pos += sz;
  }
  if (blk != block_ordinal || pos < 10 || pos >= t->len) return;  // unreachable
  const uint8_t was_playing = t->playing;
  t->pos = pos;
  t->block = block_ordinal;
  t->phase = PH_IDLE;
  t->level = 0;
  t->error = 0;
  t->bit_wr = t->bit_rd = 0;  // clear the decoded-bit scope ring
  t->playing = 0;
  if (was_playing) tape_play(dev, 1);  // start_block enters block_ordinal
}

// Drain decoded data bits recorded since the last call (host BITS-view scope):
// copy up to `max` bits (0/1) into `out`, oldest first, and return the count.
int tape_drain_bits(const Device* dev, uint8_t* out, int max) {
  tape_state* t = self_of(dev->self);
  int count = 0;
  while (t->bit_rd != t->bit_wr && count < max)
    out[count++] = t->bit_ring[t->bit_rd++];
  return count;
}

void tape_line_mode(const Device* dev, int on) {
  self_of(dev->self)->line_mode = on ? 1 : 0;
}

void tape_line_level(const Device* dev, int level) {
  self_of(dev->self)->line_level = level ? 1 : 0;
}

}  // extern "C"
