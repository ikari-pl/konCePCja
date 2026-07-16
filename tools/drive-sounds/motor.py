#!/usr/bin/env python3
"""
motor.py — physically-modeled synthesis of the CPC 6128 internal 3" drive
motor/spindle sounds (Matsushita-type belt-driven mechanism in an ABS case).

Produces three WAV assets that map 1:1 onto emulator drive-mechanics events:

    MOTOR_START  -> resources/drive-sounds/motor_spinup.wav   (~0.6 s)
    MOTOR_READY  -> resources/drive-sounds/motor_hum_loop.wav (seamless loop)
    MOTOR_STOP   -> resources/drive-sounds/motor_spindown.wav (~1.35 s)

Physical model (source -> structure-borne coupling -> case radiation):

  SOURCES (all tied to one rotational state machine):
    * DC motor rotor at MOTOR_HZ (belt pulley ratio 10:1 over the 5 Hz
      spindle).  Rotor imbalance -> weak tone at MOTOR_HZ.
    * Commutation: 3-pole motor, 2 brushes -> 6 commutation events per rotor
      rev -> tonal series at 6*MOTOR_HZ (300 Hz) + harmonics, with torque-
      ripple phase modulation at the rotor rate.
    * Brush noise: broadband "fizz", band-shaped, amplitude-modulated at the
      rotor rotation rate (and weakly at the commutation rate).
    * Belt rumble: low band-passed noise (20-60 Hz), slow AM at the belt
      pass frequency.
    * Once-per-revolution unevenness: gentle AM at the 5 Hz spindle rate.
    * Wow: +/-0.1..0.3 % frequency modulation of every tonal component.

  CASE ACOUSTICS:
    1. Modal resonator bank (parallel constant-peak band-pass biquads) for
       the ABS case + steel subframe — mode table in PARAMS.
    2. Radiation filter: 2nd-order HP ~78 Hz (small plastic box radiates no
       deep bass) + high shelf cut above 6 kHz (panel radiation roll-off).
    3. Early reflections, 2-6 ms ("inside a box sitting on a desk").

  LOOPABILITY: the hum loop is rendered over EXACTLY 8 spindle revolutions
  (1.6 s @ 5 Hz).  Every modulator (wow components, AM, belt AM) completes an
  integer number of cycles in that window, every tonal component lands on an
  FFT bin, and ALL filtering of the loop is done circularly (multiplication
  by the filter frequency response on the rFFT grid == circular convolution),
  with early reflections applied via circular np.roll.  The loop point is
  therefore mathematically seamless; motor.py verifies it numerically and
  prints the proof (also recorded in README-motor.md).

Deterministic: fixed RNG seed, no wall-clock anywhere.

Usage:  python3 tools/drive-sounds/motor.py [output_dir]
        (default output_dir: resources/drive-sounds relative to repo root)
"""

import os
import sys

import numpy as np
from scipy import signal
from scipy.io import wavfile

