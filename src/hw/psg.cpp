/* psg.cpp — the AY-3-8912 PSG Device. See docs/hardware/psg-device.md.
 *
 * 16-register file reached through the PPI's AY bus (BDIR/BC1), 3 square-wave
 * tone channels, a 17-bit-LFSR noise generator, the 10-shape hardware envelope,
 * and Port A wired to the keyboard columns of the PPI-selected row. Clocked at
 * 1 MHz (clk.psg), divided by 16 for tone/noise and 256 for the envelope. */

#include "psg.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <new>

namespace {

// Per-register write masks — the AY ignores unused high bits (matches
// SetAYRegister).
constexpr uint8_t kRegMask[16] = {
    0xFF, 0x0F, 0xFF, 0x0F,  // tone A/B period (fine/coarse)
    0xFF, 0x0F, 0x1F, 0xFF,  // tone C period, noise period, mixer
    0x1F, 0x1F, 0x1F, 0xFF,  // amplitude A/B/C, envelope fine
    0xFF, 0x0F, 0xFF, 0xFF,  // envelope coarse, envelope shape, port A/B
};

struct psg_state {
  uint8_t reg[16] = {0};
  uint8_t sel = 0;  // selected register (0..15)
  // AY-bus shadow: latch/write operations are EDGE-triggered on this
  // (bdir, bc1, da) tuple — one op per state entry (or per data change while
  // held), never per held cycle. F0-resolved semantics (psg-device.md §batch,
  // beads-7kpu): "writing this register (re-)starts the envelope" is per
  // WRITE EVENT (cpcwiki reg 0Dh) — the old per-cycle re-application pinned a
  // held reg-13 write's envelope for the whole strobe.
  bool bc1_prev = false;
  bool bdir_prev = false;
  uint8_t da_prev = 0xFF;  // rest level of the floating bus

  // Keyboard-scan tracking for the host's BufferedUntilRead key sync: a bitmask
  // of the rows the firmware actually READ (port-A input) since the last take.
  // The host relays it to the KeyboardManager so a held key releases only after
  // its row was scanned — the engine=1 equivalent of the legacy per-read
  // notify_scanned (kon_cpc_ja.cpp).
  uint16_t scanned_rows = 0;

  // Tone generators: a ÷16 prescaler drives three period counters.
  uint16_t tone_div = 0;       // 0..15
  uint16_t tone_cnt[3] = {0};  // per-channel period counter
  uint8_t tone_out = 0;        // bit k = channel k square-wave level

  // Noise generator: ÷16 prescaler, 5-bit counter, 17-bit LFSR.
  uint16_t noise_div = 0;
  uint16_t noise_cnt = 0;
  uint32_t noise_lfsr = 1;  // seeded to 1 (never all-zero)
  uint8_t noise_out = 0;

  // Envelope generator: ÷256 prescaler, 16-bit period counter, 32-step ramp.
  uint16_t env_div = 0;         // 0..255
  uint16_t env_cnt = 0;         // counts to the envelope period
  uint8_t env_step = 0;         // 0..31 within the current segment
  uint8_t env_level = 0;        // output level 0..31
  bool env_attack_seg = false;  // current segment ramps up?
  bool env_holding = false;
  uint8_t env_direction = 0;  // 0x00=hold, 0x01=up, 0xFF=down (SNA v3)

  uint8_t chan_level[3] = {0};  // per-channel amplitude 0..31 this tick

