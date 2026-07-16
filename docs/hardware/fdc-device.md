# FDC µPD765A — sub-cycle, pin-level Device simulation reference

Language-neutral spec for the sub-cycle **µPD765A floppy disc controller** Device
(`src/hw/fdc`), the CPC's disc interface (built into the 664/6128, the DDI-1 box
on a 464). For real-hardware background see
[`floppy-disc-system.md`](floppy-disc-system.md); **this** doc is the spec the
Device code follows. Companion to `ppi-device.md`, `memory-device.md`. See also
`docs/hw-spec.md` (the system bus / Device model).

The FDC is a Device like any other: `tick(in, out)` once per 16 MHz master
cycle, no heap, caller-owned state. The disc image is a **caller-owned DSK
buffer** attached with `fdc_attach_disk()` — live wiring in the
`mem_attach_expansion` sense: it persists across reset and save/load and is
never serialized.

---

## 1. Role and bus position

The CPC decodes the floppy subsystem with **partial address decoding**: address
lines **A10 and A7 both low** select it (no other standard device uses that
combination — GA needs A15=0, CRTC A14=0, ROM select A13=0, printer A12=0, PPI
A11=0, all of which leave A10 high at their conventional ports). Within the
subsystem:

| line | value | selects |
|---|---|---|
| **A8** | 0 | the **drive motor latch** (conventional port &FA7E) |
| **A8** | 1 | the **µPD765A** itself (&FB7E/&FB7F) |
| **A0** | 0 | FDC **main status register** (read-only, &FB7E) |
| **A0** | 1 | FDC **data register** (read/write, &FB7F) |

So the Device answers any I/O cycle with `(addr & 0x0480) == 0` (and `m1` low —
an interrupt acknowledge also drives `iorq` and must be ignored):

- **motor latch** (`A8 = 0`): write-only. Data bit 0 = motor on for **all**
  drives (one latch, one spindle signal — there is no per-drive motor). Reads
  float (bus pull-ups → 0xFF).
- **main status** (`A8 = 1, A0 = 0`): read-only. Writes are ignored.
- **data register** (`A8 = 1, A0 = 1`): the single byte-wide window through
  which command bytes go in and execution/result bytes come out.

The FDC drives `cpu.data` on a read it owns, like every I/O device. **It drives
no other bus line**: on the CPC the µPD765A's /INT output is *not connected* —
the firmware polls the main status register instead — so this Device never
touches `cpu.irq`. DRQ/DMA are likewise unused (the CPC runs the FDC in non-DMA,
polled mode).

### One byte per access (edge semantics)

