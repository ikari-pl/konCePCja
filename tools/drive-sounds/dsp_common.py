"""dsp_common.py — shared DSP building blocks for konCePCja drive-sound synthesis.

Everything here is deterministic given an explicit numpy Generator seed.
Sample rate is fixed at 44100 Hz; output helpers write 16-bit PCM WAVs.

The centrepiece is the *case-acoustics chain*: a physically-motivated model of
how a sound source inside an Amstrad CPC 6128 (3" drive bolted to a metal
subframe inside an ABS enclosure) reaches the listener's ear:

    source -> modal resonator bank (panel/subframe modes, ~120-900 Hz, Q 8-30,
              plus a few weak high modes)
           -> early reflections (2-6 ms, interior of the case)
           -> radiation filter (HP ~80 Hz: small panels can't radiate lows;
              soft LP ~6 kHz: ABS panel damping kills the top octave)
"""

import numpy as np
from scipy import signal
from scipy.io import wavfile

SR = 44100


# --------------------------------------------------------------------------
# small utilities
# --------------------------------------------------------------------------

def seconds(n):
    """Number of samples for n seconds."""
    return int(round(n * SR))


def env_exp(n, tau):
    """Exponential decay envelope, n samples, time constant tau seconds."""
    t = np.arange(n) / SR
    return np.exp(-t / tau)


def env_attack_decay(n, attack, tau):
    """Fast attack (raised cosine over `attack` s) then exponential decay."""
    t = np.arange(n) / SR
    env = np.exp(-np.maximum(t - attack, 0.0) / tau)
    a = int(max(1, seconds(attack)))
    ramp = 0.5 - 0.5 * np.cos(np.pi * np.arange(min(a, n)) / a)
    env[:len(ramp)] *= ramp
    return env


def normalize(x, peak=0.95):
    m = np.max(np.abs(x))
    if m > 0:
        x = x * (peak / m)
    return x


def write_wav(path, x, peak=None):
    """Write mono or stereo float array as 16-bit PCM. peak=None keeps levels."""
    if peak is not None:
        x = normalize(x, peak)
    x = np.clip(x, -1.0, 1.0)
    wavfile.write(path, SR, (x * 32767.0).astype(np.int16))


# --------------------------------------------------------------------------
# resonators
# --------------------------------------------------------------------------

def resonator(x, freq, q, gain=1.0):
    """Two-pole resonator (constant peak-gain bandpass), applied to x."""
    w0 = 2.0 * np.pi * freq / SR
    r = np.exp(-w0 / (2.0 * q))
    # poles at r * e^{+-jw}; normalise so peak gain ~= 1, then scale.
    b = [1.0 - r]
    a = [1.0, -2.0 * r * np.cos(w0), r * r]
    return gain * signal.lfilter(b, a, x)


def modal_bank(x, modes):
    """Sum of resonators. modes = list of (freq_hz, q, gain)."""
    y = np.zeros_like(x)
    for f, q, g in modes:
        y += resonator(x, f, q, g)
    return y


# --------------------------------------------------------------------------
# case-acoustics chain
# --------------------------------------------------------------------------

# Default mode set for the CPC 6128 enclosure.  Low/mid cluster: ABS top/bottom
# panel modes and the metal drive subframe; sparse weak highs: small-panel
# breakup modes.  Frequencies deliberately inharmonic.
CASE_MODES = [
    (137.0, 14.0, 1.00),   # bottom panel fundamental
    (183.0, 11.0, 0.85),   # top panel fundamental
    (241.0, 18.0, 0.70),   # panel (1,1)
    (312.0, 22.0, 0.55),   # subframe plate mode
    (429.0, 16.0, 0.45),
    (548.0, 25.0, 0.35),
    (677.0, 20.0, 0.28),
    (866.0, 28.0, 0.22),   # top of the strong cluster
    (1490.0, 12.0, 0.06),  # weak highs: local panel breakup
    (2660.0, 10.0, 0.035),
    (4300.0, 8.0, 0.02),
]


def early_reflections(x, rng, n_taps=5, t_min=0.002, t_max=0.006,
                      gain0=0.5, lp=5000.0):
    """A handful of 2-6 ms reflections off the case interior, mildly lowpassed."""
    delays = np.sort(rng.uniform(t_min, t_max, n_taps))
    gains = gain0 * (0.7 ** np.arange(n_taps)) * rng.uniform(0.8, 1.2, n_taps)
    signs = rng.choice([-1.0, 1.0], n_taps)
    y = x.copy()
    b, a = signal.butter(1, lp / (SR / 2), 'low')
    wet = signal.lfilter(b, a, x)
    for d, g, s in zip(delays, gains, signs):
        k = seconds(d)
        y[k:] += s * g * wet[:len(x) - k]
    return y


def radiation_filter(x, hp=80.0, lp=6000.0):
    """What escapes the box: HP (small panels radiate no lows), soft LP (ABS)."""
    bh, ah = signal.butter(2, hp / (SR / 2), 'high')
    bl, al = signal.butter(2, lp / (SR / 2), 'low')
    return signal.lfilter(bl, al, signal.lfilter(bh, ah, x))


def case_chain(x, rng, modes=None, modal_mix=0.6, dry_mix=0.5,
               refl_gain=0.5, hp=80.0, lp=6000.0):
    """Full case-acoustics chain.

    modal_mix : how much the case modes "ring" (structure-borne path)
    dry_mix   : direct/airborne component that leaks through vents & seams
    """
    if modes is None:
        modes = CASE_MODES
    body = modal_bank(x, modes)
    mixed = dry_mix * x + modal_mix * body
    mixed = early_reflections(mixed, rng, gain0=refl_gain)
    return radiation_filter(mixed, hp=hp, lp=lp)


# --------------------------------------------------------------------------
# analysis plots
# --------------------------------------------------------------------------

def save_spectrogram(path, x, title, fmax=8000):
    """Waveform + spectrogram figure for QA."""
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt

    if x.ndim > 1:
        x = x.mean(axis=1)
    t = np.arange(len(x)) / SR
    fig, (ax1, ax2) = plt.subplots(
        2, 1, figsize=(9, 6), sharex=True,
        gridspec_kw={'height_ratios': [1, 2.4]})
    ax1.plot(t, x, lw=0.4, color='#2060a0')
    ax1.set_ylabel('amplitude')
    ax1.set_title(title)
    nseg = min(1024, max(256, len(x) // 8))
    f, tt, S = signal.spectrogram(x, SR, nperseg=nseg,
                                  noverlap=int(nseg * 0.85))
    S = 10 * np.log10(S + 1e-12)
    im = ax2.pcolormesh(tt, f, S, shading='gouraud', cmap='magma',
                        vmin=S.max() - 90, vmax=S.max())
    ax2.set_ylim(0, fmax)
    ax2.set_ylabel('frequency [Hz]')
    ax2.set_xlabel('time [s]')
    fig.colorbar(im, ax=ax2, label='dB')
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)
