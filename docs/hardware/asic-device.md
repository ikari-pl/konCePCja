# ASIC (Plus / 6128+) — Device spec (DESIGN DRAFT — implementation pending, bead filed)

The Amstrad Plus ASIC integrates the classic chipset (Gate Array + CRTC +
gating logic) into one chip and supersets it: a 12-bit analogue palette,
16 hardware sprites, pixel-smooth scrolling, a hardware split screen, a
programmable raster interrupt with an IM2 vector, and a 3-channel DMA sound
sequencer. The behavioural oracle is `src/asic.cpp` — and unlike the 2026
peripherals re-expressed this session, **this one is a real replacement +
delete** (`asic.cpp` is in the ledger's §1 deletion path; it reaches into
the legacy `GateArray`/`CRTC`/`PSG` globals and dies with them). So this
spec describes a *chipset variant*, not an expansion Device: on `model=3`
the classic GA/CRTC/video Devices enter Plus mode and a new `src/hw/asic`
Device joins the board; on models 0-2 nothing changes.

## 1. The headline: no new bus lines

The single most useful conclusion for planning — **the ASIC needs no new
`Bus` struct fields.** Every board-level signal it uses already exists,
several added this session:

| ASIC function | Existing wiring it rides |
|---|---|
| Register page overlay (`&4000-&7FFF` when unlocked) | `cpu.romdis` + `cpu.ramdis` + the memory Device's one-tick write latch (multiface-device.md §1, memory-device.md §4b) |
| DMA sound — RAM read | `cpu.busrq`/`cpu.busak` bus-master arbitration + normal `CpuBus` memory read |
| DMA sound — PSG write | `AyBus` (`bdir`/`bc1`/`da`), the ASIC as a **second master** alongside the PPI |
| Programmable raster + DMA interrupts | `cpu.irq` (wired-OR) |
| IM2 vectored interrupt | ASIC drives `cpu.data` during the IACK cycle (`m1`+`iorq`) |
| Split screen / scroll / sprites / palette | **internal to the video pixel path** — no board bus (see §3) |

Two caveats behind that clean table:

- **`AyBus` gains a second master.** The PPI drives `bdir`/`bc1`/`da` today;
  the ASIC drives them during its DMA slots. Because the board threads one
  `Bus` per master cycle, this is safe *as long as the two never drive in the
  same cycle* — which they don't, the DMA sequencer runs in scheduled
  post-HSYNC slots. The spec formalises that as an arbitration rule, not a
  new wire. (An alternative — a private ASIC→PSG poke path — is rejected: it
  would hide a real bus master from the pin-level truth.)
- **The video seam is Device-internal, not a bus.** Sprites, the 12-bit
  palette, split and scroll all live inside the video pixel stage. That
  stage needs a genuine new internal structure (§3), but it is *not* a
  board-level line — the video Device reaches the ASIC's sprite/palette
  state through an attach reference, exactly as `video_attach` already takes
  the `gate_array` Device for `ink`/`mode`.

## 2. Unlock and the register page

The ASIC's registers are unmapped until a **17-byte knock sequence** —
`00 00 FF 77 B3 51 A8 D4 62 39 9C 46 2B 15 8A CD`
(`asic_poke_lock_sequence`, `asic.cpp:62`) — is written to the CRTC
register-select port (`&BCxx`; verify the exact caller port during
implementation). The Device watches those committed I/O writes and runs the
same lock/unlock FSM the oracle does (the sequence-minus-last-byte *locks*;
a full match plus one more byte *unlocks*).

**Unlocking alone does NOT map the register page.** The knock only *enables*
the secondary ROM-mapping register (RMR2); the CPU must then *page the register
page in*. RMR2 is a Gate-Array function-2 I/O write (`&7Fxx`, data `10xxxxxx`)
with bit 5 set, honoured only while unlocked. It carries two fields: the **page
field (bits 0-2)** selects which cartridge 16K page to map, and the **membank
field (bits 4-3)** selects the **16K CPU slot** it maps into:

| membank | maps the low-ROM cartridge page into | notes |
|---------|--------------------------------------|-------|
| 0 | `&0000-&3FFF` | the classic lower-ROM slot |
| 1 | `&4000-&7FFF` | |
| 2 | `&8000-&BFFF` | |
| 3 | — | maps the **ASIC register page** into `&4000-&7FFF` (`page_on`); the low ROM falls back to slot 0 |

