/* buses.h — the konCePCja system buses, modeled at the pin level.
 *
 * THE SPEC IS docs/hw-spec.md; this header is one implementation of it. Two
 * physical buses (CpuBus, VidBus) plus the clock fabric (Clocks), bundled as
 * one Bus value that the board threads through every device each 16 MHz master
 * cycle.
 *
 * Lines are ACTIVE-HIGH (`true` = asserted). Real Z80 control lines are
 * active-low
 * (/MREQ, /RD, …); we flip them once, here, so device logic reads as plain
 * boolean (`if (in->cpu.mreq && in->cpu.rd)`). Plain C / fixed-width fields: a
 * SPARK/Ada or Rust device maps these structs 1:1.
 */
#ifndef KONCPC_HW_BUSES_H
#define KONCPC_HW_BUSES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The Z80 / memory bus. Resting `data` is 0xFF (pull-up = floating). */
typedef struct CpuBus {
  uint16_t addr; /* A0–A15                                  */
  uint8_t data;  /* D0–D7 (floating = 0xFF)                 */
  bool m1;       /* opcode-fetch cycle      — CPU drives    */
  bool mreq;     /* memory request          — CPU drives    */
  bool iorq;     /* I/O request             — CPU drives    */
  bool rd;       /* read strobe             — CPU drives    */
  bool wr;       /* write strobe            — CPU drives    */
  bool rfsh;     /* refresh cycle           — CPU drives    */
  bool halt;     /* CPU halted              — CPU drives    */
  bool wait;     /* hold the CPU (1 µs stretch) — Gate Array drives */
  bool irq;      /* maskable /INT — WIRED-OR of sources     */
  bool nmi;      /* non-maskable interrupt                  */
  bool reset;    /* runtime reset sequence                  */
  bool busrq;    /* bus request (DMA)                       */
  bool busak;    /* bus acknowledge         — CPU drives    */
  bool romdis;   /* expansion ROM overlay — wired-OR, silences internal ROM  */
  bool ramdis;   /* expansion RAM overlay — wired-OR, silences internal RAM  */
} CpuBus;

/* The CRTC ↔ Gate-Array video/timing bus. */
typedef struct VidBus {
  uint16_t ma; /* MA0–13 refresh memory address — CRTC drives */
  uint8_t ra;  /* RA0–4 row address             — CRTC drives */
  bool hsync;  /* horizontal sync (GA counts → /INT) — CRTC drives */
  uint8_t hsw; /* chars into the current HSYNC pulse (0..15) — CRTC drives */
  bool vsync;  /* vertical sync                 — CRTC drives */
  bool dispen; /* display enable (border vs active) — CRTC drives */
  bool cursor; /* cursor match                  — CRTC drives */
  uint16_t frame_line; /* frame scanline, 0 = frame top — CRTC drives. The
                        * single raster reference the Plus hardware split and
                        * the ASIC programmable raster interrupt both count off
                        * (the legacy shares one CRTC.sl_count for both). */
} VidBus;

/* The RAM-side port: the Gate Array owns the DRAM address multiplexer (on the
 * real board the Z80's address bus never reaches the RAM pins directly — the GA
 * and the two 74LS153 muxes do). Each 1 µs window splits into a CPU slot
 * (phases 0-11, where the mux passes the CPU address through — modeled as the
 * memory Device answering the cpu bus) and two GA video fetches (drive at phase
 * 12 and 14, a page-mode pair). The GA drives `addr` (the MA/RA shuffle) +
 * `fetch`; the RAM drives `data` with the display byte. Video fetches always
 * read RAM — ROM overlays never apply to this port. */
typedef struct RamBus {
  uint16_t addr; /* video fetch address — Gate Array drives          */
  uint8_t data;  /* fetched display byte — memory drives (rest 0xFF) */
  bool fetch;    /* video-slot read strobe — Gate Array drives       */
} RamBus;

/* The internal AY bus: PPI ⇆ AY-3-8912. The AY is wired to the PPI, not the
 * Z80, so it never sees cpu.iorq. The PPI is the master: it drives
 * bdir/bc1/kbd_row and, on a write, da; the AY drives da on a read. BC2 is
 * strapped high on the CPC, so (bdir, bc1) alone selects the function. Resting
 * da is 0xFF (floating). See docs/hardware/ppi-device.md §5 and psg-device.md
 * §2. */
typedef struct AyBus {
  uint8_t da; /* DA0–7 data bus — PPI drives on write, AY on read */
  bool bdir;  /* bus direction  — PPI (Port C bit 7) drives */
  bool bc1;   /* bus control 1  — PPI (Port C bit 6) drives */
  uint8_t
      kbd_row; /* selected keyboard row 0..15 — PPI (Port C bits 0..3) drives */
  uint8_t row_ext; /* external column lines of the selected row (joystick
                      connector etc.) — pulled up to 0xFF, connector devices
                      pull bits LOW; the PSG wired-ANDs them into the read
                      (amx-mouse-device.md §1) */
} AyBus;

/* The cassette interface: three wires between the PPI and the tape deck.
 * rdata carries the POST-SCHMITT read level — the CPC's input conditioning
 * circuit squares the analog signal on the mainboard, so any source that
 * drives levels (the CDT deck Device, or a live microphone via the host's
 * Schmitt stage) is interchangeable on this wire. */
typedef struct TapeBus {
  bool rdata; /* cassette read data  — tape deck drives */
  bool motor; /* motor relay         — PPI (Port C bit 4) drives */
  bool wdata; /* cassette write data — PPI (Port C bit 5) drives */
} TapeBus;

/* The RS232 serial line: two wires between the interface card and whatever
 * is plugged into its DB25 (rs232-device.md §1). Logic levels post
 * line-driver (idle HIGH = mark); bit-serial, sampled/driven at master-cycle
 * resolution. Framing is async 8N1 in V1. */
typedef struct SerialBus {
  bool txd; /* CPC → peer data — the RS232 card drives */
  bool rxd; /* peer → CPC data — the peer Device drives */
} SerialBus;

/* The light-pen strobe: a peripheral (light gun / light pen) pulses this HIGH
 * for the master cycle its sensor sees the beam (light-gun-device.md §1). The
 * CRTC latches its current MA into R16/R17 on the rising edge. Rests LOW. */
typedef struct PenBus {
  bool strobe; /* LPEN pin — the light-gun peripheral drives */
} PenBus;

/* Clock enables, published by the Gate Array (a stub generator until it
 * exists). */
typedef struct Clocks {
  bool cpu;      /* 4 MHz  — Z80 advances one T-state this master cycle */
  bool crtc;     /* 1 MHz  — CRTC advances                              */
  bool psg;      /* 1 MHz  — PSG advances                               */
  uint8_t phase; /* 0..15 master-cycle phase within the 1 µs window     */
} Clocks;

/* The whole bus, threaded through every device each master cycle. */
typedef struct Bus {
  CpuBus cpu;
  VidBus vid;
  RamBus ram;
  AyBus ay;
  TapeBus tape;
  SerialBus serial;
  PenBus pen;
  Clocks clk;
} Bus;

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_BUSES_H */
