# M4 Board — Device spec (DESIGN DRAFT — implementation pending, beads: to be filed)

The mass-storage / WiFi / ROM-board expansion by Duke (spinpoint.org): a
command/response coprocessor on the expansion port with an SD-backed
virtual filesystem, an 8266 WiFi bridge, an RTC, and its own upper ROM.
On real hardware an **STM32 Cortex-M** MCU accepts commands over two write
ports, does the work asynchronously, and publishes results into a window
of its own ROM address space. The golden master is `src/m4board.cpp` (ours,
2026 — engine-parity work per `replacement-ledger.md` §3, not spec-and-delete;
it keeps serving until the Wave-3 host cutover), split into the silicon-side
protocol modelled here and a pure host-side SD backend (`src/m4board_http.cpp`,
§5). This Device re-models the protocol against the hw-layer contract:
caller-owned ROM, host-mediated filesystem, **no host I/O inside a tick**.

The distinctive claim of this spec: the real board's ARM coprocessor is a
genuinely asynchronous processor with real latency, so deferring the actual
filesystem work to the host and reporting **busy** to the CPU meanwhile is not
a hw-layer compromise — it is the hardware-honest model.

## 1. Port decode and the command accumulator

Two write-only ports on the expansion I/O space. The golden master registers
both by **high address byte** (`m4board_register_io`, `src/m4board.cpp:2941`);
the data port additionally decodes the low byte, the execute port does not.

| Port | Access | Effect | Golden master |
|---|---|---|---|
| `&FE00` (hi `0xFE`, lo `0x00`) | OUT | append the byte to the command accumulator | `m4board_out_handler_fe`, `:2925` (rejects lo≠0x00) |
| `&FCxx` (hi `0xFC`, any lo) | OUT | latch + execute the accumulated command | `m4board_out_handler_fc`, `:2931` (ignores lo) |
| upper-ROM read `&E800..` | IN | read the response window (§2) | serviced by the memory Device's ROM decode |

- **Edge semantics per access.** Each OUT to `&FE00` pushes exactly one byte
  (`m4board_data_out`, `:2585`); each OUT to `&FCxx` fires exactly one execute
  (`m4board_execute`, `:2587`). As with the FDC data register and the Symbiface
  IDE/mouse pops (symbiface-device.md §1), a write is honoured **once per I/O
  access** — the Device must gate on the rising edge of `iorq && wr` with the
  decoded address, not on the level, so a WAIT-stretched OUT held across many
  master cycles enqueues one byte, not one per cycle. The accumulator is drained
  and cleared on every execute regardless of outcome (`:2783`).
- **Command frame.** The accumulator is `[size_prefix, cmd_lo, cmd_hi, data…]`;
  the 16-bit command is `cmd = buf[1] | buf[2]<<8` (`:2596`), always in the
  `&43xx` family (high byte `0x43`). A frame shorter than 3 bytes is discarded
  (`:2591`). `size_prefix` is the ROM's payload length; direct I/O sends `0x00`.
  Two ROM senders build the same frame — `send_command_iy` (workspace buffer)
  and `send_command2` (streamed writes for `fwrite`) — indistinguishable at the
  pins, so the Device models only the byte stream.
- The Device carries no per-command port state beyond the accumulator; the
  command set (open/read/write/seek/dir/rename/mkdir/erase/free/fstat/getpath,
  disc-image sector R/W, raw SD LBA R/W, HTTP, TCP sockets, RTC time, ROM
  management, config) is dispatched by the `switch` at `:2609`.

## 2. The ROM overlay + response window

The M4 presents a **16K upper ROM** in a caller-selected slot (default **6**,
`M4Board::rom_slot`; the 6128 slot-6 rationale — coexist with AMSDOS in slot 7,
`|SD`/`|DISC` to switch — is in m4-board.md). The ROM image is **caller-owned
live wiring**, attached exactly like every other expansion ROM
(`m4_attach_rom(dev, uint8_t* rom16k, size_t len)`, the analogue of
`mf2_attach_rom`); it is never serialized. The memory Device's multi-ROM decode
(memory-device.md) serves reads from it when the slot is selected — the M4 does
not need its own `romdis` for the ROM body, since it *is* a normal banked
expansion ROM. `romdis` remains available for the response window if a build
serves it from Device RAM rather than by mutating the attached image (below).