Only the selected slot carries the cartridge page — the other low slots show
RAM (the memory Device applies the low ROM at `cart_lower_slot`, not always at
`&0000`, mirroring the legacy `memory_set_read_bank(lower_ROM_bank, …)`). The
membank slot is honoured only while the lower ROM is enabled (`rom_config bit2
== 0`). (Forcing the page to `&0000` regardless of membank crashed Burnin'
Rubber on race start: its RAM-LAM restart `OUT 0xB6` = membank 2, page 6 parks
the cartridge at `&8000` while the CPU fetches a `RET` from RAM at `&004E`;
forcing page 6 to `&0000` made that fetch hit cartridge data and the Z80 ran off
into a low-memory data table. See memory-device.md and test/hw/asic_test.cpp
`Rmr2MembankSelectsLowRomSlot`.)

Games unlock the ASIC to use splits/palette/sprites but page the register page
back *out* (membank ≠ 3) to bulk-copy data through `&4000-&7FFF` into RAM
without scribbling the ASIC registers — so the register-page overlay must gate
on **both** `!locked` **and** `page_on`. (Conflating the two — overlaying
whenever merely unlocked — let Burnin' Rubber's title-screen bulk copy hit the
DMA control at `&6C0F`, spuriously enabling DMA whose INT flags derailed the
game; the `page_on` gate is the fix.)

While unlocked *and* paged in, the register page overlays RAM at `&4000-&7FFF` —
the `romdis`/`ramdis` overlay pattern (reads answer from the register page or its
RAM mirror, writes land in the Device and are vetoed on the internal RAM via the
one-tick latch). Reads are one-per-access, edge-latched (the FDC data-register
discipline).

Register map (`asic_register_page_write`, offsets within the page):

| Range | Contents |
|---|---|
| `&4000-&4FFF` | 16 sprites × 16×16 pixels, 4-bit palette index (0 = transparent) |
| `&6000-&607F` | sprite attributes, 8 bytes each: X (**10-bit**, 0-1023 / -256..+767 signed), Y (9-bit), magnification (×0/1/2/4 per axis) — cpctech cpcplus |
| `&6400-&643F` | palette: 32 entries × 2 bytes, 12-bit RGB (`RRRR_BBBB`, `0000_GGGG`) |
| `&6800` | programmable raster interrupt scanline |
| `&6801` | split-screen scanline |
| `&6802-&6803` | split-screen display address (14-bit MA base) |
| `&6804` | soft scroll: H delay (bits 0-3), V offset (bits 4-6), extend-border (bit 7) |
| `&6805` | IM2 interrupt vector: `(vector & 0xF8) \| source`. On the IACK the low 3 bits carry the source — raster=6, DMA ch0=4, ch1=2, ch2=0 — chosen by priority raster > ch2 > ch1 > ch0 (cpcwiki ASIC). Real HW has the "Plus Vectored Interrupt Bug" (raster ack sometimes drives 4); we drive the intended value. |
| `&6808-&680F` | analogue joystick inputs (host-fed; low priority) |
| `&6C00-&6C0B` | DMA per channel: source address (word-aligned) + prescaler |
| `&6C0F` | DCSR: enable ch0/1/2 = bits 0/1/2; INT flags ch0/1/2 = bits 6/5/4; raster INT = bit 7 (cpcwiki). NB: bit 7 live-readback + full per-channel readback test are a scoped gap (beads-t3t7). |

## 3. The video half — the real work

**Status (increment B-1, implemented):** the 12-bit palette path and the
per-beam sprite-compositing seam are live in `src/hw/video`
(`video_attach_asic`, `render_char_plus`). When the ASIC is plugged the pixel
path composites the 16 hardware sprites per beam pixel and maps the winning
index through the 32-entry 12-bit palette, with unprogrammed entries falling
through to the classic ink colour (one palette RAM, `asic_set_palette`
semantics). Sprite attributes + palette are latched once per scanline so
mid-frame register changes act like a raster split. **Split screen (increment
B-2, implemented):** the CRTC Device reads split_line/split_addr from the ASIC
(`crtc_attach_asic`, latched once per frame) and swaps the display base at the
split scanline, then continues += R1 per row from there (matching the legacy
`crtc.cpp` sl_count/next_addr behaviour). **Soft scroll (increment B-3,
implemented):** `vscroll` lives in the CRTC (`apply_vscroll` adjusts the driven
fetch `(ma, ra)` with the char-row wrap past R9); `hscroll` lives in the video
Device (`render_char_plus` shifts the background pens right, pulling the reset
carry from a one-cell history at the left edge, leaving sprites unscrolled).
This completes the Plus video half.

Our current pixel path (video-device.md) goes byte → pen index → RGB in one
pass: the video Device consumes the GA's `RamBus` fetches and maps pens
through the GA's `ink[17]` (5-bit hardware colours) via `vid_hw_rgb`. The
ASIC breaks three assumptions, and each maps to a concrete change:

