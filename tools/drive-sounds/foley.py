#!/usr/bin/env python3
"""foley.py — physically-modeled foley set for the konCePCja 3" disc drive.

Generates (44100 Hz, 16-bit, mono, deterministic):

    disc_insert.wav  (~0.5 s)  cartridge slide-in friction sweep + spring-latch clack
    disc_eject.wav   (~0.6 s)  eject-button throw, spring release, cartridge pop-out
    head_load.wav    (~80 ms)  heavy head-engage clunk, rings the low case modes
    index_tick.wav   (~30 ms)  barely-audible once-per-rev tick of a worn drive

Every source is synthesised from first principles (friction noise, impact
impulses, spring modes) and then passed through the shared case-acoustics
chain in dsp_common.py.  Run from anywhere:

    python3 tools/drive-sounds/foley.py [output_dir]
"""

import os
import sys

import numpy as np
from scipy import signal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dsp_common import (SR, seconds, env_exp, env_attack_decay, normalize,
                        write_wav, resonator, modal_bank, case_chain,
                        CASE_MODES, save_spectrogram)


# --------------------------------------------------------------------------
# building blocks
# --------------------------------------------------------------------------

def impact(rng, n, brightness_hz, tau, stiffness=2.0):
    """Impact excitation: short noise burst shaped like a real contact event.

    brightness_hz : lowpass corner — hard/bright contacts high, dull ones low
    tau           : decay time constant of the contact noise
    stiffness     : sharpness of the attack (higher = harder contact)
    """
    x = rng.standard_normal(n)
    b, a = signal.butter(2, min(brightness_hz, SR / 2 * 0.95) / (SR / 2), 'low')
    x = signal.lfilter(b, a, x)
    x *= env_attack_decay(n, attack=0.0004 / stiffness, tau=tau)
    return x


def friction(rng, n, grit_hz, corner_lo, corner_hi, stick_slip_hz=0.0,
             stick_depth=0.0):
    """Plastic-on-plastic sliding friction.

    Broadband noise band-shaped between a sweeping pair of corners
    (corner_lo -> corner_hi arrays or scalars, in Hz), with optional
    stick-slip amplitude granulation at stick_slip_hz.
    """
    x = rng.standard_normal(n)
    # grit: pre-emphasis so the noise has surface texture
    b, a = signal.butter(1, grit_hz / (SR / 2), 'high')
    x = 0.5 * x + signal.lfilter(b, a, x)

    lo = np.broadcast_to(np.asarray(corner_lo, float), (n,))
    hi = np.broadcast_to(np.asarray(corner_hi, float), (n,))
    # time-varying bandpass: two cascaded swept one-pole pairs (steeper
    # skirts so the sweep reads clearly against the grit noise)
    c_hi = 1.0 - np.exp(-2.0 * np.pi * hi / SR)
    c_lo = 1.0 - np.exp(-2.0 * np.pi * lo / SR)
    y = x
    for _ in range(2):
        src = y
        out = np.zeros(n)
        lp1 = lp2 = 0.0
        for i in range(n):
            lp1 += c_hi[i] * (src[i] - lp1)   # lowpass at hi corner
            lp2 += c_lo[i] * (lp1 - lp2)      # lowpass at lo corner
            out[i] = lp1 - lp2                # band between the two
        y = out

    if stick_slip_hz > 0.0:
        t = np.arange(n) / SR
        # irregular stick-slip: jittered phase so it doesn't sound periodic
        jitter = np.cumsum(rng.standard_normal(n)) * 0.002
        gate = 0.5 + 0.5 * np.sin(2 * np.pi * stick_slip_hz * t + jitter)
        y *= (1.0 - stick_depth) + stick_depth * gate ** 2
    return y


def spring_ping(rng, n, f0, tau, chirp=0.0, gain=1.0):
    """A struck spring: exponentially decaying partial with slight detune
    drift (chirp, Hz/s) and its faint 2nd partial."""
    t = np.arange(n) / SR
    ph = 2 * np.pi * (f0 * t + 0.5 * chirp * t * t)
    y = np.sin(ph + rng.uniform(0, 2 * np.pi))
    y += 0.35 * np.sin(2.7 * ph + rng.uniform(0, 2 * np.pi))
    return gain * y * env_exp(n, tau)


# --------------------------------------------------------------------------
# 1. disc_insert — cartridge slide + spring-latch clack
# --------------------------------------------------------------------------