# =========================================================================
# PARAMS — every knob of the model lives here
# =========================================================================
PARAMS = {
    # --- global -----------------------------------------------------------
    "fs": 44100,                # sample rate [Hz]
    "seed": 20260703,           # RNG seed (deterministic renders)

    # --- rotational state -------------------------------------------------
    "spindle_hz": 5.0,          # 300 RPM, the CPC 3" spec speed
    "pulley_ratio": 10.0,       # belt reduction: motor rotor = 50 Hz
    "commutation_per_rev": 6,   # 3 rotor poles x 2 brushes
    # wow: FM depth of all tones. Sum of sinusoids; freqs are k/LOOP_S so the
    # hum loop stays periodic. (freq [Hz], relative weight)
    "wow_depth": 0.0022,        # ~0.22 % peak — inside the 0.1-0.3 % spec
    "wow_components": [(0.625, 1.0), (1.25, 0.55), (2.5, 0.30)],

    # --- source levels (pre-chain, arbitrary units) ------------------------
    "tone_commutation_amp": 0.30,     # 300 Hz fundamental
    "tone_commutation_harm": [1.0, 0.42, 0.20, 0.09],  # 300/600/900/1200 Hz
    "tone_rotor_amp": 0.11,           # 50 Hz imbalance tone
    "torque_ripple_pm": 0.35,         # rad, PM of commutation series @ rotor rate
    "brush_noise_amp": 0.20,          # broadband fizz
    "brush_band": (850.0, 4300.0),    # band-pass of the fizz [Hz]
    "brush_am_rotor": 0.55,           # AM depth of fizz @ rotor rate (50 Hz)
    "brush_am_comm": 0.22,            # AM depth of fizz @ commutation rate
    "belt_rumble_amp": 1.1,           # pre-chain (heavily HP-attenuated later)
    "belt_band": (20.0, 60.0),        # belt rumble band [Hz]
    "belt_pass_hz": 3.125,            # belt loop pass frequency (5 cyc / loop)
    "belt_am_depth": 0.30,            # AM of rumble at belt pass rate
    "rev_am_depth": 0.07,             # once-per-rev (5 Hz) unevenness, AM
    "rev_am_h2": 0.03,                # its 2nd harmonic (asymmetric rotation)
    "pink_bed_amp": 0.05,             # faint warm LF noise bed (<500 Hz)

    # --- spin-up (MOTOR_START, 500 ms ramp per emulator event) -------------
    "spinup_s": 0.60,           # asset length: 500 ms ramp + settle
    "spinup_tau": 0.22,         # accel time scale [s] (Weibull-shaped ramp)
    "spinup_shape": 1.7,        # >1: slow start under belt load, then catch up
    #   -> speed(t) = 1 - exp(-(t/tau)^shape); speed(0.5 s) = 98.2 %,
    #      speed(0.6 s) = 99.6 % : the glide spans the full 500 ms ramp
    "flutter_hz": 11.0,         # belt-compliance resonance during the ramp
    "flutter_depth": 0.030,     # peak speed deviation (FM), also drives AM
    "flutter_tau": 0.16,        # flutter decay [s]
    "flutter_t0": 0.06,         # belt takes up slack ~60 ms in
    "start_click_amp": 0.55,    # motor energize "tick" into the modal bank
    "start_click_s": 0.006,     # click burst length [s]

    # --- steady hum loop (MOTOR_READY) --------------------------------------
    "loop_revs": 8,             # EXACT number of 5 Hz revolutions in the loop
    #   -> loop length 1.6 s, 70560 samples

    # --- spin-down (MOTOR_STOP) ---------------------------------------------
    "spindown_s": 1.35,         # asset length
    "coast_tau": 0.36,          # spindle/belt coast time constant [s]
    "coast_amp_exp": 1.7,       # mechanical level ~ speed^exp while coasting
    "elec_off_tau": 0.07,       # commutation buzz dies with the current
    "spindown_fade_s": 0.06,    # final safety fade to digital silence

    # --- case acoustics: modal resonator bank ------------------------------
    # (freq [Hz], Q, gain) — ABS top/bottom shell modes cluster low, the
    # steel drive subframe contributes the 300-900 Hz cluster, two weak
    # high modes give the "plasticky" edge.
    "modes": [
        (128.0,  9.0, 1.00),
        (176.0, 14.0, 0.85),
        (243.0, 11.0, 0.95),
        (318.0, 22.0, 0.75),
        (412.0, 15.0, 0.70),
        (534.0, 18.0, 0.55),
        (668.0, 26.0, 0.48),
        (871.0, 20.0, 0.38),
        (1260.0, 15.0, 0.20),
        (1820.0, 13.0, 0.13),
        (2410.0, 12.0, 0.10),
    ],
    "dry_mix": 0.35,            # direct (unresonated) source in the mix
    "modal_mix": 1.00,          # modal-bank output in the mix

    # --- case acoustics: radiation / air ------------------------------------
    "radiation_hp_hz": 78.0,    # 2nd-order Butterworth HP
    "radiation_shelf_hz": 6000.0,
    "radiation_shelf_db": -9.0, # high shelf cut (plastic panel roll-off)
    # early reflections: (delay [s], gain) — desk + case interior
    "reflections": [(0.0021, 0.28), (0.0037, -0.19), (0.0055, 0.12)],
    "reflection_mix": 0.5,

    # --- mastering -----------------------------------------------------------
    "hum_rms_dbfs": -18.0,      # family reference: hum loop RMS
    "peak_ceiling_dbfs": -1.0,  # family-wide true-peak safety ceiling
}

