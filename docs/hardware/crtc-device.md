# CRTC (HD6845S) — clean-room Device implementation reference

Language-neutral spec for the clean-room CRTC **Device** (`src/hw/crtc`), companion to
`z80.md` and `gate-array-device.md`. For real-chip detail see the hardware reference
`docs/hardware/crtc.md`; **this** doc is the spec the Device code follows — the
character-timing engine that generates HSYNC/VSYNC/DISPEN and the MA/RA video address.

Scope: the **timing core** (§2–§5) first — the counters and the HSYNC/VSYNC/DISPEN/MA/RA
outputs, validated against the standard CPC register set (§6). CRTC types 0/1 first;
the type-specific quirks (§7) follow. Rendering/palette is the Gate Array + video
backend's job, not the CRTC's.

---

## 1. Role and bus position

- Clocked at **1 MHz** (`clk.crtc`, one tick per character = 1 µs).
- Drives the **video bus** `vid`: `hsync`, `vsync`, `dispen` (display enable), `cursor`,
  `ma` (14-bit memory address), `ra` (row/raster address).
- The Gate Array counts its `hsync` for the 300 Hz raster interrupt and its `vsync` for
  the frame resync (see gate-array-device.md §4); the video backend uses `ma`/`ra`/
  `dispen` to fetch and render.
- Programmed by the CPU via I/O: **select register** with a write to `&BCxx`
  (`A14=0,A13=0` … port `0xBC`), **write the value** to `&BDxx` (`0xBD`). Read-back of
  R14–R17 via `0xBE`/`0xBF` on some types.

---

## 2. Registers (R0–R17)

| Reg | Name | Unit | Std CPC | Meaning |
|---|---|---|---|---|
| R0 | Horizontal Total | chars−1 | 63 | line = R0+1 = 64 chars = 64 µs |
| R1 | Horizontal Displayed | chars | 40 | active chars per line |
| R2 | Horizontal Sync Position | chars | 46 | HSYNC starts here |
| R3 | Sync Widths | — | 0x8E | low nibble = HSYNC width (chars) = 14; high nibble = VSYNC width (lines) = 8 |
| R4 | Vertical Total | rows−1 | 38 | frame = (R4+1) char rows (+ R5) |
| R5 | Vertical Total Adjust | scanlines | 0 | extra scanlines after the last row |
| R6 | Vertical Displayed | rows | 25 | active char rows |
| R7 | Vertical Sync Position | rows | 30 | VSYNC starts at this char row |
| R8 | Interlace/skew | — | 0 | (type-dependent; ignore for now) |
| R9 | Max Raster Address | scanlines−1 | 7 | scanlines per char row = R9+1 = 8 |
| R10/R11 | Cursor start/end raster | — | — | cursor (text mode; CPC rarely uses) |
| R12/R13 | Display Start Address hi/lo | — | 0x30,0x00 | base MA (screen at 0xC000 → MA 0x3000) |
| R14/R15 | Cursor Address hi/lo | — | — | cursor position |
| R16/R17 | Light Pen hi/lo | — | RO | latched on light-pen strobe |

Standard CPC screen: 64 µs line × 312 lines = 20 ms = **50 Hz**; 39 rows × 8 scanlines
= 312 (R4=38 → 39 rows, R9=7 → 8 scanlines, R5=0). Active area 40×25 chars × 8 = 200
scanlines = the 320×200-ish display.

---

## 3. The counter engine (per 1 MHz character tick)

Three nested counters, all advancing off `clk.crtc`:

- **Horizontal char counter `hcc`** (0..R0): `hcc++` each tick; at `hcc == R0` it wraps
  to 0 on the next tick and triggers a **new scanline** (advance `vcc`/`ra`).
- **Raster counter `ra`** (0..R9): advances each scanline; at `ra == R9` it wraps to 0
  and triggers a **new character row** (advance `crow`).
