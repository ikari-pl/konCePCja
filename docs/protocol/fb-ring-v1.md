# The frame ring, version 1

Every running core publishes the machine's canonical frame to a named
shared-memory region, so a frontend in another process can show it without a
copy over a socket (beads-0ir.2, phase 4 of beads-cv2). This document is the
contract for readers; the code is `src/kcpc/fb_ring.h` (layout, writer,
reader), `src/shm_region.{h,cpp}` (the OS shim) and `src/kcpc/fb_publish.*`
(the core's side).

The KCPC binary server does not name the ring yet: `FB_INFO` and `EVT_FRAME`
gain it in beads-0ir.3. Until then the name follows from the core's pid,
which the startup manifest reports.

## Terms

- **Canonical frame**: the picture the emulated machine drew, RGB24,
  768 pixels by 272 rows, 2304 bytes a row, top row first, before any window
  scaling, scanlines or video style. The text IPC's `hash frame` is the CRC32
  of exactly these bytes.
- **Slot**: room for one frame in the region. There are 4.
- **Sequence number** (`seq`): the frame's number in the ring, 1 for the
  first frame the core published, one more for each frame after.
- **Producer**: the core's frame runner (the GUI build's Z80 thread, or the
  main thread under `-H` and in the UI-free build). **Reader**: any process
  that maps the region read-only.

## Name and lifetime

| Platform | Name | Created with |
|----------|------|--------------|
| Linux, macOS | `/kcpc-fb-<pid>` | `shm_open` + `mmap` |
| Windows | `Local\kcpc-fb-<pid>` | `CreateFileMappingA` (page file) + `MapViewOfFile` |

`<pid>` is the core's process id in decimal. macOS refuses POSIX names longer
than 31 characters; this one is at most 19.

- The region exists from before the startup manifest is printed until the
  core exits. A frame is published at **every** frame completion, whether or
  not anybody reads (user decision 2026-10-26); a paused or stopped machine
  publishes nothing, and the latest frame stays readable.
- POSIX: the core unlinks the name on every exit that cleans up: `quit`,
  closing the window, `-E`, `-B`, SIGTERM and SIGINT. A core that dies
  without cleaning up (SIGKILL, a crash, a second Ctrl+C) leaves the name
  behind. A later core that gets the same pid replaces it at startup; a
  reader that finds a region whose `producer_pid` is no longer running is
  looking at such a leftover.
- Windows: the region lives while any process has it open, so it goes with
  the core however the core ends (or with the last reader, after that). The
  `Local\` namespace is the session's: `Global\` needs a privilege a normal
  user does not have.
- The producer never waits for a reader and never learns that one exists.

## Layout

All integers little-endian. Offsets in bytes from the start of the region.

| Offset | Size | Field | Version 1 |
|-------:|-----:|-------|-----------|
| 0 | 4 | `magic` | `FBUF` (the bytes 46 42 55 46); 0 until the rest of the header is written |
| 4 | 4 | `version` | 1 |
| 8 | 4 | `header_bytes` | 256 |
| 12 | 4 | `format` | 1 = RGB24, three bytes a pixel, R first |
| 16 | 4 | `width` | 768 |
| 20 | 4 | `height` | 272 |
| 24 | 4 | `stride` | 2304 (bytes a row) |
| 28 | 4 | `ring_slots` | 4 |
| 32 | 8 | `slot_bytes` | `stride * height` = 626688 |
| 40 | 8 | `slot_stride` | `slot_bytes` rounded up to 64 |
| 48 | 8 | `slots_offset` | 256 |
| 56 | 8 | `producer_pid` | the core's pid |
| 64 | 8 | `latest_seq` | seq of the latest whole frame; 0 before the first |
| 72 | 8 | `latest_slot` | the slot that frame is in |
| 80 | 8 × 4 | `slot_seq[i]` | `2 * seq` when slot i holds frame seq whole; odd while it is being written; 0 when never written |
| 112 | 144 | reserved | 0 |
| `slots_offset + i * slot_stride` | `slot_bytes` | slot i | `height` rows of `stride` bytes |

`magic`, `latest_seq`, `latest_slot` and `slot_seq[]` change while readers
read them: they are atomic words, written with release ordering, and a
reader on a weakly ordered CPU (ARM) must load them with acquire ordering
where the steps below say so. The other fields are written once, before
`magic`.

## Writing (what the core does)

Frame n goes to slot `n % 4`:

1. `slot_seq[slot] = 2n - 1` (odd: being written), then a release fence.
2. The 626688 bytes of the frame are copied into the slot.
3. `slot_seq[slot] = 2n` (release).
4. `latest_slot = slot`, then `latest_seq = n` (release).

## Reading

1. Load `magic` (acquire). Anything but `FBUF`: not ready, or not a ring.
   Check `version`, `format`, and that `slots_offset + ring_slots *
   slot_stride` fits in the region.
2. Load `latest_seq` (acquire). 0: nothing published yet.
3. Load `latest_slot`, then `s1 = slot_seq[latest_slot]` (acquire). If `s1`
   is 0 or odd, start again at 2.
4. Copy the slot's `slot_bytes` bytes.
5. Acquire fence, then load `s2 = slot_seq[latest_slot]`. If `s2 != s1` the
   producer rewrote the slot during the copy: throw the copy away and start
   again at 2. Otherwise the copy is frame `s1 / 2`, whole.

Step 5 fails only when the producer publishes four frames during one copy
(at 50 frames a second, 80 ms; an unpaced core can do that, so a reader
retries). To wait for the next frame, compare `latest_seq` with the last one
read; `EVT_FRAME` will announce each one (beads-0ir.3).

## Cost

One copy of 626688 bytes per frame on the frame runner, before the host blit
and the pacer's sleep. The measured cost is in the PR that added the ring
(beads-0ir.2).