FS = PARAMS["fs"]
MOTOR_HZ = PARAMS["spindle_hz"] * PARAMS["pulley_ratio"]          # 50 Hz
COMM_HZ = MOTOR_HZ * PARAMS["commutation_per_rev"]                # 300 Hz
LOOP_S = PARAMS["loop_revs"] / PARAMS["spindle_hz"]               # 1.6 s
LOOP_N = int(round(LOOP_S * FS))                                  # 70560


# =========================================================================
# Filters (designed once; applied linearly for transients, circularly for
# the loop so the loop stays periodic)
# =========================================================================
def rbj_bandpass(f0, q):
    """RBJ constant-0dB-peak band-pass biquad -> sos row."""
    w0 = 2.0 * np.pi * f0 / FS
    alpha = np.sin(w0) / (2.0 * q)
    b = np.array([alpha, 0.0, -alpha])
    a = np.array([1.0 + alpha, -2.0 * np.cos(w0), 1.0 - alpha])
    return np.hstack([b / a[0], a / a[0]])[None, :]


def rbj_highshelf(f0, gain_db, s=0.7):
    """RBJ high-shelf biquad -> sos row."""
    A = 10.0 ** (gain_db / 40.0)
    w0 = 2.0 * np.pi * f0 / FS
    cw, sw = np.cos(w0), np.sin(w0)
    alpha = sw / 2.0 * np.sqrt((A + 1.0 / A) * (1.0 / s - 1.0) + 2.0)
    two_sqA_al = 2.0 * np.sqrt(A) * alpha
    b0 = A * ((A + 1) + (A - 1) * cw + two_sqA_al)
    b1 = -2 * A * ((A - 1) + (A + 1) * cw)
    b2 = A * ((A + 1) + (A - 1) * cw - two_sqA_al)
    a0 = (A + 1) - (A - 1) * cw + two_sqA_al
    a1 = 2 * ((A - 1) - (A + 1) * cw)
    a2 = (A + 1) - (A - 1) * cw - two_sqA_al
    return np.array([[b0 / a0, b1 / a0, b2 / a0, 1.0, a1 / a0, a2 / a0]])


MODE_SOS = [(rbj_bandpass(f, q), g) for f, q, g in PARAMS["modes"]]
RAD_HP_SOS = signal.butter(2, PARAMS["radiation_hp_hz"], "highpass",
                           fs=FS, output="sos")
RAD_SHELF_SOS = rbj_highshelf(PARAMS["radiation_shelf_hz"],
                              PARAMS["radiation_shelf_db"])
BRUSH_BP_SOS = signal.butter(2, PARAMS["brush_band"], "bandpass",
                             fs=FS, output="sos")
BELT_BP_SOS = signal.butter(2, PARAMS["belt_band"], "bandpass",
                            fs=FS, output="sos")
PINK_LP_SOS = signal.butter(1, 500.0, "lowpass", fs=FS, output="sos")


def sos_response(sos, n):
    """Frequency response of an sos filter on the length-n rFFT grid."""
    freqs = np.fft.rfftfreq(n, 1.0 / FS)
    _, h = signal.sosfreqz(sos, worN=freqs, fs=FS)
    return h


def filt_lin(sos, x):
    return signal.sosfilt(sos, x)


def filt_circ(sos, x):
    return np.fft.irfft(np.fft.rfft(x) * sos_response(sos, len(x)), len(x))


# =========================================================================
# Case-acoustics chain: modal bank -> radiation filter -> early reflections
# =========================================================================
def case_chain(x, circular):
    p = PARAMS
    filt = filt_circ if circular else filt_lin

    modal = np.zeros_like(x)
    for sos, g in MODE_SOS:
        modal += g * filt(sos, x)
    y = p["dry_mix"] * x + p["modal_mix"] * modal

    y = filt(RAD_HP_SOS, y)
    y = filt(RAD_SHELF_SOS, y)

    refl = np.zeros_like(y)
    for d_s, g in p["reflections"]:
        d = int(round(d_s * FS))
        if circular:
            refl += g * np.roll(y, d)
        else:
            shifted = np.zeros_like(y)
            shifted[d:] = y[:-d]
            refl += g * shifted
    return y + p["reflection_mix"] * refl