  // Keyboard columns per row (bit = 0 pressed). Live external input, not saved.
  uint8_t key_matrix[16];
};

psg_state* self_of(void* self) { return static_cast<psg_state*>(self); }

uint16_t tone_period(const psg_state* p, int chan) {
  const int lo = chan * 2;
  uint16_t const period =
      static_cast<uint16_t>(p->reg[lo] | ((p->reg[lo + 1] & 0x0F) << 8));
  return period ? period : 1;  // 0 behaves as 1 (no divide-by-zero)
}

// --- Envelope
// --------------------------------------------------------------------

void env_restart(psg_state* p) {
  const uint8_t shape = p->reg[13];
  p->env_holding = false;
  p->env_step = 0;
  p->env_attack_seg = (shape & 0x04) != 0;  // ATTACK → ramp up this segment
  p->env_level = p->env_attack_seg ? 0 : 31;
  p->env_direction = p->env_attack_seg ? 0x01 : 0xFF;
}

void env_clock(psg_state* p) {
  if (p->env_holding) {
    p->env_direction = 0;
    return;
  }
  const uint8_t shape = p->reg[13];

  if (p->env_step < 31) {  // advance within the 32-step segment
    p->env_step++;
    p->env_level = p->env_attack_seg ? p->env_step
                                     : static_cast<uint8_t>(31 - p->env_step);
    p->env_direction = p->env_attack_seg ? 0x01 : 0xFF;
    return;
  }

  // Segment boundary: decide continue / alternate / hold.
  p->env_step = 0;
  if (!(shape & 0x08)) {  // no CONTINUE → one segment then hold at 0
    p->env_holding = true;
    p->env_level = 0;
    p->env_direction = 0;
    return;
  }
  if (shape & 0x01) {  // HOLD → freeze at the final level
    p->env_holding = true;
    if (shape & 0x02) {  // ALTERNATE+HOLD holds at the opposite end
      p->env_level = p->env_attack_seg ? 0 : 31;
    } else {
      p->env_level = p->env_attack_seg ? 31 : 0;
    }
    p->env_direction = 0;
    return;
  }
  if (shape & 0x02)
    p->env_attack_seg = !p->env_attack_seg;   // ALTERNATE: reverse
  p->env_level = p->env_attack_seg ? 0 : 31;  // restart the ramp
  p->env_direction = p->env_attack_seg ? 0x01 : 0xFF;
}

// --- Sound tick (one 1 MHz step)
// -------------------------------------------------

void tone_step(psg_state* p) {  // ÷8 prescaler → three period counters
  // Tone counter clocked at 1 MHz/8 = 125 kHz; it TOGGLES each period, so a
  // full square wave = 2·period counts = 16·period µs → f = 1e6/(16·period) =
  // 62500/period (period 239 = 261.6 Hz, middle C). The ÷8 (not ÷16) is what
  // makes the toggle land on the datasheet frequency; noise, which shifts once
  // per period, stays ÷16.
  if (++p->tone_div < 8) return;
  p->tone_div = 0;
  for (int chan = 0; chan < 3; ++chan) {
    if (++p->tone_cnt[chan] >= tone_period(p, chan)) {
      p->tone_cnt[chan] = 0;
      p->tone_out ^=
          static_cast<uint8_t>(1u << chan);  // toggle the square wave
    }
  }
}

void noise_step(psg_state* p) {  // ÷16 prescaler → 5-bit counter → 17-bit LFSR
  if (++p->noise_div < 16) return;
  p->noise_div = 0;
  const uint16_t period = (p->reg[6] & 0x1F) ? (p->reg[6] & 0x1F) : 1;
  if (++p->noise_cnt < period) return;
  p->noise_cnt = 0;
  const uint32_t tap = (p->noise_lfsr ^ (p->noise_lfsr >> 3)) & 1u;  // taps 0,3
  p->noise_lfsr = (p->noise_lfsr >> 1) | (tap << 16);                // 17-bit
  p->noise_out = static_cast<uint8_t>(p->noise_lfsr & 1u);
}

void envelope_step(
    psg_state* p) {  // ÷256 prescaler → 16-bit period → env_clock
  if (++p->env_div < 256) return;
  p->env_div = 0;
  const uint16_t period = static_cast<uint16_t>(p->reg[11] | (p->reg[12] << 8));
  if (++p->env_cnt < (period ? period : 1)) return;
  p->env_cnt = 0;
  env_clock(p);
}

// Mixer + amplitude → per-channel level for this tick.
void mixer_step(psg_state* p) {
  const uint8_t mixer = p->reg[7];
  for (int chan = 0; chan < 3; ++chan) {
    const bool tone_dis = (mixer >> chan) & 1;
    const bool noise_dis = (mixer >> (chan + 3)) & 1;
    const bool tbit = (p->tone_out >> chan) & 1;
    const bool audible = (tbit || tone_dis) && (p->noise_out || noise_dis);
    const uint8_t amp = p->reg[8 + chan];
    uint8_t level;
    if (amp & 0x10)  // bit 4 → use the envelope level
      level = audible ? p->env_level : 0;
    else
      level = audible ? static_cast<uint8_t>(((amp & 0x0F) * 2) + 1)
                      : 0;  // 4→5-bit
    p->chan_level[chan] = level;
  }
}

void sound_step(psg_state* p) {
  tone_step(p);
  noise_step(p);
  envelope_step(p);
  mixer_step(p);
}

// --- AY bus protocol
// -------------------------------------------------------------

// One latch/write OPERATION for the given line state — the shared event core
// (the per-cycle ay_bus edge-detects into it; the Fast tier's psg_fast_lines
// relays line-change events to it directly).
void ay_apply(psg_state* p, bool bdir, bool bc1, uint8_t da) {
  if (bdir && bc1) {  // latch: select the register address
    p->sel = static_cast<uint8_t>(da & 0x0F);
  } else if (bdir && !bc1) {  // write the selected register
    if (p->sel < 16) {
      p->reg[p->sel] = static_cast<uint8_t>(da & kRegMask[p->sel]);
      if (p->sel == 13)
        env_restart(p);  // writing the shape restarts the envelope — ONCE per
                         // write event (edge semantics, see the shadow above)
    }
  }
}

// The READ-state bus value (what the AY drives while bdir=0, bc1=1) — a
// level, re-driven every cycle, not an event.
uint8_t ay_read_value(const psg_state* p, uint8_t kbd_row, uint8_t row_ext) {
  uint8_t v = p->reg[p->sel];
  if (p->sel == 14 && !(p->reg[7] & 0x40))  // Port A input → keyboard columns
    // The diode matrix wired-ANDs the external connector lines (joystick
    // port peripherals) into the selected row (amx-mouse-device.md §1).
    v = static_cast<uint8_t>(p->key_matrix[kbd_row & 0x0F] & row_ext);
  else if (p->sel == 15 && !(p->reg[7] & 0x80))  // Port B input
    v = 0xFF;
  return v;
}

void ay_bus(psg_state* p, const Bus* in, Bus* out) {
  const bool bdir = in->ay.bdir;
  const bool bc1 = in->ay.bc1;
  const uint8_t da = in->ay.da;

  if (bdir != p->bdir_prev || bc1 != p->bc1_prev || da != p->da_prev)
    ay_apply(p, bdir, bc1, da);  // one operation per line-state change
  if (!bdir && bc1) {            // read: drive the data bus (level)
    out->ay.da = ay_read_value(p, in->ay.kbd_row, in->ay.row_ext);
    if (p->sel == 14 && !(p->reg[7] & 0x40))  // port-A input = a keyboard scan
      p->scanned_rows |= static_cast<uint16_t>(1u << (in->ay.kbd_row & 0x0F));
  }

  p->bdir_prev = bdir;
  p->bc1_prev = bc1;
  p->da_prev = da;
}

void psg_tick(void* self, const Bus* __restrict in, Bus* __restrict out) {
  psg_state* p = self_of(self);
  ay_bus(p, in, out);
  if (in->clk.psg) sound_step(p);
}

void psg_reset(void* self) {
  psg_state* p = self_of(self);
  uint8_t saved_matrix[16];
  std::memcpy(saved_matrix, p->key_matrix, sizeof(saved_matrix));
  // Value-init via placement-new — NOT `*p = psg_state{}`. The aggregate
  // temporary `psg_state{}` leaves its interior alignment padding indeterminate,
  // and because psg_state is trivially copyable the copy-assignment is a full-
  // sizeof memcpy that would splat that garbage padding into the live struct.
  // psg_save() memcpies the whole struct into the state-hash blob, so garbage
  // padding makes the deterministic-replay hash non-deterministic (per-run).
  // Value-initialization zero-inits the whole object (padding included) before
  // applying the member initializers, keeping the save blob byte-stable.
  new (self) psg_state();
  std::memset(p->reg, 0, sizeof(p->reg));  // AY RES clears every register to 0
  p->noise_lfsr = 1;  // ...but the LFSR must not be all-zero
  std::memcpy(p->key_matrix, saved_matrix, sizeof(p->key_matrix));
}

// Save/load: version + register file + generator state; NOT the live key
// matrix.
size_t psg_dev_state_size(const void* /*unused*/) {
  return sizeof(psg_state) + 1;
}
void psg_save(const void* self, void* buf) {
  uint8_t* b = static_cast<uint8_t*>(buf);
  b[0] = 1;
  std::memcpy(b + 1, self, sizeof(psg_state));
  // key_matrix is LIVE input (current key/joystick state), which psg_load
  // deliberately keeps rather than restore. It must therefore be excluded from
  // the blob too — otherwise the blob is non-deterministic (depends on keys held
  // at save time) and save/load are asymmetric. Zero it, matching load.
  std::memset(b + 1 + offsetof(psg_state, key_matrix), 0,
              sizeof(psg_state::key_matrix));
  // scanned_rows is transient host bookkeeping (which rows the firmware read this
  // frame, drained by the KeyboardManager relay) — same rationale as key_matrix:
  // exclude it so the blob / state hashes stay deterministic across tiers.
  std::memset(b + 1 + offsetof(psg_state, scanned_rows), 0,
              sizeof(psg_state::scanned_rows));
}
void psg_load(void* self, const void* buf) {
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  if (b[0] != 1) return;
  psg_state* p = self_of(self);
  uint8_t saved_matrix[16];
  std::memcpy(saved_matrix, p->key_matrix, sizeof(saved_matrix));
  std::memcpy(self, b + 1, sizeof(psg_state));
  std::memcpy(p->key_matrix, saved_matrix,
              sizeof(p->key_matrix));  // keep live input
}

}  // namespace

