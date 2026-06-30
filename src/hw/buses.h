/* buses.h — the konCePCja system buses, modeled at the pin level.
 *
 * THE SPEC IS docs/hw-spec.md; this header is one implementation of it. Two
 * physical buses (CpuBus, VidBus) plus the clock fabric (Clocks), bundled as one
 * Bus value that the board threads through every device each 16 MHz master cycle.
 *
 * Lines are ACTIVE-HIGH (`true` = asserted). Real Z80 control lines are active-low
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
  uint16_t addr;  /* A0–A15                                  */
  uint8_t data;   /* D0–D7 (floating = 0xFF)                 */
  bool m1;        /* opcode-fetch cycle      — CPU drives    */
  bool mreq;      /* memory request          — CPU drives    */
  bool iorq;      /* I/O request             — CPU drives    */
  bool rd;        /* read strobe             — CPU drives    */
  bool wr;        /* write strobe            — CPU drives    */
  bool rfsh;      /* refresh cycle           — CPU drives    */
  bool halt;      /* CPU halted              — CPU drives    */
  bool wait;      /* hold the CPU (1 µs stretch) — Gate Array drives */
  bool irq;       /* maskable /INT — WIRED-OR of sources     */
  bool nmi;       /* non-maskable interrupt                  */
  bool reset;     /* runtime reset sequence                  */
  bool busrq;     /* bus request (DMA)                       */
  bool busak;     /* bus acknowledge         — CPU drives    */
} CpuBus;

/* The CRTC ↔ Gate-Array video/timing bus. */
typedef struct VidBus {
  uint16_t ma;    /* MA0–13 refresh memory address — CRTC drives */
  uint8_t ra;     /* RA0–4 row address             — CRTC drives */
  bool hsync;     /* horizontal sync (GA counts → /INT) — CRTC drives */
  bool vsync;     /* vertical sync                 — CRTC drives */
  bool dispen;    /* display enable (border vs active) — CRTC drives */
  bool cursor;    /* cursor match                  — CRTC drives */
} VidBus;

/* Clock enables, published by the Gate Array (a stub generator until it exists). */
typedef struct Clocks {
  bool cpu;       /* 4 MHz  — Z80 advances one T-state this master cycle */
  bool crtc;      /* 1 MHz  — CRTC advances                              */
  bool psg;       /* 1 MHz  — PSG advances                               */
  uint8_t phase;  /* 0..15 master-cycle phase within the 1 µs window     */
} Clocks;

/* The whole bus, threaded through every device each master cycle. */
typedef struct Bus {
  CpuBus cpu;
  VidBus vid;
  Clocks clk;
} Bus;

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_BUSES_H */
