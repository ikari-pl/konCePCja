# PicoGX cartridge save channel — Device spec (local zakcpc branch)

Source: the cartridge author's programming notes,
https://github.com/Neo2003/PicoGX/blob/master/Programming.md (summarised;
where they are silent, the choice made here is marked MODEL).

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

MODEL: in write mode the command reads themselves are bank-0 reads and
are stored too (the stop sequence leaves `3C C4 07` after the data). The
notes do not say; software should not depend on bytes after its own data.

## 3. Storage (host)

16 KB per cartridge: `<cart dir>/saves/<cart name>.SAV`, loaded when the
cartridge is plugged, written after each commit (the bridge's per-frame
sync, `sync_picogx`). MODEL: a missing file reads as 16 KB of `0x00`
(the real cart's blank contents are undocumented).

Enable with `[system] picogx=1` (or `-O system.picogx=1`); off by default.

## 4. State

`mem_state` v2 tail: the FSM (`pgx_seq`, `pgx_mode`, `pgx_wpos`, the
per-access latch) and a commit counter; the 16 KB buffer is host-owned
wiring (zeroed in the blob, like the cartridge image). v1 memory blobs
still load (the PicoGX starts idle).

## 5. Test

zakcpc `tools/emu/tests/picogx_test.asm`: read the blank save, write a
pattern, read it back, check the ROM returns after stop; a second run reads
the pattern from the .SAV. Identical on the fast and the faithful tier.