extern "C" {

size_t psg_state_size(void) { return sizeof(psg_state); }

Device psg_init(void* storage) {
  psg_state* p = new (storage) psg_state();
  std::memset(p->key_matrix, 0xFF, sizeof(p->key_matrix));  // no keys pressed
  psg_reset(p);
  return Device{p,        "psg",   psg_tick, psg_reset, psg_dev_state_size,
                psg_save, psg_load};
}

void psg_peek(const Device* dev, PsgRegs* out) {
  const psg_state* p = static_cast<const psg_state*>(dev->self);
  std::memcpy(out->reg, p->reg, sizeof(out->reg));
  out->sel = p->sel;
  out->tone_out = p->tone_out;
  out->noise_out = p->noise_out;
  out->env_level = p->env_level;
  out->env_step = p->env_step;
  out->env_attack = p->env_attack_seg ? 1 : 0;
  out->env_direction = p->env_direction;
  std::memcpy(out->chan_level, p->chan_level, sizeof(out->chan_level));
}

void psg_set_key_row(const Device* dev, uint8_t row, uint8_t columns) {
  static_cast<psg_state*>(dev->self)->key_matrix[row & 0x0F] = columns;
}

int psg_out(const Device* dev) {
  const psg_state* p = static_cast<const psg_state*>(dev->self);
  return p->chan_level[0] + p->chan_level[1] + p->chan_level[2];
}

void psg_levels(const Device* dev, uint8_t out[3]) {
  const psg_state* p = static_cast<const psg_state*>(dev->self);
  out[0] = p->chan_level[0];
  out[1] = p->chan_level[1];
  out[2] = p->chan_level[2];
}

void psg_poke_reg(const Device* dev, uint8_t n, uint8_t val) {
  static_cast<psg_state*>(dev->self)->reg[n & 15] = val;
}

void psg_poke_sel(const Device* dev, uint8_t sel) {
  static_cast<psg_state*>(dev->self)->sel = sel & 15;
}

void psg_poke_env(const Device* dev, uint8_t half_level, uint8_t direction) {
  psg_state* p = static_cast<psg_state*>(dev->self);
  // SNA v3 stores the CURRENT LEVEL halved into 0..15 (legacy AmplitudeEnv>>1)
  // plus a direction byte — never a segment step. Decode the level directly so
  // a held envelope restores at its held level regardless of env_attack_seg
  // (which register pokes alone never re-derive), and reconstruct the step
  // from the direction for the ramping cases.
  const uint8_t level = static_cast<uint8_t>((half_level & 15) << 1);
  p->env_level = level;
  p->env_holding = direction == 0;
  p->env_direction = direction;
  if (direction == 0x01) {
    p->env_attack_seg = true;
    p->env_step = level;
  } else if (direction == 0xFF) {
    p->env_attack_seg = false;
    p->env_step = static_cast<uint8_t>(31 - level);
  } else {
    p->env_step = 31;  // segment complete; env_clock stays parked on hold
  }
}

void psg_fast_lines(const Device* dev, int bdir, int bc1, uint8_t da) {
  // Fast tier: one AY line-state change event — the same edge core the
  // per-cycle ay_bus feeds, and it maintains the same shadow, so tiers can
  // hand over at any quiet point without a false edge.
  psg_state* p = static_cast<psg_state*>(dev->self);
  const bool nbdir = bdir != 0;
  const bool nbc1 = bc1 != 0;
  if (nbdir != p->bdir_prev || nbc1 != p->bc1_prev || da != p->da_prev)
    ay_apply(p, nbdir, nbc1, da);
  p->bdir_prev = nbdir;
  p->bc1_prev = nbc1;
  p->da_prev = da;
}

uint8_t psg_fast_read(const Device* dev, uint8_t kbd_row, uint8_t row_ext) {
  const psg_state* p = static_cast<const psg_state*>(dev->self);
  if (p->sel == 14 && !(p->reg[7] & 0x40))  // port-A input = a keyboard scan
    const_cast<psg_state*>(p)->scanned_rows |=
        static_cast<uint16_t>(1u << (kbd_row & 0x0F));
  return ay_read_value(p, kbd_row, row_ext);
}

// Take (read + clear) the keyboard rows the firmware read since the last call.
// The host relays these to the KeyboardManager as notify_scanned() so
// BufferedUntilRead releases a held key only once its row was scanned (the
// engine=1 equivalent of the legacy per-read notify in the PPI I/O handler).
uint16_t psg_take_scanned_rows(const Device* dev) {
  psg_state* p =
      const_cast<psg_state*>(static_cast<const psg_state*>(dev->self));
  const uint16_t rows = p->scanned_rows;
  p->scanned_rows = 0;
  return rows;
}

void psg_batch_step(const Device* dev) {
  sound_step(static_cast<psg_state*>(dev->self));  // one 1 MHz sound step
}

// F8 bulk pair — see psg.h. A prescaler of width `presc` at phase `div`
// ticks after (presc - div) steps, then every presc; its counter at `cnt`
// with period `per` expires on tick max(1, per - cnt) (a period lowered
// below the running count expires on the very next tick, exactly as the
// per-step `++cnt >= period` comparison).
namespace {
uint32_t psg_event_in(uint32_t presc, uint32_t div, uint32_t cnt,
                             uint32_t per) {
  const uint32_t ticks = per > cnt ? per - cnt : 1;
  return (presc - div) + ((ticks - 1) * presc);
}
}  // namespace

uint32_t psg_batch_quiet_steps(const Device* dev) {
  const psg_state* p = static_cast<const psg_state*>(dev->self);
  uint32_t quiet =
      psg_event_in(8, p->tone_div, p->tone_cnt[0], tone_period(p, 0));
  for (int chan = 1; chan < 3; ++chan) {
    const uint32_t tone =
        psg_event_in(8, p->tone_div, p->tone_cnt[chan], tone_period(p, chan));
    quiet = std::min(tone, quiet);
  }
  const uint32_t noise_per = (p->reg[6] & 0x1F) ? (p->reg[6] & 0x1F) : 1;
  const uint32_t noise =
      psg_event_in(16, p->noise_div, p->noise_cnt, noise_per);
  quiet = std::min(noise, quiet);
  const uint32_t env_per_raw =
      static_cast<uint32_t>(p->reg[11] | (p->reg[12] << 8));
  const uint32_t env = psg_event_in(256, p->env_div, p->env_cnt,
                                    env_per_raw ? env_per_raw : 1);
  quiet = std::min(env, quiet);
  return quiet;
}

void psg_batch_skip(const Device* dev, uint32_t n) {
  psg_state* p = static_cast<psg_state*>(dev->self);
  const uint32_t tone_total = p->tone_div + n;
  const uint32_t tone_ticks = tone_total / 8;
  p->tone_div = static_cast<uint16_t>(tone_total % 8);
  for (unsigned short& chan : p->tone_cnt)
    chan = static_cast<uint16_t>(chan + tone_ticks);
  const uint32_t noise_total = p->noise_div + n;
  p->noise_cnt = static_cast<uint16_t>(p->noise_cnt + (noise_total / 16));
  p->noise_div = static_cast<uint16_t>(noise_total % 16);
  const uint32_t env_total = p->env_div + n;
  p->env_cnt = static_cast<uint16_t>(p->env_cnt + (env_total / 256));
  p->env_div = static_cast<uint16_t>(env_total % 256);
}

}  // extern "C"
