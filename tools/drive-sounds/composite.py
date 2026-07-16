#!/usr/bin/env python3
"""composite.py — the `cat` scene: a full catalogue operation on a CPC 6128.

Renders resources/drive-sounds/demo_cat_sequence.wav (44100 Hz, 16-bit,
STEREO — drive panned slightly right, keyboard slightly left, as on the
physical machine).

REFERENCE MIX ONLY: this file exists to judge realism and balance of the
whole ritual.  The definitive per-event motor/stepper assets are produced by
the dedicated motor/stepper generators; the motor, stepper and clunk SOURCES
below are compact self-contained re-implementations so this script has no
dependency on those assets.

Event timeline (t = 0 at ENTER; the emulator's simulated Stage-2 mechanics):

    t=-0.86..-0.05  typing  c  a  t  ENTER  (keyboard, not the drive)
    t=0.000   MOTOR_START   spin-up ramp, 500 ms to speed
    t=0.500   MOTOR_READY   steady hum (300 RPM -> 5 Hz spindle AM)
    t=0.900   HEAD_LOAD     engage clunk
    t=1.000   STEP x2       12 ms apart (short seek to the directory track)
    t=1.08/1.28/1.47/1.66   four directory-sector reads, one revolution apart
                            (200 ms rev) — reads are nearly silent: only the
                            hum + faint index ticks every 200 ms from t=0.5
    t=2.600   MOTOR_STOP    wind-down over ~1.3 s (ticks stretch & fade as
                            the spindle decelerates)
    total ~4.5 s of drive timeline (+0.9 s typing lead-in)

Run:  python3 tools/drive-sounds/composite.py [output_dir]
"""

import os
import sys

import numpy as np
from scipy import signal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dsp_common import (SR, seconds, env_attack_decay, write_wav,
                        case_chain, CASE_MODES, save_spectrogram)

LEAD_IN = 0.90            # typing happens before ENTER (t=0)
TAIL = 4.50               # drive timeline length after ENTER
TOTAL = LEAD_IN + TAIL

MOTOR_START = 0.0
MOTOR_READY = 0.5
HEAD_LOAD_T = 0.9
STEP_TIMES = (1.000, 1.012)
MOTOR_STOP = 2.6
WINDDOWN = 1.3
REV = 0.2                 # 300 RPM -> 200 ms per revolution
SPINDLE_HZ = 5.0


def t2i(t):
    """Timeline time (t=0 at ENTER) -> sample index in the render buffer."""
    return seconds(t + LEAD_IN)


def add(buf, t, x):
    i = t2i(t)
    j = min(len(buf), i + len(x))
    if j > i:
        buf[i:j] += x[:j - i]


# --------------------------------------------------------------------------
# compact sources (self-contained — see module docstring)
# --------------------------------------------------------------------------

def _burst(rng, n, lp_hz, tau, attack=0.0004):
    """Shaped noise burst: the generic contact/impact excitation."""
    x = rng.standard_normal(n)
    b, a = signal.butter(2, min(lp_hz, SR / 2 * 0.95) / (SR / 2), 'low')
    return signal.lfilter(b, a, x) * env_attack_decay(n, attack, tau)


def motor_speed_profile(n):
    """Normalised spindle speed s(t) in [0,1] over the whole render."""
    t = np.arange(n) / SR - LEAD_IN
    s = np.zeros(n)
    # spin-up: exponential approach, hits ~98% at 500 ms
    up = (t >= MOTOR_START) & (t < MOTOR_STOP)
    s[up] = 1.0 - np.exp(-(t[up] - MOTOR_START) / 0.128)
    # wind-down: exponential decay, ~5% left after 1.3 s
    dn = t >= MOTOR_STOP
    s_at_stop = 1.0 - np.exp(-(MOTOR_STOP - MOTOR_START) / 0.128)
    s[dn] = s_at_stop * np.exp(-(t[dn] - MOTOR_STOP) / (WINDDOWN / 3.0))
    s[s < 0.02] = np.where(t[s < 0.02] > MOTOR_STOP, 0.0, s[s < 0.02])
    return s


