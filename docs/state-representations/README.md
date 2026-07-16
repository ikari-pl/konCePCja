# Representing hardware state in new ways — design exploration

konCePCja is not an emulator that *approximates* a CPC; it is a pin-level,
sub-cycle simulation whose ground truth is a two-phase 16 MHz bus threaded
through isolated Devices. That means it holds far more truth than a classic
debugger surfaces — every pin, every phase, every driver, every master cycle —
yet the conventional views (a register table, a hex dump, a disassembly pane)
*flatten* all of it back into the same 1980s monitor-ROM abstraction.

This folder collects **design proposals** (not implementations) for representing
that state in ways the hardware itself suggests: as waveforms, as dataflow, as
nested state machines, as space and time. Each is grounded in the real
architecture — `src/hw/buses.h` (the `Bus`), `src/hw/board.h` (the scheduler),
`src/hw/device.h` (the Device contract), and the specs under `docs/hardware/`.

## The designs

| Doc | Representation | The state it makes legible |
|-----|----------------|-----------------------------|
| [logic-analyzer-view.md](logic-analyzer-view.md) | Waveforms / virtual scope | Pin-level bus over master cycles: two-phase commit, CPU/video slot interleave, memory & I/O timing, IRQ/IACK, DMA BUSRQ/BUSAK |
| [motherboard-dataflow.md](motherboard-dataflow.md) | Live block diagram / signal provenance | Which Device drives/reads which bus line each cycle; where every displayed byte came from |
| [state-machine-atlas.md](state-machine-atlas.md) | Nested FSM flowcharts | Z80 M-cycle/T-state, CRTC frame→row→scanline, ASIC unlock knock + DMA sequencer, tape stages |
| [spatial-beam-memory.md](spatial-beam-memory.md) | Spatial maps | Raster beam over the framebuffer, MA/RA→RAM shuffle, sprite compositing layers, memory banking + ROM/RAM overlays |

## Framing questions each design answers

1. What hardware truth does konCePCja hold that a classic debugger throws away?
2. What visual language makes that truth *legible at a glance* (a flowchart, a
   waveform, a spatial map), and how does it map onto real `Bus`/Device state?
3. How would it be captured (which peek surface / probe hook feeds it) without
   violating the pin-level-truth bar?

See also the existing hardware-debug-panels vision (MB overview, pin watch,
virtual scopes) — these designs sharpen and extend it.
