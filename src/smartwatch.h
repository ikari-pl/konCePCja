/* konCePCja - Amstrad CPC Emulator
   Dobbertin SmartWatch - Dallas DS1216 RTC in ROM socket

   The DS1216 is a phantom device between CPU and ROM. It intercepts
   ROM reads using a serial bit-banging protocol:
   1. Read ≥64 bits to reset (auto-handled by state machine)
   2. Write 64-bit pattern by reading from A2=0 addresses, data on A0
   3. Pattern: C5 3A A3 5C C5 3A A3 5C (LSB first per byte)
   4. If matched, next 64 reads (A2=1) return BCD time via D0
*/

#ifndef SMARTWATCH_H
#define SMARTWATCH_H

#include "types.h"

struct SmartWatch {
   bool enabled = false;
   enum State { IDLE, MATCHING, READING } state = IDLE;
   int bit_index = 0;           // 0-63
   uint64_t shift_reg = 0;      // accumulated pattern bits
   uint8_t rtc_data[8] = {};    // BCD time snapshot (filled on match)
};

extern SmartWatch g_smartwatch;

void smartwatch_reset();

// Called on every upper ROM read when SmartWatch is enabled.
// addr = full 16-bit Z80 address, rom_byte = normal ROM data.
// Returns the (possibly modified) byte to deliver to Z80.
byte smartwatch_rom_read(word addr, byte rom_byte);

#endif