- **Character-row counter `crow`** (0..R4): advances each char row; at `crow == R4`
  (after its last raster line, plus R5 adjust scanlines) it wraps to 0 → **new frame**.
- **Vertical total adjust**: after `crow` passes R4, `R5` extra scanlines run before the
  frame restarts (fine 312-line tuning).

A **scanline = R0+1 char ticks**; a **char row = (R9+1) scanlines**; a **frame =
(R4+1) char rows + R5 scanlines**.

---

## 4. HSYNC / VSYNC / DISPEN

- **HSYNC**: asserted while `hcc` is in `[R2, R2 + hsync_width)` where hsync_width =
  `R3 & 0x0F` characters (0 → treated as 16 on some types; CPC uses a non-zero width).
- **VSYNC**: asserted starting when `crow` first reaches R7 (at `ra==0`), for
  `vsync_width` scanlines (`R3 >> 4`, or a fixed 16 on type 0/1 where the high nibble is
  ignored — confirm per type in §7). The Gate Array only needs the VSYNC **rising edge**.
- **DISPEN** (display enable): `hcc < R1 && crow < R6`. True → active pixels; false →
  border. (Refined by R6/R8 skew and the "MA overflow" border cases on real types.)
- **CURSOR**: `ma == cursor_addr (R14/R15)` and `ra` in `[R10, R11]` — low priority.

---

## 5. MA / RA video address

- **RA** output = `ra` (the raster counter, 0..R9) — which of the 8 scanlines of the
  char row is being fetched.
- **MA**: a per-character linear address. At the **start of each char row**, MA reloads
  to `row_base`; MA increments once per displayed character across the line. `row_base`
  starts at the Display Start Address (R12<<8 | R13) at the top of the frame and advances
  by **R1** each char row (so successive rows are R1 chars apart in memory).
- The CPC maps MA→RAM address non-linearly (MA bits + RA bits form the address:
  `addr = (RA & 7)<<11 | (MA & 0x3FF)<<1 | …`) — that mapping lives in the video fetch,
  not the CRTC; the CRTC just emits MA and RA. (Cross-ref hw-spec.md §3.1 MA/RA mapping.)

---

## 6. Verification

With the standard CPC register set (R0=63,R1=40,R2=46,R3=0x8E,R4=38,R5=0,R6=25,R7=30,
R9=7,R12=0x30,R13=0):
- One scanline is exactly **64 char ticks (64 µs)**.
- One frame is exactly **312 scanlines (20 ms → 50 Hz)**.
- HSYNC fires once per scanline at char R2, width `R3&0xF`.
- VSYNC fires once per frame at char row R7.
- **Integration**: feed this CRTC's `hsync`/`vsync` to the Gate Array (replacing the
  synthetic stub) and confirm the GA produces exactly **6 raster interrupts per frame**
  (312/52) → 300 Hz, and the VSYNC resync lands. This is the CRTC↔GA↔Z80 loop at real
  CPC frame timing.
- MA/RA: check the active-area address sequence for the first row matches
  `row_base + 0..R1-1`, and row N starts at `base + N*R1`.

---

## 7. CRTC type differences (0–3) — model type 0/1 first

Brief note (detail in crtc.md): the CPC shipped four CRTC types with subtle timing
differences — VSYNC width handling (R3 high nibble honoured vs fixed 16), how R4/R5/R9
interact at frame end, HSYNC-during-VSYNC, and the exact DISPEN/MA border behaviour.
Model **type 1** (the common Amstrad HD6845S) first; add a `type` field and branch the
known differences later. The counter engine (§3) is shared.

---

## 8. Mapping to our Device model

- `tick(in,out)` runs every master cycle but only advances a character on `clk.crtc`.
- Owns `out->vid.*`. Reads the CPU bus for register I/O (`&BC`/`&BD`).
- Caller-owned, no-heap: `crtc_state_size()` + `crtc_init(storage)`, uniform
  `tick`/`reset`/`state_size`/`save`/`load`. `crtc_peek()` exposes the counters +
  registers for tests.
