/* konCePCja - Amstrad CPC Emulator
   (c) Copyright 1997-2004 Ulrich Doewich

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/* CDT tape image emulation
   (c) Copyright 2002,2003 Ulrich Doewich
*/

#include <vector>

#include "koncepcja.h"
#include "tape.h"
#include "z80.h"
#include "imgui_state.h"
#include "imgui_ui_testable.h"  // safe_read_word / safe_read_dword for tape_scan_blocks

#define TAPE_PILOT_STAGE 1
#define TAPE_SYNC_STAGE 2
#define TAPE_DATA_STAGE 3
#define TAPE_SAMPLE_DATA_STAGE 4
#define TAPE_PAUSE_STAGE 5
#define TAPE_END 6

#define CYCLE_SCALE ((40 << 16) / 35)
//#define CYCLE_SCALE ((3993600 / 3500000) * 65536)
#define CYCLE_ADJUST(p) ((static_cast<dword>(p) * CYCLE_SCALE) >> 16)
#define MS_TO_CYCLES(p) (static_cast<dword>(p) * 4000)
//#define MS_TO_CYCLES(p) ((dword)(p) * 3994)

extern std::vector<byte> pbTapeImage;
extern byte *pbTapeImageEnd;
extern t_CPC CPC;

#ifdef DEBUG_TAPE
extern FILE *pfoDebug;
#endif

byte bTapeLevel;
byte bTapeData;
byte *pbTapeBlock;
byte *pbTapeBlockData;
word *pwTapePulseTable;
word *pwTapePulseTableEnd;
word *pwTapePulseTablePtr;
word wCycleTable[2];
int iTapeCycleCount;
dword dwTapePulseCycles;
dword dwTapeZeroPulseCycles;
dword dwTapeOnePulseCycles;
dword dwTapeStage = TAPE_END;
dword dwTapePulseCount;
dword dwTapeDataCount;
dword dwTapeBitsToShift;



void Tape_GetCycleCount()
{
   dwTapePulseCycles = CYCLE_ADJUST(*pwTapePulseTablePtr++);
   if (pwTapePulseTablePtr >= pwTapePulseTableEnd) {
      pwTapePulseTablePtr = pwTapePulseTable;
   }
}



void Tape_SwitchLevel()
{
   if (bTapeLevel == TAPE_LEVEL_LOW) {
      bTapeLevel = TAPE_LEVEL_HIGH; // reverse the level
   }
   else {
      bTapeLevel = TAPE_LEVEL_LOW; // reverse the level
   }
}



int Tape_ReadDataBit()
{
   if (dwTapeDataCount) {
      if (!dwTapeBitsToShift) {
         bTapeData = *pbTapeBlockData; // get the next data byte
         pbTapeBlockData++;
         dwTapeBitsToShift = 8;
         #ifdef DEBUG_TAPE
         fprintf(pfoDebug, ">>> 0x%02x\r\n", (byte)bTapeData);
         #endif
      }
      byte bBit = bTapeData & 0x80;
      // Push decoded bit into ring buffer for UI visualization
      imgui_state.tape_decoded_buf[imgui_state.tape_decoded_head] = bBit ? 1 : 0;
      imgui_state.tape_decoded_head = (imgui_state.tape_decoded_head + 1) % ImGuiUIState::TAPE_DECODED_SAMPLES;
      bTapeData <<= 1;
      dwTapeBitsToShift--;
      dwTapeDataCount--;
      if (bBit) {
         dwTapePulseCycles = dwTapeOnePulseCycles;
      }
      else {
         dwTapePulseCycles = dwTapeZeroPulseCycles;
      }
      dwTapePulseCount = 2; // two pulses = one bit
      return 1;
   }
   return 0; // no more data
}



