# AMX Mouse — Device spec

A quadrature mouse on the joystick port. The joystick connector is wired
into **keyboard matrix row 9** through the keyboard connector's diode
matrix, so the mouse is not a CPU-bus peripheral at all: it watches the
ROW-SELECT lines and drives COLUMN lines, exactly like a bank of keys. The
AMX interface converts the quadrature stream into the CPC-readable
protocol: a direction bit pulses LOW for **one mickey per deselect/reselect
cycle of row 9** (a monostable inside the interface arms on deselect and
consumes one count on the next select).

## 1. A new bus line: the external column lines

`AyBus` gains `row_ext` — the externally driven column lines of the
CURRENTLY SELECTED row, **pulled up to 0xFF at rest** (`bus_resting`).
Anything hanging off the keyboard/joystick connectors (this mouse, future
joystick Devices) sees `kbd_row` on the committed bus and drives its bits
LOW when its row is selected; the PSG's port-A read returns
`key_matrix[row] & row_ext` — the wired-AND the diode matrix performs
physically. The one-tick settle after a row-select change is far inside
the firmware's scan timing (dozens of microseconds per row).

## 2. Row 9 protocol (the golden master, verbatim)

| Bit | Line | LOW means |
|---|---|---|
| 0 | Up | one pending mickey up |
| 1 | Down | one pending mickey down |
| 2 | Left | one pending mickey left |
| 3 | Right | one pending mickey right |
| 4 | Fire2 | LEFT button held |
| 5 | Fire1 | RIGHT button held |
| 6 | Fire3 | MIDDLE button held |

Direction bits stay LOW while the axis' mickey counter is nonzero; each
row-9 **deselect → reselect** edge consumes one count per axis (opposite
signs never mix — the counter is signed). Buttons are level-driven,
active-low, no monostable.

## 3. Host feed and layering

Sub-pixel accumulation is HOST-side (SDL deltas are floats; the analog
part of the mouse lives above the hw layer, like every host-input
concern): the host hands the Device whole mickeys and a button mask —
`amx_feed(dev, dx_mickeys, dy_mickeys, buttons)` (buttons bit0 = left,
bit1 = middle, bit2 = right, SDL order; the Device does the CPC mapping).
`amx_set_plugged` gates everything. The bridge drains the legacy
`g_amx_mouse` accumulator each frame, so the existing SDL event plumbing,
config flag (`input`/`amx_mouse`) and UI checkbox feed the sub-cycle
Device unchanged (the same benign cross-thread pattern the legacy hooks
already rely on).

## 4. State

Serialized: pending mickey counters, button mask, the row-select
monostable (selected/armed), plugged. Reset: counters and monostable
clear; plugged persists (a reset does not unplug the connector).
