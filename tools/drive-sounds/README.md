# drive-sounds — physically-modeled 3" drive foley for konCePCja

Reproducible generators for the disc-drive sound assets in
`resources/drive-sounds/`. Pure Python (numpy/scipy/matplotlib), no
external samples — every asset is synthesised from physical first
principles with fixed RNG seeds, so re-running a generator reproduces the
committed WAV byte-for-byte.

```bash
python3 tools/drive-sounds/foley.py        # Part A: per-event foley set
python3 tools/drive-sounds/composite.py    # Part B: `cat` scene reference mix
# optional: composite.py --plot writes a QA spectrogram to /tmp
```

All Part A assets are 44100 Hz, 16-bit, mono. The composite is stereo.

## The case-acoustics chain (`dsp_common.py`)

Every source is rendered "inside the machine" and then passed through a
model of how sound leaves a CPC 6128 — a 3" drive on a metal subframe
inside an ABS enclosure:

1. **Modal resonator bank** — 8 low/mid modes (137–866 Hz, Q 8–28: ABS
   panel fundamentals + subframe plate modes) plus 3 weak high modes
   (1.5/2.7/4.3 kHz panel breakup), mixed with a direct/airborne path that
   leaks through vents and seams.
2. **Early reflections** — 5 taps at 2–6 ms (case interior), mildly
   lowpassed, alternating sign.
3. **Radiation filter** — 2nd-order HP at 80 Hz (small panels cannot
   radiate lows) and a soft 2nd-order LP at 6 kHz (ABS damping).

Sources can override the mode set: heavy events boost the <500 Hz cluster,
tiny events use only the low modes.

## Part A — foley set (`foley.py`)

| Asset | Length | Model |
|---|---|---|
| `disc_insert.wav` | 0.52 s | Rigid 3" cartridge sliding into the steel guide: stick-slip friction noise through a *swept* bandpass (band rises as the cartridge accelerates) + low guide-rail rumble, ending in the spring-latch CLACK: 1–5 kHz snap, two metallic latch-spring pings (3.2/4.5 kHz, one with downward chirp), a short case thunk, and a latch bounce 9 ms later. |
| `disc_eject.wav` | 0.62 s | Distinct render, not a reversal: dull eject-button click → lever-linkage scrape (60–130 ms) → spring RELEASE (hard snap + 780 Hz spring twang with downward chirp + thunk) → cartridge pop-out friction with the band sweeping *down* as it decelerates → soft end-stop tap. |
| `head_load.wav` | 85 ms | Head-load solenoid slamming the arm down: heavy dull impact (LP 900 Hz) + settling contact 12 ms later, driven through the case chain with the <500 Hz modes boosted 2.2× and highs cut (LP 3.5 kHz) — duller and heavier than a stepper click by construction. |
| `index_tick.wav` | 32 ms | Worn hub catching once per revolution: tiny soft impact (LP 420 Hz) through the low case modes only. Mastered at ≈ **−30 dBFS peak** while the other assets sit near −1.4 dBFS: at least 20 dB below a step click, nearly subliminal. |

QA spectrograms (regenerated on every run):
`resources/drive-sounds/disc_insert_spectrogram.png`,
`resources/drive-sounds/head_load_spectrogram.png`.

## Part B — composite `cat` scene (`composite.py`)

`resources/drive-sounds/demo_cat_sequence.wav` — **a reference mix for
judging realism and balance only.** The definitive per-event motor and
stepper assets come from the dedicated motor/stepper generators (built
separately); the motor, stepper and clunk sources inside `composite.py`
are compact self-contained re-implementations so the scene renders without
any external assets.

Timeline (t = 0 at ENTER, from the emulator's simulated Stage-2 mechanics;
the file has a 0.9 s typing lead-in):

| t (s) | Event |
|---|---|
| −0.86 / −0.63 / −0.42 / −0.05 | keys `c` `a` `t` ENTER (plastic tick + bottom-out + release click) |
| 0.000 | MOTOR_START — spin-up ramp, 500 ms to speed (pitch and level track the speed profile; extra broadband "grunt" while the motor fights inertia) |
| 0.500 | MOTOR_READY — steady hum: commutation-harmonic stack + bearing noise, 5 Hz spindle-eccentricity AM (300 RPM) |
| 0.900 | HEAD_LOAD clunk |
| 1.000, 1.012 | 2 STEP events, 12 ms apart |
| 1.08 / 1.28 / 1.47 / 1.66 | four directory-sector reads, one 200 ms revolution apart — nearly silent by design: nothing rendered, only hum + ticks |
| 0.5 → 2.6 | faint index tick every 200 ms |
| 2.600 | MOTOR_STOP — exponential wind-down over ~1.3 s; index ticks stretch and fade with the decelerating spindle; soft head-unload click at +50 ms |
| ~4.5 | end of drive timeline (file total 5.4 s) |

Stereo image: drive bus panned slightly right, keyboard slightly left of
centre, matching the physical CPC 6128 layout.

Balance is **calibrated, not hoped for** — each bus is scaled to a target
level after synthesis:

| Bus | Target |
|---|---|
| motor hum bed | −32 dBFS RMS (quiet bed) |
| head-load clunk | −9 dBFS peak |
| stepper clicks | −14 dBFS peak |
| index ticks | −36 dBFS peak (22 dB under the steps, nearly subliminal) |
| key clicks | −12 dBFS peak |
| master | −6 dBFS peak trim |