- **12-bit palette.** The `ink` table of 5-bit indices becomes 32 entries of
  12-bit RGB held in ASIC state. In Plus mode the video Device reads the
  palette from the ASIC Device (attach reference) instead of `vid_hw_rgb`.
- **Sprites → a pixel-index compositing seam.** This is the one new internal
  structure. The video Device's per-pixel emit must, in Plus mode, produce a
  **background pixel index** first, then composite any sprite pixel covering
  this beam position (16 sprites, per-axis magnification, transparency on
  index 0, priority high-to-low), then map the winning index through the
  12-bit palette. The oracle's `asic_draw_sprites` runs as a whole-frame
  post-pass over the surface; the pin-level model composites per beam pixel
  as the raster scans (so raster-split sprite tricks work). The seam is
  internal to the video Device — no `VidBus` field is added.
- **Split screen + soft scroll → Plus CRTC registers.** At the split
  scanline the display MA base switches to the split address; `hscroll`
  skews the line by pixels and `vscroll` offsets it by lines. These are new
  registers on the CRTC Device (it already carries `split_sl`/`split_addr`/
  `interrupt_sl` in the legacy `t_CRTC`; the hw CRTC — types 0-3 today —
  gains the ASIC/"type-4" behaviour). The CRTC keeps driving `VidBus.ma`;
  it simply computes a different MA after the split line.

## 4. DMA sound