A Z80 I/O cycle holds `iorq`+`rd`/`wr` asserted for many master cycles (T1–T3 of
the I/O M-cycle, ×4 at the ÷4 clock, plus WAIT stretching). Data-register
accesses have side effects (they advance the FDC's byte stream), so the Device
acts once per *access*, not once per master cycle:

- it tracks whether the previous master cycle was already an owned data-register
  access (`rd` or `wr` asserted with the FDC selected);
- on the **rising edge** it performs the transfer (latches the outgoing byte and
  advances its FSM, or consumes the written byte);
- for the remaining cycles of the access it keeps driving the **latched** byte,
  so the Z80 samples a stable value at its T4.

Main-status reads are side-effect-free and are simply answered every cycle.

---

## 2. Main status register (MSR)

| bit | name | meaning |
|---|---|---|
| 7 | **RQM** | request for master: data register ready for a transfer |
| 6 | **DIO** | direction: 1 = FDC→CPU (read the data reg), 0 = CPU→FDC (write it) |
| 5 | **EXM** | execution phase in progress (non-DMA mode) |
| 4 | **CB** | controller busy: a command is being processed |
| 3..0 | **D3B..D0B** | per-drive seek busy |

Values produced by this Device (the rotating-medium model, §7):

| FSM state | MSR |
|---|---|
| command phase, idle | `0x80` (RQM, direction CPU→FDC) |
| command phase, parameters outstanding | `0x90` (RQM + CB) |
| BUSY (mechanics turning: rotation/seek wait) | `0x10` (CB only, RQM low) |
| execution phase, FDC→CPU, byte cell passed | `0xF0` (RQM + DIO + EXM + CB) |
| execution phase, awaiting the next byte cell | `0x70` (RQM low — the 32 µs pacing) |
| result phase | `0xD0` (RQM + DIO + CB) |

**D0B/D1B** carry the per-unit seek-busy state in every phase while the head is
stepping (§7).

---

## 3. The three phases

Every command walks the µPD765A's three phases through the data register:

1. **Command phase.** The CPU writes the opcode byte, then the fixed number of
   parameter bytes for that opcode. The opcode's **low 5 bits** select the
   command; bits 7/6/5 are the MT (multi-track), MF (MFM) and SK (skip)
   modifiers, accepted and stored but not otherwise modeled in V1 (the DSK
   layer has no FM/MFM distinction; MT is meaningless on the single-sided 3"
   layout; SK only matters with deleted data marks, deferred).
2. **Execution phase.** Only data-transfer commands have one. In non-DMA mode
   the CPU polls MSR and moves one byte per data-register access. Control
   commands (SPECIFY, SEEK, …) execute instantly and skip straight to the
   result phase (or back to the command phase if they have no result).
3. **Result phase.** The CPU reads the fixed number of status bytes. Reading
   the last byte returns the FDC to the command phase.

An **unknown opcode** immediately enters the result phase with the single byte
ST0 = `0x80` (invalid command) — the real chip's behaviour.

---

## 4. Command set

| command | opcode (low 5) | cmd bytes | result bytes | V1 status |
|---|---|---|---|---|
| SPECIFY | 0x03 | 3 | 0 | **implemented** (SRT drives the per-step seek timing, §7/§7b) |
| SENSE DRIVE STATUS | 0x04 | 2 | 1 (ST3) | **implemented** |
| RECALIBRATE | 0x07 | 2 | 0 | **implemented** (steps to track 0 at the SRT rate) |
| SENSE INTERRUPT STATUS | 0x08 | 1 | 2 (ST0, PCN) | **implemented** |
| SEEK | 0x0F | 3 | 0 | **implemented** (steps to C at the SRT rate; seek-end at arrival) |
| READ ID | 0x0A | 2 | 7 | **implemented** |
| READ DATA | 0x06 | 9 | 7 | **implemented** (multi-sector, EOT) |
| READ DELETED DATA | 0x0C | 9 | 7 | **stub**: result AT + ST1 ND (no deleted-mark model in V1) |
| WRITE DATA | 0x05 | 9 | 7 | **implemented** (§10: rotational wait, CPU→FDC pacing, overrun, multi-sector, in-place DSK mutation) |
| WRITE DELETED DATA | 0x09 | 9 | 7 | **implemented** (§10: as WRITE DATA + sets the sector's ST2 Control Mark in the image's Track-Info) |
| FORMAT TRACK | 0x0D | 6 | 7 | **implemented** (§10: consumes SC×4 ID bytes in the execution phase, rewrites the track in place, completes at the index hole) |
| READ TRACK (diagnostic) | 0x02 | 9 | 7 | **not implemented** → invalid command (ST0 0x80) |
| SCAN EQUAL / LOW / HIGH | 0x11/0x19/0x1D | 9 | 7 | **not implemented** → invalid command |

The stubs consume their full command-phase byte count first, so a caller's
handshake never wedges; the documented result then tells it truthfully that
nothing was transferred. AMSDOS reacts to NW with "Drive A: read only", to the
invalid-command ST0 with a retry/abort — both benign for a read-only V1.

### Command details

**Command parameter layout** (data-transfer commands): opcode, unit/head
(`bits 1..0` = US unit select, `bit 2` = HD head), C, H, R, N, EOT, GPL, DTL —
see the READ DATA walkthrough in `floppy-disc-system.md`.

**SPECIFY** — SRT sets the seek step period ((16 − SRT) × 2 ms, §7); HUT/HLT/ND
are latched (introspectable) but not otherwise modeled. No result phase.

**SENSE DRIVE STATUS** — returns ST3 for the addressed unit. On the real
µPD765A every ST3 bit is a **direct pin-image of the addressed drive's status
lines** (datasheet "Status Register 3" + the FLT/TRKO, WPRT/2SIDE and SIDE pin
descriptions). This Device reproduces those hardware semantics — **not** the
legacy core's fabricated "WP + two-sided when no disc" values (`beads-7mo4`):

| bit | line | value |
|---|---|---|
| 7 | FT (fault) | 0 — the CPC never wires the drive's fault line |
| 6 | WP (write protect) | 1 when the media cannot be written: **no disc, or flux-backed (read-only) media**; 0 for a writable DSK |
| 5 | RY (ready) | 1 if a disc is present **and** the motor has spun up (unit 0 only, §7) |
| 4 | T0 (track 0) | 1 if the unit's head sits on physical track 0 |
| 3 | TS (two-sided) | 0 — the CPC's 3" drive is single-sided (the 2SIDE line is never asserted) |
| 2..0 | HD, US1, US0 | echoed from the command (the side/unit-select outputs) |

Nothing in AMSDOS issues SENSE DRIVE STATUS (readiness is inferred from ST0's
Not-Ready bit on a failed read, and disc-change from READ ID — see the XBIOS
disc routines), so this datasheet-faithful ST3 is unobservable to the stock CPC
software but correct for any program that does read it.

**RECALIBRATE / SEEK** — move the unit's head (RECALIBRATE to 0, SEEK to C,
clamped to the DSK track maximum). Instantaneous in V1. Both set the unit's
**seek-end flag** and have **no result phase**; the CPU is expected to poll
with SENSE INTERRUPT STATUS. If the drive is not ready the head still does not
move, but the seek-end flag is set regardless (the state machine must
terminate; ST0 from the following SENSE INTERRUPT reports the ready state).