def motor_hum(rng, n):
    """Compact spindle-motor model.

    A small DC motor belted to the 3" hub: commutation tone stack whose
    pitch tracks the speed profile, bearing/brush noise, and a 5 Hz
    amplitude modulation from hub eccentricity once at speed.
    """
    s = motor_speed_profile(n)
    t = np.arange(n) / SR

    # commutation harmonics (Hz at full speed, relative amplitude)
    partials = [(118.0, 1.00), (236.0, 0.45), (357.0, 0.22),
                (472.0, 0.13), (591.0, 0.07)]
    hum = np.zeros(n)
    for f0, a0 in partials:
        # slight per-partial detune drift so the stack sounds mechanical
        drift = 1.0 + 0.004 * np.sin(2 * np.pi * rng.uniform(0.3, 0.9) * t
                                     + rng.uniform(0, 2 * np.pi))
        phase = 2 * np.pi * np.cumsum(f0 * s * drift) / SR
        hum += a0 * np.sin(phase + rng.uniform(0, 2 * np.pi))

    # bearing / brush noise: band noise that scales a bit steeper than s
    noise = rng.standard_normal(n)
    b, a = signal.butter(2, [180.0 / (SR / 2), 1400.0 / (SR / 2)], 'band')
    noise = signal.lfilter(b, a, noise)
    hum = hum + 0.9 * noise * s ** 1.8

    # spin-up "grunt": extra broadband while the motor fights inertia
    grunt = signal.lfilter(*signal.butter(2, 500.0 / (SR / 2), 'low'),
                           rng.standard_normal(n))
    hum += 1.4 * grunt * s * (1.0 - s) ** 1.5

    # 5 Hz spindle-eccentricity AM (the "soft 5 Hz life"), tracks speed
    spindle_phase = 2 * np.pi * np.cumsum(SPINDLE_HZ * s) / SR
    am = 1.0 + 0.16 * s * np.sin(spindle_phase)
    hum *= am * s ** 1.4

    return case_chain(hum, rng, modal_mix=0.30, dry_mix=0.75, refl_gain=0.3)


def stepper_click(rng):
    """Compact head-stepper model: one track step.

    Bright, short: the stepper detent snap (1.2-4.5 kHz) plus a tiny
    low thump as the carriage jerks, through the case chain.
    """
    n = seconds(0.030)
    snap = _burst(rng, n, lp_hz=6000.0, tau=0.0035, attack=0.0002)
    b, a = signal.butter(2, [1200.0 / (SR / 2), 4500.0 / (SR / 2)], 'band')
    x = 1.0 * signal.lfilter(b, a, snap)
    x += 0.5 * _burst(rng, n, lp_hz=700.0, tau=0.005)
    return case_chain(x, rng, modal_mix=0.35, dry_mix=0.7, refl_gain=0.3)


def head_clunk(rng, unload=False):
    """Compact head-load solenoid clunk (dull, rings the low case modes)."""
    n = seconds(0.08)
    x = 1.6 * _burst(rng, n, lp_hz=900.0, tau=0.010)
    k = seconds(0.012)
    x[k:] += 0.5 * _burst(rng, n - k, lp_hz=500.0, tau=0.008)
    heavy = [(f, q, g * (2.2 if f < 500 else 0.35)) for f, q, g in CASE_MODES]
    y = case_chain(x, rng, modes=heavy, modal_mix=0.9, dry_mix=0.35,
                   refl_gain=0.5, lp=3500.0)
    return 0.55 * y if unload else y


def index_tick(rng):
    """Compact once-per-rev tick of a worn hub: soft low-mid thump."""
    n = seconds(0.030)
    x = _burst(rng, n, lp_hz=420.0, tau=0.005, attack=0.0006)
    lowish = [(f, q, g) for f, q, g in CASE_MODES if f < 700]
    return case_chain(x, rng, modes=lowish, modal_mix=0.8, dry_mix=0.25,
                      refl_gain=0.3, lp=2000.0)


def key_click(rng, enter=False):
    """CPC 6128 keyboard: plastic key press (tick + bottom-out) + release."""
    n = seconds(0.16)
    y = np.zeros(n)
    # press: bright plastic tick
    tick = _burst(rng, seconds(0.02), lp_hz=7000.0, tau=0.003, attack=0.0002)
    b, a = signal.butter(2, [900.0 / (SR / 2), 4200.0 / (SR / 2)], 'band')
    y[:len(tick)] += 0.8 * signal.lfilter(b, a, tick)
    # bottom-out thud (ENTER is the big key — heavier)
    thud = _burst(rng, seconds(0.03), lp_hz=650.0, tau=0.008)
    y[:len(thud)] += (1.5 if enter else 0.9) * thud
    # release click ~70-90 ms later, quieter and thinner
    kr = seconds(rng.uniform(0.07, 0.09))
    rel = _burst(rng, seconds(0.015), lp_hz=6000.0, tau=0.002, attack=0.0002)
    y[kr:kr + len(rel)] += 0.35 * signal.lfilter(b, a, rel)
    # the keyboard sits in the same case: lighter modal coupling
    return case_chain(y, rng, modal_mix=0.25, dry_mix=0.8, refl_gain=0.35)


# --------------------------------------------------------------------------
# scene assembly
# --------------------------------------------------------------------------

# Balance targets (dBFS), calibrated per bus so the mix is deterministic:
# hum is a quiet bed, clunks clearly above it, index ticks nearly subliminal
# (>= 20 dB below the step clicks).
LVL_HUM_RMS = -32.0     # steady-state hum bed, RMS
LVL_HEAD = -9.0         # head-load clunk, peak
LVL_STEP = -14.0        # stepper click, peak
LVL_TICK = -36.0        # index tick, peak (22 dB under the steps)
LVL_KEY = -12.0         # key click, peak


def _to_peak(x, db):
    """Scale x so its peak sits at `db` dBFS."""
    p = np.max(np.abs(x))
    return x * (10 ** (db / 20.0) / p) if p > 0 else x

