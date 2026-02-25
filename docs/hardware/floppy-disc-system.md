# CPC Floppy Disc System

Hardware reference for the Amstrad CPC disc subsystem. This describes the
real hardware, not the emulator's implementation.

## Overview

The CPC 664/6128 has a built-in 3" floppy disc drive (Hitachi HT-3P or
compatible). The CPC 464 can use an external DDI-1 disc interface. All
models use the same FDC and firmware interface.

```
                          ┌─────────────────┐
  Z80 CPU ──── I/O bus ───┤  uPD765A (FDC)  ├──── drive cable ──── 3" drive
                          │                 │
  &FB7E ── Main Status ──→│  Status Register │
  &FB7F ── Data ─────────→│  Data Register   │
                          │                 │
                          │  Internal:       │
                          │  ┌─ PLL ────────┐│
                          │  │ clock recovery││
                          │  └──────────────┘│
                          │  ┌─ MFM decoder ┐│
                          │  │ flux → bytes  ││
                          │  └──────────────┘│
                          │  ┌─ CRC logic ──┐│
                          │  │ CCITT CRC-16  ││
                          │  └──────────────┘│
                          └─────────────────┘
```

## What the CPC sees vs. what the disc stores

### The CPC never sees raw flux data

The uPD765A completely abstracts the physical disc surface. The Z80
interacts with it through just two I/O ports:

| Port   | Name            | Access | Purpose                        |
|--------|-----------------|--------|--------------------------------|
| &FB7E  | Main Status Reg | Read   | Ready/busy, direction, DIO     |
| &FB7F  | Data Register   | R/W    | Command bytes in, data in/out  |

The FDC handles everything between the magnetic surface and those two
ports internally:

```
Disc surface          Flux transitions (analog)
    ↓
Drive head            Analog signal from magnetic coating
    ↓
uPD765A PLL           Clock recovery — locks onto data rate
    ↓
MFM/FM decoder        Flux transition pairs → data bits
    ↓
Shift register        Bits → bytes (8 bits at a time)
    ↓
CRC checker           Validates sector integrity (CCITT CRC-16)
    ↓
Data Register         One byte at a time via I/O port &FB7F
    ↓
Z80 CPU               Reads byte, stores to RAM
```

The Z80 **never** sees:
- Raw flux transitions or their timing
- MFM clock bits (only data bits reach the CPU)
- Sync marks (A1 A1 A1 with missing clock — FDC uses these internally)
- Gap bytes (inter-sector padding — FDC skips over them)
- CRC bytes (FDC checks them and reports pass/fail in status registers)

The Z80 **does** see (via FDC status registers):
- Whether a sector was found or not (ST1 bit 2: No Data)
- Whether a CRC error occurred (ST1 bit 5: Data Error)
- Whether the wrong sector size was found (ST1 bit 0: Missing Address Mark)
- Sector ID fields: C, H, R, N (cylinder, head, record number, size code)
- End-of-transfer conditions (Terminal Count, overrun)

### Implications for copy protection

Copy protection exploits the gap between what the FDC reports and what a
normal disc format provides:

| Trick              | How it works                                     | What FDC tells the CPC                  |
|--------------------|--------------------------------------------------|-----------------------------------------|
| Weak/fuzzy bits    | Area of disc with unreliable flux                | Different data each read attempt        |
| Missing sectors    | Sector ID exists but no data field follows        | ST1: No Data error                      |
| Bad CRC sectors    | Intentionally corrupted CRC                      | ST1: Data Error, but data still arrives |
| Non-standard sizes | Sector N≠2 (not 512 bytes)                       | FDC handles it, reports N in result     |
| Overlapping sectors| Two sectors claim same C/H/R, different data      | Which one you get depends on head position |
| Extra sectors      | More than 9 sectors per track                    | FDC finds them during READ ID scan      |
| Speed variation    | Track written at non-standard RPM                | Timing-dependent: overruns possible     |

All these tricks are visible to the CPC only through the FDC's status
registers and data output — never through direct flux access.

## FDC command protocol

### Command phases

Every FDC operation follows three phases:

1. **Command phase** — Z80 writes command bytes to &FB7F
2. **Execution phase** — FDC reads/writes disc, Z80 transfers data via &FB7F
3. **Result phase** — Z80 reads status bytes from &FB7F

The Main Status Register (&FB7E) gates access: bit 7 (RQM) says "ready
for transfer", bit 6 (DIO) says "direction: 0=CPU→FDC, 1=FDC→CPU".

### Key commands