# =========================================================================
# Source synthesis. `speed` (0..1 array) drives every frequency; separate
# amplitude laws for electrical (commutation) vs mechanical (belt/rotation)
# components let spin-up/down behave physically.
# =========================================================================
def synth_sources(n, speed, elec_amp, mech_amp, wow, noise_bank, circular):
    p = PARAMS
    filt = filt_circ if circular else filt_lin

    # -- rotational phases (phase integration => smooth glides, no steps) --
    f_motor = MOTOR_HZ * speed * (1.0 + wow)
    th_m = 2.0 * np.pi * np.cumsum(f_motor) / FS           # rotor phase
    th_c = p["commutation_per_rev"] * th_m                 # commutation phase
    th_s = th_m / p["pulley_ratio"]                        # spindle phase

    # -- tonal components ---------------------------------------------------
    pm = p["torque_ripple_pm"] * np.sin(th_m)              # torque ripple PM
    tones = np.zeros(n)
    for k, h in enumerate(p["tone_commutation_harm"], start=1):
        tones += h * np.sin(k * (th_c + pm) + 1.7 * k)     # fixed phase offsets
    tones *= p["tone_commutation_amp"] * elec_amp
    tones += p["tone_rotor_amp"] * mech_amp * np.sin(th_m + 0.4)

    # -- brush / commutation fizz -------------------------------------------
    fizz = filt(BRUSH_BP_SOS, noise_bank["brush"])
    am = (1.0
          + p["brush_am_rotor"] * 0.5 * (1.0 + np.cos(th_m))
          + p["brush_am_comm"] * np.cos(th_c))
    fizz *= p["brush_noise_amp"] * elec_amp * am / (1.0 + p["brush_am_rotor"])

    # -- belt rumble -----------------------------------------------------------
    rumble = filt(BELT_BP_SOS, noise_bank["belt"])
    belt_ph = th_s * (p["belt_pass_hz"] / p["spindle_hz"])
    rumble *= p["belt_rumble_amp"] * mech_amp * \
        (1.0 + p["belt_am_depth"] * np.sin(belt_ph + 0.9))

    # -- warm LF bed ------------------------------------------------------------
    bed = p["pink_bed_amp"] * mech_amp * filt(PINK_LP_SOS, noise_bank["bed"])

    src = tones + fizz + rumble + bed

    # -- once-per-revolution unevenness ------------------------------------------
    src *= (1.0 + p["rev_am_depth"] * np.cos(th_s)
            + p["rev_am_h2"] * np.cos(2.0 * th_s + 1.1))
    return src


def make_noise_bank(rng, n):
    return {k: rng.standard_normal(n) for k in ("brush", "belt", "bed")}


def wow_periodic(n):
    """Zero-mean wow that completes integer cycles over the hum loop."""
    p = PARAMS
    t = np.arange(n) / FS
    w = np.zeros(n)
    for i, (f, a) in enumerate(p["wow_components"]):
        w += a * np.sin(2.0 * np.pi * f * t + 2.399 * (i + 1))
    return p["wow_depth"] * w / np.sum([a for _, a in p["wow_components"]])


def wow_random(rng, n):
    """Non-periodic wow for the transient assets (LP-filtered noise)."""
    sos = signal.butter(2, 3.0, "lowpass", fs=FS, output="sos")
    w = signal.sosfilt(sos, rng.standard_normal(n))
    w /= max(np.max(np.abs(w)), 1e-12)
    return PARAMS["wow_depth"] * w


# =========================================================================
# Asset renderers
# =========================================================================
def render_hum_loop(rng):
    n = LOOP_N
    speed = np.ones(n)
    wow = wow_periodic(n)
    src = synth_sources(n, speed, np.ones(n), np.ones(n), wow,
                        make_noise_bank(rng, n), circular=True)
    return case_chain(src, circular=True)