**SENSE INTERRUPT STATUS** — the polled substitute for the unconnected /INT
line. Priority order (mirrors the legacy core):

1. a pending **seek-end** (unit 0 checked before unit 1): ST0 = `0x20 | unit`
   (+ `0x48` abnormal/not-ready if the drive is not ready), PCN = the unit's
   current track; clears that unit's seek-end *and* status-change flags;
2. else a pending **status change** (motor toggled, disc attached/ejected):
   ST0 = `0xC0 | unit` (+ `0x08` if not ready), PCN = current track; clears
   that unit's status-change flag;
3. else: **invalid command** — single result byte ST0 = `0x80`.

**READ ID** — requires the drive ready (else AT + not-ready ST0, CHRN from the
head position). Returns ST0 (normal), ST1 = ST2 = 0 and the **C/H/R/N of the
next sector header under the head**: the Device keeps a rotational index per
drive, returns that sector's ID and advances the index (wrapping). This is how
AMSDOS detects the format (&C1 → DATA, &41 → SYSTEM, &01 → IBM). On an
unformatted/absent track: AT + ST1 MA (missing address mark).

**READ DATA** — the workhorse. After the 9 command bytes:

1. Ready check (motor on + disc in the unit) — failure gives AT + not-ready
   ST0 with the command's CHRN, no execution phase.
2. The requested sector is located on the **current physical track** (the head
   does *not* seek — C is only an ID-field match): starting at the rotational
   index, scan the track's sector IDs for a full C/H/R/N match, at most two
   index passes. While scanning, a header whose C ≠ requested C sets ST2 **NC**
   (no cylinder, `0x10`); C = 0xFF sets ST2 **BC** (bad cylinder, `0x02`, which
   suppresses NC). Not found → AT + ST1 **ND** (no data, `0x04`) + whatever
   BC/NC accumulated, result phase.
3. Found: execution phase, direction FDC→CPU. Transfer length is `128 << N`
   (or DTL, capped at 128, when N = 0), served one byte per data-register
   access from the DSK sector data. The sector's stored ST1/ST2 (from the DSK
   image — CRC-error emulation etc.) are merged into the running result.
4. Sector exhausted: if any error bit is set (ST1 & 0x31 or ST2 & 0x21) →
   result phase. Else if **R ≠ EOT**: R is incremented and the next sector is
   located (step 2) — the multi-sector stream continues without the CPU doing
   anything but reading bytes. Else (**R = EOT**, the normal AMSDOS
   termination, since the CPC has no working Terminal Count line):
   ST0 |= `0x40` (AT) and ST1 |= `0x80` (EN, end of cylinder) — then the
   status-masking rule below.
5. Result phase: ST0, ST1, ST2, C, H, R, N (the command's CHRN buffer as it
   stands — R still equals EOT). See **Divergence A** below: this is a
   *documented* golden-master divergence, not an oversight.