int Tape_ReadSampleDataBit()
{
   if (dwTapeDataCount) {
      if (!dwTapeBitsToShift) {
         bTapeData = *pbTapeBlockData; // get the next data byte
         pbTapeBlockData++;
         dwTapeBitsToShift = 8;
      }
      byte bBit = bTapeData & 0x80;
      bTapeData <<= 1;
      dwTapeBitsToShift--;
      dwTapeDataCount--;
      if (bBit) {
         bTapeLevel = TAPE_LEVEL_HIGH; // set high level
      }
      else {
         bTapeLevel = TAPE_LEVEL_LOW; // set low level
      }
      iTapeCycleCount += static_cast<int>(dwTapePulseCycles); // set cycle count for current level
      #ifdef DEBUG_TAPE
      fprintf(pfoDebug, "%c %d\r\n",(bTapeLevel == TAPE_LEVEL_HIGH ? 'H':'L'), iTapeCycleCount);
      #endif
      return 1;
   }
   return 0; // no more data
}



int Tape_GetNextBlock()
{
   while (pbTapeBlock < pbTapeImageEnd) { // loop until a valid block is found
      #ifdef DEBUG_TAPE
      fprintf(pfoDebug, "--- New Block\r\n%02x\r\n", *pbTapeBlock);
      #endif

      switch (*pbTapeBlock) {

         case 0x10: // standard speed data block
            dwTapeStage = TAPE_PILOT_STAGE; // block starts with a pilot tone
            #ifdef DEBUG_TAPE
            fputs("--- PILOT\r\n", pfoDebug);
            #endif
            dwTapePulseCycles = CYCLE_ADJUST(2168);
            iTapeCycleCount += static_cast<int>(dwTapePulseCycles); // set cycle count for current level
            dwTapePulseCount = 3220;
            #ifdef DEBUG_TAPE
            fprintf(pfoDebug, "%c %d\r\n",(bTapeLevel == TAPE_LEVEL_HIGH ? 'H':'L'), iTapeCycleCount);
            #endif
            return 1;

         case 0x11: // turbo loading data block
            dwTapeStage = TAPE_PILOT_STAGE; // block starts with a pilot tone
            #ifdef DEBUG_TAPE
            fputs("--- PILOT\r\n", pfoDebug);
            #endif
            dwTapePulseCycles = CYCLE_ADJUST(*reinterpret_cast<word *>(pbTapeBlock+0x01));
            iTapeCycleCount += static_cast<int>(dwTapePulseCycles); // set cycle count for current level
            dwTapePulseCount = *reinterpret_cast<word *>(pbTapeBlock+0x01+0x0a);
            #ifdef DEBUG_TAPE
            fprintf(pfoDebug, "%c %d\r\n",(bTapeLevel == TAPE_LEVEL_HIGH ? 'H':'L'), iTapeCycleCount);
            #endif
            return 1;

         case 0x12: // pure tone
            dwTapeStage = TAPE_PILOT_STAGE; // block starts with a pilot tone
            #ifdef DEBUG_TAPE
            fputs("--- TONE\r\n", pfoDebug);
            #endif
            dwTapePulseCycles = CYCLE_ADJUST(*reinterpret_cast<word *>(pbTapeBlock+0x01));
            iTapeCycleCount += static_cast<int>(dwTapePulseCycles); // set cycle count for current level
            dwTapePulseCount = *reinterpret_cast<word *>(pbTapeBlock+0x01+0x02);
            #ifdef DEBUG_TAPE
            fprintf(pfoDebug, "%c %d\r\n",(bTapeLevel == TAPE_LEVEL_HIGH ? 'H':'L'), iTapeCycleCount);
            #endif
            return 1;

         case 0x13: // sequence of pulses of different length
            dwTapeStage = TAPE_SYNC_STAGE;
            #ifdef DEBUG_TAPE
            fputs("--- PULSE SEQ\r\n", pfoDebug);
            #endif
            dwTapePulseCount = *(pbTapeBlock+0x01);
            pwTapePulseTable =
            pwTapePulseTablePtr = reinterpret_cast<word *>(pbTapeBlock+0x01+0x01);
            pwTapePulseTableEnd = pwTapePulseTable + dwTapePulseCount;
            Tape_GetCycleCount();
            iTapeCycleCount += static_cast<int>(dwTapePulseCycles); // set cycle count for current level
            #ifdef DEBUG_TAPE
            fprintf(pfoDebug, "%c %d\r\n",(bTapeLevel == TAPE_LEVEL_HIGH ? 'H':'L'), iTapeCycleCount);
            #endif
            return 1;

         case 0x14: // pure data block
            dwTapeStage = TAPE_DATA_STAGE;
            #ifdef DEBUG_TAPE
            fputs("--- DATA\r\n", pfoDebug);
            #endif
            dwTapeZeroPulseCycles = CYCLE_ADJUST(*reinterpret_cast<word *>(pbTapeBlock+0x01)); // pulse length for a zero bit
            dwTapeOnePulseCycles = CYCLE_ADJUST(*reinterpret_cast<word *>(pbTapeBlock+0x01+0x02)); // pulse length for a one bit
            dwTapeDataCount = ((*reinterpret_cast<dword *>(pbTapeBlock+0x01+0x07) & 0x00ffffff) - 1) << 3; // (byte count - 1) * 8 bits
            dwTapeDataCount += *(pbTapeBlock+0x01+0x04); // add the number of bits in the last data byte
            pbTapeBlockData = pbTapeBlock+0x01+0x0a; // pointer to the tape data
            dwTapeBitsToShift = 0;
            Tape_ReadDataBit(); // get the first bit of the first data byte
            iTapeCycleCount += static_cast<int>(dwTapePulseCycles); // set cycle count for current level
            #ifdef DEBUG_TAPE
            fprintf(pfoDebug, "%c %d\r\n",(bTapeLevel == TAPE_LEVEL_HIGH ? 'H':'L'), iTapeCycleCount);
            #endif
            return 1;

         case 0x15: // direct recording
            dwTapeStage = TAPE_SAMPLE_DATA_STAGE;
            #ifdef DEBUG_TAPE
            fputs("--- SAMPLE DATA\r\n", pfoDebug);
            #endif
            dwTapePulseCycles = CYCLE_ADJUST(*reinterpret_cast<word *>(pbTapeBlock+0x01)); // number of T states per sample
            dwTapeDataCount = ((*reinterpret_cast<dword *>(pbTapeBlock+0x01+0x05) & 0x00ffffff) - 1) << 3; // (byte count - 1) * 8 bits
            dwTapeDataCount += *(pbTapeBlock+0x01+0x04); // add the number of bits in the last data byte
            pbTapeBlockData = pbTapeBlock+0x01+0x08; // pointer to the tape data
            dwTapeBitsToShift = 0;
            Tape_ReadSampleDataBit(); // get the first bit of the first data byte
            return 1;

         case 0x20: // pause
            if (*reinterpret_cast<word *>(pbTapeBlock+0x01)) { // was a pause requested?
               dwTapeStage = TAPE_PAUSE_STAGE;
               #ifdef DEBUG_TAPE
               fputs("--- PAUSE\r\n", pfoDebug);
               #endif
               dwTapePulseCycles = MS_TO_CYCLES(1); // start with a 1ms level opposite to the one last played
               iTapeCycleCount += static_cast<int>(dwTapePulseCycles); // set cycle count for current level
               dwTapePulseCycles = MS_TO_CYCLES(*reinterpret_cast<word *>(pbTapeBlock+0x01) - 1); // get the actual pause length
               dwTapePulseCount = 2; // just one pulse
               #ifdef DEBUG_TAPE
               fprintf(pfoDebug, "%c %d\r\n",(bTapeLevel == TAPE_LEVEL_HIGH ? 'H':'L'), iTapeCycleCount);
               #endif
               return 1;
            }
            else {
               pbTapeBlock += 2 + 1; // skip block if pause length is 0
            }
            break;

         case 0x21: // group start
            pbTapeBlock += *(pbTapeBlock+0x01) + 1 + 1; // nothing to do, skip the block
            break;

         case 0x22: // group end
            pbTapeBlock += 1; // nothing to do, skip the block
            break;

         case 0x30: // text description
            pbTapeBlock += *(pbTapeBlock+0x01) + 1 + 1; // nothing to do, skip the block
            break;

         case 0x31: // message block
            pbTapeBlock += *(pbTapeBlock+0x01+0x01) + 2 + 1; // nothing to do, skip the block
            break;

         case 0x32: // archive info
            pbTapeBlock += *reinterpret_cast<word *>(pbTapeBlock+0x01) + 2 + 1; // nothing to do, skip the block
            break;

         case 0x33: // hardware type
            pbTapeBlock += (*(pbTapeBlock+0x01) * 3) + 1 + 1; // nothing to do, skip the block
            break;

         case 0x34: // emulation info
            pbTapeBlock += 8 + 1; // nothing to do, skip the block
            break;

         case 0x35: // custom info block
            pbTapeBlock += *reinterpret_cast<dword *>(pbTapeBlock+0x01+0x10) + 0x14 + 1; // nothing to do, skip the block
            break;

         case 0x40: // snapshot block
            pbTapeBlock += (*reinterpret_cast<dword *>(pbTapeBlock+0x01+0x01) & 0x00ffffff) + 0x04 + 1; // nothing to do, skip the block
            break;

         case 0x5A: // another tzx/cdt file
            pbTapeBlock += 9 + 1; // nothing to do, skip the block
            break;

         default: // "extension rule"
            pbTapeBlock += *reinterpret_cast<dword *>(pbTapeBlock+0x01) + 4 + 1; // nothing to do, skip the block
      }
   }

   return 0; // we've reached the end of the image
}



