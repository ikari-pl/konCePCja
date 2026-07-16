/* psg.h — the AY-3-8912 PSG as a Device. See docs/hardware/psg-device.md.
 *
 * The CPC's sound chip and keyboard/joystick input port. Reached ONLY through
 * the PPI's internal AY bus (Bus.ay: da/bdir/bc1/kbd_row) — never by the Z80
 * directly. 16-register file, 3 tone channels + noise + hardware envelope, Port
 * A wired to the keyboard columns of the row the PPI selected.
 *
 * Caller-owned, no heap: allocate psg_state_size() bytes, hand them to
 * psg_init(). */
#ifndef KONCPC_HW_PSG_H
#define KONCPC_HW_PSG_H

#include <stddef.h>
#include <stdint.h>

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Introspection snapshot (tests / debugging). */
typedef struct PsgRegs {
  uint8_t reg[16];       /* the register file */
  uint8_t sel;           /* selected register (0..15) */
  uint8_t tone_out;      /* bit k = channel k square-wave level */
  uint8_t noise_out;     /* current noise bit */
  uint8_t env_level;     /* current envelope level 0..31 */
  uint8_t env_step;      /* 0..31 within the current segment */
  uint8_t env_attack;    /* 1 = attack segment (ramps up) */
  uint8_t env_direction; /* 0x00=hold, 0x01=up, 0xFF=down (SNA v3 encoding) */
  uint8_t chan_level[3]; /* per-channel amplitude 0..31 this tick */
} PsgRegs;

size_t psg_state_size(void);
Device psg_init(void* storage);
void psg_peek(const Device* dev, PsgRegs* out);
/* Write a register / the selection directly (snapshot restore, tests) —
 * bypasses the PPI's AY bus dance; all outputs derive from the file live. */
void psg_poke_reg(const Device* dev, uint8_t n, uint8_t val);
void psg_poke_sel(const Device* dev, uint8_t sel);
/* Restore envelope generator mid-ramp state (SNA v3). half_level: the current
 * envelope LEVEL halved into 0..15 (legacy AmplitudeEnv>>1); direction: 0
 * hold, 1 up, 0xFF down — same encoding as legacy snapshots. */
void psg_poke_env(const Device* dev, uint8_t half_level, uint8_t direction);

/* Set one keyboard row's column sense (bit = 0 means pressed). The PPI selects
 * the row via the AY bus; register-14 reads in input mode return
 * key_matrix[row]. Wired by the input bridge / tests; NOT part of saved state
 * (live external input). */
void psg_set_key_row(const Device* dev, uint8_t row, uint8_t columns);

/* Mono AY output level for the current tick: sum of the 3 channel levels
 * (0..93). Board-specific stereo/DAC mixing lives in the audio bridge, not the
 * AY core. */
int psg_out(const Device* dev);

/* The three channel amplitudes (A, B, C) for the current tick, each 0..31. The
 * audio bridge pans these per the CPC's stereo wiring (A → left, C → right, B →
 * both). */
void psg_levels(const Device* dev, uint8_t out[3]);

/* --- Fast-tier batch seam (psg-device.md §batch, plan §4.6) ---
 *
 * AY bus-control operations are EDGE-triggered on the (bdir, bc1, da) tuple
 * in BOTH execution shapes (the F0-resolved reg-13 semantics): the per-cycle
 * ay_bus edge-detects line changes off the bus; the Fast scheduler relays
 * each line-state CHANGE event here instead. Both maintain the same shadow,
 * so tiers hand over at any quiet point without a false edge. */
void psg_fast_lines(const Device* dev, int bdir, int bc1, uint8_t da);
/* The READ-state bus value (bdir=0, bc1=1) — what a PPI port-A input read
 * latches; a level, not an event. */
uint8_t psg_fast_read(const Device* dev, uint8_t kbd_row, uint8_t row_ext);

/* Take (read + clear) the bitmask of keyboard rows the firmware READ (port-A
 * input) since the last call — the host relays it to the KeyboardManager as
 * notify_scanned() for the BufferedUntilRead key-hold mode. */
uint16_t psg_take_scanned_rows(const Device* dev);
/* One 1 MHz sound step (tone/noise/envelope/mixer) — the clk.psg tick minus
 * the bus. The Fast audio loop pairs it with the machine's accumulator. */
void psg_batch_step(const Device* dev);
/* F8 bulk pair: distance (in 1 MHz steps, ≥1) to the next step that can move
 * any output state — a tone-counter expiry (the flip-flop toggles even on
 * mixer-muted channels), a noise LFSR shift, or an envelope clock. Valid
 * until the next register write or real step. */
uint32_t psg_batch_quiet_steps(const Device* dev);
/* Advance n 1 MHz steps KNOWN to contain no such event (n strictly less than
 * psg_batch_quiet_steps()): rolls the prescaler phases and period counters
 * forward in closed form; chan_level is untouched by construction. */
void psg_batch_skip(const Device* dev, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_PSG_H */