**Status masking on termination** (**Divergence C**, legacy-core rule kept
verbatim): when the transfer ends at EOT, AT+EN are set first; then, if any
*other* error bit is present in ST1 bits 0..6 or ST2 bits 0..6, EN is cleared
again (the error, not the cylinder end, is the story); DE/DD errors clear CM; a
CM-only termination clears both AT and EN.

If a transfer's byte demand runs past the sector's stored data (short stored
sectors in an extended DSK), the stream continues into the following bytes of
the track's data block, wrapping at its end — the legacy core's behaviour.

### 4b. Golden-master divergences from the µPD765A datasheet

These three behaviours deliberately follow the **engine=0 legacy oracle**
(Caprice32) rather than a literal datasheet reading. Each is either
unobservable to CPC software or an emulation-synthesis question the datasheet
does not settle. They are marked here, in the code (`beads-7mo4`), and in the
tests so nobody mistakes a convention for hardware truth. (Audited against the
NEC µPD765 datasheet Dec78, `cpcwiki.eu` UPD765_Datasheet_OCRed, seasip XBIOS,
and the `floooh/chips` clean-room FDC.)

- **A — result-phase R stays = EOT (no cylinder rollover).** The datasheet's
  *prose* says the Sector Number "is incremented by one" after each sector and
  that a read "may be terminated by the receipt of a Terminal Count signal";
  the result-register **values** live in the datasheet's *Table 2*, which
  describes **processor- (TC-) terminated** completion. **The CPC has no
  Terminal Count line** — every read instead terminates *abnormally* with EN
  ("the FDC tries to access a Sector beyond the final Sector of a Cylinder").
  What Table 2's `C←C+1, R←01` rollover does on that EN-without-TC path is *not*
  unambiguously specified by the accessible primary sources (Table 2 and the
  read flowchart are datasheet images; the community's own analysis ran to five
  forum pages), leaving three plausible result-R values (`EOT`, `EOT+1`,
  `C+1/R=01`). We keep the oracle's `R=EOT` — the same value `floooh/chips`
  effectively yields (it does not implement multi-sector EOT rollover at all).
  This is safe because **AMSDOS never reads the READ DATA result CHRN**: a read
  succeeds/fails on ST0/ST1/ST2, and disc-change is detected via a separate
  READ ID (seasip XBIOS). Encoding a *guessed* rollover would be a fabricated
  spec — strictly worse than an honest, cited divergence.

- **B — SENSE INTERRUPT flags a ready-change on both units at once.** On any
  ready-line event (motor spin-up READY edge, motor OFF, disc attach/eject) the
  Device raises the status-change flag for *both* units, and SENSE INTERRUPT
  drains them unit-0-first. The real chip reports a ready-change only for a
  drive whose RY line actually *transitions*, so unit 1 (never populated in V1)
  would never report. Ours is a **harmless superset**: AMSDOS drains SENSE
  INTERRUPT until it sees `0x80` (nothing pending), so an extra unit-1 report is
  absorbed — and the µPD765's documented *post-RESET* behaviour (a SENSE
  INTERRUPT result per drive) actually favours over-reporting. (Note: the flag
  is raised on the **ready-line event**, not — as an earlier draft of §6 said —
  on *every* latch write.)

- **C — ST0/ST1/ST2 termination masking (above).** The datasheet defines **no**
  result-code masking algorithm; the real chip sets EN and the independent error
  bits and leaves them. The masking that picks a single "reason" (error over
  cylinder-end; data-error over control-mark) is Caprice32's *synthesis*, and
  AMSDOS's retry/error path was validated against exactly that priority. There
  is therefore nothing in the datasheet to "align" to; the oracle is the
  authority for the synthesis the CPC firmware expects.

---

## 5. DSK geometry mapping

`fdc_attach_disk(dev, image, len)` parses the caller's buffer **in place** (no
copy, no heap): the Device stores per-track/per-sector descriptors that carry
*offsets into the caller's buffer*. Rejects (returns −1, drive left empty)
anything that fails validation; returns 0 on success. Drive A (unit 0) only in
V1. `fdc_eject_disk()` detaches. Both set the drive's status-change flag.

Accepted containers (see `floppy-disc-system.md` for the on-disc story):