**The response window.** Command results appear in a **1.5 KB** region
(`RESPONSE_SIZE = 0x600`) at ROM offset **`0x2800`**, which maps to CPC
**`&E800`** when the M4 ROM is paged in; the ROM's link table at `&FF02` points
there (`m4board_write_response`, `:2786`; `rom_base + 0x2800`). The ROM reads
command-specific data from window offset **+3** (`respond_ok`, `:169`); the
response layout is `[status, len_lo, len_hi, data…]` with `status = 0x00` OK /
`0xFF` error at +0 and a command-level marker at +3.

- **Busy sentinel** (`beads-315e`). While a command is latched and the host
  coprocessor has not yet answered (`busy = 1`), a read of the status byte at
  `&E800` returns **`0xFF` ("not ready")**, taking precedence over any stale
  prior response still sitting under `response_len`. The M4 ROM polls the byte
  until it flips; `m4_complete_response` then clears `busy` and writes the real
  `0x00` (OK) status into the window. (An error result is also `0xFF`; the ROM
  disambiguates busy-vs-error via its own request bookkeeping, not this byte.)

- **Device RAM overlaying ROM.** The response window is **device state**, not
  part of the attached ROM image. The golden master mutates the ROM image in
  place (`memcpy(rom_base + 0x2800, response, len)`), which works only because
  the image is writable host memory. The Device instead owns a `response[0x600]`
  buffer and overlays it on reads to `&E800..&EDFF` while the M4 slot is
  selected — asserting `romdis` for exactly that window and driving the response
  byte, the mf2-device.md §1 pattern (an expansion silencing the internal decode
  and driving its own byte; two-phase settling is safe because the read holds
  its strobes and samples at cycle end). This keeps the caller-owned ROM image
  immutable and makes the response window serialize cleanly (§4).
- **Config data area.** `C_CONFIG` (`&43FE`, `cmd_config` `:2421`) writes the
  ROM's runtime data area at offset **`0x3400`** (jump vectors, slot number,
  AMSDOS version) — on the sub-cycle Device this is a second small overlay
  region backed by `config_buf[128]`, populated by the ROM's own init-time
  `C_CONFIG` stream, not by host-side ROM patching. The golden master's boot-
  message trampoline patch at ROM offset `0x0268`→`0x3800` (`:2864`) is a
  host-side ROM-image edit and stays host-side (§5); it is not Device behaviour.

## 3. The key design decision — filesystem commands are host-mediated

A device tick is pure and allocation-free and **may not touch the host**: no
`fopen`/`fread`/`fwrite`, no `dsk_load`, no sockets, no `time()`, no `curl`
(hw-spec §1). Yet nearly every M4 command *is* host I/O (`cmd_open` `fopen`,
`cmd_read`/`cmd_write` `fread`/`fwrite`, `cmd_readsector` DSK parsing,
`cmd_httpget` curl, the `C_NET*` socket family). The golden master executes all
of it **synchronously inside the OUT handler** — which is exactly why it cannot
live in a tick.

The resolution is to model the STM32 as what it physically is: an asynchronous
coprocessor. The Device **latches** a completed command frame and exposes it;
the **host** executes it against the SD backend between frames and writes the
result back; while the work is outstanding the Device reports **busy** to the
CPC, precisely as the real board holds the CPC off while its MCU works.

**Deferred-execution protocol.**

