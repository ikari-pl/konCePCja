/* cpct_tap.h — a bus tap that records every qualified Z80 access as a CPCT
 * record (docs: ~/src/cpc/cpcien/docs/trace-format.md, v0). Bench equipment
 * like the probe (probe.h): it watches the committed bus every master cycle
 * and DRIVES NOTHING.
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
 *   drove on a write; the record is emitted when the strobe ends.
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
  uint32_t cycle; /* T-state count (master cycles / 4) at the access start */
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
};

size_t cpct_tap_state_size(void);
Device cpct_tap_init(void* storage);

/* Records so far (a pointer into the tap's own buffer; valid until the next
 * tick). *count receives the number of records; the buffer is bounded by
 * `capacity` given at init through cpct_tap_set_capacity, records beyond it
 * are dropped and counted in *dropped. */
const CpctRecord* cpct_tap_records(const Device* dev, size_t* count,
                                   size_t* dropped);
void cpct_tap_set_capacity(const Device* dev, size_t capacity);

/* Write the records as a CPCT v0 file (32-byte header + 8-byte records,
 * little-endian). Returns 0 on success. */
int cpct_tap_write(const Device* dev, const char* path, uint8_t machine);

#ifdef __cplusplus
}
#endif

#endif /* KONCPC_HW_CPCT_TAP_H */
