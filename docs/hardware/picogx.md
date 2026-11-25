# PicoGX cartridge save channel — Device spec

Source: the cartridge author's programming notes,
https://github.com/Neo2003/PicoGX/blob/master/Programming.md (summarised).
Where they are silent, the behaviour was measured on a real PicoGX in a
GX4000 (marked HARDWARE) or chosen here (marked MODEL).

## 1. What the cartridge sees

A cartridge has no write line: it sees only reads, as (bank, 14-bit
offset). It cannot tell through which CPU window (RMR2 low slot or the
upper-ROM window) a read arrives — the model therefore keys on the
BANK, never the CPU address. Only real CPU reads count; debugger and IPC
peeks (`mem_peek_cpu`, copy views) never touch the channel.

## 2. Commands

Three consecutive bank-0 reads at offsets `#133C`, `#25C4`, then `N`
(any other cartridge read in between — a code fetch included — breaks
the sequence; RAM reads do not) issue command `N` (its offset's low
byte):

| N | Effect |
|---|---|
| 5 | read mode: bank-0 reads return the save (random access, offset = save offset) |
| 6 | write mode: every bank-0 read stores its offset's low byte into the save, sequentially from offset 0 |
| 7 | stop: normal bank 0 again; if a write was open, the save is committed |

HARDWARE: in write mode a command sequence's own reads are NOT stored -
a save written as 256 bytes of data then stop ends at the data on the SD
card (byte for byte; the rest of the 16 KB untouched).  The model holds a
possible sequence's first reads and stores them only if the sequence
breaks (another bank-0 read, or a read of another bank).

HARDWARE: after the stop that commits a write the device is busy writing
its SD card: a read command issued straight after it is ignored (bank 0
keeps serving the ROM).  Returning to the ROM after any stop is immediate.
MODEL: commands are ignored and the ROM served for `system.picogx_busy_ms`
after a commit (default 100 ms, counted in 20 ms frames).  The real
duration was not measured - software should poll for read mode (bank 0
no longer showing the ROM) and retry rather than assume a delay.

## 3. Storage (host)

16 KB per cartridge: `<cart dir>/saves/<cart name>.SAV`, loaded when the
cartridge is plugged, written after each commit (the bridge's per-frame
sync, `sync_picogx`). A missing file reads as 16 KB of `0x00` (HARDWARE: a
PicoGX with no save for the cartridge reads back zeros).

Enable with `[system] picogx=1` (or `-O system.picogx=1`); off by default.
`[system] picogx_busy_ms` (default 100) sets the busy time after a commit.

## 4. State

`mem_state` v2 tail: the FSM (`pgx_seq`, `pgx_mode`, `pgx_wpos`, the
per-access latch) and a commit counter; v3 adds the busy count.  The 16 KB
buffer is host-owned wiring (zeroed in the blob, like the cartridge
image).  v1 and v2 memory blobs still load (the PicoGX starts idle).

## 5. Test

A cartridge that runs from RAM: wait 3 s after power-on, read the save,
stop, write 256 pad bytes and an 8-byte pattern, stop (the commit), read
again, stop, and show how long each switch took and the bytes read.  On a
real PicoGX (three power-ups): read mode and the ROM switch at once; the
first power-up reads zeros, later ones the pattern (the commit survives a
power-off); the read straight after the commit is refused.  The model
gives the same results, and the same .SAV byte for byte, on the fast and
the faithful tier.