**Status (increment C, implemented):** live in `src/hw/asic` (`dma_service` +
the `DmaPhase` FSM). The Z80 now honours BUSRQ/BUSAK (increment C-0), and the
PPI yields the AY bus while BUSAK is granted (C-1a), so the ASIC is a genuine
second master: per scanline it raises `cpu.busrq`, takes `cpu.busak`, reads the
list from RAM over `CpuBus` (`source_address` as a Z80-space address — the
memory Device applies the banking, so the ASIC needs no `RAM_config` back-
channel), and writes the PSG over `AyBus`. Two deviations from the
`src/asic.cpp` oracle, both toward real hardware: LOOP **decrements** the loop
counter (the oracle omits it, near issue #40) and REPEAT remembers the
instruction **after** it. The DMA INT flag is set + mirrored in the DCSR here;
raising `cpu.irq` from it is increment D.

Three channels, each a tiny 16-bit-instruction sequencer clocked once per
scanline after the HSYNC leading edge (timed off `VidBus.hsync` edges —
`asic_dma_cycle`). Per enabled, non-paused channel: fetch a 16-bit word from
RAM at the channel's source address (through the current RAM bank config),
decode and execute — `LOAD R,DD` (write PSG register R), `PAUSE N`
(× `prescaler+1`), `REPEAT NNN` / `LOOP`, `INT` (raise the channel's
interrupt), `STOP`, `NOP` (the last four OR-combinable) — then advance the
source address and mirror it + the DCSR back into the register page.

Pin-level shape: the ASIC raises `busrq`, takes `busak`, performs its RAM
reads as a bus master on `CpuBus`, and drives the PSG over `AyBus` in its
scheduled slot (§1's second-master rule). `LOAD` to the PSG while the CPU
also wants it stalls the CPU on the real board — model the contention via
`cpu.wait` if `testplus.cpr`'s RAM test demands it (the oracle notes this is
unverified, `asic.cpp:121-123`).

## 5. Interrupts

**Status (increment D, implemented):** live in `src/hw/asic` (`asic_irq`). The
ASIC counts frame scanlines off the `VidBus` sync edges and asserts `cpu.irq`
(wired-OR) at `pri_line`; a DMA `INT` opcode asserts it too. On the CPU's
interrupt-acknowledge cycle (`m1`+`iorq`) the ASIC drives `cpu.data` with
`int_vector` — the Z80 already latches that as the IM2 vector low byte — and
clears its sources. The Gate Array defers its fixed 52-line interrupt to the
ASIC whenever PRI is active (`ga_attach_asic` + `asic_vid_pri_active`), so the
two never double-fire. This closes the last functional gap; the `src/asic.cpp`
oracle is now fully superseded (ledger §1 delete).


- **Programmable raster (PRI):** the ASIC counts scanlines (off `VidBus`)
  and asserts `cpu.irq` at `interrupt_sl` instead of the classic GA's
  fixed 52-line cadence. In Plus mode the GA's interrupt counter defers to
  the ASIC's programmable one.
- **DMA channel INT:** an `INT` instruction raises `cpu.irq` and sets the
  channel's DCSR flag.
- **IM2 vector:** on the interrupt-acknowledge cycle (`m1`+`iorq`), the ASIC
  drives `cpu.data` with `interrupt_vector` so the Z80 in IM2 dispatches
  through the vector table. (The oracle leaves this unimplemented,
  `asic.cpp:384-389` — this spec specifies it.)

## 6. Board composition and media

`model=3` assembles a different board: GA, CRTC and video Devices enter Plus
mode (12-bit palette, ASIC CRTC registers, sprite compositing) and the
`src/hw/asic` Device is `board_add`ed to own the register page, the DMA
sequencer and the interrupt logic. Models 0-2 assemble exactly today's
board. The Plus boots from **cartridge** — CPR media is a caller-owned ROM
image mapped into the ROM banks (media handling like the AMSDOS/MF2 ROM
attach, not wiring); the classic lower/upper ROM split is replaced by the
cartridge's 32×16K banks.

## 7. State, serialization, host API

Device state (serialized): lock FSM + position, the register page contents
(sprites, palette, config, scroll, split, PRI, vector), the three DMA
channels (source/loop address, prescaler, enabled, paused, loop counter,
interrupt), interrupt vector. Live wiring (never serialized): the cartridge
ROM image. Host-fed (like the RTCs / mouse): the analogue joystick inputs at
`&6808`. Host API sketch: `asic_state_size/init/peek`, `asic_attach_cpr`,
`asic_set_analogue(dev, axis, value)`; `Sf2Regs`-style `AsicRegs { locked,
raster_line, dma_active }`.

## 8. Open questions (resolve during implementation)

- The exact I/O port carrying the unlock knock (assumed `&BCxx` CRTC
  register-select — confirm against the `asic_poke_lock_sequence` caller).
- Whether DMA `LOAD`↔CPU PSG contention (`cpu.wait` stall) is observable
  enough to require modelling (`asic.cpp:121`; gate on `testplus.cpr`).
- Sprite/border geometry edge cases the oracle marks TODO (`asic.cpp:470` —
  border width vs CRTC R1/R2/R6); pin them against real Plus output.

## Batch contract (RunTier::Fast)

- **Raster events (per scanline)**: PRI compare + split-screen apply consume
  the CRTC's frame_line events; the PRI contributes to the scheduler's
  `next_irq_at` horizon (wired-OR with the GA — the scheduler owns the OR).
- **DMA sound**: executed as ONE burst per HSYNC event (the documented
  model, §4, and the legacy oracle's behavior): fetch/decode/execute the
  channels' instructions for that scanline, forwarding PSG register LOADs as
  AY events (catch the PSG up first). PAUSE/REPEAT counters are exact
  per-scanline arithmetic. BUSAK bus stealing is subsumed into the burst's
  time accounting on the master clock.
- **Register page**: map/unmap (RMR2) are banking events (memory-device.md
  §batch rebuilds the tables + /RAMDIS veto); page WRITES catch the video
  renderer up first (palette/sprite/config are raster-trick state), then
  decode-once (the pgwr edge is natural at event granularity). The unlock
  knock consumes CRTC-select I/O write events.
- Bestiary audit: class (a) — decode_write publishes state read by
  video/CRTC/mem: discharged by catch-up-before-apply ordering; class (b) —
  DMA pause counters are exact per-scanline math; class (d) — /RAMDIS keys
  on the page state at the event, one truth shared with the memory seam.

Implementation (F7): the register page, PRI, split/scroll, palette, sprites
and the unlock knock run under RunTier::Fast as events — `asic_fast_io_write`
(knock/classic-palette/RMR2, asic_tick's three iorq-write arms),
`asic_fast_mem_read/write` (the &4000-&7FFF overlay: one decode per access,
RAM vetoed), `asic_batch_frame_line` (the PRI reach-edge, fed per chain
view; CPU-visible at rel T-state 4k+1 like every chain edge),
`asic_batch_int_ack` (the IM2 vector by priority + source clear, returned
through Z80BatchIO.int_ack), `asic_batch_set_sync` (handover shadows). The
compositor batches through the shared `render_cell_plus` with the same
per-scanline `plus_refresh_line` snapshot — catch-up-then-apply preserves
its per-cycle snapshot timing exactly.

SCOPE NOTE — DMA sound does NOT batch: the sequencer steals bus masters from
the CPU at M-cycle boundaries the batch driver cannot retro-track (the
CPU-ahead margin), so a frame with any DMA channel enabled runs on the
per-cycle tiers (run_frame's frame-start gate) and a mid-frame enable BAILS
the fast frame through the exit materializer — byte-exact either way. A
steal-aware batch driver is the F8-adjacent follow-up.
ORACLES: PlusCartBoot.FastTierMatchesWakeIncludingAudio (boot→menu→F2→title,
fb per frame + concatenated audio), the Plus bench CKSUM.
