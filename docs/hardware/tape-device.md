# Tape deck — CDT/TZX playback + live line-in as a bus Device

Language-neutral spec for the **cassette deck** (`src/hw/tape`). Companion to
`ppi-device.md` (the other end of the wires). The legacy `tape.cpp` state
machine is the baseline oracle; feature parity per the replacement ledger.

## 1. The wires (TapeBus)

The deck touches nothing but the three cassette wires: it drives `tape.rdata`
(the POST-SCHMITT read level — the CPC's input conditioning circuit squares
the analog signal on the mainboard) and reads `tape.motor` (PPI Port C bit 4,
the relay the firmware switches). `tape.wdata` (PC5) is reserved for the
save path. Because `rdata` carries plain levels, a CDT decoder and a LIVE
SOURCE (microphone + host-side Schmitt stage → a real tape deck) are
interchangeable drivers of the same wire.

## 2. Time base

CDT/TZX durations are Z80-Spectrum T-states at 3.5 MHz. Against the 16 MHz
master clock: 3.5/16 = **7/32 exactly** — each pulse is loaded as
`t_states × 32` sub-units and the deck consumes **7 sub-units per master
cycle**, so long runs accumulate zero drift. Pauses are milliseconds
(× 16 000 cycles). The deck advances only while `motor && play` — the
firmware owns the relay, the user owns the PLAY button, exactly like the
legacy `tape_motor && tape_play_button` gate.

## 3. CDT block state machine (V1)

Header `ZXTape!\x1A` + major/minor. Blocks: **0x10** standard speed (pilot
2168 ts × 8063 pulses for header flags < 0x80, 3223 otherwise; sync 667/735;
bits as pulse PAIRS 855/855 (0) and 1710/1710 (1), MSB first, `used bits` in
the last byte; then pause ms), **0x11** turbo (explicit timings from the
block), **0x12** pure tone, **0x13** pulse sequence, **0x14** pure data,
**0x20** pause (0 = stop the tape). Metadata blocks (0x21/0x22/0x30/0x31/
0x32/0x33) are skipped by their length rules; an unknown block stops playback
and latches an error flag in the peek (never guess a length). The level
TOGGLES at each pulse boundary; playback ends with PLAY released.

## 4. Live line-in

`tape_line_mode(on)` detaches the CDT timeline from `rdata` and follows the
host-fed `tape_line_level()` instead — the digital output of the host's
Schmitt stage over microphone/line-in samples (hysteresis thresholds live
host-side, mirroring the mainboard's conditioning stage; the Device stays
sample-rate-free). Motor gating still applies: the firmware "hears" silence
when the relay is off, exactly like a real deck wired to the DIN socket.

## 5. Host API

`tape_state_size/init/peek` (TapeRegs: attached/playing/motor/level/
line_mode/error, block ordinal, byte position — enough for a tape counter
UI); `tape_attach_cdt` (caller-owned buffer, live wiring, validates header),
`tape_eject`, `tape_play(on)`, `tape_rewind`, `tape_line_mode`,
`tape_line_level`. Serialization covers the playback cursor (block/offset/
phase/pulse remainder) — NOT the media, per the Device contract.

## 6. Verified against CPCWiki ("Cassette data information")

- Read = **PPI Port B bit 7**; write = **Port C bit 5** — confirming this
  Device's wiring and the PC4-motor/PC5-write correction to the PPI.
- "The hardware tape data separator inside the CPC only extracts **1 bit**"
  and "it is **not known exactly** how the amplitude maps to the 0/1
  measurement" — the post-Schmitt single-level `rdata` wire is the right
  abstraction, and the host-side threshold/hysteresis MUST stay configurable
  (there is no canonical analog transfer to hard-code).
- Standard routines peak at **~2500 Hz**; 44.1 kHz line-in is ample (8 kHz
  would already suffice).
- Loaders time **level transitions** by polling PB7 — matching the deck's
  exact edge-timed output.
- The CPC's write output is **unamplified** on real hardware; our jack
  rendering is already line-level, so the DIY cable feeds AUX/LINE inputs
  directly (MIC inputs still need the pad, tape-remote-cable.md §4).

## 7. The firmware acid test (parity proof)

`test/hw/tape_acid_test.cpp` synthesizes a CDT in the firmware's own
cassette format — header record (sync `&2C`) + data record (sync `&16`),
256-byte segments each followed by a complemented CRC-16 (`X^16+X^12+X^5+1`,
preset `&FFFF`, high byte first), 2048-one-bit leader, one zero start bit,
32-one-bit trailer, SPEED WRITE 1 timings (half-a-zero 583 CDT T-states) —
and proves the whole chain on the sub-cycle machine: `RUN"` typed on the
matrix, "Press PLAY then any key", motor relay through the PPI, CAS READ
calibrating on the leader, both CRCs verified, BASIC tokenizing the loaded
ASCII listing, and the program's own `PRINT` arriving through `&BB5A`.

Two lessons the test taught, now part of this spec:

- **Blank-tape lead-in is mandatory.** CAS MOTOR ON (6128 OS, `&2BE2`) waits
  ~2 s for the tape to reach speed before the firmware reads OR writes —
  a real SAVE therefore leaves ~2 s of silence before its first leader, and
  a synthesized firmware CDT must model it (an initial `0x20` pause block;
  the test uses 3000 ms). Without it the deck faithfully plays the leader
  into the spin-up wait and the reader syncs on the WRONG record: the
  observed signature is the byte reader assembling a single `&16`.
- **Firmware ROM messages bypass `&BB5A`.** "Press PLAY then any key" and
  "Loading X block N" are printed by the lower ROM through its internal
  TXT routine — a jumpblock tap only sees output that goes through the RAM
  vector (BASIC's own PRINTs, the boot banner). Debug harnesses must not
  expect cassette-manager text on the tap.

## Batch contract (RunTier::Fast)

- **Playing**: the pulse timeline is pure countdown arithmetic
  (`sub_remaining` per master cycle, PAUSE counters, block advance) —
  batchable exactly from transition to transition; rdata level changes are
  timestamped events consumed by PPI port-B reads (post-catch-up).
- **Motor**: an event from the PPI's port-C writes; play/rewind/eject are
  host events (frame-boundary or forced sync).
- **Line-in mode**: host-fed levels arrive at the feed rate — the deck
  advances per queued level transition (event-per-transition, not per
  sample), preserving one-sample timing.
- **Idle deck**: no deadlines, no events — fully dormant (the wake tier's
  tape_live gating already proves the contract).
- Bestiary audit: class (b) — counters are exact countdowns; motor_seen is
  an event-sampled latch (no per-cycle freshness needed at event
  granularity).
