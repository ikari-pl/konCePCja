# USB↔CPC floppy bridge — engineering plan (Greaseweazle-class, open hardware)

Planning document for a cheap, beginner-buildable USB flux bridge that connects
a **real Amstrad 3-inch drive** (internal EME-15x mechanisms or the external
FD-1) to a modern host, and that this emulator consumes **out of the box** —
both as an archival dumper (→ `.scp`, already a first-class medium via
`fdc_attach_flux`, see [`flux-media.md`](flux-media.md) §7) and as a **live
drive backend**: the simulated µPD765A's on-demand track cache
(`ensure_flux_cache`, `src/hw/fdc.cpp`) turns a cache miss into "seek the real
drive and read two revolutions", and the FDC's rotating-medium timing model
([`fdc-device.md`](fdc-device.md) §7) hides the real mechanism's latency inside
the BUSY phase it already simulates.

This is a **plan**: no code, no PCB files. Facts pulled from the web are cited
inline; anything not verifiable is marked **needs-bench-confirmation** and
collected in §10. Companion: [`gw-protocol-notes.md`](gw-protocol-notes.md)
(the Greaseweazle host protocol summary the firmware and the emulator's
`gw_host` module must speak).

---

## 0. Status & how to resume (added at integration, 2026-07-03)

**Where things stand**: plan complete and reviewed; **no hardware ordered, no
code written**. Everything the emulator side needs already exists on
`feat/sub-cycle-chip-sim`: `fdc_attach_flux()` + the per-revolution track cache
(`ensure_flux_cache`, `kFluxRevs = 2`) in `src/hw/fdc.cpp`,
`flux_decode_track_rev()` in `src/hw/flux.h`, `--flux` in the sim, and the
weak-bit tests (`FdcFlux.*`) that double as the passthrough acceptance suite.

**Issue tracking** (epic `beads-o0wn`) — note the bd breakdown splits this
document's M2 into two issues:

| Plan milestone (§9.3) | bd issue | Blocks |
|---|---|---|
| M1 breadboard read + Q1–Q3 | `beads-raim` | → board work |
| M2 PCB rev A — the **board** | `beads-i1z4` | (blocked by M1) |
| M2 PCB rev A — the **firmware** | `beads-ob2j` | → emulator work |
| M3 emulator passthrough | `beads-21fg` | (blocked by firmware) |
| M4 write support | `beads-bsui` | (blocked by M3) |

**The single next action is M1 bench work (user + a real drive).** Checklist:

- [ ] Order: a **genuine** STM32F103 Blue Pill (see §3's counterfeit warning —
      buy from a distributor, not a marketplace) + an ST-Link clone for
      flashing stock Greaseweazle F1 firmware; 1× CD4050B + 2× 74HCT06 (DIP)
      + 4.7 kΩ pull-ups for the breadboard buffer stage (§5); 34-pin IDC
      ribbon; a 26-way 0.1" breakout for the internal-mech FPC (§2.2); a bench
      12 V supply with current limiting (Q3!).
- [ ] Subject drive: an EME-150/155 mech or the FD-1 — mind the **swapped
      5 V/12 V power connector** (§2.4); miswiring it is the classic mech
      killer.
- [ ] **Q1**: beep out FPC pins 6/26 to the mech PCB (candidates: RDY, +5 V);
      scope RDY vs MOTOR-on timing; check INDEX behaviour with flipped media.
- [ ] **Q2**: measure resistance from each output line to +5 V on the mech
      (internal pull-ups?) and confirm whether RDATA needs our jumperable
      termination.
- [ ] **Q3**: current-probe the 12 V rail at motor start (inrush) and steady
      spin; log both.
- [ ] Exit test: `gw read` a known disc → SCP → `flux_scp_to_dsk` decodes it
      byte-exact vs its DSK; log all three answers into §10/§11 and unblock
      `beads-i1z4`.

Companion reference: [`gw-protocol-notes.md`](gw-protocol-notes.md) — the
protocol both the M2 firmware and the M3 `gw_host` module must speak.

---

## 1. Requirements

| # | Requirement | Notes |
|---|---|---|
| R1 | Read flux from 3" CPC drives | Internal Amstrad/Matsushita EME-150/155 (664/6128) mechs and the external FD-1; any Shugart-subset drive should also work (it's just a Shugart bus) |
| R2 | ≥ 2 revolutions per track, index-synchronized | Matches the FDC flux cache (`kFluxRevs = 2`) and the weak-bit multi-revolution rule ([`flux-media.md`](flux-media.md) §4); 3+ revs selectable for archival |
| R3 | SCP-compatible output | The emulator's existing container (§1 of `flux-media.md`); `gw` host tools must also produce/accept dumps from the device unmodified |
| R4 | Greaseweazle host protocol | "Out of the box" = the device is indistinguishable from a Greaseweazle to both `gw` and the emulator; no custom host stack to maintain |
| R5 | Live-passthrough latency budget | A cold cylinder (seek + index sync + 2 revs) served in ≤ 1 s wall time; warm/prefetched cylinders at simulated-BUSY cost only (§7.4) |
| R6 | Write support | Later phase (M4). The bus wiring (WDATA/WGATE) and protocol command (`WRITE_FLUX`) are designed in from rev A; firmware/emulator write paths deferred |
| R7 | BOM ≤ ~$15/unit | At qty 1, excluding the 12 V brick and the drive itself (§9) |
| R8 | Beginner-solderable | Through-hole only: a carrier PCB for a castellated/pin-header MCU module + DIP logic; no QFP/QFN hand-soldering |
| R9 | Open hardware | CERN-OHL for our board; firmware under a GW-ecosystem-compatible permissive license (§10.4) |

