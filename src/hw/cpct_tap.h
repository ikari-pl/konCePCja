/* cpct_tap.h — a bus tap that records every qualified Z80 access as a CPCT
 * record (docs: ~/src/cpc/cpcien/docs/trace-format.md, v0; docs/cpct-tap.md
 * here). Bench equipment like the probe (probe.h): it watches the committed
 * bus every master cycle and DRIVES NOTHING.
 *
 * The rule, so that another implementation (the CoPyCat RTL bench,
 * copycat/sim/tap) can apply exactly the same one to its own pins:
 *   an access begins on the master cycle a qualified strobe first appears --
 *     fetch  = m1 & mreq & rd        -> flags M1|MEM_RD
 *     mrd    = mreq & rd & !m1       -> MEM_RD
 *     mwr    = mreq & wr             -> MEM_WR
 *     iord   = iorq & rd & !m1       -> IO_RD
 *     iowr   = iorq & wr & !m1       -> IO_WR
 *   (refresh cycles, mreq without rd/wr, and interrupt acknowledges, iorq with
 *   m1, are not accesses);
 *   its record carries the cycle of that first master cycle divided by 4
 *   (T-states at 4 MHz), and the address and data bus AS LAST SEEN while the
 *   strobe was active -- the byte the CPU consumed on a read, the byte it
 *   drove on a write; the record is emitted when the strobe ends. HALT is
 *   added to its flags when /HALT was low at any point during the strobe.
 *
 * Line events: /INT falling (cpu.irq rising) and /RESET falling (cpu.reset
 * rising) each produce a pure line-event record (INT or RESET, plus HALT
 * when /HALT is low on that cycle; addr = data = 0) stamped with the edge's
 * cycle. An edge that lands while an access is open -- including on the
 * master cycle the access opens -- is emitted right after that access's
 * record, so the stream stays in non-decreasing cycle order. A
 * power-on reset of the tap (Device.reset) starts the stream with a RESET
 * record at cycle 0, as the spec asks of a capture that began at power-on.
 *
 * Overflow: the buffer is bounded (cpct_tap_set_capacity). Records past it
 * are counted, not stored; cpct_tap_write() then ends the file with a GAP
 * record (flags 0, addr = the saturated lost count, cycle = the cycle of the
 * first record that was dropped, where the loss began).
 *
 * At write time an access whose strobe is still active is not written (it
 * has no end yet); line events already queued behind it are.
 *
 * KNOWN MODEL ARTEFACT, HALT: this Z80 runs no bus cycles while halted -- it
 * holds /HALT low and waits (z80.cpp). Real silicon keeps executing NOP M1
 * fetches (one every 4 T-states, each with /HALT low). So in a
 * konCePCja-produced trace no access record carries HALT; HALT appears only
 * on the INT/RESET line events that end the halt. A hardware or RTL capture
 * of the same program has an M1|MEM_RD|HALT fetch record every 4 T-states
 * while halted; a comparison must skip those.
 *
 * KNOWN MODEL ARTEFACT in the cycle stamps (2026-09-05, measured against the
 * CoPyCat standalone RTL on the same program): this Z80 aligns a memory
 * M-cycle to the Gate Array's microsecond grid by HOLDING T1 until the grid
 * (z80.cpp, "T1 hold"), so its /MREQ appears on the grid; real silicon
 * asserts /MREQ in T1 wherever T1 falls and is then held in T2 by /WAIT.
 * Instruction totals are identical either way; the START of a bus strobe in
 * this trace is therefore up to 3 T-states LATER than hardware's for the
 * same access (the end is the same). A consumer placing sub-microsecond
 * effects by `cycle` should expect that jitter from a konCePCja-produced
 * trace and none from a hardware capture.
 */
#ifndef KONCPC_HW_CPCT_TAP_H
#define KONCPC_HW_CPCT_TAP_H

#include <stddef.h>
#include <stdint.h>

#include "device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CpctRecord {
  uint32_t cycle; /* T-state count (master cycles / 4) at the event */
  uint8_t flags;  /* CPCT flag bits */
  uint8_t data;
  uint16_t addr;
} CpctRecord;

enum : uint8_t {
  CPCT_M1 = 0x80,
  CPCT_MEM_RD = 0x40,
  CPCT_MEM_WR = 0x20,
  CPCT_IO_RD = 0x10,
  CPCT_IO_WR = 0x08,
  CPCT_HALT = 0x04,
  CPCT_INT = 0x02,
  CPCT_RESET = 0x01,
  CPCT_GAP = 0x00, /* flags == 0: lost events, addr = count */
};

enum : uint8_t { CPCT_HEADER_SIZE = 32, CPCT_RECORD_SIZE = 8 };

size_t cpct_tap_state_size(void);
Device cpct_tap_init(void* storage);

/* Records so far (a pointer into the tap's own buffer; valid until the next
 * tick). *count receives the number of stored records; records beyond the
 * capacity are dropped and counted in *dropped. */
const CpctRecord* cpct_tap_records(const Device* dev, size_t* count,
                                   size_t* dropped);
void cpct_tap_set_capacity(const Device* dev, size_t capacity);

/* Destroy the tap's state (it owns a heap buffer) before its storage is
 * freed. The Device must not be ticked afterwards. */
void cpct_tap_destroy(const Device* dev);

/* The v0 byte layout, little-endian: a 32-byte header and 8-byte records. */
void cpct_encode_header(uint8_t out[CPCT_HEADER_SIZE], uint8_t crtc_type,
                        uint8_t machine, uint64_t start_cycle);
void cpct_encode_record(uint8_t out[CPCT_RECORD_SIZE], const CpctRecord* r);

/* Write the records as a CPCT v0 file (header + records, plus a trailing GAP
 * record if any were dropped). crtc_type 0..3, machine 0=464 .. 3=6128+.
 * Returns 0 on success, -1 on any open/write/close failure. */
int cpct_tap_write(const Device* dev, const char* path, uint8_t crtc_type,
                   uint8_t machine);

#ifdef __cplusplus
}

/* The one owner of a tap Device: destroys the tap's heap buffer when it goes
 * out of scope, so no caller has to remember cpct_tap_destroy(). Every
 * consumer (the rig, the tests, a live-capture hook) declares one next to the
 * Device it built. Non-copyable: two owners would destroy the same state. */
struct CpctTapOwner {
  const Device* dev;
  explicit CpctTapOwner(const Device* d) : dev(d) {}
  ~CpctTapOwner() { cpct_tap_destroy(dev); }
  CpctTapOwner(const CpctTapOwner&) = delete;
  CpctTapOwner& operator=(const CpctTapOwner&) = delete;
};
#endif

#endif /* KONCPC_HW_CPCT_TAP_H */
