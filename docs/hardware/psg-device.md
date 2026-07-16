# PSG AY-3-8912 — sub-cycle, pin-level Device simulation reference

Language-neutral spec for the sub-cycle **AY-3-8912 PSG** Device (`src/hw/psg`), the
CPC's sound chip and keyboard/joystick input port. Companion to `ppi-device.md` (which
owns the bus that reaches this chip). See `docs/hw-spec.md` for the Device model.

The AY is reached **only through the PPI's internal AY bus** (`ay.da`, `ay.bdir`,
`ay.bc1`, `ay.kbd_row` — see `ppi-device.md` §5), never by the Z80 directly. It
provides: a **16-register file**, **3 square-wave tone channels**, a **noise
generator**, a **hardware envelope**, and **Port A** wired to the keyboard columns.

Clocking: the CPC feeds the AY a **1 MHz** clock (`clk.psg`). Internally the AY divides
by 16 for the tone/noise counters and by 256 for the envelope.

---

## 1. Register file

16 registers, written through the AY bus. Writes are **masked per register** (the AY
ignores unused high bits — matching the legacy `SetAYRegister`):

| reg | meaning | mask |
|---|---|---|
| 0 / 1 | tone A period — fine (8) / coarse (4) | 0xFF / 0x0F |
| 2 / 3 | tone B period — fine / coarse | 0xFF / 0x0F |
| 4 / 5 | tone C period — fine / coarse | 0xFF / 0x0F |
| 6 | noise period | 0x1F |
| 7 | mixer / I/O control | 0xFF |
| 8 / 9 / 10 | amplitude A / B / C | 0x1F |
| 11 / 12 | envelope period — fine (8) / coarse (8) | 0xFF / 0xFF |
| 13 | envelope shape | 0x0F (write of 0xFF ≠ reset — see §4) |
| 14 / 15 | I/O port A / B | 0xFF |

- **Tone period** for channel X = `(coarse << 8) | fine`, a 12-bit value.
- **Register 7 (mixer)**: bit 0..2 = tone enable A/B/C (**0 = on**), bit 3..5 = noise
  enable A/B/C (**0 = on**), bit 6..7 = Port A/B direction (**1 = output**). On the
  CPC Port A is input (bit 6 = 0) for the keyboard.
- **Amplitude X**: bits 0..3 = fixed level 0..15; **bit 4 = 1** → use the envelope
  level instead.

---

## 2. Bus protocol (BDIR / BC1)

Each master cycle the Device reacts to the AY-bus control lines the PPI drives
(BC2 tied high; `(BDIR, BC1)` selects the function):

| BDIR BC1 | action |
|---|---|
| 0 0 | inactive |
| 0 1 | **read**: drive `out->ay.da` = the selected register's value (§keyboard for reg 14) |
| 1 0 | **write**: `reg[sel] = in->ay.da` (masked) |
| 1 1 | **latch**: `sel = in->ay.da & 0x0F` (reg address) |

Edge vs level: latch/write take effect on the control code being present; the register
file is idempotent under a held code, so level-sensitive handling is exact (the PPI
holds a code for the duration of one `OUT`, then returns to inactive). A **read**
continuously drives `ay.da` while BC1 is asserted, so the PPI samples a stable value.

---

## 3. Keyboard / Port A (input) {#keyboard}

The AY Port A pins are wired to the keyboard **column** sense of the row selected by
the PPI (`ay.kbd_row`). The Device owns a 16-byte **key matrix** (one byte per row,
**bit = 0 means pressed**), set from outside by the input bridge / tests
(`psg_set_key_matrix`). When register 14 is **read** and Port A is in **input** mode
(mixer bit 6 = 0), the Device drives:

```
ay.da = key_matrix[ay.kbd_row & 0x0F]
```

If Port A is in output mode, reg 14 reads back its latched value. Register 15 (Port B)
behaves symmetrically with mixer bit 7. This is the exact keyboard path the firmware
scans (see `ppi-device.md` §6); joysticks share rows 9 (and 6 on some maps).

---

## 4. Sound generation

Clocked by `clk.psg` (1 MHz). Three dividers run off it:

- **Tone counters** (÷8). Each channel's counter is clocked at 1 MHz/8 = 125 kHz;
  when it reaches the channel period it resets and **toggles** the square-wave bit. A
  full cycle is 2·period counts → **f = 1 MHz / (16·period) = 62500/period** (period
  239 = 261.6 Hz, middle C — matches the CPC `SOUND` note table). The ÷8 (vs the ÷16
  the datasheet quotes) is what makes the *toggle* land on the datasheet frequency;
  noise, which shifts once per period, uses ÷16 and reaches the same
  `f = fclock/(16·period)`. A period of 0 behaves as 1. Duty is 50 %.
- **Noise** (÷16). A **17-bit LFSR** (`tap = bit0 ^ bit3`); when the noise counter
  reaches register 6's period it shifts, and the low bit is the noise output. A period
  of 0 behaves as 1.