1. **Latch (in tick).** On the `&FCxx` execute edge the Device copies the
   accumulator into a single-slot pending mailbox (`M4Pending { uint16_t cmd;
   uint8_t frame[…]; uint16_t len; bool valid; }`), clears the accumulator, and
   sets an internal `busy` flag. No work is done in the tick. If a mailbox is
   already occupied (host hasn't drained it) the execute is held — the CPC
   cannot outrun the coprocessor, matching real latency.
2. **Peek / drain (host, between frames).**
   `bool m4_pending_command(dev, M4Pending* out)` returns and removes the
   latched frame (empty-safe). The host runs the *existing* command
   implementations — the whole `src/m4board.cpp` dispatch body, unchanged
   in behaviour — against the virtual-SD directory. **Path-traversal
   protection stays host-side** (`resolve_path`, `:124`; the
   `weakly_canonical` + `lexically_relative` "must stay within `sd_root`"
   check).
3. **Complete (host → Device).**
   `m4_complete_response(dev, const uint8_t* buf, uint16_t len)` copies the
   result into the Device's `response[0x600]` window and clears `busy`. Length
   is clamped to `RESPONSE_SIZE` (`:2791`).
4. **Read-back (in tick).** Subsequent CPU reads of `&E800..` return the window
   bytes via the §2 overlay. The ROM then does its `LDIR` from `rom_response+3`
   / `+4` / `+8` per command, exactly as today.

**What "busy" looks like to the CPC.** Between the execute OUT and
`m4_complete_response`, the Device is *busy*. The real board makes the CPC spin
on a poll during MCU work; the golden master never modelled this because it
completes synchronously (the response is written inside the same OUT handler,
`:2931`), so **the exact busy signalling the M4 ROM polls is an open question**
(see the summary). The Device therefore exposes the mechanism and leaves the
byte-level contract to be pinned against the ROM: the recommended model is a
**busy sentinel in the response window** — hold `status`/`&E800` at a reserved
"in-progress" value (e.g. `0xAA`, distinct from `0x00` OK / `0xFF` error) from
latch until `m4_complete_response`, so the ROM's existing status poll blocks
until the host answers. In the emulator's normal one-command-per-frame cadence
the host drains and completes within the same frame, so the CPC observes at most
one poll iteration of busy — indistinguishable from the golden master's
zero-latency behaviour, while remaining hardware-honest for software that
deliberately watches the busy edge.

## 4. Host API summary + serialization split

Free functions over caller-owned state (`m4_state_size` / `m4_init` /
`m4_reset` / `m4_peek`), no heap:

- **Wiring (never serialized):** `m4_attach_rom(dev, rom16k, len)` (the ROM
  image, live like every ROM); `m4_set_slot(dev, n)` (default 6);
  `m4_set_plugged(dev, on)` (gates all decode, like `mf2_set_plugged`).
- **Deferred bridge:** `m4_pending_command` / `m4_complete_response` (§3), the
  only path by which filesystem/network results enter the Device.
- **Peek:** `M4Regs { plugged, slot, busy, cmd_count, response_len, last_op }`.

**Serialization.** Device state = the command **accumulator** bytes, the
**pending mailbox**, the **`busy`** flag, the **`response[0x600]`** window +
`response_len`, and the **`config_buf[128]`** overlay — the in-flight protocol
state, versioned per hw-spec §9. **NOT serialized:** the attached ROM image
(wiring); and everything host-side — the open `FILE*` handles, the SD root
path, `current_dir`, directory-listing cursor, DSK-container browse state, TCP
sockets, activity/LED counters. Those live in the host bridge (the current
`M4Board` struct minus the four serialized protocol fields), not in the Device
blob, because they are host resources a snapshot can neither capture nor
restore.

**Reset** (bus reset; the golden master `m4board_reset`, `:2556`): clear the
accumulator + pending mailbox + `busy`, zero the response window and
`config_buf`, drop `response_len`. `plugged`/`slot`/attached ROM persist. The
host bridge separately resets its own view (`current_dir="/"`, close handles
and sockets, `network_enabled=true` — the `|WIFI,0` power-cycle default,
`:2570`) when the board is reset.

## 5. Explicitly out of scope for the Device

These are **host consumers of the same host-side SD backend**, above the hw
layer — they are not silicon and have no place in a tick:

- **The embedded HTTP server** (`src/m4board_http.cpp`, `m4board_web_assets.h`):
  the web file browser and the `cpcxfer` / M4-Board-Android-app compatibility
  surface. It runs in its own thread and shares the virtual-SD directory with
  the deferred command path; the CPC never sees it. It stays a pure host
  service.
- **Host port mappings** (`m4 port set/del`, the `M4PortMapping` table): host
  networking configuration, not bus behaviour.
- **The socket/HTTP command bodies themselves** (`C_HTTPGET`, `C_HTTPGETMEM`,
  the `C_NET*` family, raw curl/BSD-socket calls): they are dispatched through
  the *same* deferred bridge as filesystem commands (§3) and executed host-side.
  The Device only ferries their request frames out and their response bytes in.
- **Path-traversal protection** (`resolve_path`, `:124`) and DSK-container
  browsing (`container_enter_dsk` / `container_exit`, `:208`): host-side, where
  the real filesystem is.