void Tape_BlockDone()
{
   if (pbTapeBlock < pbTapeImageEnd) {
      switch (*pbTapeBlock) {

         case 0x10: // standard speed data block
            pbTapeBlock += *reinterpret_cast<word *>(pbTapeBlock+0x01+0x02) + 0x04 + 1;
            break;

         case 0x11: // turbo loading data block
            pbTapeBlock += (*reinterpret_cast<dword *>(pbTapeBlock+0x01+0x0f) & 0x00ffffff) + 0x12 + 1;
            break;

         case 0x12: // pure tone
            pbTapeBlock += 4 + 1;
            break;

         case 0x13: // sequence of pulses of different length
            pbTapeBlock += *(pbTapeBlock+0x01) * 2 + 1 + 1;
            break;

         case 0x14: // pure data block
            pbTapeBlock += (*reinterpret_cast<dword *>(pbTapeBlock+0x01+0x07) & 0x00ffffff) + 0x0a + 1;
            break;

         case 0x15: // direct recording
            pbTapeBlock += (*reinterpret_cast<dword *>(pbTapeBlock+0x01+0x05) & 0x00ffffff) + 0x08 + 1;
            break;

         case 0x20: // pause
            pbTapeBlock += 2 + 1;
            break;
      }

      if (!Tape_GetNextBlock()) {
         dwTapeStage = TAPE_END;
         CPC.tape_play_button = 0;
      }
   }
}