def render_spinup(rng):
    p = PARAMS
    n = int(round(p["spinup_s"] * FS))
    t = np.arange(n) / FS

    speed = 1.0 - np.exp(-(t / p["spinup_tau"]) ** p["spinup_shape"])
    # belt-compliance flutter once the belt takes up slack
    tf = t - p["flutter_t0"]
    flutter = np.where(
        tf > 0.0,
        p["flutter_depth"] * np.exp(-np.maximum(tf, 0.0) / p["flutter_tau"])
        * np.sin(2.0 * np.pi * p["flutter_hz"] * np.maximum(tf, 0.0)),
        0.0)
    speed_f = np.clip(speed * (1.0 + flutter), 0.0, None)

    elec = (0.35 + 0.65 * speed ** 1.2) * (1.0 + 1.5 * flutter)
    mech = speed ** 2 * (1.0 + 2.0 * flutter)

    wow = wow_random(rng, n)
    src = synth_sources(n, speed_f, elec, mech, wow,
                        make_noise_bank(rng, n), circular=False)

    # motor energize "tick": short damped burst kicking the modal bank
    click_n = int(p["start_click_s"] * FS)
    click = np.zeros(n)
    burst = rng.standard_normal(click_n) * np.exp(-np.arange(click_n) / (click_n / 4.0))
    click[:click_n] = p["start_click_amp"] * burst
    click[0] += p["start_click_amp"]            # DC step edge (solenoid-ish)
    src += click

    y = case_chain(src, circular=False)
    fade_n = int(0.004 * FS)                    # de-click the very start
    y[:fade_n] *= np.linspace(0.0, 1.0, fade_n)
    return y


def render_spindown(rng):
    p = PARAMS
    n = int(round(p["spindown_s"] * FS))
    t = np.arange(n) / FS

    speed = np.exp(-t / p["coast_tau"])         # belt/spindle coast
    elec_gate = np.exp(-t / p["elec_off_tau"])  # current dies quickly
    elec = np.maximum(elec_gate, 0.25 * speed ** 2)  # brushes still slide
    mech = speed ** p["coast_amp_exp"]

    wow = wow_random(rng, n)
    src = synth_sources(n, speed, elec, mech, wow,
                        make_noise_bank(rng, n), circular=False)
    y = case_chain(src, circular=False)

    fade_n = int(p["spindown_fade_s"] * FS)     # guarantee digital silence
    y[-fade_n:] *= np.linspace(1.0, 0.0, fade_n) ** 2
    return y


# =========================================================================
# Mastering, verification, output
# =========================================================================
def dbfs(x):
    return 20.0 * np.log10(max(np.max(np.abs(x)), 1e-12))


def rms_dbfs(x):
    return 20.0 * np.log10(max(np.sqrt(np.mean(x ** 2)), 1e-12))