def gen_disc_insert():
    rng = np.random.default_rng(0xD15C1)
    dur = 0.52
    n = seconds(dur)
    y = np.zeros(n)
    t = np.arange(n) / SR

    # --- friction sweep: rigid 3" cartridge sliding into the steel guide.
    # The cartridge accelerates as the user pushes; contact band rises.
    slide_dur = 0.36
    ns = seconds(slide_dur)
    prog = (np.arange(ns) / ns) ** 1.4              # accelerating motion
    corner_lo = 300.0 + 900.0 * prog                # band sweeps up
    corner_hi = 1400.0 + 2600.0 * prog
    fr = friction(rng, ns, grit_hz=2500.0, corner_lo=corner_lo,
                  corner_hi=corner_hi, stick_slip_hz=38.0, stick_depth=0.55)
    # slide gets louder toward the detent, tiny gap right before the latch
    slide_env = (0.25 + 0.75 * prog) * \
        (1.0 - 0.85 * np.exp(-((np.arange(ns) / ns - 0.97) / 0.02) ** 2))
    y[:ns] += 0.60 * fr * slide_env

    # a chunky 3" cartridge also rumbles the guide rails while sliding
    rumble = friction(rng, ns, grit_hz=150.0, corner_lo=90.0,
                      corner_hi=260.0)
    y[:ns] += 0.18 * rumble * (0.3 + 0.7 * prog)

    # --- spring-latch CLACK at the end of travel (t ~= 0.37 s)
    k = seconds(0.37)
    nc = n - k
    clack = np.zeros(nc)
    # bright snap 1-5 kHz: latch tooth snapping over the detent
    snap = impact(rng, nc, brightness_hz=5200.0, tau=0.006, stiffness=3.0)
    bs, as_ = signal.butter(2, [1000.0 / (SR / 2), 5000.0 / (SR / 2)], 'band')
    clack += 1.0 * signal.lfilter(bs, as_, snap)
    # metallic latch-spring pings
    clack += spring_ping(rng, nc, 3170.0, 0.010, chirp=-2000.0, gain=0.30)
    clack += spring_ping(rng, nc, 4480.0, 0.006, gain=0.18)
    # case thunk: the whole drive front takes the hit (short — the ABS
    # panel is damped by the subframe screws, it must not "boing")
    thunk = impact(rng, nc, brightness_hz=700.0, tau=0.012)
    clack += 1.0 * thunk
    # latch bounce: a second, smaller contact ~9 ms later
    kb = seconds(0.009)
    bounce = impact(rng, nc - kb, brightness_hz=4200.0, tau=0.004)
    clack[kb:] += 0.35 * signal.lfilter(bs, as_, bounce)
    y[k:] += 2.2 * clack

    out = case_chain(y, rng, modal_mix=0.42, dry_mix=0.62, refl_gain=0.45)
    return normalize(out, 0.85)


# --------------------------------------------------------------------------
# 2. disc_eject — button throw, spring release, cartridge pop-out
# --------------------------------------------------------------------------

def gen_disc_eject():
    rng = np.random.default_rng(0xE1EC7)
    dur = 0.62
    n = seconds(dur)
    y = np.zeros(n)

    # --- eject button press: dull plastic click at t=0
    nb = seconds(0.10)
    btn = impact(rng, nb, brightness_hz=1800.0, tau=0.008)
    y[:nb] += 0.55 * btn

    # --- mechanical lever travel: short scrape while the button pushes
    # the ejector linkage through its throw (t = 0.01 .. 0.13)
    k = seconds(0.012)
    nl = seconds(0.12)
    lever = friction(rng, nl, grit_hz=1200.0, corner_lo=500.0,
                     corner_hi=2200.0, stick_slip_hz=55.0, stick_depth=0.4)
    y[k:k + nl] += 0.14 * lever * env_attack_decay(nl, 0.01, 0.06)

    # --- spring RELEASE at t ~= 0.14: the latch lets go with a hard metallic
    # snap and the ejector spring twangs
    k = seconds(0.14)
    nr = n - k
    rel = np.zeros(nr)
    snap = impact(rng, nr, brightness_hz=6000.0, tau=0.005, stiffness=3.0)
    bs, as_ = signal.butter(2, [1200.0 / (SR / 2), 5600.0 / (SR / 2)], 'band')
    rel += 0.9 * signal.lfilter(bs, as_, snap)
    rel += spring_ping(rng, nr, 780.0, 0.045, chirp=-350.0, gain=0.45)  # twang
    rel += spring_ping(rng, nr, 2330.0, 0.018, chirp=-900.0, gain=0.22)
    rel += 1.1 * impact(rng, nr, brightness_hz=600.0, tau=0.014)        # thunk
    y[k:] += 1.9 * rel

    # --- cartridge pop-out: friction burst, band sweeping DOWN as the
    # cartridge decelerates out of the slot (t = 0.16 .. 0.42)
    k = seconds(0.165)
    np_ = seconds(0.26)
    prog = (np.arange(np_) / np_) ** 0.7
    corner_lo = 1100.0 - 800.0 * prog
    corner_hi = 3600.0 - 2400.0 * prog
    pop = friction(rng, np_, grit_hz=2200.0, corner_lo=corner_lo,
                   corner_hi=corner_hi, stick_slip_hz=30.0, stick_depth=0.5)
    pop_env = np.exp(-prog * 3.2) * (0.4 + 0.6 * np.exp(-prog * 12.0))
    y[k:k + np_] += 0.38 * pop * pop_env

    # --- cartridge hits the end stop / user's fingers: soft final tap
    k = seconds(0.40)
    nt = n - k
    y[k:] += 0.5 * impact(rng, nt, brightness_hz=1100.0, tau=0.012)

    out = case_chain(y, rng, modal_mix=0.55, dry_mix=0.55, refl_gain=0.45)
    return normalize(out, 0.85)


