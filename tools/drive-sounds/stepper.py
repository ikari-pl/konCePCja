#!/usr/bin/env python3
"""Physically-modeled synthesis of the CPC 3" disc drive head-stepper sounds.

Model overview
==============

One STEP event = one stepper-motor detent advance of the head carriage on its
leadscrew.  Each step is synthesized from four source components:

  1. click  -- sharp electromagnetic/mechanical click onset: a band-passed
               noise burst (~1-4 kHz, per-click centre randomised) with an
               instantaneous attack and ~1.2 ms exponential decay.
  2. tick   -- very short damped sine (~2.3 kHz, tau ~0.6 ms): the stepper
               coil snap itself.
  3. thunk  -- the carriage impact: a 1.5-2.5 ms Hann force pulse (broadband
               LF energy that drives the case modal bank hard) plus a small
               direct damped sine at ~300 Hz for the immediate thud.
  4. ring   -- faint leadscrew ring: two quiet damped sines near 3.1/4.4 kHz
               with ~6-12 ms decay.

The sources are written onto two excitation timelines (exc_lo carries the
thunk force pulses, exc_hi the click/tick/ring components) and the WHOLE
timeline is then run through the case model, so at 12 ms spacing successive
transients genuinely overlap inside the resonators -- the "brrrp" fusion is
produced by the physics of the model, never by pasting rendered clicks.

Case acoustics chain (mandatory, shared design with the motor task):

  source -> modal resonator bank (ABS case + metal subframe:
            9 damped modes 128-873 Hz, Q 8-30, strongly coupled to the
            carriage thunk; 3 weak modes 1.35-2.4 kHz)
         -> early reflections (2-6 ms taps inside the case)
         -> radiation filter (2nd-order HP at 80 Hz, gentle 1st-order
            LP at 6 kHz)

Per-click variation (deterministic, fixed-seed RNG 0xC9C5):
  * amplitude       : gaussian sigma 0.7 dB, clipped to +/-1.8 dB
  * timing jitter   : uniform +/-0.2 ms
  * spectral tilt   : click-band centre 2.2 kHz * 2^N(0, 0.15) and a
                      click/thunk balance wobble of +/-1 dB
  * backlash take-up: the FIRST step from rest and any direction reversal
                      get the thunk boosted x1.30 and the click x1.12

Event mapping (uPD765A on the CPC): the FDC issues STEP pulses every
(16 - SRT) * 2 ms.  AMSDOS programs SRT=10 -> 12 ms/step (~83 Hz repetition);
the power-on default SRT=0 gives 32 ms/step.  The emulator can either
schedule step_single.wav per STEP event (its tail is mix-tolerant at 12 ms
overlap) or play the pre-rendered trains.

All renders share one family gain, calibrated so the single-step transient
peaks at -10 dBFS.  Output: 44100 Hz, 16-bit PCM, mono.
"""

import os

import numpy as np
from scipy import signal
from scipy.io import wavfile

SR = 44100
SEED = 0xC9C5
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "..", "resources", "drive-sounds")

# ----------------------------------------------------------------------------
# Case model: modal resonator bank + early reflections + radiation filter
# ----------------------------------------------------------------------------

# (freq Hz, Q, gain) -- ABS case panels + metal subframe.
LOW_MODES = [
    (128.0, 12.0, 0.80),
    (176.0, 18.0, 0.90),
    (243.0, 22.0, 1.10),
    (317.0, 26.0, 0.85),
    (402.0, 16.0, 0.70),
    (498.0, 20.0, 0.62),
    (611.0, 14.0, 0.50),
    (742.0, 18.0, 0.40),
    (873.0, 12.0, 0.33),
]
# Weak upper modes (thin panel breakup).
HIGH_MODES = [
    (1350.0, 10.0, 0.14),
    (1780.0, 9.0, 0.11),
    (2320.0, 8.0, 0.08),
]


def resonator_coeffs(freq, q, sr=SR):
    """Two-pole constant-peak-gain bandpass resonator."""
    bw = freq / q
    r = np.exp(-np.pi * bw / sr)
    theta = 2.0 * np.pi * freq / sr
    a = np.array([1.0, -2.0 * r * np.cos(theta), r * r])
    b = np.array([(1.0 - r * r) / 2.0, 0.0, -(1.0 - r * r) / 2.0])
    return b, a


