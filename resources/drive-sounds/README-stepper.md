# Drive head-stepper sounds (physically modeled)

Synthesized assets for the 3" drive's head-stepper: the iconic seek "clicks"
and the recalibrate "brrrp". Generated deterministically (fixed RNG seed
`0xC9C5`) by [`tools/drive-sounds/stepper.py`](../../tools/drive-sounds/stepper.py):

```bash
python3 tools/drive-sounds/stepper.py   # needs numpy, scipy, matplotlib
```

All files: 44 100 Hz, 16-bit PCM, mono.

## Files

| File | Content |
|------|---------|
| `step_single.wav` | One step incl. case ring-out (50 ms, 10 ms fade-out tail) |
| `seek_4steps_12ms.wav` | Short seek, 4 steps @ 12 ms (AMSDOS SRT) |
| `seek_16steps_12ms.wav` | Longer seek, 16 steps @ 12 ms |
| `recal_40steps_12ms.wav` | Recalibrate, 40 steps @ 12 ms — the "brrrp" |
| `seek_4steps_32ms.wav` | 4 steps @ 32 ms (power-on default SRT, for contrast) |
| `step_single_spectrogram.png` | Spectrogram of the single step |
| `recal_40steps_spectrogram.png` | Spectrogram of the recalibrate buzz |

## Physical model

One STEP event = one stepper detent advance of the head carriage on its
leadscrew. Four source components per step:

1. **Click** — band-passed noise burst (centre ≈2.2 kHz, passband ~1–4 kHz),
   instantaneous attack, τ ≈ 0.7–1.2 ms decay, plus a one-sample low-passed
   onset spike that stiffens the attack.
2. **Tick** — the stepper coil snap: damped sine 2.3 kHz, τ 0.6 ms.
3. **Thunk** — the carriage impact: a 1.5–2.5 ms Hann force pulse (broadband
   LF energy) plus a direct damped sine at 255–345 Hz, delayed 0.5 ms after
   the coil snap.
4. **Ring** — faint leadscrew ring: two quiet damped sines near 3.13/4.41 kHz
   (per-click detuned ±2.5 %), τ 6–12 ms.

### Case acoustics chain

`source → modal resonator bank → early reflections → radiation filter`

- **Modal bank** (ABS case + metal subframe): 9 two-pole resonators at
  128/176/243/317/402/498/611/742/873 Hz, Q 12–26, plus 3 weak modes at
  1350/1780/2320 Hz, Q 8–10. The thunk force pulse couples strongly into the
  low modes (the thud rings them noticeably); the click couples at 0.25×.
- **Early reflections**: taps at 2.1/3.4/5.3 ms, gains +0.24/−0.17/+0.11.
- **Radiation filter**: 2nd-order Butterworth HP at 80 Hz + soft 1st-order
  LP at 6 kHz.

Step trains are rendered by placing all step excitations on one timeline and
running the *whole* timeline through the case model — the transients overlap
inside the resonators, which is what fuses the 12 ms train into the textured
83 Hz buzz. The trains are **not** concatenated single-step renders.

## Per-click variation (deterministic)

Every step differs slightly, driven by `np.random.default_rng(0xC9C5)`:

| Parameter | Variation |
|-----------|-----------|
| Amplitude | gaussian σ 0.7 dB, clipped ±1.8 dB |
| Timing jitter | uniform ±0.2 ms |
| Spectral tilt | click band centre 2.2 kHz × 2^N(0, 0.15); click/thunk balance ±1 dB |
| Thunk | width 1.5–2.5 ms, body frequency 255–345 Hz |
| Leadscrew ring | detune ±2.5 %, τ 6–12 ms |
| Backlash take-up | FIRST step from rest and direction reversals: thunk ×1.30, click ×1.12 |

Measured on `recal_40steps_12ms.wav`: envelope repetition 83.1 Hz (expected
83.3 Hz), per-click RMS spread 12 % — audibly a mechanical train, not a
repeated sample.

## Loudness / headroom

Family gain is calibrated so the single-step transient peaks at **−10 dBFS**;
the same absolute gain is applied to every render (train peaks land at
−9.0…−10.0 dBFS from resonator overlap). Mixing `step_single.wav` instances
at 12 ms spacing is clip-safe: 16 overlapping copies peak at −9.2 dBFS.

## Event mapping for the emulator

The uPD765A issues STEP pulses every **(16 − SRT) × 2 ms**:

- AMSDOS programs SRT = 10 → **12 ms/step** (~83 Hz — the classic sound)
- Power-on default SRT = 0 → **32 ms/step**

The emulator may either schedule `step_single.wav` per STEP event (its tail
is mix-tolerant at 12 ms overlap; boost the first step of a burst ~+2 dB for
extra realism) or play the pre-rendered trains: `recal_40steps_12ms.wav` for
RECALIBRATE, `seek_*steps_12ms.wav` for short/long SEEKs.
