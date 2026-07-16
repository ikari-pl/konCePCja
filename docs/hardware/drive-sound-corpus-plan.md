# Drive-sound recording corpus — test program + capture plan

Plan for recording REAL CPC 6128 drive sounds as a reusable, labeled corpus:
a deterministic CPC-side test program exercises the mechanics section by
section, a microphone captures the audio, and — the key idea — **the same
program runs in the simulator first**, where the FDC's mechanical event ring
(`fdc_drain_events`, [`fdc-device.md`](fdc-device.md) §7b) emits the exact
event timeline as machine-readable labels. One deterministic script, two
executions: the sim yields ground truth, the hardware yields audio, and
alignment marries them into a training set.

The corpus must serve **every plausible final sound-maker**, in increasing
order of sophistication:

| Consumer | What it needs from the corpus |
|---|---|
| **A. Parameter fitting** for the existing physical models (`tools/drive-sounds/*.py` PARAMS blocks) | clean isolated one-shots; long steady hum for modal analysis (case resonance freq/Q extraction); spin-up/down ramps |
| **B. Direct sample playback** (replace the synthetic WAVs) | de-noised isolated events; seamless-loopable hum; enough take variation to avoid machine-gun repetition |
| **C. Granular / wavetable hybrid** | densely varied click populations; hum textures at several durations |
| **D. Learned synthesis** (DDSP-style or similar) | minutes of *labeled, varied* material incl. combined/overlapping events; consistent mic chain; correct silences |

Design rule: **record for D, and A–C fall out** — the most demanding consumer
defines the coverage matrix.

---

## 1. The test program (`DRVSND.BAS` + a poked helper routine)

A BASIC program (with a small machine-code helper POKEd into memory for the
FDC command handshake — BASIC's `OUT`/`INP` is fast enough for the motor
latch and for slating, but the data-register MSR handshake is cleaner in
assembler). It drives every section below, **slating each section and each
repetition with PSG beep patterns** — the beeps serve three jobs at once:
an audible label for a human editor, a sync anchor for automatic alignment
(the PSG also sounds in the sim's capture), and a section ID (beep count =
section number, double-beep = repetition boundary).

Determinism requirements:
- Fixed delays via frame-flyback counting (`CALL &BD19` loops), never
  `EVERY`/`AFTER` (those interact with the AMSDOS ticker).