Non-goals (V1): HD/ED media (CPC is DD only), hard-sectored formats, more than
two drives, flippy-modded 5.25" support.

---

## 2. Drive interface research

### 2.1 The signal set (Shugart subset)

The CPC disc system is a plain Shugart bus: open-collector, active-low,
5 V TTL levels, odd pins ground. The CPC-side external ("2nd drive")
connector documents exactly which Shugart signals the ecosystem uses
(CPCWiki, *Connector:2nd disc drive (CPC664, CPC6128, CPC6128+)*, retrieved
via web.archive.org snapshot
`web.archive.org/web/20110910190520/http://www.cpcwiki.eu:80/index.php/Connector:2nd_disc_drive_(CPC664,_CPC6128,_CPC6128%2B)`):

| Shugart pin | Signal | Direction (bridge view) | On the CPC bus | Verified |
|---|---|---|---|---|
| 2 | /REDWC (density) | out | N/C — CPC is DD-only | source above |
| 8 | /INDEX | in | wired | source above |
| 10 | /DRVSA (DS0) | out | **N/C on the CPC's external connector** (external drive is always B:) | source above |
| 12 | /DRVSB (DS1) | out | wired | source above |
| 14 | (DS2) | out | N/C | source above |
| 16 | /MOTEB (motor) | out | wired (single motor line) | source above |
| 18 | /DIR | out | wired | source above |
| 20 | /STEP | out | wired | source above |
| 22 | /WDATA | out | wired | source above |
| 24 | /WGATE | out | wired | source above |
| 26 | /TRK00 | in | wired | source above |
| 28 | /WPT | in | wired | source above |
| 30 | /RDATA | in | wired | source above |
| 32 | /SIDE1 | out | wired (unused by single-head 3" mechs) | source above |
| 34 | /RDY | in | wired — **the CPC expects a true READY here, not the PC's /DSKCHG** (the two are *not* compatible: DSKCHG stays high until stepped, so a PC drive never looks ready to a CPC) | source above (note 3 on the page) |
| all odd | GND | — | ground | source above |

Bridge-relevant consequences:

- **Outputs the bridge must drive**: DS0, DS1, MOTOR, DIR, STEP, WGATE, WDATA,
  SIDE1, (DENSEL for non-CPC drives) — 9 lines.
- **Inputs the bridge must read**: INDEX, TRK00, WPT, RDATA, RDY — 5 lines.
- The FD-1/DDI-1 drives are **soldered to answer both DS0 and DS1**; the cable
  decides which unit they are (CPCWiki, *Amstrad External Disk Drive*,
  `web.archive.org/web/2020/http://www.cpcwiki.eu:80/index.php/Amstrad_External_Disk_Drive`).
  So the bridge must be able to assert either select — Greaseweazle's
  `--drive 0|1|2` Shugart addressing covers this
  ([github.com/keirf/greaseweazle/wiki/Drive-Select](https://github.com/keirf/greaseweazle/wiki/Drive-Select)).

### 2.2 34-pin IDC vs the internal 26-way FPC

Two physical form factors reach the same electrical bus:

1. **34-pin card edge / IDC** — the FD-1's cable and any generic Shugart drive.
   Our board carries a standard 34-pin boxed IDC header (shrouded, keyed).
2. **26-way FFC/FPC** — the CPC 664/6128 *internal* drive (EME-150/155
   mechanisms) connects with a 26-way flat cable: even pins signals, odd pins
   ground. The Greaseweazle project itself documents the mapping
   ([github.com/keirf/greaseweazle/wiki/Blue-Pill-Direct](https://github.com/keirf/greaseweazle/wiki/Blue-Pill-Direct)):

   | Signal | 34-pin (PC) | 26-way (Amstrad) |
   |---|---|---|
   | INDEX | 8 | 2 |
   | SELECT | 12 | 4 |
   | MOTOR | 16 | 8 |
   | DIR | 18 | 10 |
   | STEP | 20 | 12 |
   | WDATA | 22 | 14 |
   | WGATE | 24 | 16 |
   | TRK00 | 26 | 18 |
   | WRPROT | 28 | 20 |
   | RDATA | 30 | 22 |
   | SIDE | 32 | 24 |
   | GND | odd pins | odd pins |

   Pins **6 and 26** of the 26-way connector are *not* assigned in that table —
   candidates are READY and +5 V (the FD-1 service documentation notes some
   "N/C" pins in fact carry +5 V from the drive to the DDI-1 controller —
   CPCWiki, *Amstrad External Disk Drive*, ibid.). **needs-bench-confirmation**
   (§10, Q1). Keir Fraser publishes an open KiCad design for exactly this
   26-way adapter (`gw-26` in
   [github.com/keirf/pcb-projects](https://github.com/keirf/pcb-projects)) —
   we adopt its mapping and offer the adapter as an optional second header
   footprint rather than a separate board.

### 2.3 Electrical conventions

- **Open-collector, active-low**: drive outputs (INDEX/TRK00/WPT/RDATA/RDY)
  only pull low; the *receiving* end provides the pull-up. Terminating
  pull-ups on vintage drives are strong — Greaseweazle documents drives with
  input pull-ups **< 1 kΩ** needing 40 mA-class buffered outputs
  ([github.com/keirf/greaseweazle/wiki/Greaseweazle-Models](https://github.com/keirf/greaseweazle/wiki/Greaseweazle-Models),
  "Buffered Outputs"). Design consequence: §5.
- Whether the EME-15x mechs contain internal pull-ups to +5 V on their output
  lines (which would present 5 V at our MCU pins) is
  **needs-bench-confirmation** (§10, Q2); the input buffer in §5 is chosen so
  it does not matter.

### 2.4 Power: the +5 V / +12 V question

- 3" mechanisms need **both 5 V (logic) and 12 V (motors)**. The Greaseweazle
  wiki states it directly: "Older 5.25- and 3-inch drives typically are more
  power hungry and require 12 V power too: these you must connect to a
  separate power source" (Blue-Pill-Direct page, ibid.).
- The CPC 6128 itself has a **separate 12 V input used for the internal disc
  drive**, supplied by the GT65/CTM monitor or MP-2 modulator alongside the
  5 V rail (CPCWiki, *Power Supply for CPC and CPC plus*,
  `https://www.cpcwiki.eu/index.php/Power_Supply_for_CPC_and_CPC_plus`) —
  corroborating that the EME-15x motor rail is 12 V.
- The **FD-1 is mains-powered** (internal PSU; it even back-feeds +5 V to the
  DDI-1 — CPCWiki, *Amstrad External Disk Drive*, ibid.), so an FD-1 user
  needs **no 12 V from the bridge at all** — data cable only.
- ⚠ **The 3" drive's 4-pin power connector has +5 V and +12 V swapped**
  relative to the PC convention on the same housing — plugging a PC-wired
  plug straight in destroys the drive ("Amstrad switched the default
  connections of 5 and 12 volts" — Frank van Empel via CPCWiki, *Connect a 3
  inch drive to a PC*,
  `web.archive.org/web/2020/http://cpcwiki.eu:80/index.php/Connect_a_3_inch_drive_to_a_PC`).
  The board silkscreens the correct 3"-drive polarity next to its power
  output header (§5.3). Exact EME-150/155 motor current draw:
  **needs-bench-confirmation** (§10, Q3).

### 2.5 Single-sided media and the flip ritual

CF2 3" media are 40-track, and on the CPC used **single-sided**: side B is
reached by physically flipping the cartridge, not by a second head (the EME
mechs have one head; the FD-1/DDI-1 drives are single-sided CF2 units —
CPCWiki, *Amstrad External Disk Drive*, ibid.). Consequences:

- SIDE1 is wired but a no-op on these mechs; the SCPs we produce are
  `heads = 1` (side-0-only), which `flux_scp_*` already accepts
  ([`flux-media.md`](flux-media.md) §5).
- A "whole disc" archival dump is **two passes with a user flip prompt**,
  producing two side-0 SCPs (`disc-a.scp`, `disc-b.scp`) — mirroring how the
  DSK ecosystem treats flipped 3" discs as two independent images.
- Whether the index sensor and RDY behave identically with a flipped
  cartridge (the index hole is sensed per side) is
  **needs-bench-confirmation** (§10, Q1).

---

## 3. MCU / firmware decision matrix

### 3.1 The math both candidates must pass

**USB bandwidth (DD flux).** CPC media are 250 kbit/s MFM: legal flux
intervals are 4/6/8 µs, so the worst case is a solid run of 4 µs intervals =
250 000 transitions/s. The GW wire format spends 1 byte on intervals ≤ 249
sample ticks and 2 bytes up to 1524 ticks (see
[`gw-protocol-notes.md`](gw-protocol-notes.md) §3; verified against
`src/floppy.c` of
[github.com/keirf/greaseweazle-firmware](https://github.com/keirf/greaseweazle-firmware)).
At a 72 MHz sample clock a 4 µs interval is 288 ticks → 2 bytes, so worst
case is **500 KB/s ≈ 4 Mbit/s**; a typical CPC track (~40 000 transitions
per 200 ms revolution — [`flux-media.md`](flux-media.md) §2) is ~400 KB/s.
USB Full Speed bulk sustains ~1 MB/s in practice → **≥ 2× headroom on FS
USB; High Speed is unnecessary for DD** (Greaseweazle itself states HS is
"not needed for floppy disks up to High Density" — Models page, ibid.).

**Timer resolution vs the 25 ns SCP tick.** SCP files tick at 25 ns
(40 MHz) ([`flux-media.md`](flux-media.md) §1.1).

| MCU | capture clock | resolution | vs SCP tick |
|---|---|---|---|
| STM32F103 (GW F1 firmware) | 72 MHz timer (`SAMPLE_MHZ = 72` in firmware `src/floppy.c`, ibid.) | 13.9 ns | finer ✓ |
| RP2040 @ 125 MHz | PIO 2-instruction count loop → sysclk/2 | 16 ns | finer ✓ |
| RP2040 @ 200 MHz (Adafruit_Floppy's recommended overclock) | sysclk/2 | 10 ns | finer ✓ |

Both are far inside the DD jitter budget (the emulator's PLL tolerates ±10 %
per-interval jitter on a 2 µs half-cell = ±200 ns; a 16 ns quantization is
noise). Host-side conversion GW-ticks → 25 ns SCP ticks is a rational
rescale the `gw` tools already perform.

### 3.2 The matrix

| Criterion | (a) STM32F103 "Blue Pill" + stock GW firmware | (b) RP2040/RP2350 Pi Pico + PIO capture | (c) AT32F435 (GW V4 clone) |
|---|---|---|---|
| BOM (MCU module, qty 1) | ~$2–4, **but** the market is flooded with fake/clone STM32F103 that fail GW ([github.com/keirf/greaseweazle/wiki/STM32-Fakes](https://github.com/keirf/greaseweazle/wiki/STM32-Fakes)) | $4 (Pico) / $5 (Pico 2, official RRP — [raspberrypi.com/news/raspberry-pi-pico-2-our-new-5-microcontroller-board-on-sale-now](https://www.raspberrypi.com/news/raspberry-pi-pico-2-our-new-5-microcontroller-board-on-sale-now/)) | ~$3 bare LQFP + custom PCB |
| Availability (2026) | Genuine C8T6 boards increasingly a lottery; the GW project itself warns against non-retail sourcing | Excellent — first-party, multi-source, no counterfeits | AliExpress-grade sourcing |
| Firmware effort | **Zero** — flash keirf's binary, done; battle-tested read *and* write paths | Moderate — a working GW-protocol implementation for RP2040 already exists as Adafruit_Floppy's `examples/greaseweazle` sketch (reports itself as hardware model 8, so `gw` tools drive it; MIT-licensed — [github.com/adafruit/Adafruit_Floppy](https://github.com/adafruit/Adafruit_Floppy)); we harden it rather than start from scratch | High — port GW firmware or trust upstream V4 support on a self-made board |
| Protocol compatibility | Native | Native once the example is adopted (same CDC protocol) | Native |
| USB bandwidth | FS 12 Mbit/s — passes §3.1 | FS 12 Mbit/s — passes §3.1 | FS — passes |
| Timer resolution | 13.9 ns ✓ | 10–16 ns ✓ | 3.5 ns ✓ (irrelevant surplus) |
| Write-path feasibility | Proven (GW mainline) | PIO is symmetric (clocked pin toggling is PIO's home turf); Adafruit example includes WRITEFLUX but it is the least-proven part — gate behind M4 validation | Proven if the port works |
| Beginner solderability | Module + carrier, through-hole ✓ (keirf publishes an F1 adapter board design — [github.com/keirf/greaseweazle/wiki/Design-Files](https://github.com/keirf/greaseweazle/wiki/Design-Files)); direct Dupont wiring is explicitly deprecated by GW | Module + carrier, through-hole ✓ (Pico is pin-header/castellated) | ✗ LQFP-100 hand soldering |
| Long-term maintenance | Upstream firmware maintained by keirf; zero control for us | We own a small firmware; upstream `gw` protocol is stable and versioned | Upstream, but hardware niche |

### 3.3 Recommendation

**RP2040/RP2350 Raspberry Pi Pico on a through-hole carrier board, running a
Greaseweazle-protocol firmware derived from the Adafruit_Floppy
`greaseweazle` example.**

Rationale: the deciding criteria are *supply chain* and *solderability*, not
firmware effort. A $4–5 first-party module with zero counterfeit risk beats a
$3 module that the Greaseweazle project itself documents as a fake-chip
minefield; and PIO gives capture/write timing that is independent of CPU load
by construction. The firmware risk is bounded because (i) a working
GW-protocol RP2040 implementation exists under MIT, (ii) the protocol is
small and fully documented in public headers (see
[`gw-protocol-notes.md`](gw-protocol-notes.md)), and (iii) **milestone M1
uses a genuine Blue Pill + stock GW firmware as the reference instrument**,
so our firmware is always validated against a known-good implementation
rather than against itself. The Blue Pill path remains documented as the
zero-firmware fallback for builders who already own a genuine board.

Pico vs Pico 2: identical carrier; Pico 2 preferred for the $1 that buys
double SRAM (flux buffering headroom) and a second PIO block. The plan
costs the BOM with Pico 2.

---

## 4. Firmware plan

Protocol: **implement the Greaseweazle host protocol** over USB CDC-ACM,
version-compatible with current `gw` host tools. Command set, flux wire
encoding, and ACK codes are summarized with citations in
[`gw-protocol-notes.md`](gw-protocol-notes.md); the normative source is
`inc/cdc_acm_protocol.h` of the firmware repo (public domain).

Mandatory commands for M1–M3 (read-only): `GET_INFO`, `RESET`, `SET_BUS_TYPE`
(Shugart), `SELECT`/`DESELECT`, `MOTOR`, `SEEK` (+`NOCLICK_STEP`), `HEAD`,
`READ_FLUX`, `GET_FLUX_STATUS`, `GET_PIN`/`SET_PIN` (RDY polling, §7.5),
`SOURCE_BYTES`/`SINK_BYTES` (bandwidth self-test). M4 adds `WRITE_FLUX` and
`ERASE_FLUX`.

**Flux timestamping (capture).** One PIO state machine per direction:

- *Read*: a free-running 2-instruction counting loop samples RDDATA; on each
  falling edge it pushes the elapsed loop count to the RX FIFO (32-bit),
  giving sysclk/2 resolution with zero CPU involvement. A second, trivial SM
  watches INDEX and pushes an "index seen at count N" marker; firmware
  interleaves it into the stream as `FLUXOP_INDEX` (28-bit tick count since
  the previous index), matching GW semantics.
- *Write (M4)*: the TX-side SM pulls interval words from a DMA-fed FIFO and
  toggles WDATA after exactly N cycles; WGATE asserted by firmware,
  index-cued writes armed by the INDEX SM.

**Buffering + streaming.** DMA drains the RX FIFO into a ring buffer in SRAM
(Pico 2: 520 KB — an entire 2-revolution CPC track is ≤ ~200 KB even fully
expanded to 32-bit counts); a core-1 loop encodes ring content into the GW
1/2-byte wire format and writes USB CDC packets. Backpressure: if USB stalls
long enough to wrap the ring (host fault), abort with `ACK_FLUX_OVERFLOW`
exactly as GW does. At DD rates (§3.1) this never engages in practice.

**Index handling.** `READ_FLUX` takes the GW `ticks`/`max_index` framing: the
capture starts at the next index edge (index-cued), runs for N index pulses
(N = revolutions + 1 edges), and each pulse is embedded in-stream — this is
precisely the per-revolution segmentation `flux_decode_track_rev` needs.

**Error cases** (all mapped to GW ACK codes — see protocol notes §4): no
index within timeout (`ACK_NO_INDEX` — dead motor/no disc/belt failure), no
TRK00 during recalibrate (`ACK_NO_TRK0`), write on protected media
(`ACK_WRPROT`), select of an unwired unit (`ACK_NO_UNIT`), stream overflow
(`ACK_FLUX_OVERFLOW`). Firmware watchdog: motor and select auto-drop after a
host-configurable idle timeout (GW `gw_delay` parameters).

---

## 5. Electrical design (schematic-level)

Prose netlist; no CAD in this phase. Architecture: Pico module + one input
buffer + two open-drain output drivers + connectors + power. All logic DIP,
all passives through-hole (R8).

### 5.1 Input path (drive → MCU): 5 lines

RDATA, INDEX, TRK00, WPT, RDY. Drive side is open-collector; we must both
terminate the lines and protect the 3.3 V (non-5 V-tolerant) RP2040 pins
against drives with internal pull-ups to 5 V (§2.3).

| Item | Choice | Notes |
|---|---|---|
| Buffer | **CD4050B** (hex non-inverting, DIP-16), V_DD = 3.3 V | The classic level-down buffer: inputs tolerate up to 15 V regardless of V_DD, output swings 0–3.3 V. One package covers all 5 inputs + 1 spare |
| Termination pull-ups | **1 kΩ to +5 V** on RDATA; **4.7 kΩ to +5 V** on INDEX/TRK00/WPT/RDY | 1 kΩ on the data line preserves edges on a long ribbon (Shugart convention); the slow status lines don't need hard termination. Jumper to disconnect RDATA's pull-up when the drive terminates internally (bench-determined, §10 Q2) |
| RC glitch filter | 100 pF to GND on INDEX only | Index sensor bounce; RDATA must NOT be filtered (edge timing is the product) |

### 5.2 Output path (MCU → drive): 9 lines

DS0, DS1, MOTOR, DIR, STEP, SIDE1, DENSEL, WGATE, WDATA. Requirement: sink
40 mA against sub-1 kΩ drive-internal pull-ups (§2.3), open-drain, 5 V
domain, and never glitch WGATE during MCU reset.

| Item | Choice | Notes |
|---|---|---|
| Drivers | 2 × **74HCT06** (hex open-drain inverter, DIP-14), V_CC = 5 V | HCT input threshold (V_IH = 2 V) is met by 3.3 V GPIO; open-drain outputs rated for the bus. Inverting is convenient: GPIO high = line asserted (low) with a defined power-on state |
| Reset safety | 10 kΩ pull-**down** on the GPIO side of WGATE and MOTOR driver inputs | RP2040 GPIOs float at reset → pulled-down input → HCT06 output released (high) → write gate guaranteed deasserted while flashing/rebooting |
| Write-protect jumper | Series jumper in WGATE (remove = physically write-incapable) | Mirrors GW "Write-Protect Jumper" feature; archival units run open |

### 5.3 Power architecture

| Rail | Source | Consumers |
|---|---|---|
| +5 V | USB VBUS through the Pico's VSYS diode; also exported to the drive power header | Pico, 74HCT06s, pull-ups, drive **logic** (EME mechs; ~hundreds of mA budget) |
| +3.3 V | Pico's onboard regulator | CD4050, GPIO domain |
| +12 V | **Barrel jack (2.1 mm, center +12 V)** with reverse-polarity P-FET and 1 A polyfuse; NOT generated on-board | Drive **motor** rail (EME mechs). A 5→12 V boost was considered and rejected for V1: motor inrush is unmeasured (§10 Q3), boost adds cost/EMI/failure modes, and two of three target setups need no 12 V at all — the FD-1 is mains-powered (§2.4) and a bench PSU serves developers |
| Drive power out | 4-pin floppy-style header, **silkscreened with the 3"-swapped pinout** and a second silkscreen row for standard-Molex wiring | The §2.4 swapped-connector hazard is addressed by making the board the labelled source of truth; ship-with note in the build guide |

Protection: polyfuse on USB 5 V export (500 mA), TVS on VBUS and on the 12 V
input, 100 nF decoupling per IC + 10 µF bulk per rail. GND of USB, logic and
drive commoned at the connector (mandatory when the drive is externally
powered — GW wiki notes the same for their Dupont setup, ibid.).

### 5.4 Connector strategy

- **34-pin shrouded IDC male**, PC-numbered, Shugart signal assignment — the
  primary port; any 3.5"/5.25"/3" drive or the FD-1 (whose cable ends in a
  34-way edge connector per §2.1's table) attaches with a straight ribbon.
- **26-way FFC footprint** (2.54 mm through-hole alternative: 2×13 header)
  wired per the §2.2 mapping, for direct attachment of a bare EME-150/155
  mech pulled from a 664/6128 — adopting keirf's `gw-26` mapping. Pins 6/26
  left on solder jumpers until Q1 is bench-resolved.
- Both ports share the bus (only one drive cabled at a time; both selects
  available on both ports).

---

## 6. Host-side: what "out of the box" means

Because the device speaks the GW protocol on a USB CDC serial port:

1. `gw info`, `gw read --drive 0 --tracks c=0-39:h=0 dump.scp`, `gw rpm` all
   work with zero additions — the entire existing preservation toolchain is
   available for triage and cross-checks.
2. The emulator gains a `gw_host` module (§7) that speaks the same protocol
   directly — no dependency on the Python tools at runtime.

---

## 7. Emulator integration design

### 7.1 `gw_host` module

A new host-layer module (application layer, alongside `slotshandler.cpp` —
**not** under `src/hw`, which stays pure/heap-free): opens the CDC device,
implements the client side of the protocol in
[`gw-protocol-notes.md`](gw-protocol-notes.md), owns a worker thread, and
exposes three operations: `probe()` (GET_INFO + bus setup), `fetch(cyl) →
2×(raw flux ticks + index framing)`, and `dump(path)`. All device I/O happens
on the worker thread; the emulation thread only ever touches completed
buffers (same deferred pattern as `g_m4_http.drain_pending()`).

### 7.2 Feature (a): `--drive /dev/cu.usbmodem*` live passthrough

The mapping the whole design builds on: **`ensure_flux_cache` is already an
on-demand, per-cylinder decode with a miss path** (`src/hw/fdc.cpp`; docs
[`flux-media.md`](flux-media.md) §7). A miss on cylinder N becomes "command
the real drive: SEEK N, READ_FLUX 2 revolutions", and the simulated FDC's
BUSY phase — which already models motor spin-up (500 ms), SRT-timed seeks and
3.2 M-cycle revolutions ([`fdc-device.md`](fdc-device.md) §7) — is the
latency budget that hides the real mechanism.

**Plumbing (two variants, pick 1 at implementation review):**

- **V1 — SCP-in-memory shim (zero `src/hw` changes).** `gw_host` maintains a
  single in-memory SCP container (168-slot offset table, slots initially 0 =
  absent) and calls `fdc_attach_flux()` on it once. Fetched cylinders are
  appended as TDH + 2 revolutions of 16-bit big-endian words (25 ns ticks,
  rescaled from device ticks) and their slot offset patched in. The FDC
  decodes absent slots to empty tracks (`count = 0` — "a head over nothing
  reads nothing"), AMSDOS retries on No Data, and the retry lands after the
  fetch completes: eventual consistency for free. Cache-invalidation nuance:
  after patching a slot, the FDC's `cache_cyl` must be forced stale — with
  zero API additions that is only reachable by re-attaching, which is crude
  but correct (attach is cheap: header parse only).
- **V2 — provider hook (small, clean `src/hw` addition, NOT implemented
  now).** See §7.6.

**Trigger.** `gw_host`'s worker polls `fdc_peek()` (existing introspection
API) each frame for the head position and motor state; a head position whose
cylinder is missing from the shim triggers a fetch. The real drive is thus
commanded at *simulated seek start*, maximizing overlap with the simulated
BUSY window.

**Latency analysis vs the simulated BUSY phase.**

| Event | Simulated budget (§7 of fdc-device.md) | Real drive cost | Verdict |
|---|---|---|---|
| Motor spin-up | 500 ms (8 M cycles) | motor-on + first index ≤ ~500 ms | hidden ✓ |
| Long seek (39 tracks, AMSDOS SRT=0xA → 12 ms/step) | 468 ms | ~39 steps + settle ≈ 400–600 ms | ~hidden ✓ |
| 1-track seek + read | 12 ms + rotation | step + settle + index sync + 2 revs ≈ 450–650 ms | **not hidden** |

So the steady-state sequential case (the common one: AMSDOS loads walk
cylinders in order) is covered by **prefetch**: after serving cylinder N,
the worker fetches N+1 (and N−1) while the emulated FDC spends ≥ 2 simulated
revolutions (400 ms) reading N's sectors — real fetch time ≈ simulated
consumption time, so the pipeline stays full. For genuinely random seeks the
policy is configurable: `drive_latency=block` stalls the emulation clock
(exactly like a debugger pause — emulated time is unaffected, wall time hics
up to ~0.7 s) or `drive_latency=retry` lets AMSDOS's own retry loop absorb it
(authentic-feeling, occasionally audible as an extra revolution). Default:
`block`, because it is deterministic.

**Caching + invalidation (disc-change detection).** Fetched cylinders are
sticky for the session (a 40-track disc is ≤ ~6 MB of flux — trivially
RAM-resident; a completed session has organically dumped the disc). The
worker polls the RDY pin (`GET_PIN` 34) at ~2 Hz while the motor is off and
before every fetch: RDY deasserted → `fdc_eject_disk()` + shim reset; RDY
reasserted → fresh container, cold cache. 3" mechs have true RDY rather than
DSKCHG (§2.1), which makes this simple polling sound; its exact timing on
EME mechs is **needs-bench-confirmation** (§10 Q1). A manual
`gw eject`-equivalent IPC command backs it up.

### 7.3 Feature (b): `dump` tool → .scp archival

`koncepcja --gw-dump out.scp` (and an IPC command `gw dump <path>`): seek
0→39, READ_FLUX with revolutions = 3 (belt-and-braces above `kFluxRevs = 2`),
rescale ticks to 25 ns, emit an index-cued side-0 SCP per
[`flux-media.md`](flux-media.md) §1, then run the existing `flux_scp_probe`
+ `flux_scp_to_dsk` as an immediate readability report (per-track sector
counts, weak-sector report). Two-pass flip workflow for side B (§2.5). The
output is bit-identical in format to what the emulator already loads, and
cross-checkable with `gw read`.

### 7.4 Feature (c, later): write-back to real discs

Deferred to M4, designed-for now: the modified image (DSK or flux) is
MFM-encoded to a flux interval stream (the test suite's `flux_synth.h`
already synthesizes exactly this for the decoder tests — the encoder
methodology exists), sent via `WRITE_FLUX` index-cued per track, then
verified by read-back + `flux_scp_to_dsk` compare. Precompensation is a
non-issue at DD on 40-track media (GW applies none by default at DD). Scope
gate: write only whole tracks; no sector-level splice.

### 7.5 IPC surface

`gw status | attach <dev> | detach | dump <path> | eject` — mirrors the
existing `m4 http` command family; all mutation deferred to the main thread.

### 7.6 Small API additions `src/hw` needs (design only — DO NOT implement)

1. **`flux_decode_raw_rev(const uint16_t* words, size_t n, uint32_t tick_ns_x100 /* 2500 = 25.00 ns */, FluxTrack* out, uint8_t* payload, size_t cap)`** —
   the existing `flux_decode_track_rev` minus the SCP container walk: PLL +
   MFM over one caller-provided revolution of flux words. Lets `gw_host`
   (V2) skip building SCP containers entirely and removes the tick-rescale
   round-trip (the PLL's cell estimator absorbs the clock difference
   natively). Pure, heap-free, same contract; `flux_decode_track_rev`
   becomes a thin wrapper (container walk + call).
2. **`fdc_attach_flux_provider(const Device*, const FdcFluxProvider*)`** with
   `FdcFluxProvider { void* ctx; int (*decode)(void* ctx, uint8_t cyl, uint8_t rev, FluxTrack*, uint8_t* payload, size_t cap); uint8_t revs; uint8_t cylinders; }`
   — `ensure_flux_cache` calls `provider->decode` instead of
   `flux_decode_track_rev`. Live wiring exactly like `fdc_attach_flux`
   (never serialized); the SCP path becomes one built-in provider. This is
   the V2 that eliminates the re-attach hack and gives the live backend a
   first-class seam — and it is *testable* with a mock provider, no hardware.
3. **`fdc_flux_cache_invalidate(const Device*)`** — sets `cache_cyl = 0xFF`;
   needed by both variants on disc change.

---

## 8. Validation plan

| Stage | Test | Pass criterion |
|---|---|---|
| V-1 Bench loopback | Board A `WRITE_FLUX`s a known synthetic stream (from `flux_synth.h` vectors) on WDATA; board B (or the same board, WDATA jumpered to RDDATA, index simulated by a GPIO) captures it | Captured intervals match emitted within ±1 sample tick + a bounded constant offset; `FLUXOP_INDEX` framing intact |
| V-2 Protocol conformance | Run unmodified `gw info / read / rpm` against our firmware; diff behavior against a genuine Blue Pill GW F1 on the same drive | `gw` completes without errors; RPM within ±1 %; identical track coverage |
| V-3 Cross-validation dump | Same physical disc dumped by reference GW and by our board; both SCPs through `flux_scp_to_dsk` | **Byte-exact DSK equality** + equal weak-sector reports (raw SCPs will differ — tick clocks differ; the decode must not) |
| V-4 Acceptance corpus | The user's real GSX/plotter discs, dumped and booted in the emulator via both `--gw-dump`+load and live `--drive` passthrough | AMSDOS `cat`, program load and run identical to the known-good DSKs of the same discs |
| V-5 Protected/weak-bit statistics | A protected title with known weak sectors: dump ≥ 3 revolutions; reuse the FdcFlux tests' methodology (`test/hw/flux_test.cpp`: per-revolution payload divergence, ST1/ST2 per revolution) | Weak sectors show inter-revolution divergence on real captures; re-reads in the emulator return varying data, matching the documented Stage-3 behavior |
| V-6 Live-latency soak | Scripted IPC run: random seeks + sequential loads for 30 min under `drive_latency=block` and `=retry` | No deadlock (reuse `wait bp` deadlock-detector pattern from the IPC harness), no cache corruption, prefetch hit rate logged ≥ 90 % on sequential loads |
| V-7 Write round-trip (M4) | Format + write a generated DSK to a sacrificial CF2, read back on the reference GW | Byte-exact decode; then boot on a real CPC as the final authority |

---

## 9. BOM (2026 prices) and milestones

### 9.1 Bill of materials

USD; qty-1 from hobby distributors (Pimoroni/Adafruit/Mouser-class), qty-10
from LCSC/JLC-class where sane. Prices are estimates except where cited;
re-quote at M2.

| Item | Qty | Unit @1 | Line @1 | Line @10 (per unit) |
|---|---|---|---|---|
| Raspberry Pi Pico 2 (RP2350) | 1 | $5.00 (RRP — raspberrypi.com, ibid.) | $5.00 | $5.00 |
| CD4050B DIP-16 | 1 | $0.60 | $0.60 | $0.35 |
| 74HCT06 DIP-14 | 2 | $0.60 | $1.20 | $0.70 |
| 34-pin shrouded IDC header | 1 | $0.60 | $0.60 | $0.40 |
| 2×13 header (26-way port) | 1 | $0.40 | $0.40 | $0.25 |
| Barrel jack 2.1 mm + P-FET + polyfuses + TVS | 1 set | $1.20 | $1.20 | $0.80 |
| Floppy power header 4-pin | 1 | $0.30 | $0.30 | $0.20 |
| R/C kit (pull-ups, decoupling, bulk) | ~20 | — | $1.00 | $0.60 |
| Jumpers, DIP sockets ×3 | 1 set | $0.80 | $0.80 | $0.50 |
| 2-layer PCB ~80×60 mm | 1 | $4.00 (min-order share, qty 5) | $4.00 | $1.00 |
| **Total** | | | **≈ $15.10** | **≈ $9.80** |

On target: qty-1 lands at ~$15 (PCB minimum-order economics dominate), qty-10
under $10. Not counted, per R7: 12 V brick (~$8, only for bare EME mechs),
ribbon cable (~$2), the drive. Blue Pill variant BOM is ~$1–3 cheaper but
inherits the fake-chip lottery (§3.2).

### 9.2 Build difficulty

All through-hole; 3 DIP sockets, 2 headers, ~25 passives, one 2×20-pin module.
Comparable to a beginner kit (≈ 1–2 h with a fine tip); no rework tools. The
one sharp edge — 3" drive power polarity — is mitigated in copper + silk
(§5.3) and a build-guide warning box.

### 9.3 Milestones

| M | Deliverable | Exit criterion | Effort (rough) |
|---|---|---|---|
| **M1** Breadboard read | Genuine Blue Pill + stock GW firmware + hand-wired buffers on a real EME-155/FD-1; resolves Q1–Q3 | `gw read` produces an SCP that `flux_scp_to_dsk` decodes; bench answers logged | 1–2 weekends + parts lead time |
| **M2** PCB rev A | Carrier board per §5, Pico 2 + our firmware (read-only), V-1/V-2/V-3 green | Cross-validation byte-exact vs M1 reference | 2–3 weekends design + fab turnaround |
| **M3** Emulator passthrough | `gw_host`, `--gw-dump`, `--drive` live mode (V1 shim), IPC commands, V-4/V-5/V-6 green | User's GSX/plotter discs boot live from real hardware | 2–3 weekends (host code + tests) |
| **M4** Write support | `WRITE_FLUX` firmware path, write-back tool, WP jumper doc, V-7 green | Round-trip on sacrificial media verified on a real CPC | 2 weekends + caution |

V2 provider-hook refactor (§7.6) is scheduled inside M3 only if the shim's
re-attach invalidation proves annoying in practice; otherwise it is follow-up
work with its own review.

---

## 10. Risks and open questions

### 10.1 3" mechanism quirks

- **Belts.** EME-15x mechs are belt-driven and 40-year-old belts are dead or
  dying (documented replacement procedure and dimensions ~70×0.5×2.5 mm —
  CPCWiki *Changing the drive belt*,
  `https://www.cpcwiki.eu/index.php/Changing_the_drive_belt`; drive-belt notes
  also at `https://www.fvempel.nl/belt.html`). Symptom at the bridge: motor on
  but `ACK_NO_INDEX`. The build guide gets a belt-first troubleshooting order,
  and firmware distinguishes "no index" from "no drive" precisely so this is
  diagnosable.
- **READY timing (Q1).** How fast RDY asserts after motor-on + media-in on
  EME-150 vs EME-155 (which reportedly differ in readiness detection —
  CPCWiki forum, *Floppy drive EME-155 problem*), and whether pins 6/26 of
  the 26-way FPC carry RDY/+5 V. **needs-bench-confirmation → M1.**
- **Index on flipped media / sensor health (Q1b).** Single-sided mechs sense
  the index hole of the side in use; a flipped CF2 must still index cleanly.
  **needs-bench-confirmation → M1.**
- **Internal termination (Q2).** Unknown whether EME mechs pull their outputs
  to +5 V internally and how hard their inputs are pulled up (drives with
  < 1 kΩ input pull-ups exist per GW). Board carries jumperable termination
  (§5.1) so either answer is accommodated. **needs-bench-confirmation → M1.**
- **Motor current (Q3).** EME-15x 12 V inrush/steady current is undocumented
  in sources we could verify; the barrel-jack + 1 A polyfuse budget is an
  educated guess. **needs-bench-confirmation → M1** (bench PSU with current
  readout).

### 10.2 12 V handling mistakes

The single most destructive user error is the §2.4 swapped power connector —
mitigated by silkscreen, build-guide warning, and by not shipping a
pre-crimped PC-convention cable. Second is a 12 V-center-negative CPC brick
(the 6128's own 12 V plug is center-negative! — Power Supply page, ibid.)
plugged into our center-positive jack — mitigated by the reverse-polarity
FET, which makes it a non-event.

### 10.3 USB timing jitter — why it doesn't matter for capture

All flux timestamping happens **on the MCU** (PIO/timer counts between
edges); USB merely transports already-quantized interval integers. Host
scheduling jitter, CDC buffering, and OS latency can stall the *stream*, not
distort the *timestamps* — a stall only risks `ACK_FLUX_OVERFLOW`, which the
§4 buffering makes unreachable at DD rates. (Writes are the same in reverse:
the PIO replays pre-computed intervals from a deep buffer.) This is the same
argument the GW architecture rests on.

### 10.4 Licensing

- Greaseweazle **firmware and host tools are both Unlicense** (public
  domain) — verified via the GitHub license API for
  `keirf/greaseweazle-firmware` and `keirf/greaseweazle`, and the header of
  `inc/cdc_acm_protocol.h` ("free and unencumbered software released into
  the public domain"). Speaking the protocol and reusing firmware code is
  unencumbered.
- Adafruit_Floppy is **MIT** ([github.com/adafruit/Adafruit_Floppy](https://github.com/adafruit/Adafruit_Floppy))
  — compatible base for our firmware; we keep MIT for derived firmware.
- GW **hardware** design files carry no formal license text, only "may be
  freely reproduced" grants on the Design-Files wiki page (ibid.) — fine for
  reference, but our board is an original design and gets a real license:
  **CERN-OHL-S v2** (strongly-reciprocal), keeping derived boards open;
  documentation CC-BY-SA 4.0.

### 10.5 Project risks

- **Firmware write path least proven** (§3.2) — gated to M4 behind the WP
  jumper and sacrificial media only.
- **Reference-instrument dependency**: M1 needs one genuine Blue Pill or a
  purchased GW; budget $10–35 for it. It permanently doubles as the V-2/V-3
  reference.
- **Emulator coupling**: kept one-way — the emulator consumes a public,
  versioned protocol; nothing in `src/hw` learns about USB (§7.6 keeps the
  seam pure). If the bridge project stalls, every emulator addition remains
  useful for any Greaseweazle.

---

## 11. The three questions M1 must answer first

1. **Q1**: 26-way FPC pins 6/26 assignment + RDY assertion timing (and
   flipped-media index behavior) on real EME-150/155 and FD-1 hardware.
2. **Q2**: EME mech internal pull-up/termination reality (sets final pull-up
   values and the RDATA jumper default).
3. **Q3**: 12 V motor inrush/steady current (sets polyfuse rating and settles
   the barrel-jack-vs-boost decision for rev B).
