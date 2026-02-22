#include "smartwatch.h"
#include <ctime>

SmartWatch g_smartwatch;

// DS1216 recognition pattern: C5 3A A3 5C C5 3A A3 5C (LSB first per byte)
static const uint64_t DS1216_PATTERN = 0x5CA33AC55CA33AC5ULL;

static uint8_t to_bcd(int val)
{
   return static_cast<uint8_t>(((val / 10) << 4) | (val % 10));
}

static void smartwatch_snapshot_time()
{
   time_t now = time(nullptr);
   struct tm *t = localtime(&now);

   // DS1216 register map (8 bytes BCD, LSB first in serial stream):
   // Byte 0: hundredths of seconds (always 00 — host clock lacks sub-second)
   // Byte 1: seconds (00-59)
   // Byte 2: minutes (00-59)
   // Byte 3: hours (bit 7: 12/24, bit 5: AM/PM if 12h, bits 4-0: hour BCD)
   // Byte 4: day of week (1-7), bit 4: OSC flag, bit 5: RST flag
   // Byte 5: day of month (01-31)
   // Byte 6: month (01-12)
   // Byte 7: year (00-99)
   g_smartwatch.rtc_data[0] = 0x00;                     // hundredths
   g_smartwatch.rtc_data[1] = to_bcd(t->tm_sec);        // seconds
   g_smartwatch.rtc_data[2] = to_bcd(t->tm_min);        // minutes
   g_smartwatch.rtc_data[3] = 0x80 | to_bcd(t->tm_hour); // 24h mode (bit 7 set)
   g_smartwatch.rtc_data[4] = (t->tm_wday == 0 ? 7 : t->tm_wday); // day of week 1-7
   g_smartwatch.rtc_data[5] = to_bcd(t->tm_mday);       // day of month
   g_smartwatch.rtc_data[6] = to_bcd(t->tm_mon + 1);    // month (1-12)
   g_smartwatch.rtc_data[7] = to_bcd(t->tm_year % 100); // year
}

void smartwatch_reset()
{
   g_smartwatch.state = SmartWatch::IDLE;
   g_smartwatch.bit_index = 0;
   g_smartwatch.shift_reg = 0;
}

byte smartwatch_rom_read(word addr, byte rom_byte)
{
   bool a0 = addr & 0x01;  // data bit (for pattern matching)
   bool a2 = addr & 0x04;  // mode: 0=write(pattern), 1=read(data)

   switch (g_smartwatch.state) {
      case SmartWatch::IDLE:
         // Any read with A2=0 starts pattern matching
         if (!a2) {
            g_smartwatch.state = SmartWatch::MATCHING;
            g_smartwatch.bit_index = 0;
            g_smartwatch.shift_reg = 0;
            // Process this first bit
            g_smartwatch.shift_reg |= (static_cast<uint64_t>(a0) << g_smartwatch.bit_index);
            g_smartwatch.bit_index++;
         }
         // A2=1 reads are normal ROM reads, returned unchanged
         return rom_byte;

      case SmartWatch::MATCHING:
         if (!a2) {
            // Accumulate pattern bit from A0
            g_smartwatch.shift_reg |= (static_cast<uint64_t>(a0) << g_smartwatch.bit_index);
            g_smartwatch.bit_index++;
            if (g_smartwatch.bit_index == 64) {
               // Check pattern
               if (g_smartwatch.shift_reg == DS1216_PATTERN) {
                  // Pattern matched — snapshot time and enter reading mode
                  smartwatch_snapshot_time();
                  g_smartwatch.state = SmartWatch::READING;
                  g_smartwatch.bit_index = 0;
               } else {
                  // Pattern didn't match — reset
                  g_smartwatch.state = SmartWatch::IDLE;
                  g_smartwatch.bit_index = 0;
               }
            }
         } else {
            // A2=1 during matching resets the state machine
            g_smartwatch.state = SmartWatch::IDLE;
            g_smartwatch.bit_index = 0;
         }
         return rom_byte;

      case SmartWatch::READING:
         if (a2) {
            // Return RTC data via D0, rest of bits from ROM
            int byte_idx = g_smartwatch.bit_index / 8;
            int bit_idx = g_smartwatch.bit_index % 8;
            byte rtc_bit = (g_smartwatch.rtc_data[byte_idx] >> bit_idx) & 1;
            g_smartwatch.bit_index++;
            if (g_smartwatch.bit_index >= 64) {
               g_smartwatch.state = SmartWatch::IDLE;
               g_smartwatch.bit_index = 0;
            }
            return (rom_byte & 0xFE) | rtc_bit;  // replace D0
         } else {
            // A2=0 during reading — abort and start new pattern
            g_smartwatch.state = SmartWatch::MATCHING;
            g_smartwatch.bit_index = 0;
            g_smartwatch.shift_reg = 0;
            g_smartwatch.shift_reg |= (static_cast<uint64_t>(a0) << g_smartwatch.bit_index);
            g_smartwatch.bit_index++;
            return rom_byte;
         }
   }
   return rom_byte;
}
