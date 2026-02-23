/* konCePCja - Amstrad CPC Emulator
   Cheetah AmDrum - 8-bit DAC on port &FFxx

   The AmDrum is a simple 8-bit DAC that maps to the uncontested I/O space
   where all upper address bits are high (port.b.h == 0xFF). Writing any
   value to &FF00-&FFFF sets the DAC output level.
*/

#ifndef AMDRUM_H
#define AMDRUM_H

#include "types.h"

struct AmDrum {
   bool enabled = false;
   byte dac_value = 128;  // 128 = silence (unsigned midpoint)
};

extern AmDrum g_amdrum;

void amdrum_reset();
void amdrum_register_io();

#endif