# --------------------------------------------------------------------------
# 3. head_load — heavy engage clunk
# --------------------------------------------------------------------------

def gen_head_load():
    rng = np.random.default_rng(0x4EAD)
    dur = 0.085
    n = seconds(dur)

    # heavy, dull impact: the head-load solenoid slams the pad/arm down.
    exc = 1.6 * impact(rng, n, brightness_hz=900.0, tau=0.010, stiffness=1.2)
    # small secondary contact (arm settling) 12 ms later, even duller
    k = seconds(0.012)
    exc[k:] += 0.5 * impact(rng, n - k, brightness_hz=500.0, tau=0.008)
    # a whisper of mechanical click so it isn't a pure thud
    b, a = signal.butter(2, [900.0 / (SR / 2), 2400.0 / (SR / 2)], 'band')
    exc += 0.12 * signal.lfilter(b, a, impact(rng, n, 3000.0, 0.003))

    # ring the LOW case modes hard: boost the low cluster, kill the highs
    heavy_modes = [(f, q, g * (2.2 if f < 500 else 0.35))
                   for (f, q, g) in CASE_MODES]
    out = case_chain(exc, rng, modes=heavy_modes, modal_mix=0.9, dry_mix=0.35,
                     refl_gain=0.5, lp=3500.0)
    return normalize(out, 0.85)


# --------------------------------------------------------------------------
# 4. index_tick — barely audible once-per-rev tick
# --------------------------------------------------------------------------

def gen_index_tick():
    rng = np.random.default_rng(0x71C4)
    dur = 0.032
    n = seconds(dur)

    # a worn hub catching once per revolution: tiny, soft, low-mid thump
    exc = impact(rng, n, brightness_hz=420.0, tau=0.005, stiffness=0.8)
    lowish = [(f, q, g) for (f, q, g) in CASE_MODES if f < 700]
    out = case_chain(exc, rng, modes=lowish, modal_mix=0.8, dry_mix=0.25,
                     refl_gain=0.3, lp=2000.0)
    # SUBTLE: normalise to ~-30 dBFS peak — at least 20 dB below a step click
    # (step clicks and the other foley assets are mastered near -1.5 dBFS).
    return normalize(out, 0.030)


# --------------------------------------------------------------------------

def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        '..', '..', 'resources', 'drive-sounds')
    out_dir = os.path.abspath(out_dir)
    os.makedirs(out_dir, exist_ok=True)

    assets = {
        'disc_insert': gen_disc_insert(),
        'disc_eject': gen_disc_eject(),
        'head_load': gen_head_load(),
        'index_tick': gen_index_tick(),
    }
    for name, x in assets.items():
        path = os.path.join(out_dir, name + '.wav')
        write_wav(path, x)
        peak_db = 20 * np.log10(np.max(np.abs(x)) + 1e-12)
        print(f'{name:12s}  {len(x)/SR*1000:6.1f} ms  peak {peak_db:6.1f} dBFS'
              f'  -> {path}')

    save_spectrogram(os.path.join(out_dir, 'disc_insert_spectrogram.png'),
                     assets['disc_insert'],
                     'disc_insert — friction sweep + spring-latch clack')
    save_spectrogram(os.path.join(out_dir, 'head_load_spectrogram.png'),
                     assets['head_load'],
                     'head_load — head-engage clunk (low case modes)',
                     fmax=4000)
    print('spectrograms written.')


if __name__ == '__main__':
    main()