def mode_bank(x, modes):
    y = np.zeros_like(x)
    for freq, q, gain in modes:
        b, a = resonator_coeffs(freq, q)
        y += gain * signal.lfilter(b, a, x)
    return y


# Early reflections inside/around the case: (delay ms, gain).
REFLECTIONS = [(2.1, 0.24), (3.4, -0.17), (5.3, 0.11)]


def early_reflections(x):
    y = x.copy()
    for delay_ms, gain in REFLECTIONS:
        d = int(round(delay_ms * 1e-3 * SR))
        y[d:] += gain * x[:-d]
    return y


def radiation_filter(x):
    """HP 80 Hz (2nd order) + soft LP 6 kHz (1st order)."""
    bh, ah = signal.butter(2, 80.0 / (SR / 2.0), "highpass")
    bl, al = signal.butter(1, 6000.0 / (SR / 2.0), "lowpass")
    return signal.lfilter(bl, al, signal.lfilter(bh, ah, x))


def case_process(exc_lo, exc_hi):
    """Run the two excitation timelines through the full case chain.

    The carriage thunk (exc_lo) couples strongly into the low case modes;
    the click (exc_hi) couples weakly into them but drives the high modes
    and also radiates a partly-direct path (the crisp onset).
    """
    low = mode_bank(exc_lo + 0.25 * exc_hi, LOW_MODES)
    high = mode_bank(0.15 * exc_lo + exc_hi, HIGH_MODES)
    y = 1.00 * low + 0.55 * high + 0.50 * exc_hi + 0.12 * exc_lo
    y = early_reflections(y)
    return radiation_filter(y)


# ----------------------------------------------------------------------------
# Per-step source synthesis
# ----------------------------------------------------------------------------

def _damped_sine(n, freq, tau, phase=0.0):
    t = np.arange(n) / SR
    return np.exp(-t / tau) * np.sin(2.0 * np.pi * freq * t + phase)


def step_params(rng, first=False, reversal=False):
    """Deterministic per-step variation."""
    amp_db = float(np.clip(rng.normal(0.0, 0.7), -1.8, 1.8))
    p = {
        "amp": 10.0 ** (amp_db / 20.0),
        "jitter_s": float(rng.uniform(-0.2e-3, 0.2e-3)),
        "click_fc": 2200.0 * 2.0 ** float(rng.normal(0.0, 0.15)),
        "click_tau": float(rng.uniform(0.7e-3, 1.2e-3)),
        "ring_detune": 2.0 ** float(rng.uniform(-0.036, 0.036)),  # +/-2.5 %
        # click/thunk tilt wobble, +/-1 dB
        "tilt": 10.0 ** (float(rng.uniform(-1.0, 1.0)) / 20.0),
        "thunk_width": float(rng.uniform(1.5e-3, 2.5e-3)),
        "thunk_f": float(rng.uniform(255.0, 345.0)),
        "ring_tau": float(rng.uniform(6e-3, 12e-3)),
        "phase": float(rng.uniform(0.0, 2.0 * np.pi)),
        "click_gain": 1.0,
        "thunk_gain": 1.0,
    }
    if first or reversal:
        p["thunk_gain"] *= 1.30   # backlash take-up impact
        p["click_gain"] *= 1.12
    return p


