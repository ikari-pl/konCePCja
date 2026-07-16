# The HP 7470A plotter — Device spec

A two-pen HP-GL plotter as a bus Device on `SerialBus` (rs232-device.md §1),
at the far end of the RS232 card's wire. The Device is the plotter's
**controller**: UART, input buffer, HP-GL parser, pen/carriage state, and
the response transmitter. The host renders the resulting PAGE (ImGui
preview, SVG/PNG export) exactly as the video plugin renders the CRT — an
interpretation of Device state, never behavior.

Precedent: the tape DECK is a Device (beyond-the-connector hardware with
machine-coupled real-time behavior). Baseline oracle: the legacy
`plotter.cpp` HpglPlotter parser + `PlotterBackend` response protocol,
proven e2e against the CP/M Plus GSX driver DDHP7470.PRL (retrofun article
plots). Together with the RS232 card this retires the BDOS serial hooks —
a Wave-1 S3 deletion blocker for z80.cpp.

## 1. The wires

Reads `serial.txd` (CPC→plotter), drives `serial.rxd` (plotter→CPC). 8N1
framing at the configured baud (fixed by DIP in hardware; V1: one `baud`
config datum shared with the card's programmed rate — mismatch is a user
configuration error that garbles frames, as in life). Unplugged: rxd rests
at mark, txd is ignored.

## 2. Input buffer and flow control

255-byte input buffer (the 7470A's). Every RECEIVED byte enters the buffer;
the parser drains it at a deterministic service rate (V1: one byte per
completed frame time — the parser is never the bottleneck, but the buffer
is real so ENQ answers are honest).

- **ENQ ($05)** — buffer-space query, answered immediately (bypasses the
  buffer): decimal free space + CR. Legacy oracle answered a constant
  `"128\r"`; the Device answers `min(free, 128)` formatted the same way —
  identical on the wire in the GSX flow, honest under stress.
- **XON/XOFF**: transmit XOFF ($13) when free space drops below 32,
  XON ($11) when it recovers past 128. The DDHP7470.PRL XON poll (BDOS
  C=6 loop until $11 — memory: ddhp7470-prl) is satisfied by the XON the
  plotter emits after processing each command burst. V1 acid test 3 settles
  the exact expectation from the driver's byte log.

## 3. HP-GL parser (V1 command set = the legacy oracle's)

7-bit ASCII (`byte & 0x7F`). Two-letter mnemonics, comma/space-separated
numeric parameters, terminated by `;` or the next mnemonic. State machine
carried verbatim from the proven legacy parser:

- Motion: `PU` `PD` `PA` `PR` (pen up/down, absolute/relative moves — each
  coordinate pair emits a Line segment when the pen is down)
- Geometry: `CI` (circle) `AA` `AR` (arcs) `EA` `ER` (edge rectangles)
- Pen/line: `SP` (select 0–2) `LT` (line type −1 solid, 0–6 patterns)
- Labels: `LB…terminator` (default ETX $03), `DT` (redefine terminator),
  `SI` (char size, cm) `DI` (label direction)
- Setup: `IN` (initialize) `DF` (defaults) `IP` (P1/P2) `SC` (user scaling)
  `IW` (clip window)
- Device control `ESC.` sequences (`ESC.(`, `ESC.)`, `ESC.I`, `ESC.@` …):
  consumed and ignored in V1 (the legacy oracle's behavior — the GSX init
  sends them; they configure handshake modes the in-band XON/XOFF already
  covers).

Geometry: plotter units, 0.025 mm; hard-clip 0,0–10365,7962 (A4 landscape);
`SC` maps user coordinates onto the P1/P2 window; `IW` clips. All carried
from the legacy implementation, which is the segment-level oracle.

## 4. Response protocol

Output commands queue a response into the TX path, transmitted on
`serial.rxd` at wire speed (framed, paced — NOT instant; the BDOS3
read-until-CR loop at PRL+$1A32 polls until CR arrives, authentic pacing
included):

| Query | Response (legacy oracle, byte-exact) |
|-------|--------------------------------------|
| `OS;` | `16\r` (Ready; bit3 Initialized not modeled — oracle constant) |
| `OD;` | `0,0,10300,7650\r` |
| `OI;` | `7470A\r` |
| ENQ   | free-space decimal + `\r` (§2) |

## 5. Logical state (snapshot)

Pen position/up-down/selected pen, line type, P1/P2, SC window, IW clip,
label state (terminator, size, direction, in-label flag + partial buffer),
command buffer, input buffer contents, UART shift state, response queue —
AND the accumulated segment list (the page IS plotter state: a plot in
progress survives a snapshot round-trip mid-line). Segment list serializes
as count + fixed-width records; `state_size` is therefore dynamic, which
the Device contract already supports (`state_size(self)`).

## 6. Host API

```c
size_t plotter_state_size(const void* self);
Device plotter_init(void* storage);
void   plotter_peek(const Device* dev, PlotterRegs* out); /* pen xy/down/
                                        selected, buffer fill, page rev */
/* The page, for rendering + export — segments are logical state: */
size_t plotter_segments(const Device* dev, const PlotSeg** out);
void   plotter_clear_page(const Device* dev);   /* operator tears sheet off */
void   plotter_set_plugged(const Device* dev, int on);
```

Host-side SVG/PNG export walks `plotter_segments()`; the existing
`plotter.cpp` exporter relocates to the UI layer, reading Device truth. A
`page rev` counter in the peek lets the preview cache redraws.

## 7. Acid tests

1. **Parser oracle** — feed the identical HP-GL byte corpus to the legacy
   `HpglPlotter` and the Device; segment lists must match field-for-field
   (the clean-room parity proof, per ledger practice).
2. **Response protocol** — `OS;`/`OD;`/`OI;`/ENQ answer byte-exactly per §4,
   framed on the wire at the configured baud.
3. **GSX e2e** — CP/M Plus + DDHP7470.PRL under engine=1 produces the
   retrofun article plot; the wire byte log (both directions) is captured
   as the regression corpus. This test settles the XON expectation (§2)
   and removes the serial_interface LOG_ERROR mitigation.
4. **Snapshot mid-plot** — save between `PD` and the closing `PA`, load,
   finish the plot: identical page to an uninterrupted run.
5. **Tier lockstep** — fast vs wake through a full plot: byte-exact wire
   log and identical segment list.

## Batch contract (RunTier::Fast)

The plotter's next event horizon is the next bit edge of an in-flight RX or
TX frame, else the next buffered-byte service tick, else infinity. Idle
plotter (empty buffer, both UARTs idle, rxd at mark) contributes no events —
elision-eligible. Buffered parsing catches up arithmetically: N whole frame
times elapsed → N bytes drained through the parser in order.
