# Drive motor/spindle sound assets

Physically-modeled synthesis of the CPC 6128 internal 3-inch drive
(Matsushita-type, **belt-driven** spindle) mounted on a steel subframe inside
the ABS case. Rendered by the deterministic generator
[`tools/drive-sounds/motor.py`](../../tools/drive-sounds/motor.py)
(fixed RNG seed `20260703`, no wall-clock — re-running reproduces these files
bit-for-bit).

All assets: **44,100 Hz, 16-bit PCM, mono**, one family gain (see
[Mastering](#mastering)).

## Event mapping

| Emulator event | Asset | Length | Playback |
|---|---|---|---|
| `MOTOR_START` | `motor_spinup.wav` | 0.600 s | Play once. Contains the full 500 ms speed ramp + 100 ms settle. Crossfade ~10-20 ms into the hum loop at the end. |
| `MOTOR_READY` (motor running) | `motor_hum_loop.wav` | 1.600 s | Loop seamlessly (no crossfade needed — see [Loop-continuity proof](#loop-continuity-proof)). |
| `MOTOR_STOP` | `motor_spindown.wav` | 1.350 s | Stop the loop, play once. Ends in digital silence (last samples are exactly 0). |

## Physical model

Signal chain: **sources → structure-borne coupling (modal bank) → case
radiation (air filter + early reflections)**. One rotational state machine
drives every source, so all components stay phase-coherent.

### Rotational state

| Quantity | Value | Why |
|---|---|---|
| Spindle speed | 5 Hz (300 RPM) | 3" drive spec speed |
| Belt pulley ratio | 10 : 1 | small DC motor rotor at 50 Hz (3000 RPM) |
| Commutation events / rotor rev | 6 | 3-pole rotor × 2 brushes |
| Commutation fundamental | **300 Hz** | 6 × 50 Hz — the "motor whine" |
| Wow | ±0.22 % FM | sum of sinusoids at 0.625/1.25/2.5 Hz (weights 1.0/0.55/0.30) — each completes an integer number of cycles per loop |

### Sources (pre-chain levels in parentheses)

* **Commutation tone series** — harmonics of 300 Hz with amplitudes
  1.0/0.42/0.20/0.09 (master 0.30), phase-modulated ±0.35 rad at the 50 Hz
  rotor rate (torque ripple). The PM + wow + modal filtering keep it from
  ever sounding like a naked sine.
* **Rotor imbalance tone** — 50 Hz (0.11).
* **Brush/commutation fizz** — Gaussian noise band-passed 850-4300 Hz
  (2nd-order Butterworth, master 0.20), amplitude-modulated at the rotor
  rate (depth 0.55) and weakly at the commutation rate (0.22).
* **Belt rumble** — noise band-passed 20-60 Hz (pre-chain 1.1; the radiation
  high-pass attenuates it back to "felt more than heard"), AM at the belt
  pass frequency 3.125 Hz (5 cycles/loop, depth 0.30).
* **Warm LF bed** — noise low-passed at 500 Hz (0.05).
* **Once-per-revolution unevenness** — global AM at the 5 Hz spindle rate,
  depth 0.07 + 0.03 second harmonic.

### Case acoustics 1: modal resonator bank

Parallel RBJ constant-peak band-pass biquads (structure-borne coupling into
the ABS shell + steel subframe). Mix: 0.35 dry + 1.00 modal.

| f [Hz] | Q | gain | | f [Hz] | Q | gain |
|---|---|---|---|---|---|---|
| 128 | 9 | 1.00 | | 534 | 18 | 0.55 |
| 176 | 14 | 0.85 | | 668 | 26 | 0.48 |
| 243 | 11 | 0.95 | | 871 | 20 | 0.38 |
| 318 | 22 | 0.75 | | 1260 | 15 | 0.20 |
| 412 | 15 | 0.70 | | 1820 | 13 | 0.13 |
| | | | | 2410 | 12 | 0.10 |

### Case acoustics 2: radiation / air

1. 2nd-order Butterworth **high-pass at 78 Hz** — a small plastic box
   radiates almost no deep bass.
2. RBJ **high shelf −9 dB above 6 kHz** — plastic-panel radiation roll-off.
3. **Early reflections** ("inside a box on a desk"): taps at 2.1 ms (+0.28),
   3.7 ms (−0.19), 5.5 ms (+0.12), mixed at 0.5.

### Spin-up (`motor_spinup.wav`)

* Speed profile `s(t) = 1 − exp(−(t/0.22)^1.7)` (Weibull-shaped: slow start
  under belt load, then catch-up) → 98.2 % at 0.5 s, 99.6 % at 0.6 s. The
  pitch glide spans the full **500 ms MOTOR_START ramp** and is synthesized
  by phase integration (`cumsum` of instantaneous frequency) — perfectly
  smooth, no stair-stepping.
* **Belt-compliance flutter**: damped 11 Hz speed oscillation (depth 3 %,
  τ = 160 ms) starting 60 ms in when the belt takes up slack; drives both FM
  and AM.
* Electrical level `0.35 + 0.65·s^1.2` (stall current buzzes from the first
  moment while the pitch is still low → the classic rising "urrrr"),
  mechanical level `s²`.
* Motor-energize "tick": 6 ms damped noise burst + step edge exciting the
  modal bank at t = 0 (4 ms fade-in de-clicks the file start).

### Spin-down (`motor_spindown.wav`)

* Power cut at t = 0: commutation buzz gated with τ = 70 ms (the current
  dies), residual brush slide ∝ 0.25·s².
* Belt/spindle **coast**: speed `exp(−t/0.36)`, every tone's frequency
  follows it down; mechanical level ∝ s^1.7.
* Final 60 ms squared-cosine fade guarantees exact digital silence.

## Mastering

The **hum loop is normalized to −18.0 dBFS RMS** and the *same* gain is
applied to spin-up and spin-down, so relative loudness between the three
assets is the physical one (a family peak ceiling of −1 dBFS would trim all
three together; with the final parameters no trim was needed).

| Asset | RMS | Peak |
|---|---|---|
| `motor_hum_loop.wav` | −18.0 dBFS | −8.2 dBFS |
| `motor_spinup.wav` | −18.3 dBFS | −8.3 dBFS |
| `motor_spindown.wav` | −31.8 dBFS (decays to silence) | −8.8 dBFS |

## Loop-continuity proof

The loop is **exactly 8 spindle revolutions**: 70,560 samples = 1.600 s at
5 Hz. Seamlessness is by construction:

* every tonal component and every modulator (wow sinusoids, rev AM, belt AM)
  completes an integer number of cycles in 1.6 s;
* **all** loop filtering is circular — the modal bank, radiation high-pass
  and shelf are applied by multiplying the signal's rFFT by the filters'
  frequency response (circular convolution), and early reflections use
  circular `np.roll`. The rendered buffer is therefore mathematically
  periodic.

Numeric verification (printed by `motor.py` on every render, float domain
before quantization):

| Check | Value | Interpretation |
|---|---|---|
| first sample / last sample | 0.010506 / 0.005543 | wrap step 0.00496 |
| wrap step vs. max in-loop step | 0.00496 vs 0.05974 | the loop point steps **12×
less** than the signal's own maximum slew |
| wrap step vs. RMS in-loop step | 0.00496 vs 0.01285 | below the *average* sample-to-sample step |
| max \|Δ\| within ±100 samples of the wrap (loop tiled twice) | 0.03566 | ≤ max in-loop step ⇒ no discontinuity spike |
| spectral continuity (smoothed Welch, 30 Hz-6 kHz): first vs. last 200 ms | max dev 2.86 dB | normal noise variance between segments |
| spectrum of a segment *wrapped across* the loop point vs. head/tail mean | max dev 2.37 dB | the boundary region is spectrally indistinguishable from loop interior |

In the quantized int16 file the wrap step is 162 LSB (first = 344,
last = 182) against a maximum in-loop step of ~1960 LSB — i.e. the loop point
is an ordinary, unremarkable sample transition.

## Spectrograms

`motor_spinup.png`, `motor_hum_loop.png`, `motor_spindown.png` — 0-5 kHz,
symlog frequency axis, 85 dB dynamic range. Regenerated alongside the WAVs.

## Regenerating

```bash
python3 tools/drive-sounds/motor.py            # writes into resources/drive-sounds/
python3 tools/drive-sounds/motor.py /tmp/out   # or anywhere else
```

Requires `numpy`, `scipy`, `matplotlib`. Every parameter mentioned above
lives in the `PARAMS` block at the top of the script.