def winddown_tick_times():
    """Index ticks during wind-down: revolution period stretches as the
    spindle decelerates (integrate speed, tick every full revolution)."""
    tau = WINDDOWN / 3.0
    ticks, t = [], 0.0
    revs = 0.0
    dt = 1e-4
    next_rev = 1.0
    while t < WINDDOWN:
        speed = np.exp(-t / tau)          # normalised
        revs += SPINDLE_HZ * speed * dt
        if revs >= next_rev and speed > 0.25:
            ticks.append(MOTOR_STOP + t)
            next_rev += 1.0
        t += dt
    return ticks


def render():
    rng = np.random.default_rng(0xCA7A106)
    n = seconds(TOTAL)

    drive = np.zeros(n)     # everything from the drive bay
    keys = np.zeros(n)      # keyboard

    # --- keyboard: c a t ENTER (human-ish inter-key timing, deterministic)
    for kt, is_enter in ((-0.86, False), (-0.63, False), (-0.42, False),
                         (-0.05, True)):
        add(keys, kt, _to_peak(key_click(rng, enter=is_enter),
                               LVL_KEY + rng.uniform(-1.5, 1.0)))

    # --- motor bed (covers the whole render; silent while s=0).
    # Calibrate on the steady-state window so the bed sits at LVL_HUM_RMS.
    hum = motor_hum(rng, n)
    ref = hum[t2i(1.9):t2i(2.1)]
    rms = np.sqrt(np.mean(ref ** 2))
    hum *= 10 ** (LVL_HUM_RMS / 20.0) / max(rms, 1e-12)
    drive += hum

    # --- head load
    add(drive, HEAD_LOAD_T, _to_peak(head_clunk(rng), LVL_HEAD))

    # --- seek: 2 steps, 12 ms apart
    for st in STEP_TIMES:
        add(drive, st, _to_peak(stepper_click(rng), LVL_STEP))

    # --- directory reads at 1.08/1.28/1.47/1.66: nearly silent by design.
    #     Nothing is rendered for them — only hum and index ticks remain.

    # --- index ticks: every REV from MOTOR_READY while at speed...
    t = MOTOR_READY
    while t < MOTOR_STOP - 1e-9:
        add(drive, t, _to_peak(index_tick(rng),
                               LVL_TICK + rng.uniform(-1.5, 1.5)))
        t += REV
    # ...then stretching & fading during wind-down
    for wt in winddown_tick_times():
        fade_db = 20 * np.log10(np.exp(-(wt - MOTOR_STOP) / (WINDDOWN / 3.0)))
        add(drive, wt, _to_peak(index_tick(rng), LVL_TICK + fade_db))

    # --- head unload click shortly after MOTOR_STOP (soft)
    add(drive, MOTOR_STOP + 0.05,
        _to_peak(head_clunk(rng, unload=True), LVL_HEAD - 7.0))

    # --- stereo mix: drive slightly right, keyboard slightly left of centre
    out = np.zeros((n, 2))
    out[:, 0] = 0.72 * drive + 1.00 * keys * 0.9
    out[:, 1] = 1.00 * drive + 0.82 * keys * 0.9
    # master trim: peak at -6 dBFS, relative balance untouched
    peak = np.max(np.abs(out))
    if peak > 0:
        out *= 0.5 / peak
    return out


def report(out):
    """Print level report for the balance QA."""
    def db(x):
        return 20 * np.log10(np.max(np.abs(x)) + 1e-12)
    mono = out.mean(axis=1)
    hum = mono[t2i(1.9):t2i(2.1)]
    rms = 20 * np.log10(np.sqrt(np.mean(hum ** 2)) + 1e-12)
    print(f'  total          {len(mono)/SR:.2f} s ({LEAD_IN:.2f} s lead-in '
          f'+ {TAIL:.2f} s drive timeline)')
    print(f'  master peak    {db(mono):6.1f} dBFS')
    print(f'  hum bed RMS    {rms:6.1f} dBFS  (t=1.9..2.1)')
    print(f'  head_load pk   {db(mono[t2i(0.895):t2i(0.99)]):6.1f} dBFS')
    print(f'  steps peak     {db(mono[t2i(0.998):t2i(1.05)]):6.1f} dBFS')
    print(f'  index tick pk  {db(mono[t2i(1.298):t2i(1.33)]):6.1f} dBFS '
          f'(t=1.3 tick)')
    print(f'  keys peak      {db(mono[:t2i(-0.02)]):6.1f} dBFS')


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('-')]
    out_dir = args[0] if args else os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        '..', '..', 'resources', 'drive-sounds')
    out_dir = os.path.abspath(out_dir)
    os.makedirs(out_dir, exist_ok=True)

    out = render()
    path = os.path.join(out_dir, 'demo_cat_sequence.wav')
    write_wav(path, out)
    print(f'wrote {path}')
    report(out)

    if '--plot' in sys.argv:
        save_spectrogram('/tmp/demo_cat_sequence_spec.png', out,
                         'demo_cat_sequence — cat + ENTER, full drive cycle')
        print('QA plot: /tmp/demo_cat_sequence_spec.png')


if __name__ == '__main__':
    main()