- **Standard DSK** — magic `"MV - CPC"`. Header byte 0x30 = tracks, 0x31 =
  sides (1 or 2), 0x32/0x33 = uniform track-block size (little-endian,
  including the 256-byte Track-Info header). Track blocks follow the header
  sequentially (side-interleaved when two-sided). Every sector in a track has
  the track-level size `128 << size_code` (header byte 0x14); sector count at
  0x15; 8-byte sector-info entries (C, H, R, N, ST1, ST2, unused×2) from 0x18;
  sector data concatenated in table order from offset 0x100 of the block.
- **Extended DSK** — magic `"EXTENDED"`. Same head fields, but a per-track
  **size table** at header offset 0x34 (one byte per track×side = size/256,
  including the Track-Info header; 0 = unformatted track, no block present).
  Sector-info bytes 6/7 hold each sector's individual **stored length**
  (little-endian); data is concatenated in table order using those lengths.
  The ID-field size `128 << N` may differ from the stored length (weak-sector
  images store multiples — V1 reads the first copy).

Validation: magic, sides 1..2, tracks ≤ 102, sectors/track ≤ 29, and every
track block and sector datum must lie inside `len`. The parse is tolerant of
trailing junk after the last track.

The attachment (pointer, length, parsed descriptors) is **live wiring**: it
survives `reset` and `load` (like `mem_attach_expansion`'s buffer) and is never
serialized. Serialized FDC state that references media positions (rotational
index, in-flight transfer offsets) is only meaningful when the same image is
re-attached — the same caveat every caller-owned attachment carries.

---

## 6. Motor latch and readiness

One write-only latch (§1) drives the spindle motor of every drive. A drive is
**ready** iff it is unit 0, a disc is attached, and the motor latch is on.
Spin-up is instantaneous in V1 (deferred; the firmware's own motor-on ticker
delay still elapses, it just waits on a drive that is already ready).

Each **ready-line event** — the motor spin-up READY edge (§7), motor OFF, and
disc attach/eject — raises the **status-change** flag of *both* drives, and the
firmware drains the resulting SENSE INTERRUPT STATUS reports. Flagging both
units (rather than only the one whose RY actually transitioned) is a documented
golden-master superset — see **Divergence B** in §4b.

---

## 7. Timing model — the rotating medium (Stage 2, IMPLEMENTED)

The disc is a physically rotating object clocked by the 16 MHz master grid; every
mechanical delay is exact in master cycles:

| Quantity | Value | Master cycles |
|---|---|---|
| Revolution (300 RPM) | 200 ms | **3,200,000** |
| Byte cell (DD MFM, 250 kbit/s) | 32 µs | **512** (→ 6250 byte-cells/track) |
| Motor spin-up → ready | 500 ms | 8,000,000 (under AMSDOS's own ~1 s wait) |
| Seek step | (16 − SRT) × 2 ms | (16 − SRT) × 64,000 (SRT from SPECIFY; 4 MHz FDC clock doubles the datasheet's 8 MHz units; AMSDOS's SRT = 0xA → the classic 12 ms/step) |

- **Rotation**: a per-device counter advances every master cycle while the motor
  spins; `byte cell = (rot / 512) mod 6250` is the angle under the head. Motor off
  freezes rotation (coast-down not modeled — documented).
- **Motor**: OFF → SPINUP (rotation starts, drive **not ready**) → READY after
  spin-up; the ready-line change fires SENSE-INTERRUPT status-change at the READY
  edge and on OFF. Commands issued before READY terminate Not-Ready as before.
- **Seek/recalibrate**: ETA = |Δtracks| × step time (Δ=0 completes at once).
  While seeking the MSR carries the unit's **D0B/D1B busy bit**; `seek_done` (and
  thus SENSE INTERRUPT's Seek End) fires only at the ETA — before it, SENSE INT
  reports nothing pending (0x80), which AMSDOS's poll loop handles naturally. The
  head position updates at the ETA (mid-seek granularity not modeled — documented).
- **Angular track layout** (System 34, synthesized for sector-backed media):
  post-index preamble 146 byte-cells (GAP4a 80 + sync 12 + IAM 4 + GAP1 50); each
  sector occupies 62 + size byte-cells of structure (sync 12 + IDAM 4 + CHRN 4 +
  CRC 2 + GAP2 22 + sync 12 + DAM 4 + CRC 2) plus GAP3 = the remaining track space
  divided evenly (clamped ≥ 2) — this even split absorbs the physical format's
  trailing GAP4b, so the standard 9×512 CPC track gets 104 cells between sectors
  (format GAP3 82 + its share of GAP4b). Over-full (long) tracks wrap modulo the
  revolution — a documented approximation. Each sector's **ID-complete** and
  **first-data** byte-cell positions are precomputed at attach.
- **READ ID / READ DATA scheduling**: after the command phase the FDC enters a
  BUSY sub-state (MSR = CB only, RQM low) until the target passes under the head:
  READ ID completes when the next ID field arrives; READ DATA scans IDs from the
  current angle and starts its transfer at the matching sector's first-data cell.
  No match → **No Data after two index passes** (2 revolutions), the real bound.
- **Byte pacing + overrun**: during execution, data byte *k* becomes readable at
  `start + k×512`; RQM is low until then. If byte *k+1* arrives while *k* is still
  unread, the transfer aborts with **ST1 OR (0x10) + AT** — the CPC's real 26 µs
  polling deadline, which is why disc loops run with interrupts disabled.

### 7b. Mechanical event ring + per-step seeks (drive sounds)

The FDC logs its physical events — `MOTOR_ON`, `MOTOR_READY` (the 500 ms
spin-up completing), `MOTOR_OFF`, `STEP` (one per head step, arg = the new
track), `INDEX` (every 200 ms while spinning) — with master-cycle timestamps
into a small live ring (drop-oldest, never serialized), drained by the host
via `fdc_drain_events()`. Since 16 cycles = 1 µs, `sample = cycle × rate /
16 MHz` is exact math: the audio bridge schedules the physically-modeled WAV
assets (`resources/drive-sounds/`, see their READMEs) at sample-quantized
offsets. Seeks now STEP the head one track per SRT period (mid-seek position
observable; same total time as before) — each step is one audible click, so a
recalibrate is a click train at the true rate, not a sound effect.

**Still simplified (documented)**: result/command-phase handshake bytes are
immediate (real chip: ~12 µs); head-load (HLT) and unload times ignored; motor
coast-down instant; mid-seek head position not observable; write paths still
stubbed. None of these change the register interface.

---

## 8. State

```
phase                  : CMD / EXEC / RESULT
cmd[9], cmd_len,
cmd_count              : command-phase accumulator
mt, mf, sk             : modifier bits of the current opcode
res[7], res_len,
res_count              : result-phase buffer
dir_to_cpu             : execution-phase direction (DIO while EXM)
motor                  : the motor latch
track_pos[2]           : physical head position per unit (survives reset — mechanics)
sector_idx             : rotational index on the current track (unit 0)
seek_done[2],
status_changed[2]      : SENSE INTERRUPT sources
srt_hut, hlt_nd        : SPECIFY latches
data_off / data_end /
remaining / track_*    : in-flight execution-phase transfer window (image offsets)
out_latch, access_prev : the per-access edge machinery (§1)
sectors_read           : introspection counter (tests)
media                  : the caller attachment + parsed geometry (live wiring, §5)
```

`fdc_peek()` exposes phase, MSR, motor, last opcode, per-unit track, the last
ST0/ST1/ST2, and the sectors-read counter for tests and trace tooling.

---

## 9. Tick / reset / save

- **tick**: update the access edge tracker; if the cycle is an owned I/O access
  (§1 decode, `m1` low), service it — motor-latch write, MSR read, or one
  data-register byte on the access edge (§3 machinery). Nothing else is driven.
- **reset**: back to the command phase, buffers cleared, motor off, SPECIFY
  latches and pending flags cleared. **Head positions and the media attachment
  persist** (mechanical position / live wiring, like the PPI's jumpers).
- **save/load**: version byte + the logical state above. The media attachment
  (pointer *and* parsed geometry) is preserved across `load` — the mem-Device
  pattern for caller-owned wiring — and never leaves the machine in the blob.

## 10. Writes + FORMAT (V2 — DSK-backed media becomes mutable)

**Ownership.** `fdc_attach_disk` now takes a **mutable** caller buffer
(`uint8_t*`): the Device mutates the DSK image in place, exactly where the
parsed sector windows point. Flux-backed media (SCP) stays **read-only** —
WRITE/FORMAT against it return AT + ST1 NW (write-to-flux is its own future
project). A **dirty flag** (`fdc_media_dirty` / `fdc_media_mark_clean`) tells
the host the buffer diverged from the file; persistence is the HOST's job
(the bridge mirrors dirty into the legacy `driveA.altered`, so the existing
save-on-eject flows keep working unchanged — parity with the golden master).

**WRITE DATA (0x05) / WRITE DELETED DATA (0x09).** Mirrors READ DATA's
machinery with the direction reversed: ready check → head_track (unformatted
→ AT + MA, per the golden master) → rotational sector scan (two index passes
→ AT + ND) → PH_BUSY until the sector's data field passes the head →
PH_EXEC with `dir_to_cpu = false`. The CPU must FEED a byte per 32 µs cell:
MSR raises RQM per cell (DIO = CPU→FDC), each accepted byte advances
`next_byte_at` by one cell, and a cell arriving with the previous byte still
unfed is an **OVERRUN** (ST1 OR, abnormal termination) — symmetric with the
read side. Bytes land at the sector's image window and **wrap inside the
track's data block** on overlong transfers (the golden master's rule — the
container cannot model a write running over the next ID field, so it wraps
like the reads do). Multi-sector chaining via EOT and the status-masking
termination rules are shared with READ DATA. WRITE DELETED additionally
sets the sector's **ST2 Control Mark** both in the parsed descriptor and in
the image's Track-Info sector entry (byte 5), so a later READ reports it.
Every completed sector write sets the dirty flag.

**FORMAT TRACK (0x0D).** Command bytes: op, unit, N, SC, GPL, D. Ready and
writability checks as above, then PH_EXEC (CPU→FDC) collecting **SC × 4**
ID bytes (C, H, R, N per sector) at byte-cell pacing, then PH_BUSY until the
next index hole (a format occupies the revolution); completion rewrites the
track **inside its existing DSK track block**: Track-Info sector table
rebuilt from the collected IDs (ST1/ST2 = 0, size code = command N), data
area filled with the filler byte D, per-sector windows and System 34 angles
recomputed (the same layout pass the parser runs). **Container constraint:**
the new layout must fit the track block the image already allocates —
`0x100 + SC × (128 << N) ≤ block`. A layout that does not fit terminates
with AT + ST1 NW and leaves the image untouched (a real chip would happily
write it; the DSK container cannot hold it — documented approximation,
covers every same-or-smaller-geometry reformat, e.g. AMSDOS FORMAT).
Standard (non-extended) images additionally require the sector size to
match the image-wide `0x80 << size_code` sizing rule.

**Result phase.** Both writes and FORMAT report the usual 7 bytes; FORMAT's
CHRN reflects the head position and the command's N (the golden master's
convention).

## Batch contract (RunTier::Fast)

- **Deadline set (all exact arithmetic on `now`, §7)**: `rot` wrap →
  FDC_EV_INDEX each revolution; `motor_ready_at` → READY (both units'
  status_changed, divergence B §4b); `next_step_at[u]` → one head STEP per
  SRT period until seek end; `due_at` → PH_BUSY completion; `next_byte_at` +
  kByteCycles → execution-phase OVERRUN. Batch-advance runs deadline to
  deadline — event timestamps identical to a per-cycle run by construction
  (the shipped fdc_quiet/fdc_advance contract is the degenerate no-deadline
  case of this).
- **I/O accesses**: catch up to the access timestamp, then the per-access
  handshake (motor latch edge, MSR read, data-register one-byte-per-access
  latch) — which is exactly the documented "fast timing" model, so batch
  behavior IS the spec's behavior.
- Bestiary audit: class (b) throughout — `now` is the timestamp base for
  PrinterEvent-style mechanical events, so advances must be exact (they
  are: pure deadline math); the access edge-detector becomes one-latch-per-
  access naturally (no held-strobe re-decode exists at event granularity).

Fast-tier note (F6): while `fdc_quiet`, the scheduler skips + `fdc_advance`s
(the wake contract); an I/O access is a crafted access-tick + release-tick
pair at the exact master. When an access leaves the FDC non-quiet, the
machine switches it to DEFERRED BURST ticking — the skipped masters run as
real resting-bus ticks at the next access/exit, in order, so `now` and every
deadline decision and event timestamp are per-cycle-identical (nothing can
observe the FDC between accesses). Frames that BEGIN non-quiet run on the
per-cycle tiers (the frame-start gate in run_frame).
