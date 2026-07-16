# Symbiface II — Device spec

The multi-function expansion board: **ATA/IDE (PIO)**, a **DS12887 RTC**
with 50 bytes of CMOS NVRAM, and a **PS/2 mouse** behind a multiplexed
FIFO. Full 16-bit I/O decode on `&FDxx` (high byte exactly `0xFD`). The
golden master is `src/symbiface.cpp` (ours, 2026 — it keeps serving
engine=0 until the Wave-3 cutover); this Device re-models it against the
hw-layer contract: caller-owned media, host-fed wall clock, no host I/O
inside a tick.

## 1. Port map (CPCWiki SYMBiFACE_II, as the golden master decodes it)

| Port | Read | Write |
|---|---|---|
| `&FD06` | IDE alternate status (= status, no side effects) | device control: bit 2 = SRST (board reset) |
| `&FD08`-`&FD0F` | IDE register file 0-7 (data, error, count, LBA0-2, drive/head, status) | (features, count, LBA0-2, drive/head, command) |
| `&FD10`, `&FD18` | PS/2 mouse FIFO pop | — |
| `&FD14` | RTC data (selected register) | RTC data |
| `&FD15` | — | RTC index (6-bit) |

Reads with SIDE EFFECTS (IDE data pop, mouse FIFO pop) follow the one-per-
ACCESS edge rule with an output latch across the access' master cycles —
the same discipline as the FDC data register.

## 2. IDE (ATA PIO), against caller-owned media

Two drives (master/slave, selected by drive/head bit 4). Each attachment
is a caller-owned **mutable** byte buffer (`symbiface_ide_attach(dev,
drive, uint8_t* img, size_t len)`); `total_sectors = len / 512`. WRITE
SECTORS mutates the buffer in place and raises a per-board **dirty flag**
(`sf2_media_dirty` / `sf2_media_mark_clean`) — persistence is the host's
job, exactly the FDC §10 story (the bridge writes the buffer back to its
.img on detach/stop).

Commands, verbatim from the golden master: READ SECTORS (0x20, multi-
sector chaining on data-register exhaustion), WRITE SECTORS (0x30, 512
bytes in then commit + chain), IDENTIFY (0xEC — the KONCEPCJA001 /
"konCePCja Virtual CF" identity block, CHS approximation + LBA capacity),
IDLE IMMEDIATE (0xE1) and INIT PARAMS (0x91) as DRDY no-ops; anything
else aborts (ERR + 0x04). Status bits: DRDY when a drive is attached, DRQ
while the sector buffer wants the CPU, ERR + error code (0x04 abort,
0x10 IDNF past the end). Transfers complete immediately per access (a CF
card's PIO latency is sub-microsecond at CPC bus speeds — the "fast IDE"
model is the hardware-honest one here).

## 3. RTC (DS12887): host-fed time + serialized CMOS

Registers 0-13 are the clock: the HOST feeds registers 0-9 (BCD sec, x,
min, x, hour, x, day-of-week, day, month, year — the golden master's
layout) via `sf2_rtc_set_time(dev, regs10[10])`, refreshed once per frame
by the bridge; the Device serves reads from the fed values (no `time()`
in the hw layer). Registers 10-13 read the golden master's constants
(0x26, 0x02, 0x00, 0x80). Registers 14-63 are the 50-byte CMOS NVRAM —
read/write, SERIALIZED (it is battery-backed state). Time registers are
read-only, as in the golden master.

## 4. PS/2 mouse: the multiplexed FIFO

Status byte: bits 7-6 = mode (00 no data, 01 X offset, 10 Y offset, 11
buttons), bits 5-0 = payload (signed 6-bit offsets; button bits D0 left,
D1 right, D2 middle, active high). The host feeds WHOLE mickeys + a
button mask (`sf2_mouse_feed(dev, dx, dy, buttons)` — SDL button order,
bit0 left / bit1 middle / bit2 right; the Device does the SF2 mapping and
the Y-axis flip). Large deltas are CHUNKED into multiple ±31/32 packets
(the golden master clamps-and-drops because it is fed per SDL event; the
Device is fed per frame, so chunking preserves the same observable motion).
Button packets are pushed on CHANGE only. The 64-deep FIFO drops new
entries when full (golden master). FIFO pops are empty-safe (mode 00).

## 5. State, reset, host API

Serialized: both drives' register files + sector buffers + transfer
state, RTC index + CMOS, mouse FIFO + last-buttons, plugged. Live wiring
(never serialized): the two image attachments and the host-fed time.
Reset (bus reset or SRST): the golden master's `symbiface_reset` —
register files and FIFO clear, CMOS and attachments persist; plugged
persists. `Sf2Regs { plugged, active_drive, ide_status, rtc_index,
fifo_used }` via peek.