def add_step(exc_lo, exc_hi, t_s, p, rng):
    """Write one step's source components onto the excitation timelines."""
    i0 = int(round((t_s + p["jitter_s"]) * SR))
    amp = p["amp"]

    # -- click: band-passed noise burst, instant attack -----------------
    n_click = int(0.006 * SR)
    t = np.arange(n_click) / SR
    burst = rng.standard_normal(n_click) * np.exp(-t / p["click_tau"])
    fc = p["click_fc"]
    lo = max(fc * 0.45, 900.0) / (SR / 2.0)
    hi = min(fc * 1.9, 5200.0) / (SR / 2.0)
    b, a = signal.butter(2, [lo, hi], "bandpass")
    click = signal.lfilter(b, a, burst)
    click *= amp * p["click_gain"] * p["tilt"] * 0.9
    # hard onset spike: one lowpassed impulse to stiffen the attack
    spike = np.zeros(n_click)
    spike[0] = 1.0
    bs, as_ = signal.butter(1, 5500.0 / (SR / 2.0), "lowpass")
    click += 0.55 * amp * p["click_gain"] * signal.lfilter(bs, as_, spike)

    # -- tick: stepper coil snap ----------------------------------------
    n_tick = int(0.003 * SR)
    tick = 0.25 * amp * _damped_sine(n_tick, 2300.0, 0.6e-3, p["phase"])

    # -- leadscrew ring: faint, longer ----------------------------------
    n_ring = int(0.030 * SR)
    det = p["ring_detune"]
    ring = 0.030 * amp * (_damped_sine(n_ring, 3130.0 * det, p["ring_tau"],
                                       p["phase"])
                          + 0.6 * _damped_sine(n_ring, 4410.0 / det,
                                               p["ring_tau"] * 0.7,
                                               p["phase"] * 0.5))

    # -- thunk: carriage impact force pulse + direct thud ----------------
    n_pulse = max(int(p["thunk_width"] * SR), 8)
    pulse = np.hanning(2 * n_pulse)[n_pulse:]          # sharp rise, soft fall
    pulse = np.concatenate([np.hanning(2 * (n_pulse // 4))[: n_pulse // 4],
                            pulse])                     # ~0.4 ms rise
    n_thud = int(0.015 * SR)
    thud = 0.5 * _damped_sine(n_thud, p["thunk_f"], 5e-3, p["phase"])
    tg = amp * p["thunk_gain"] / p["tilt"]

    def mix(dst, i, sig):
        j = min(i + len(sig), len(dst))
        if j > i:
            dst[i:j] += sig[: j - i]

    mix(exc_hi, i0, click)
    mix(exc_hi, i0, tick)
    mix(exc_hi, i0 + int(0.0008 * SR), ring)            # ring starts after hit
    mix(exc_lo, i0 + int(0.0005 * SR), tg * pulse)      # impact follows coil
    mix(exc_lo, i0 + int(0.0005 * SR), tg * thud)


# ----------------------------------------------------------------------------
# Train rendering
# ----------------------------------------------------------------------------

def render_train(n_steps, period_s, tail_s=0.060, reversals=(), seed=SEED):
    """Render a step train through the full case model (never concatenated)."""
    rng = np.random.default_rng(seed)
    n = int((n_steps - 1) * period_s * SR + tail_s * SR) + 1
    exc_lo = np.zeros(n)
    exc_hi = np.zeros(n)
    for k in range(n_steps):
        p = step_params(rng, first=(k == 0), reversal=(k in reversals))
        add_step(exc_lo, exc_hi, 0.002 + k * period_s, p, rng)
    return case_process(exc_lo, exc_hi)


def render_single(dur_s=0.050, seed=SEED):
    """One step incl. case ring-out; tail faded so 12 ms overlaps mix clean."""
    rng = np.random.default_rng(seed)
    n = int(dur_s * SR)
    exc_lo = np.zeros(n)
    exc_hi = np.zeros(n)
    p = step_params(rng, first=True)
    p["jitter_s"] = 0.0                    # canonical asset: no offset
    add_step(exc_lo, exc_hi, 0.002, p, rng)
    y = case_process(exc_lo, exc_hi)
    fade = int(0.010 * SR)                 # raised-cosine ring-out fade
    y[-fade:] *= 0.5 * (1.0 + np.cos(np.linspace(0.0, np.pi, fade)))
    return y


def write_wav(path, y, gain):
    data = np.clip(y * gain, -1.0, 1.0)
    wavfile.write(path, SR, (data * 32767.0).astype(np.int16))
    peak_db = 20.0 * np.log10(np.max(np.abs(data)) + 1e-12)
    print(f"  {os.path.basename(path):26s} {len(data)/SR*1000:7.1f} ms  "
          f"peak {peak_db:6.2f} dBFS")
    return peak_db


# ----------------------------------------------------------------------------
# Self-review metrics
# ----------------------------------------------------------------------------

def envelope(y, win_ms=1.0):
    w = int(win_ms * 1e-3 * SR)
    return np.sqrt(signal.lfilter(np.ones(w) / w, [1.0], y * y))


def report_single(y):
    env = envelope(y)
    peak = np.max(env)
    i_peak = int(np.argmax(env))
    above = np.nonzero(env > 0.05 * peak)[0]
    onset = above[0]
    attack_ms = (i_peak - onset) / SR * 1e3
    below = np.nonzero(env[i_peak:] < peak * 10 ** (-30 / 20))[0]
    decay_ms = (below[0] / SR * 1e3) if len(below) else float("nan")
    spec = np.abs(np.fft.rfft(y))
    freqs = np.fft.rfftfreq(len(y), 1.0 / SR)
    centroid = float(np.sum(freqs * spec) / np.sum(spec))
    print(f"  single: attack {attack_ms:.2f} ms, -30 dB decay {decay_ms:.1f} ms,"
          f" spectral centroid {centroid:.0f} Hz")


def report_train(y, period_s):
    env = envelope(y, 0.5)
    env = env - np.mean(env)
    ac = np.correlate(env, env, "full")[len(env) - 1:]
    lo = int(0.5 * period_s * SR)
    hi = int(1.5 * period_s * SR)
    lag = lo + int(np.argmax(ac[lo:hi]))
    print(f"  train : envelope repetition {SR/lag:.1f} Hz"
          f" (expected {1.0/period_s:.1f} Hz)")
    # per-click variation: RMS of each inter-step segment
    n_steps = int(len(y) / (period_s * SR))
    seg = int(period_s * SR)
    rms = [np.sqrt(np.mean(y[k*seg:(k+1)*seg] ** 2)) for k in range(min(n_steps, 16))]
    rms = np.array(rms[1:])                # skip boosted first step
    print(f"  train : per-click RMS spread {100*np.std(rms)/np.mean(rms):.1f}%"
          f" (0% would mean identical clicks)")


def spectrogram_png(path, y, title):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(9, 4.5), dpi=110)
    nfft = 1024 if len(y) > SR // 4 else 512
    pxx, freqs, bins, im = ax.specgram(y, NFFT=nfft, Fs=SR,
                                       noverlap=nfft * 7 // 8,
                                       cmap="magma", vmin=-130, vmax=-40)
    ax.set_ylim(0, 8000)
    ax.set_xlabel("time [s]")
    ax.set_ylabel("frequency [Hz]")
    ax.set_title(title)
    fig.colorbar(im, ax=ax, label="dB")
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)
    print(f"  wrote {os.path.basename(path)}")