| Command       | Code  | Purpose                                    |
|---------------|-------|--------------------------------------------|
| READ DATA     | 0x06  | Read sector(s) — the workhorse command     |
| WRITE DATA    | 0x05  | Write sector(s)                            |
| READ ID       | 0x0A  | Read next sector header passing under head |
| FORMAT TRACK  | 0x0D  | Write an entire track with sector layout   |
| SEEK          | 0x0F  | Move head to target cylinder               |
| RECALIBRATE   | 0x07  | Move head to track 0                       |
| SENSE INT     | 0x08  | Acknowledge seek/recalibrate completion    |
| SPECIFY       | 0x03  | Set step rate, head load/unload times      |
| SCAN EQUAL    | 0x11  | Compare disc data with memory pattern      |

### READ DATA example

```
Command phase (9 bytes written to &FB7F):
  Byte 0: 0x46        (READ DATA, MFM mode, skip deleted = no)
  Byte 1: 0x00        (drive 0, head 0)
  Byte 2: C = 0x00    (cylinder / track number)
  Byte 3: H = 0x00    (head number)
  Byte 4: R = 0xC1    (sector number — AMSDOS starts at &C1)
  Byte 5: N = 0x02    (sector size: 2 = 512 bytes)
  Byte 6: EOT = 0xC9  (last sector number on track)
  Byte 7: GPL = 0x2A  (gap length)
  Byte 8: DTL = 0xFF  (data length, ignored when N≠0)

Execution phase:
  FDC reads sector from disc, Z80 reads 512 bytes from &FB7F
  (Must read fast enough — FDC will overrun if Z80 is too slow)

Result phase (7 bytes read from &FB7F):
  Byte 0: ST0         (status register 0 — interrupt code, drive)
  Byte 1: ST1         (status register 1 — errors)
  Byte 2: ST2         (status register 2 — more errors)
  Byte 3: C           (cylinder where operation ended)
  Byte 4: H           (head)
  Byte 5: R           (sector number — advanced to next)
  Byte 6: N           (sector size code)
```

### Status register error bits

**ST0** (bits 7-6 = interrupt code):
- 00 = Normal termination
- 01 = Abnormal termination (error occurred)
- 10 = Invalid command
- 11 = Ready signal changed during operation

**ST1** (error flags):
| Bit | Name | Meaning |
|-----|------|---------|
| 7   | EN   | End of cylinder — tried to read past last sector |
| 5   | DE   | Data Error — CRC check failed |
| 4   | OR   | Overrun — Z80 didn't read/write fast enough |
| 2   | ND   | No Data — sector ID not found |
| 1   | NW   | Not Writable — disc is write-protected |
| 0   | MA   | Missing Address Mark — no valid sector header |

**ST2** (additional flags):
| Bit | Name | Meaning |
|-----|------|---------|
| 6   | CM   | Control Mark — read a deleted data mark |
| 5   | DD   | Data Error in Data field (vs. header) |
| 1   | BC   | Bad Cylinder — sector C field ≠ requested |
| 0   | MD   | Missing Data Address Mark |

## Physical disc format

### MFM encoding (Modified Frequency Modulation)

The 3" discs use MFM encoding at 250 kbit/s (double density):

- Each data bit occupies a **bit cell** (4 µs at 250 kbit/s)
- A flux transition at the **centre** of a cell represents a `1`
- No transition at the centre represents a `0`
- A **clock** transition at the cell boundary is inserted only between
  consecutive `0` data bits (to keep the PLL locked)

```
Data bits:     1   0   0   1   1   0   1   0
MFM:          ─╥───╥─╥───╥─╥─────╥───╥─╥───
               D   C D   D D     C   D D
               (D = data transition, C = clock transition)
```

The FDC's PLL locks onto these transitions and separates clock from data
automatically. The Z80 never sees the MFM layer.

### Track layout (standard AMSDOS format)

A standard CPC track contains 9 sectors of 512 bytes each:

```
┌──────┬──────────┬─────┬──────────┬─────────┬──────────┬─────┐
│ GAP  │ Sector 1 │ GAP │ Sector 2 │   ...   │ Sector 9 │ GAP │
│ 4A   │ &C1      │ 22  │ &C2      │         │ &C9      │ 4E  │
└──────┴──────────┴─────┴──────────┴─────────┴──────────┴─────┘
```

Each sector on the track:

```
┌──────────────────── Sector ──────────────────────┐
│                                                   │
│  ┌─ ID Field (IDAM) ─┐   ┌─ Data Field (DAM) ─┐ │
│  │ A1 A1 A1 FE       │   │ A1 A1 A1 FB        │ │
│  │ C  H  R  N        │   │ 512 bytes of data   │ │
│  │ CRC-hi CRC-lo     │   │ CRC-hi CRC-lo       │ │
│  └────────────────────┘   └─────────────────────┘ │
│                                                   │
│  GAP2 (22 bytes of &4E between ID and Data)       │
└───────────────────────────────────────────────────┘
```