def verify_loop(x):
    """Numeric loop-continuity proof for the hum loop. Returns report dict."""
    d = np.diff(x)
    wrap_step = abs(x[0] - x[-1])
    report = {
        "first_sample": float(x[0]),
        "last_sample": float(x[-1]),
        "wrap_step": float(wrap_step),
        "max_abs_step_inside": float(np.max(np.abs(d))),
        "rms_step_inside": float(np.sqrt(np.mean(d ** 2))),
    }
    # spectral continuity: compare Welch spectra of the loop's last 200 ms,
    # first 200 ms, and a segment wrapped across the boundary.
    seg = int(0.2 * FS)
    head, tail = x[:seg], x[-seg:]
    wrapped = np.concatenate([x[-seg // 2:], x[:seg // 2]])
    f, ph = signal.welch(head, FS, nperseg=2048)
    _, pt = signal.welch(tail, FS, nperseg=2048)
    _, pw = signal.welch(wrapped, FS, nperseg=2048)
    band = (f >= 30) & (f <= 6000)
    smooth = np.ones(9) / 9.0          # ~190 Hz smoothing: compare envelopes,
    lh, lt, lw = (                     # not per-bin noise variance
        10 * np.log10(np.convolve(p, smooth, "same")[band] + 1e-20)
        for p in (ph, pt, pw))
    report["spec_maxdev_head_tail_db"] = float(np.max(np.abs(lh - lt)))
    report["spec_maxdev_wrap_db"] = float(np.max(np.abs(lw - 0.5 * (lh + lt))))
    # discontinuity spike test: |diff| around the wrap point of x tiled twice
    tiled = np.concatenate([x, x])
    dt = np.abs(np.diff(tiled))
    around = dt[len(x) - 100:len(x) + 100]
    report["wrap_region_max_step"] = float(np.max(around))
    report["wrap_ok"] = bool(np.max(around) <= report["max_abs_step_inside"])
    return report


def write_wav(path, x):
    q = np.clip(np.round(x * 32767.0), -32768, 32767).astype(np.int16)
    wavfile.write(path, FS, q)


def save_spectrogram(path, x, title):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    f, t, s = signal.spectrogram(x, FS, window="hann", nperseg=2048,
                                 noverlap=2048 - 128, mode="psd")
    sdb = 10.0 * np.log10(s + 1e-14)
    vmax = np.max(sdb)
    fig, ax = plt.subplots(figsize=(9, 4.5), dpi=110)
    im = ax.pcolormesh(t, f, sdb, vmin=vmax - 85.0, vmax=vmax,
                       cmap="magma", shading="gouraud")
    ax.set_ylim(0, 5000)
    ax.set_yscale("symlog", linthresh=200.0)
    ax.set_yticks([50, 100, 200, 300, 500, 1000, 2000, 5000])
    ax.set_yticklabels(["50", "100", "200", "300", "500", "1k", "2k", "5k"])
    ax.set_xlabel("time [s]")
    ax.set_ylabel("frequency [Hz]")
    ax.set_title(title)
    fig.colorbar(im, ax=ax, label="dB")
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "..", "..", "resources", "drive-sounds")
    out_dir = os.path.abspath(out_dir)
    os.makedirs(out_dir, exist_ok=True)

    rng = np.random.default_rng(PARAMS["seed"])
    hum = render_hum_loop(rng)
    spinup = render_spinup(rng)
    spindown = render_spindown(rng)

    # family normalization: hum loop -> reference RMS, same gain everywhere
    gain = 10.0 ** (PARAMS["hum_rms_dbfs"] / 20.0) / np.sqrt(np.mean(hum ** 2))
    family = {"motor_hum_loop": hum * gain,
              "motor_spinup": spinup * gain,
              "motor_spindown": spindown * gain}
    peak = max(np.max(np.abs(x)) for x in family.values())
    ceiling = 10.0 ** (PARAMS["peak_ceiling_dbfs"] / 20.0)
    if peak > ceiling:                     # keep relative loudness physical:
        trim = ceiling / peak              # trim the WHOLE family together
        family = {k: v * trim for k, v in family.items()}
        print(f"note: family trimmed by {20*np.log10(trim):+.2f} dB for headroom")

    print(f"loop: {LOOP_N} samples = {LOOP_S:.3f} s "
          f"= {PARAMS['loop_revs']} revolutions @ {PARAMS['spindle_hz']} Hz")
    for name, x in family.items():
        write_wav(os.path.join(out_dir, f"{name}.wav"), x)
        save_spectrogram(os.path.join(out_dir, f"{name}.png"), x,
                         f"{name} — {len(x)/FS:.2f} s, "
                         f"RMS {rms_dbfs(x):.1f} dBFS, peak {dbfs(x):.1f} dBFS")
        print(f"{name:18s} len={len(x)/FS:5.3f}s "
              f"rms={rms_dbfs(x):7.2f} dBFS peak={dbfs(x):6.2f} dBFS")

    rep = verify_loop(family["motor_hum_loop"])
    print("loop continuity proof:")
    for k, v in rep.items():
        print(f"  {k}: {v}")
    if not rep["wrap_ok"]:
        raise SystemExit("LOOP CHECK FAILED: wrap-point discontinuity detected")
    print("loop check PASSED")


if __name__ == "__main__":
    main()