# ----------------------------------------------------------------------------

def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    single = render_single()
    # Family gain: calibrate the single step transient to -10 dBFS peak.
    gain = 10.0 ** (-10.0 / 20.0) / np.max(np.abs(single))

    trains = {
        "seek_4steps_12ms.wav": render_train(4, 0.012),
        "seek_16steps_12ms.wav": render_train(16, 0.012),
        "recal_40steps_12ms.wav": render_train(40, 0.012),
        "seek_4steps_32ms.wav": render_train(4, 0.032),
    }
    # Headroom check: overlap in the case modes can push train peaks up.
    worst = max(np.max(np.abs(y)) for y in trains.values())
    if worst * gain > 10.0 ** (-1.0 / 20.0):
        gain = 10.0 ** (-1.0 / 20.0) / worst
        print(f"  family gain reduced for train headroom -> "
              f"single peak {20*np.log10(np.max(np.abs(single))*gain):.1f} dBFS")

    print("renders:")
    write_wav(os.path.join(OUT_DIR, "step_single.wav"), single, gain)
    for name, y in trains.items():
        write_wav(os.path.join(OUT_DIR, name), y, gain)

    print("self-review metrics:")
    report_single(single * gain)
    report_train(trains["recal_40steps_12ms.wav"] * gain, 0.012)

    spectrogram_png(os.path.join(OUT_DIR, "step_single_spectrogram.png"),
                    single * gain, "step_single (one head-stepper step)")
    spectrogram_png(os.path.join(OUT_DIR, "recal_40steps_spectrogram.png"),
                    trains["recal_40steps_12ms.wav"] * gain,
                    "recal_40steps_12ms (recalibrate 'brrrp', 83 Hz)")


if __name__ == "__main__":
    main()
