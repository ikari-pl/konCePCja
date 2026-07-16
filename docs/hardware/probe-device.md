# Bus probe — ICE-style debug Device (test equipment, not a CPC chip)

Language-neutral spec for the **bus probe** (`src/hw/probe`), the debug
harness's breakpoint/watchpoint engine. Companion to `docs/hw-spec.md`.

## 1. What it is

A real Amstrad never had breakpoints; an **in-circuit emulator** clipped onto
the bus did — comparators watching the address lines and the control strobes,
halting the clock on a match. The probe models exactly that piece of bench
equipment: a Device on the board that **drives nothing** (infinite input
impedance: it never writes the `out` bus) and watches the committed `in` image
of every master cycle. Debug features therefore stay OUT of the chip models —
the Z80 stays silicon-faithful, and the probe is honest hardware too, just
hardware from the test bench rather than the CPC mainboard.

The same comparator machinery is the trigger engine for the planned
hardware-debug panels (motherboard view / logic analyzer / VCD capture): a
breakpoint and a capture trigger are the same circuit.

## 2. Comparators (fixed capacity, no heap)

| Kind | Capacity | Fires on |
|---|---|---|
| **Exec** | 32 addresses | the rising edge of an opcode fetch (`m1 && mreq && rd`) at the address — like a real ICE this matches ANY M1 fetch there, including the later fetches of a prefixed instruction sitting at that address |
| **Memory watch** | 16 entries (addr + read/write flags) | the rising edge of `mreq && rd` / `mreq && wr` at the address. Opcode fetches count as reads — that is what the wires do; a data-only view is a UI-layer filter |
| **I/O watch** | 8 entries (port value + mask + read/write flags) | the rising edge of `iorq && rd` / `iorq && wr` with `(port & mask) == value`, `m1` low (an interrupt acknowledge also drives `iorq` and must not match) |

Edges, not levels: a Z80 access holds its strobes across many master cycles
(T-states × 4, plus WAIT stretching), so each comparator remembers the
previous cycle's strobe state and matches only on the 0→1 transition of the
qualified condition.

## 3. The latch

The first match **latches** `{kind, addr, data, cycle}` and stops further
matching — a real ICE halts the target; here the host (the machine's run loop)
polls the latch each master cycle while any comparator is armed and stops
ticking the board when it finds it set. `probe_ack()` clears the latch and
re-arms. Single-hit-at-a-time by design: simultaneous matches in one cycle
resolve in table order (exec, then memory, then I/O).

The probe never halts the board by itself — it has no clock-stretching output
(that would be a different, more invasive piece of bench gear). Stopping is
the host's move, which also means headless/tests can inspect the latch without
any control coupling.

**Halt semantics (pin-level truth):** an exec hit latches on the fetch EDGE,
when the CPU is mid-M1 and has already incremented PC — so the halted
instruction's identity is `hit.addr`, not the CPU's live PC (which reads
`addr + 1` at that moment). Debug UIs must center on `hit.addr` while halted.
Stepping completes the current instruction (the probe is ignored during a
step, so stepping off a breakpoint never immediately re-trips it).

## 4. Host API

`probe_state_size/init` per the Device contract; `tick` observes only;
`reset` clears the latch but **keeps the comparators** (breakpoints survive a
CPC reset, exactly like an ICE keeps its setup when the target reboots);
`save/load` serialize comparators and latch (they are bench state, small and
deterministic).

```
int  probe_add_exec(dev, addr)            0 = added, -1 = full or duplicate
int  probe_del_exec(dev, addr)            0 = removed, -1 = not found
void probe_clear_exec(dev)
int  probe_list_exec(dev, out[], max)     returns count copied
     (same trio for watch: probe_add_watch(dev, addr, on_read, on_write), …)
     (same trio for io:    probe_add_io(dev, port, mask, on_read, on_write), …)
int  probe_pending(dev, ProbeHit* out)    nonzero if latched; does not clear
void probe_ack(dev)                       clear the latch, resume matching
```

## 5. Cost discipline

With no comparators armed and no latch set, `tick` is a two-load early-out.
The host checks `probe_pending` per master cycle only while armed (arming
happens on the host thread between frames, so "armed at frame start" is a
stable per-frame fact).
