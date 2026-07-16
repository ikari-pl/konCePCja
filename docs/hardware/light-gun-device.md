# The light gun (Amstrad Magnum Phaser / Trojan Light Phazer) — Device spec

A light gun is a photo-sensor plus a trigger. Aimed at the CRT, its sensor
fires the instant the electron beam sweeps under the muzzle; that pulse
drives the 6845's **LPEN** pin, and the CRTC latches its current refresh
address (MA) into the read-only registers R16 (high 6 bits) / R17 (low 8).
Software polls R16/R17 to learn where the gun points.

Two guns share this behaviour on the CPC:
- **Amstrad Magnum Phaser** — LPEN on the light-pen input; trigger is the
  gun's own button (`CPC.phazer_pressed`).
- **Trojan Light Phazer** — same LPEN strobe; trigger is wired to joystick
  Fire 1 (the host already routes that to `CPC_J0_FIRE1`).

The latch is a genuine **CRTC feature**, so it lives in the sub-cycle CRTC
Device; the gun is a small peripheral Device that drives the strobe wire.
This replaces `phazer.cpp`/`phazer.h` and the legacy latch in `crtc.cpp`'s
render path (which runs only under engine=0 — so today the gun is DEAD under
the default engine=1, the parity gap that blocks the legacy-core deletion,
replacement-ledger row `phazer.cpp/h`).

## 1. The wire

One new bus line — the 6845 LPEN pin, an input to the CRTC driven by the
peripheral:

```c
/* The light-pen strobe: a peripheral (light gun / light pen) pulses this
 * HIGH for the master cycle its sensor sees the beam. The CRTC latches MA
 * into R16/R17 on the rising edge. Rests LOW. */
typedef struct PenBus {
  bool strobe;
} PenBus;
```

Added to `Bus` after `SerialBus`; rests LOW in `bus_resting()`. Nothing but
the gun drives it, and only the CRTC reads it.

## 2. The CRTC side — LPEN latch (6845-faithful)

In the CRTC Device tick, on the **rising edge** of `pen.strobe`:

```
reg[16] = (MA >> 8) & 0x3F;   // light-pen address high (6 bits)
reg[17] =  MA       & 0xFF;   // light-pen address low
```

MA is the address the CRTC is refreshing this cycle (`c->ma`) — the same
value the display fetch uses. The latch is edge-triggered: a strobe held
across several cycles latches once. R16/R17 remain read-only to the I/O
decode (writes ignored — unchanged). The latch is the ONLY new CRTC
behaviour; it is gated entirely on `pen.strobe`, which rests LOW, so an
unplugged machine never executes it and the canonical CKSUMs are unchanged.

All four CRTC types carry R16/R17, so the latch is type-independent.

## 3. The gun side — beam match

The gun holds: type (off / Magnum / Trojan), trigger state, and the aim in
**CRTC beam space** — `(aim_line, aim_col)`, where `aim_line` is a displayed
scanline (matched against `vid.frame_line`) and `aim_col` is a character
column within the active line.

Per cycle, while `dispen` is high the gun advances an active-char counter
(reset when `dispen` falls / a new line begins via an `hsync`/`frame_line`
change); that counter is the beam's current column. The gun asserts
`pen.strobe` for one cycle when **the trigger is pressed** AND the beam is
within the aim tolerance window:

```
|frame_line - aim_line| < 2   AND   |active_col - aim_col| < 2
```

(The ±2 window mirrors the legacy's 16px × 2-line slack — a gun aim is fuzzy
and the sensor integrates a small area; exactness is neither achievable nor
wanted.)

**Trigger released**: no strobe. On real hardware the LPEN then floats and
R16/R17 hold their last latch; games poll R16/R17 only after a trigger, so
holding is correct. (The legacy engine=0 hack of *incrementing* R17 while
released is a rendering-loop artifact, not hardware — it is NOT reproduced.)

## 4. Coordinate mapping (host aim → beam space)

The host tracks the aim in framebuffer pixels (`CPC.phazer_x/phazer_y`, the
mouse position through the video plugin's offset/scale). The bridge converts
once per frame into beam space using the machine's fixed render geometry:

```
aim_line = phazer_y / Y_SCALE               // framebuffer row → scanline
aim_col  = (phazer_x - ACTIVE_X0) / CHARW   // framebuffer x → active column
```

`ACTIVE_X0` (left edge of the active display in the framebuffer) and `CHARW`
(framebuffer pixels per character column) come from the same geometry the
video Device renders with — expose them as constants/accessors rather than
hard-coding a second copy. The conversion lives in the bridge (host-facing),
NOT in the Device, which stays in pure beam space and host-agnostic.

## 5. Host API

```c
size_t light_gun_state_size(void);
Device light_gun_init(void* storage);
void   light_gun_peek(const Device* dev, LightGunRegs* out);  /* type,
                                              pressed, aim_line/col, plugged */
void   light_gun_set_type(const Device* dev, int type);       /* 0/1/2 */
void   light_gun_set_aim(const Device* dev, uint16_t line, uint16_t col);
void   light_gun_set_trigger(const Device* dev, int pressed);
```

`set_type(0)` is the unplug (dormant in `recompose_active`; degrades the
effective tier to Faithful when plugged — no wake contract, the Symbiface/
serial precedent, so the strobe path need not thread through `wake_slot`).

## 6. Acid tests

1. **CRTC latch**: drive `pen.strobe` for one cycle at a known CRTC state;
   assert R16/R17 == the MA of that cycle (peek). Hold it 5 cycles: latches
   once (edge), value == MA at the rising edge.
2. **Beam match**: a board with CRTC + gun; set an aim, press the trigger,
   run a frame; assert R16/R17 got latched, and that the latched MA tracks
   the aim monotonically (aim further right/down → larger MA), across a few
   aim points.
3. **Released = no latch**: trigger up, run a frame, R16/R17 unchanged from
   their reset value (no legacy increment).
4. **Unplugged neutral**: `set_type(0)`; the four canonical CKSUMs unchanged
   (strobe never asserts; the CRTC latch never runs).
5. **Tier**: plugging degrades the effective tier to Faithful (recompose);
   the latch is identical Faithful vs Soldered.

## Batch contract (RunTier::Fast)

None needed: an active gun forces Faithful (§5), so the strobe/latch path
never runs under the wake/fast schedulers. Unplugged, the gun contributes no
events and `pen.strobe` rests LOW — elision-eligible, byte-exact neutral.