void Tape_UpdateLevel()
{
   switch (dwTapeStage) {

      case TAPE_PILOT_STAGE:
         Tape_SwitchLevel();
         dwTapePulseCount--;
         if (dwTapePulseCount > 0) { // is the pilot tone still playing?
            iTapeCycleCount += static_cast<int>(dwTapePulseCycles); // set cycle count for current level
            #ifdef DEBUG_TAPE
            fprintf(pfoDebug, "%c %d\r\n",(bTapeLevel == TAPE_LEVEL_HIGH ? 'H':'L'), iTapeCycleCount);
            #endif
         }
         else { // finished with the pilot tone
            switch (*pbTapeBlock) {

               case 0x10: // standard speed data block
                  dwTapeStage = TAPE_SYNC_STAGE;
                  #ifdef DEBUG_TAPE
                  fputs("--- SYNC\r\n", pfoDebug);
                  #endif
                  wCycleTable[0] = 667;
                  wCycleTable[1] = 735;
                  pwTapePulseTable =
                  pwTapePulseTablePtr = &wCycleTable[0];
                  pwTapePulseTableEnd = &wCycleTable[2];
                  Tape_GetCycleCount();
                  iTapeCycleCount += static_cast<int>(dwTapePulseCycles); // set cycle count for current level
                  #ifdef DEBUG_TAPE
                  fprintf(pfoDebug, "%c %d\r\n",(bTapeLevel == TAPE_LEVEL_HIGH ? 'H':'L'), iTapeCycleCount);
                  #endif
                  dwTapePulseCount = 2;
                  break;

               case 0x11: // turbo loading data block
                  dwTapeStage = TAPE_SYNC_STAGE;
                  #ifdef DEBUG_TAPE
                  fputs("--- SYNC\r\n", pfoDebug);
                  #endif
                  pwTapePulseTable =
                  pwTapePulseTablePtr = reinterpret_cast<word *>(pbTapeBlock+0x01+0x02);
                  pwTapePulseTableEnd = reinterpret_cast<word *>(pbTapeBlock+0x01+0x06);
                  Tape_GetCycleCount();
                  iTapeCycleCount += static_cast<int>(dwTapePulseCycles); // set cycle count for current level
                  #ifdef DEBUG_TAPE
                  fprintf(pfoDebug, "%c %d\r\n",(bTapeLevel == TAPE_LEVEL_HIGH ? 'H':'L'), iTapeCycleCount);
                  #endif
                  dwTapePulseCount = 2;
                  break;

               case 0x12: // pure tone
                  Tape_BlockDone();
                  break;
            }
         }
         break;

      case TAPE_SYNC_STAGE:
         Tape_SwitchLevel();
         dwTapePulseCount--;
         if (dwTapePulseCount > 0) {
            Tape_GetCycleCount();
            iTapeCycleCount += static_cast<int>(dwTapePulseCycles); // set cycle count for current level
            #ifdef DEBUG_TAPE
            fprintf(pfoDebug, "%c %d\r\n",(bTapeLevel == TAPE_LEVEL_HIGH ? 'H':'L'), iTapeCycleCount);
            #endif
         }
         else {
            switch (*pbTapeBlock) {

               case 0x10: // standard speed data block
                  dwTapeStage = TAPE_DATA_STAGE;
                  #ifdef DEBUG_TAPE
                  fputs("--- DATA\r\n", pfoDebug);
                  #endif
                  dwTapeZeroPulseCycles = CYCLE_ADJUST(855); // pulse length for a zero bit
                  dwTapeOnePulseCycles = CYCLE_ADJUST(1710); // pulse length for a one bit
                  dwTapeDataCount = *reinterpret_cast<word *>(pbTapeBlock+0x01+0x02) << 3; // byte count * 8 bits;
                  pbTapeBlockData = pbTapeBlock+0x01+0x04; // pointer to the tape data
                  dwTapeBitsToShift = 0;
                  Tape_ReadDataBit();
                  iTapeCycleCount += static_cast<int>(dwTapePulseCycles); // set cycle count for current level
                  #ifdef DEBUG_TAPE
                  fprintf(pfoDebug, "%c %d\r\n",(bTapeLevel == TAPE_LEVEL_HIGH ? 'H':'L'), iTapeCycleCount);
                  #endif
                  break;

               case 0x11: // turbo loading data block
                  dwTapeStage = TAPE_DATA_STAGE;
                  #ifdef DEBUG_TAPE
                  fputs("--- DATA\r\n", pfoDebug);
                  #endif
                  dwTapeZeroPulseCycles = CYCLE_ADJUST(*reinterpret_cast<word *>(pbTapeBlock+0x01+0x06)); // pulse length for a zero bit
                  dwTapeOnePulseCycles = CYCLE_ADJUST(*reinterpret_cast<word *>(pbTapeBlock+0x01+0x08)); // pulse length for a one bit
                  dwTapeDataCount = ((*reinterpret_cast<dword *>(pbTapeBlock+0x01+0x0f) & 0x00ffffff) - 1) << 3; // (byte count - 1) * 8 bits;
                  dwTapeDataCount += *(pbTapeBlock+0x01+0x0c); // add the number of bits in the last data byte
                  pbTapeBlockData = pbTapeBlock+0x01+0x12; // pointer to the tape data
                  dwTapeBitsToShift = 0;
                  Tape_ReadDataBit();
                  iTapeCycleCount += static_cast<int>(dwTapePulseCycles); // set cycle count for current level
                  #ifdef DEBUG_TAPE
                  fprintf(pfoDebug, "%c %d\r\n",(bTapeLevel == TAPE_LEVEL_HIGH ? 'H':'L'), iTapeCycleCount);
                  #endif
                  break;

               case 0x13: // sequence of pulses of different length
                  Tape_BlockDone();
                  break;
            }
         }
         break;

      case TAPE_DATA_STAGE:
         Tape_SwitchLevel();
         dwTapePulseCount--;
         if (dwTapePulseCount > 0) {
            iTapeCycleCount += static_cast<int>(dwTapePulseCycles); // set cycle count for current level
            #ifdef DEBUG_TAPE
            fprintf(pfoDebug, "%c %d\r\n",(bTapeLevel == TAPE_LEVEL_HIGH ? 'H':'L'), iTapeCycleCount);
            #endif
         }
         else {
            if (Tape_ReadDataBit()) {
               iTapeCycleCount += static_cast<int>(dwTapePulseCycles); // set cycle count for current level
               #ifdef DEBUG_TAPE
               fprintf(pfoDebug, "%c %d\r\n",(bTapeLevel == TAPE_LEVEL_HIGH ? 'H':'L'), iTapeCycleCount);
               #endif
            }
            else {
               switch (*pbTapeBlock) {

                  case 0x10: // standard speed data block
                     if (*reinterpret_cast<word *>(pbTapeBlock+0x01)) { // was a pause requested?
                        dwTapeStage = TAPE_PAUSE_STAGE;
                        #ifdef DEBUG_TAPE
                        fputs("--- PAUSE\r\n", pfoDebug);
                        #endif
                        dwTapePulseCycles = MS_TO_CYCLES(1); // start with a 1ms level opposite to the one last played
                        iTapeCycleCount += static_cast<int>(dwTapePulseCycles); // set cycle count for current level
                        #ifdef DEBUG_TAPE
                        fprintf(pfoDebug, "%c %d\r\n",(bTapeLevel == TAPE_LEVEL_HIGH ? 'H':'L'), iTapeCycleCount);
                        #endif
                        dwTapePulseCycles = MS_TO_CYCLES(*reinterpret_cast<word *>(pbTapeBlock+0x01) - 1); // pause in ms
                        dwTapePulseCount = 2; // just one pulse
                     }
                     else {
                        Tape_BlockDone();
                     }
                     break;

                  case 0x11: // turbo loading data block
                     if (*reinterpret_cast<word *>(pbTapeBlock+0x01+0x0d)) { // was a pause requested?
                        dwTapeStage = TAPE_PAUSE_STAGE;
                        #ifdef DEBUG_TAPE
                        fputs("--- PAUSE\r\n", pfoDebug);
                        #endif
                        dwTapePulseCycles = MS_TO_CYCLES(1); // start with a 1ms level opposite to the one last played
                        iTapeCycleCount += static_cast<int>(dwTapePulseCycles); // set cycle count for current level
                        #ifdef DEBUG_TAPE
                        fprintf(pfoDebug, "%c %d\r\n",(bTapeLevel == TAPE_LEVEL_HIGH ? 'H':'L'), iTapeCycleCount);
                        #endif
                        dwTapePulseCycles = MS_TO_CYCLES(*reinterpret_cast<word *>(pbTapeBlock+0x01+0x0d) - 1); // pause in ms
                        dwTapePulseCount = 2; // just one pulse
                     }
                     else {
                        Tape_BlockDone();
                     }
                     break;

                  case 0x14: // pure data block
                     if (*reinterpret_cast<word *>(pbTapeBlock+0x01+0x05)) { // was a pause requested?
                        dwTapeStage = TAPE_PAUSE_STAGE;
                        #ifdef DEBUG_TAPE
                        fputs("--- PAUSE\r\n", pfoDebug);
                        #endif
                        dwTapePulseCycles = MS_TO_CYCLES(1); // start with a 1ms level opposite to the one last played
                        iTapeCycleCount += static_cast<int>(dwTapePulseCycles); // set cycle count for current level
                        #ifdef DEBUG_TAPE
                        fprintf(pfoDebug, "%c %d\r\n",(bTapeLevel == TAPE_LEVEL_HIGH ? 'H':'L'), iTapeCycleCount);
                        #endif
                        dwTapePulseCycles = MS_TO_CYCLES(*reinterpret_cast<word *>(pbTapeBlock+0x01+0x05) - 1); // pause in ms
                        dwTapePulseCount = 2; // just one pulse
                     }
                     else {
                        Tape_BlockDone();
                     }
                     break;

                  default:
                     Tape_BlockDone();
               }
            }
         }
         break;

      case TAPE_SAMPLE_DATA_STAGE:
         if (!Tape_ReadSampleDataBit()) {
            if (*reinterpret_cast<word *>(pbTapeBlock+0x01+0x02)) { // was a pause requested?
               dwTapeStage = TAPE_PAUSE_STAGE;
               #ifdef DEBUG_TAPE
               fputs("--- PAUSE\r\n", pfoDebug);
               #endif
               dwTapePulseCycles = MS_TO_CYCLES(1); // start with a 1ms level opposite to the one last played
               iTapeCycleCount += static_cast<int>(dwTapePulseCycles); // set cycle count for current level
               #ifdef DEBUG_TAPE
               fprintf(pfoDebug, "%c %d\r\n",(bTapeLevel == TAPE_LEVEL_HIGH ? 'H':'L'), iTapeCycleCount);
               #endif
               dwTapePulseCycles = MS_TO_CYCLES(*reinterpret_cast<word *>(pbTapeBlock+0x01+0x02) - 1); // pause in ms
               dwTapePulseCount = 2; // just one pulse
            }
            else {
               Tape_BlockDone();
            }
         }
         break;

      case TAPE_PAUSE_STAGE:
         bTapeLevel = TAPE_LEVEL_LOW;
         dwTapePulseCount--;
         if (dwTapePulseCount > 0) {
            iTapeCycleCount += static_cast<int>(dwTapePulseCycles); // set cycle count for current level
            #ifdef DEBUG_TAPE
            fprintf(pfoDebug, "%c %d\r\n",(bTapeLevel == TAPE_LEVEL_HIGH ? 'H':'L'), iTapeCycleCount);
            #endif
         }
         else {
            Tape_BlockDone();
         }
         break;

      case TAPE_END:
         CPC.tape_play_button = 0;
         break;
   }
}



