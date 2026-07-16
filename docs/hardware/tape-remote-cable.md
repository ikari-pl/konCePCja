# The tape-deck cable — audio + motor REMOTE over the headphone jack

Companion to [`tape-device.md`](tape-device.md) (§4 line-in) and bead
`beads-p84f`. This is the **host-world** side of the tape feature: a DIY
cable that connects a real cassette deck to the emulated CPC — data both
ways plus **motor start/stop**, re-creating the CPC 6128's cassette DIN
socket (which carried exactly these lines: read, write, ground, REMOTE)
on a modern computer's 3.5 mm jacks.

Everything here is above the simulation layer: the Device sees only wire
levels; channel choices and the carrier scheme are host configuration.

## 1. The three connections

| # | Host side | Deck side | Carries |
|---|---|---|---|
| A | headphone jack, **left** (tip) | AUX/MIC IN | data OUT of the CPC (saving, or replaying a CDT onto real tape) |
| B | mic/line-in ← | EAR / LINE OUT | data INTO the CPC (`tape.rdata` after the host Schmitt stage; default source channel: **left**, never mix — out-of-phase stereo dubs cancel under summing) |
| C | headphone jack, **right** (ring) | REMOTE (2.5 mm TS sub-mini) | motor control: the emulator's relay (`tape.motor`, PPI PC4) as a **~20 kHz carrier** |

The CPC interface is **mono** — one data line each way. Keep data on a single
channel at full scale; never split it across both.

## 2. Why a carrier, not a DC level

Headphone outputs are AC-coupled: a steady "on" voltage will not pass the
jack. So the host emits a ~20 kHz sine on the control channel **while the
relay is closed** and silence when open; the cable rectifies presence-of-tone
into a switch closure. Bonus: 20 kHz is inaudible if it ever bleeds.

## 3. The remote circuit (no external power, polarity-safe)

A voltage doubler feeding a **PhotoMOS solid-state relay** (e.g. TLP222A /
AQY212). The deck's REMOTE jack interrupts the motor's supply — real motor
current flows through the plug — so the switching element must be a real
switch, not an optocoupler's bare phototransistor. A PhotoMOS output is
isolated, polarity-agnostic, and rated well above a cassette motor.

```
 ring (R, carrier) ──┤10 µF├──┬────────────►|──┬──── 100 Ω ──► PhotoMOS LED +
                              │   BAT85 (D2)   │
                          D1 ▼|               ═╪═ 10 µF
                     BAT85    │                │
 sleeve (GND) ────────────────┴────────────────┴───────────────► PhotoMOS LED −

 PhotoMOS output (2 pins, no polarity) ──► 2.5 mm TS plug ──► deck REMOTE
```

- D1, D2: **BAT85/BAT54 Schottky** (low forward drop — a 1N4148 wastes too
  much of a ~1 Vrms headphone signal).
- The doubler yields ≈2× peak ≈ 2.5 V at full volume: enough for the PhotoMOS
  LED (~1.3 V, 3 mA) through 100 Ω. **Run the control channel at maximum
  volume**; if your jack is unusually weak, substitute a USB-powered
  optocoupler stage.

## 4. Levels on the data legs

- **Leg A into a MIC input**: attenuate ~40 dB (47 kΩ series + 470 Ω to
  ground at the deck end). Into **AUX/LINE IN**: direct.
- **Leg B into the host mic port**: start with the deck volume low; the
  host-side Schmitt thresholds are configurable.

## 5. Bench checklist

1. Meter in continuity mode across the 2.5 mm plug; toggle the emulator's
   tape motor (e.g. `cat` from tape) — the beep must follow the relay.
2. Verify tip vs ring assignment with the meter before first connection —
   TRS conventions: tip = left, ring = right, sleeve = ground.
3. **Verify on your deck** (marked, not assumed): REMOTE jack polarity and
   motor current draw — measure before trusting any switch rating.
4. First data test: replay a known CDT out of leg A onto a real tape, then
   play it back through leg B — the emulator should load its own recording.

## 6. Bill of materials (~$8)

3.5 mm TRS plug ×2 · 2.5 mm TS plug · TLP222A/AQY212 PhotoMOS · 2× BAT85 ·
2× 10 µF (16 V) · 100 Ω · 47 kΩ + 470 Ω (MIC pad, if needed) · heat-shrink,
screened cable.

## 7. Cable v2: self-identification (planned)

One extra part makes the cable DETECTABLE, so the emulator never risks a
full-level carrier into human ears: a ~100 kΩ resistor + small cap coupling
a whisper of the control channel into the mic leg. The emulator then does a
challenge–response (a brief low-level 19–21 kHz chirp on the ring, matched
echo expected on the input) before unlocking full output level. Without the
handshake the host stays in opt-in/soft-ramp mode and auto-mutes on any
output-device change; the recommended setup remains a DEDICATED audio device
(cheap USB dongle) for tape I/O so the main output never carries tape
signals at all.

*Unverified until the bench says so: your deck's REMOTE polarity/current, and
the exact 6128 DIN pin numbering (check CPCWiki before citing it anywhere).*