- **A1 A1 A1** — sync bytes with a missing clock bit (the FDC recognises
  this special pattern to find the start of a field)
- **FE** — ID Address Mark (identifies a sector header)
- **FB** — Data Address Mark (identifies sector data; F8 = deleted data)
- **C, H, R, N** — Cylinder, Head, Record (sector number), size code
- **CRC** — CCITT CRC-16 over the address mark + field data

### AMSDOS disc geometry

| Parameter        | DATA format  | SYSTEM format | IBM format  |
|------------------|-------------|---------------|-------------|
| Tracks           | 40          | 40            | 40          |
| Sides            | 1           | 1             | 1           |
| Sectors/track    | 9           | 9             | 8           |
| Bytes/sector     | 512         | 512           | 512         |
| First sector ID  | &C1         | &41           | &01         |
| Directory track  | 0           | 2             | 0 (reserved)|
| Reserved tracks  | 0           | 2 (boot)      | 1           |
| Block size       | 1024        | 1024          | 1024        |
| Dir entries      | 64          | 64            | 64          |
| Total capacity   | 180 KB      | 169 KB        | 160 KB      |

The sector numbering starting at &C1 (not &01) is an Amstrad convention.
The FDC doesn't care — it matches whatever C/H/R/N values are in the
sector headers on the disc.

## Disc image formats

### DSK (standard)

Sector-level format. Stores decoded sector data only — no gaps, sync
marks, or CRC bytes. All sectors on a track must be the same size.

```
Header (256 bytes): "MV - CPCEMU Disk-File\r\n..."
  Tracks, sides, track size

Per track (track header + sector data):
  Track Info block (256 bytes): sector count, size, C/H/R/N per sector
  Sector data: raw bytes, concatenated
```

### Extended DSK (EDSK)

Enhanced sector-level format. Supports variable sector sizes per track,
individual sector status bytes (for emulating CRC errors), and per-track
size information.

Handles most copy protection tricks except:
- Weak/fuzzy bits (some EDSK implementations use duplicate sectors)
- Timing-dependent protections
- Flux-level anomalies

### Flux-level formats (IPF, SCP, KryoFlux)

Store actual flux transition timings from the disc surface. These
preserve **everything** including all copy protection tricks, speed
variations, and physical anomalies.

Not currently supported by konCePCja — see issue konCePCja-5j7.

To use these, an emulator needs a **bitstream disc model**: a layer below
the FDC that simulates the PLL, MFM decoder, and track rotation, feeding
the FDC bit-by-bit as if reading from a real disc.

## Motor and drive control

The FDC does not control the drive motor directly on the CPC. Instead:

- **Motor ON/OFF**: PPI Port A, bit 4 (active HIGH to turn motor on).
  The CPC firmware controls this.
- **Drive select**: FDC commands include a drive select field (US0/US1),
  but the CPC normally has only one drive (drive 0).
- **Head select**: FDC commands include a head select field. The standard
  3" drive is single-sided, but the disc can be flipped manually to
  access "side 2" — which becomes side 0 of the other physical surface.
- **Ready signal**: The drive asserts READY when the disc is inserted and
  spinning at speed (~300 RPM, ~200 ms spin-up).
- **Track 0 sensor**: The drive signals when the head is at track 0,
  used by the RECALIBRATE command.

## Timing

| Parameter          | Value        | Notes                           |
|--------------------|--------------|---------------------------------|
| Rotation speed     | 300 RPM      | 200 ms per revolution           |
| Data rate          | 250 kbit/s   | MFM double density              |
| Bytes per track    | ~6250        | 250000 bits / 8 / (300/60)      |
| Track capacity     | ~6250 bytes  | Usable: ~5990 (gaps take rest)  |
| Sector read time   | ~16 ms       | 512 bytes at 250 kbit/s + gaps  |
| Full track read    | ~200 ms      | One revolution                  |
| Step time          | 3-12 ms      | Head movement per track (varies)|
| Head settle time   | 15 ms        | After seek, before read/write   |

The `max_track_size` in koncepcja.cfg (default 5990) reflects the usable
bytes per track after accounting for gaps and sync fields.

## Further reading

- **uPD765A datasheet** — Complete FDC register and command reference
- **Amstrad CPC Firmware Guide** — Chapter 14: The Disc Drive
- **CPCEMU DSK format** — Marco Vieth's original disc image spec
- **Kevin Thacker's EDSK spec** — Extended DSK format documentation
- **Jean-François Del Nero's HxC** — Disc image format conversions
- **SPS/CAPS IPF specification** — Flux-level preservation format