- **Neutralize AMSDOS's motor-off ticker** for the sections that own the
  motor: drive the latch directly (`OUT &FA7E,1` / `,0`) and keep AMSDOS from
  fighting it (implementation note: either patch the firmware motor tick
  counts for the session or simply re-assert the latch each iteration; to be
  settled when the program is written — flagged as the program's main risk).
- All FDC commands issued through the helper: SPECIFY (explicit SRT per
  section), SEEK, RECALIBRATE, READ ID — no reliance on AMSDOS's own
  parameters.

### Section script (v1)

| § | Slate | Contents | Repeats | Why |
|---|---|---|---|---|
| S0 | — | *Protocol, not program*: 30 s room tone, machine OFF; then 30 s machine ON, drive idle | 1 | noise profiles for cleanup (two: room, machine-baseline) |
| S1 | 1 beep | Motor ON → hold 60 s → OFF, 10 s gap | ×5 | spin-up ramps, long hum (loop + modal analysis), spin-downs; 5 takes for variation |
| S2 | 2 beeps | Single steps, inward: SEEK +1, 2 s silence between | ×30 | the click population (amplitude/spectral variation statistics) |
| S3 | 3 beeps | Single steps, outward: SEEK −1, 2 s silence | ×30 | direction asymmetry + backlash (first-step-after-reversal is louder) |
| S4 | 4 beeps | Step trains at SRT=0xA (12 ms): seeks of 2, 4, 8, 16, 39 tracks, 2 s gaps | ×5 each | train fusion into the buzz; how overlap rings the case |
| S5 | 5 beeps | SRT sweep: 5-track seeks at SRT = 0x0..0xE (32 ms down to 4 ms) | ×3 each | repetition-rate dependence — gold for consumers A and D |
| S6 | 6 beeps | RECALIBRATE from track 39 | ×10 | the canonical "brrrp" |
| S7 | 7 beeps | Realistic composites: `CAT`, then LOAD of a known file, motor allowed to auto-stop | ×5 | overlapping events + AMSDOS's real motor management, as validation material |
| S8 | 8 beeps | Motor ON, NO seeks, 120 s | ×1 | pristine hum: loop extraction, index-tick hunting (5 Hz), wow measurement |
| S9 | 9 beeps | *Human foley, machine prompts via beeps*: eject, insert, eject-with-flip-insert; 5 s gaps | ×10 each | the foley set; the program only paces and slates |

Total runtime ≈ 25–30 minutes. The program prints each section name on screen
too (the video recording of a phone doubles as a backup log).

### The simulator twin

The same command sequence runs headless first:
`koncepcja_sim_headless --disk corpus.dsk --type 'run"drvsnd' ...` — with a
planned small tool (`tools/drive-sounds/labels_from_sim.py`, NOT yet written)
that runs the sim, drains `fdc_drain_events`, and emits `labels.json`:
`[{t_rel_s, type, arg, section, rep}, ...]`, timestamps relative to the first
slate beep. Alignment of the hardware recording: cross-correlate the slate
beeps (the PSG tone is spectrally unmistakable against drive noise), then a
single global offset + drift term (crystal tolerances ≈ 10⁻⁴ — one linear
fit) maps every simulated event timestamp onto the audio. Result: **every
click in the recording carries an exact label without hand-editing.**

Honesty note: sim event times assume our modeled mechanics (500 ms spin-up,
SRT stepping). S1's MOTOR_READY label is a *model* time — for training, treat
motor-phase boundaries from the sim as approximate and refine from the audio
envelope; STEP times are exact by construction (the FDC issues them).

---

## 2. Recording protocol

- **Format**: 96 kHz / 24-bit WAV (mode analysis benefits from headroom and
  bandwidth; downsample later, never up). Mono per mic.
- **Mics / positions** (as many as available, simultaneously if possible):
  P1 close-mech (~10 cm above the drive slot), P2 case-right (~40 cm, the
  listener-ish perspective the assets model), P3 ambient (~1 m, user
  position). P2 is the primary; P1 feeds parameter extraction; P3 gives the
  room contribution.
- **Gain staging**: set so the LOUDEST event (recalibrate buzz / eject clack)
  peaks ≤ −12 dBFS; never touch gain mid-session (re-record the session if
  clipped).
- **Room**: quiet as available; what matters more is the S0 room-tone
  profile and consistency within a session. No music/fans/HVAC.
- **Multiple machines/mechs when possible**: belt condition dominates 3"
  mech character. Each machine = its own session directory.
- Phone video of the screen alongside = timestamped visual log (free).

## 3. Corpus layout, metadata, licensing

```
corpus/drive-sounds/                  (separate repo or LFS — raw is GBs)
  <machine-id>/<session-date>/
    raw/  p1.wav p2.wav p3.wav        (untouched captures)
    labels.json                        (from the sim twin, post-alignment)
    session.json                       (machine, mech model+serial, belt notes,
                                        mic models, positions cm, preamp, gain,
                                        room, temperature, program version)
    video.mp4                          (optional)
  processed/ ...                       (cleanup outputs, §4 — regenerable)
```

License the recordings **CC0** (they're the user's own captures; CC0 keeps
every downstream use — including model training — friction-free). The repo
keeps only `session.json` + `labels.json` + small processed excerpts;
raw audio lives in the corpus store.

## 4. Cleanup pipeline (plan)

1. **Noise profile** from S0 (both profiles); light spectral subtraction on
   the hum sections ONLY — **never de-noise the transients** (de-noisers eat
   exactly the click attacks we care about). Clicks are cleaned by *gating
   between events* instead (labels make the gates trivial).
2. **Segmentation by labels**: cut one-shots with fixed pre/post-roll
   (10 ms / 200 ms for clicks, 500 ms for foley), fades only in silence.
3. **Loop extraction** from S8: pick an integer number of 5 Hz revolutions
   with the loop-continuity checks the synthetic assets already document
   (`README-motor.md`).
4. **Normalization policy**: preserve RELATIVE levels within a session
   (one gain per session, derived from the hum RMS), because inter-event
   loudness ratios are physical data.
5. **Parameter extraction** (consumer A): spectral-peak fitting on the hum
   (mode table: freq/Q/level → compare against the modal banks in
   `tools/drive-sounds/*.py`), attack/decay fits on click populations,
   spin-up ramp profile fit (against the Weibull the motor model uses).

## 5. Coverage / adequacy checklist (what "a good training set" means)

- [ ] ≥ 60 isolated steps (30 in, 30 out) per machine — variation statistics
- [ ] step trains at ≥ 5 lengths and ≥ 15 SRT rates — repetition-rate axis
- [ ] ≥ 5 full motor cycles + one ≥ 120 s hum — loops, ramps, modal analysis
- [ ] ≥ 10 recalibrates — the signature buzz, take variation
- [ ] ≥ 10 of each foley gesture
- [ ] composites (S7) held out as VALIDATION, never training material
- [ ] every event labeled via the sim twin; label spot-check on 20 random events
- [ ] room tone + machine baseline captured before anything else

## 6. Deliverables & sequencing

1. `DRVSND.BAS` + helper (authored and first *verified in the simulator* —
   the program is correct when the sim's event log matches the section
   script exactly). Ships on a DSK in the repo.
2. `tools/drive-sounds/labels_from_sim.py` (the twin runner + aligner).
3. One pilot session (any machine) → shake out the protocol, check S9
   prompting pace, verify alignment quality.
4. Full sessions per available machine.
5. Cleanup pipeline pass → `processed/`, parameter-extraction report
   comparing fitted modes against the synthetic models' PARAMS.

Risks: AMSDOS motor-ticker interference (S1/S8 — the program's one tricky
bit); mains hum contaminating the 100–300 Hz modes (record away from the
CRT if possible, note it in session.json); 40-year-old belts making "the"
drive sound a distribution, not a point — which is precisely why machines
are recorded separately and metadata is mandatory.