void Tape_Rewind()
{
   pbTapeBlock = &pbTapeImage[0];
   bTapeLevel = TAPE_LEVEL_LOW;
   iTapeCycleCount = 0;
   CPC.tape_play_button = 0;
   Tape_GetNextBlock();
}


// ─────────────────────────────────────────────────
// Tape block scanner — builds offset table from TZX image.
// Moved here from imgui_ui.cpp so the headless build can call it
// without dragging in the modern UI translation unit.  Pure tape format
// walker; helpers (safe_read_word / safe_read_dword) live in
// imgui_ui_testable.h which is header-only and ImGui-free.
// ─────────────────────────────────────────────────
void tape_scan_blocks()
{
  imgui_state.tape_block_offsets.clear();
  imgui_state.tape_current_block = 0;
  if (pbTapeImage.empty()) return;

  byte* p = &pbTapeImage[0];
  byte* end = pbTapeImageEnd;

  while (p < end) {
    imgui_state.tape_block_offsets.push_back(p);

    // Calculate block size with bounds checking
    // Same size logic as Tape_BlockDone + Tape_GetNextBlock.
    //
    // `block_size` is uint64_t (not size_t) because on 32-bit targets —
    // e.g. the Win32 MSVC CI configuration — size_t is uint32_t, and
    // `static_cast<size_t>(d) + N` would still wrap when `d` is near
    // UINT32_MAX in a malformed TZX file.  Using uint64_t guarantees the
    // addition is wide enough on every supported host.
    uint64_t block_size = 0;
    word w; dword d;

    switch (*p) {
      case 0x10: // Standard speed data
        if (!safe_read_word(p, end, 0x03, w)) goto done;
        block_size = w + 0x04 + 1;
        break;
      case 0x11: // Turbo speed data
        if (!safe_read_dword(p, end, 0x10, d)) goto done;
        block_size = (d & 0x00ffffff) + 0x12 + 1;
        break;
      case 0x12: // Pure tone
        block_size = 4 + 1;
        break;
      case 0x13: // Pulse sequence
        if (p + 2 > end) goto done;
        block_size = *(p+0x01) * 2 + 1 + 1;
        break;
      case 0x14: // Pure data
        if (!safe_read_dword(p, end, 0x08, d)) goto done;
        block_size = (d & 0x00ffffff) + 0x0a + 1;
        break;
      case 0x15: // Direct recording
        if (!safe_read_dword(p, end, 0x06, d)) goto done;
        block_size = (d & 0x00ffffff) + 0x08 + 1;
        break;
      case 0x20: // Pause
        block_size = 2 + 1;
        break;
      case 0x21: // Group start
        if (p + 2 > end) goto done;
        block_size = *(p+0x01) + 1 + 1;
        break;
      case 0x22: // Group end
        block_size = 1;
        break;
      case 0x30: // Text description
        if (p + 2 > end) goto done;
        block_size = *(p+0x01) + 1 + 1;
        break;
      case 0x31: // Message
        if (p + 3 > end) goto done;
        block_size = *(p+0x02) + 2 + 1;
        break;
      case 0x32: // Archive info
        if (!safe_read_word(p, end, 0x01, w)) goto done;
        block_size = w + 2 + 1;
        break;
      case 0x33: // Hardware type
        if (p + 2 > end) goto done;
        block_size = (*(p+0x01) * 3) + 1 + 1;
        break;
      case 0x34: // Emulation info
        block_size = 8 + 1;
        break;
      case 0x35: // Custom info
        if (!safe_read_dword(p, end, 0x11, d)) goto done;
        // Promote to uint64_t BEFORE adding the header bytes.  `d` is
        // dword (uint32_t); on 32-bit hosts size_t is also uint32_t, so
        // `static_cast<size_t>(d) + 0x14 + 1` would still wrap when d
        // is near UINT32_MAX in a malformed file.  uint64_t is always
        // wide enough.  Same pattern applied to the default case.
        block_size = static_cast<uint64_t>(d) + 0x14 + 1;
        break;
      case 0x40: // Snapshot
        if (!safe_read_dword(p, end, 0x02, d)) goto done;
        block_size = (d & 0x00ffffff) + 0x04 + 1;
        break;
      case 0x5A: // Glue
        block_size = 9 + 1;
        break;
      default: // Unknown block with 4-byte length
        if (!safe_read_dword(p, end, 0x01, d)) goto done;
        // Promote to uint64_t before adding (see case 0x35 above).
        block_size = static_cast<uint64_t>(d) + 4 + 1;
        break;
    }

    // Validate we won't advance past end.  Compare against remaining
    // distance instead of `p + block_size > end`: block_size comes from
    // a malformed TZX header and `p + block_size` could wrap on 32-bit
    // targets where pointer arithmetic isn't ULP-safe.  block_size is
    // uint64_t; promote `(end - p)` (ptrdiff_t) to uint64_t so the
    // comparison happens in 64-bit on every host.
    if (block_size > static_cast<uint64_t>(end - p)) goto done;
    p += static_cast<size_t>(block_size);  // safe: bounded by check above
  }
done:;
}