- **Envelope** (÷256). Register 13's 4 shape bits (Continue / Attack / Alternate /
  Hold) drive the classic 10-distinct-waveform state machine over a **5-bit** level
  (0..31). Writing register 13 **restarts** the envelope (even with the same value).

**Per-channel output**: a channel is audible when `(tone_out OR tone_disabled) AND
(noise_out OR noise_disabled)` — i.e. mixer bit clear enables that source. The gated
bit then selects amplitude: fixed 4-bit level (doubled to the 5-bit scale) or the live
envelope level. Level → linear sample via a 32-entry volume table.

**Output**: `psg_out()` returns the summed 3-channel level for the current tick.
Board-specific stereo panning + the digiblaster/tape mix are *outside* the AY core
(they belong to the audio bridge); this Device emits the mono AY level. Sample-rate
resampling from 1 MHz to the host rate is the bridge's job.

---

## 5. State / tick / reset / save

```
reg[16]          register file
sel              selected register (0..15)
tone_cnt[3]      12-bit tone counters      tone_out[3]   square-wave bits
tone_div         ÷16 prescaler             noise_div     ÷16 prescaler
noise_cnt        5-bit counter             noise_lfsr    17-bit shift register  noise_out
env_div          ÷256 prescaler            env_cnt (16)  env_step/level/state
key_matrix[16]   keyboard columns (external input)
```

- **tick**: service the AY bus (§2); when `clk.psg` is asserted, advance the tone,
  noise, and envelope dividers one 1 MHz step and recompute outputs.
- **reset**: the AY RES pin clears **every register to 0** (so mixer = 0, Port A =
  input → keyboard-ready; the firmware later programs the mixer to silence sound),
  `sel` 0, LFSR **seeded to 1** (must never be all-zero), counters cleared, envelope
  idle. `key_matrix` persists (it is external input, not chip state).
- **save/load**: version byte + register file + generator state (no pointers,
  no `key_matrix` — it is live external input).

---

## 6. Validation

- **Register file + masks**: write every register, read back through the bus, assert
  the per-register mask.
- **Bus protocol**: latch→write→read round-trips; held-code idempotence.
- **Keyboard**: set `key_matrix`, drive the firmware scan sequence via the PPI, assert
  the pressed bits appear on Port A for the selected row.
- **Tone**: a known period toggles the output at the expected 1 MHz/(16·period)/2 rate.
- **Envelope**: each of the 10 shapes produces its documented level sequence.
- Full analog-mix golden-master vs the legacy stereo mixer is deferred (board audio,
  not AY core).

## Batch contract (RunTier::Fast)

- **Render**: `render(n 1 MHz steps)` advances tone/noise/envelope dividers
  in a tight loop (closed forms only if profiling demands) and accumulates
  into the host-rate sample sink — the audio stream is byte-compared against
  Faithful per frame (the lockstep oracle).
- **AY bus ops are discrete events**: an I/O-driven control transition
  (ADDRESS / WRITE / READ, §2) catches the renderer up, then applies ONCE.
  Keyboard reads (reg 14) sample the matrix + row after catching up the PPI
  so kbd_row is current.
- **THE reg-13 exception (bestiary class c) — RESOLVED (F0 review,
  2026-07-09)**: the hardware latches a register write as ONE operation —
  cpcwiki (PSG_AY-3-8912, reg 0Dh): "Writing to this register (re-)starts
  the envelope" — per write, not per held bus cycle. THE SPEC for ALL tiers
  is therefore EDGE-TRIGGERED: the write applies once, on the transition
  INTO the (bdir, /bc1) WRITE state, sampling da then. The per-cycle core's
  re-application every held cycle (which pinned the envelope for the strobe)
  is a modeling artifact to be fixed with its existing bdir_prev/bc1_prev
  latches; once fixed, the wake tier's held-WRITE keep-awake term becomes
  unnecessary. Batch semantics (apply-once per event) then match by
  construction. The audio byte-compare oracle enforces tier equality.
- Bestiary audit: class (c) above; dividers are exact arithmetic (class b);
  the da drive is event-scoped (class d has no batch counterpart).

Implementation (F6): AY bus-control operations are EDGE-triggered on the
(bdir, bc1, da) tuple in BOTH execution shapes — `ay_apply` is the one event
core; the per-cycle `ay_bus` edge-detects line changes off the bus, the Fast
scheduler relays each change via `psg_fast_lines` (same shadow, so tiers hand
over without a false edge). This lands the F0-resolved reg-13 semantics
(beads-7kpu): one envelope restart per WRITE EVENT. `psg_batch_step` is one
1 MHz sound step; `psg_fast_read` the READ-state bus value. The machine's
Fast audio rides (step; deferred-accumulate) units — the accumulate trails
one step so a frame cut never double-counts it at the tier seam.
ORACLES: FastTierMachine.PsgRegisterFileLockstep + the concatenated-audio
equality in FastTierMachine.BootTypesAndSoundsInLockstepWithWake.
