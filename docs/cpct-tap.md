# CPCT bus tap

The CPCT tap records what a CPC's Z80 does on its bus: every memory and I/O
access, with the T-state it started on. It writes these records in the CPCT v0
trace format. CPCień, the shadow twin, reads them. It rebuilds the machine's
video from the trace alone and checks the result against the frames konCePCja
drew. The CoPyCat RTL bench runs the same rule on its own pins, so the two
traces can be compared record for record.

Source: `src/hw/cpct_tap.h` / `.cpp`. The header comment holds the recording
rule in full. Tests: `test/cpct_tap_test.cpp`.

## What it records

The tap is a `Device` on the board. It reads the committed CPU bus once per
16 MHz master cycle and drives nothing, so adding it changes no other chip's
behaviour.

| Record | When | `flags` |
|---|---|---|
| opcode fetch | `m1 & mreq & rd` | `M1 \| MEM_RD` |
| memory read | `mreq & rd & !m1` | `MEM_RD` |
| memory write | `mreq & wr` | `MEM_WR` |
| I/O read | `iorq & rd & !m1` | `IO_RD` |
| I/O write | `iorq & wr & !m1` | `IO_WR` |
| /INT fell | `cpu.irq` rises | `INT` (line event) |
| /RESET fell | `cpu.reset` rises, and once at cycle 0 on power-on | `RESET` (line event) |
| lost events | the buffer was full | `0x00` (GAP, last record of the file) |

- One access produces one record. Its `cycle` is the master cycle on which
  the strobe first appeared, divided by 4, so it counts 4 MHz T-states.
  `addr` and `data` hold the bus as last seen while the strobe was active. For
  a read that is the byte the CPU took. For a write it is the byte the CPU
  drove.
- If /HALT was low at any point during the access, the record also gets the
  `HALT` flag.
- Refresh cycles and interrupt acknowledges (`iorq` together with `m1`) are
  not accesses, so they produce no record. Idle cycles produce no record
  either: the `cycle` field carries the time between records.
- A line event is a record with `addr = data = 0`. If its edge falls while an
  access is open, including on the master cycle the access opens, it is
  written just after that access's record. This keeps the stream in
  non-decreasing cycle order. Up to four edges can wait behind one access; a
  fifth and later edge adds its flag to the fourth marker.
- The record buffer is bounded (`cpct_tap_set_capacity`). Once it is full,
  further records are counted but not stored. `cpct_tap_write` then ends the
  file with a GAP record: `addr` holds the lost count, saturated at `0xFFFF`,
  and `cycle` is the tap's current cycle.
- At write time an access whose strobe is still active is left out, because
  it has not ended. Line events already waiting behind it are written.

Known artefact, HALT: this Z80 runs no bus cycles while halted. It holds
/HALT low and waits. Real silicon keeps fetching NOPs, one M1 cycle every
4 T-states, each with /HALT low. So in a konCePCja trace no access record
carries `HALT`; it appears only on the /INT or /RESET line event that ends
the halt. A hardware or RTL capture of the same program has an
`M1 | MEM_RD | HALT` record every 4 T-states while halted, and a comparison
has to skip those.

Known artefact, T1: this Z80 holds T1 until the Gate Array's microsecond grid.
Real silicon asserts /MREQ in T1 wherever T1 falls, then waits in T2.
Because of that, a strobe in a konCePCja trace can start up to 3 T-states
later than the same strobe on hardware. The end of the strobe is the same,
and so are the instruction totals.

## Format

The canonical spec is `docs/trace-format.md` in the CPCień repository
(`~/src/cpc/cpcien`). A file is a 32-byte little-endian header (`"CPCT"`,
version 0, crtc_type, machine, crystal_hz = 4000000, start_cycle) followed
by 8-byte records (`u32 cycle`, `u8 flags`, `u8 data`, `u16 addr`).
`cpct_encode_header` and `cpct_encode_record` produce that layout, and the
test checks it byte for byte.

## The rig

`sim/cpct_tap_rig.cpp` is a headless board with no SDL. It has the Z80, Gate
Array, CRTC, PPI, PSG and memory, plus the tap. It boots a lower ROM you
supply:

```bash
make ARCH=macos cpct_tap_rig          # not part of the default build
head -c 16384 rom/cpc6128.rom > os.bin
tail -c 16384 rom/cpc6128.rom > basic.bin
./cpct_tap_rig --rom os.bin --upper basic.bin --out boot.cpct \
               --cycles 48000000 --dump-frames frames/
```

| Option | Meaning |
|---|---|
| `--rom FILE` | 16 KB lower ROM (required) |
| `--out FILE` | trace to write (required) |
| `--cycles N` | master cycles to run, 16 per µs (default 2000000) |
| `--upper FILE` | 16 KB upper ROM 0, for example BASIC |
| `--key ROW,COLS` | hold a keyboard row (0-15): the columns byte (0-255), 0 = pressed; up to 16 times |
| `--expansion KB` | attach a dk'tronics-style RAM expansion (64-512) |
| `--capacity N` | tap buffer in records (default 1<<24) |
| `--screen FILE` | dump RAM `&C000-&FFFF` at the end |
| `--garegs FILE` | dump the Gate Array's mode and inks at the end |
| `--dump-frames DIR` | write the video Device's framebuffer to `DIR/frame_NNNNN.ppm` at every completed frame |

`--dump-frames` provides the per-frame oracle for the shadow twin. The video
Device is attached only when this option is given. It only listens to the RAM
fetch bus and the CRTC timing, so the trace is the same with or without it.
Each frame is 768×272 RGB, the size the main emulator's framebuffer uses.

Numbers must be whole and in range (`0x` hex is accepted); anything else is
rejected with exit status 2. The exit status is 0 when the run worked, 1 on
an I/O failure, 2 for bad arguments or input files, and 3 when the trace was
written but is not usable: records were dropped (raise `--capacity`) or no
access was recorded at all.

The file header says `crtc_type = 0` and `machine = 2` (6128), which match
the rig's board.

`sim/psg_oracle_rig.cpp` (`make psg_oracle_rig`) is the PSG counterpart. It
drives the PSG Device through the AY bus from a scripted register program and
dumps the generators once per PSG clock, for `copycat/sim/psg`.
