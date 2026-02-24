/* konCePCja - Amstrad CPC Emulator
   (c) Copyright 1997-2005 Ulrich Doewich

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

#include <algorithm>
#include <iostream>
#include <sstream>
#include <chrono>
#include <string>
#include <thread>
#include <filesystem>
#include <unistd.h>

#include "SDL3/SDL.h"

static inline Uint32 MapRGBSurface(SDL_Surface* surface, Uint8 r, Uint8 g, Uint8 b) {
  const SDL_PixelFormatDetails* fmt = SDL_GetPixelFormatDetails(surface->format);
  SDL_Palette* pal = SDL_GetSurfacePalette(surface);
  return SDL_MapRGB(fmt, pal, r, g, b);
}

#include "koncepcja.h"
#include "koncepcja_ipc_server.h"
#include "autotype.h"
#include "crtc.h"
#include "symfile.h"
#include "disk.h"
#include "tape.h"
#include "video.h"
#include "z80.h"
#include "configuration.h"
#include "memutils.h"
#include "stringutils.h"
#include "zip.h"
#include "keyboard.h"
#include "trace.h"
#include "wav_recorder.h"
#include "amdrum.h"
#include "smartwatch.h"
#include "amx_mouse.h"
#include "drive_sounds.h"
#include "symbiface.h"
#include "m4board.h"
#include "io_dispatch.h"
#include "ym_recorder.h"
#include "avi_recorder.h"
#include "macos_menu.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_ui.h"
#include "command_palette.h"
#include "menu_actions.h"

Symfile g_symfile;

namespace {
  KoncepcjaIpcServer* g_ipc = new KoncepcjaIpcServer();
}
#include "cartridge.h"
#include "asic.h"
#include "argparse.h"
#include "slotshandler.h"
#include "fileutils.h"
#include "imgui_ui_testable.h"
#include "session_recording.h"
#include "silicon_disc.h"

#include <errno.h>
#include <cstring>

#include "portable-file-dialogs.h"

#include "errors.h"
#include "log.h"

#include "savepng.h"

#define MAX_LINE_LEN 256

#define MAX_NB_JOYSTICKS 2

#define POLL_INTERVAL_MS 1

#ifndef DESTDIR
#define DESTDIR ""
#endif

extern byte bTapeLevel;
extern t_z80regs z80;
extern std::vector<Breakpoint> breakpoints;

extern dword *ScanPos;
extern dword *ScanStart;
extern word MaxVSync;
extern t_flags1 flags1;
extern t_new_dt new_dt;
extern t_disk_format disk_format[];

extern byte* pbCartridgePages[];

extern SDL_Window* mainSDLWindow;
extern SDL_Renderer* renderer;

SDL_AudioStream* audio_stream = nullptr;
SDL_Surface *back_surface = nullptr;
video_plugin* vid_plugin;

static bool g_take_screenshot = false;
bool g_headless = false;
static bool g_exit_on_break = false;
static enum { EXIT_NONE, EXIT_FRAMES, EXIT_MS } g_exit_mode = EXIT_NONE;
static dword g_exit_target = 0;
static dword g_exit_start_ticks = 0;

static int topbar_height_px = 24;

extern t_CPC CPC;
SDL_Joystick* joysticks[MAX_NB_JOYSTICKS];

dword dwTicks, dwTicksOffset, dwTicksTarget, dwTicksTargetFPS;
dword dwFPS, dwFrameCount;
dword dwXScale, dwYScale;

dword osd_timing;
std::string osd_message;

std::string lastSavedSnapshot;

dword dwBreakPoint, dwTrace, dwMF2ExitAddr;
dword dwMF2Flags = 0;
std::unique_ptr<byte[]> pbSndBuffer;
byte *pbGPBuffer = nullptr;
byte *pbSndBufferEnd = nullptr;
byte *pbSndStream = nullptr;
byte *membank_read[4], *membank_write[4], *memmap_ROM[256];
byte *pbRAM = nullptr;
byte *pbRAMbuffer = nullptr;
byte *pbROM = nullptr;
byte *pbROMlo = nullptr;
byte *pbROMhi = nullptr;
byte *pbExpansionROM = nullptr;
byte *pbMF2ROMbackup = nullptr;
byte *pbMF2ROM = nullptr;
std::vector<byte> pbTapeImage;
byte keyboard_matrix[16];

std::list<SDL_Event> virtualKeyboardEvents;
dword nextVirtualEventFrameCount, dwFrameCountOverall = 0;
dword breakPointsToSkipBeforeProceedingWithVirtualEvents = 0;

t_MemBankConfig membank_config;

FILE *pfileObject;
FILE *pfoPrinter;

#ifdef DEBUG
dword dwDebugFlag = 0;
FILE *pfoDebug = nullptr;
#endif

#define MAX_FREQ_ENTRIES 5
dword freq_table[MAX_FREQ_ENTRIES] = {
   11025,
   22050,
   44100,
   48000,
   96000
};

#include "font.h"

void set_osd_message(const std::string& message, uint32_t for_milliseconds) {
   osd_timing = SDL_GetTicks() + for_milliseconds;
   osd_message = " " + message;
}

double colours_rgb[32][3] = {
   { 0.5, 0.5, 0.5 }, { 0.5, 0.5, 0.5 },{ 0.0, 1.0, 0.5 }, { 1.0, 1.0, 0.5 },
   { 0.0, 0.0, 0.5 }, { 1.0, 0.0, 0.5 },{ 0.0, 0.5, 0.5 }, { 1.0, 0.5, 0.5 },
   { 1.0, 0.0, 0.5 }, { 1.0, 1.0, 0.5 },{ 1.0, 1.0, 0.0 }, { 1.0, 1.0, 1.0 },
   { 1.0, 0.0, 0.0 }, { 1.0, 0.0, 1.0 },{ 1.0, 0.5, 0.0 }, { 1.0, 0.5, 1.0 },
   { 0.0, 0.0, 0.5 }, { 0.0, 1.0, 0.5 },{ 0.0, 1.0, 0.0 }, { 0.0, 1.0, 1.0 },
   { 0.0, 0.0, 0.0 }, { 0.0, 0.0, 1.0 },{ 0.0, 0.5, 0.0 }, { 0.0, 0.5, 1.0 },
   { 0.5, 0.0, 0.5 }, { 0.5, 1.0, 0.5 },{ 0.5, 1.0, 0.0 }, { 0.5, 1.0, 1.0 },
   { 0.5, 0.0, 0.0 }, { 0.5, 0.0, 1.0 },{ 0.5, 0.5, 0.0 }, { 0.5, 0.5, 1.0 }
};

// original RGB color to GREEN LUMA converted by Ulrich Doewich
// unknown formula.
double colours_green_classic[32] = {
   0.5647, 0.5647, 0.7529, 0.9412,
   0.1882, 0.3765, 0.4706, 0.6588,
   0.3765, 0.9412, 0.9098, 0.9725,
   0.3451, 0.4078, 0.6275, 0.6902,
   0.1882, 0.7529, 0.7216, 0.7843,
   0.1569, 0.2196, 0.4392, 0.5020,
   0.2824, 0.8471, 0.8157, 0.8784,
   0.2510, 0.3137, 0.5333, 0.5961
};

// added by a proposal from libretro project,
// see https://github.com/ikari/konCePCja/issues/135

double colours_green_libretro[32] = {
   0.5755,  0.5755,  0.7534,  0.9718,
   0.1792,  0.3976,  0.4663,  0.6847,
   0.3976,  0.9718,  0.9136,  1.0300,
   0.3394,  0.4558,  0.6265,  0.7429,
   0.1792,  0.7534,  0.6952,  0.8116,
   0.1210,  0.2374,  0.4081,  0.5245,
   0.2884,  0.8626,  0.8044,  0.9208,
   0.2302,  0.3466,  0.5173,  0.6337
};

// interface to use the palette also from tests
double *video_get_green_palette(int mode) {
   if (!mode)
      return colours_green_classic;
   return colours_green_libretro;
}

double *video_get_rgb_color(int color) {
   return colours_rgb[color];
}

SDL_Color colours[32];

byte bit_values[8] = {
   0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80
};

#define MAX_ROM_MODS 2
#include "rom_mods.h"

char chAppPath[_MAX_PATH + 1];
std::filesystem::path binPath; // Where the binary is
char chROMSelected[_MAX_PATH + 1];
std::string chROMFile[4] = {
   "cpc464.rom",
   "cpc664.rom",
   "cpc6128.rom",
   "system.cpr"
};

t_CPC::t_CPC() {
  driveA.drive = DRIVE::DSK_A;
  driveB.drive = DRIVE::DSK_B;
  tape.drive = DRIVE::TAPE;
  cartridge.drive = DRIVE::CARTRIDGE;
  snapshot.drive = DRIVE::SNAPSHOT;
}

t_CPC CPC;
t_CRTC CRTC;
t_FDC FDC;
t_GateArray GateArray;
t_PPI PPI;
t_PSG PSG;
t_VDU VDU;

t_drive driveA;
t_drive driveB;

#define psg_write \
{ \
   byte control = PSG.control & 0xc0; /* isolate PSG control bits */ \
   if (control == 0xc0) { /* latch address? */ \
      PSG.reg_select = psg_data; /* select new PSG register */ \
   } else if (control == 0x80) { /* write? */ \
      if (PSG.reg_select < 16) { /* valid register? */ \
         SetAYRegister(PSG.reg_select, psg_data); \
      } \
   } \
}

enum ApplicationWindowState
{
   Minimized,              // application window has been iconified
   Restored,               // application window has been restored
   GainedFocus,            // application window got input focus
   LostFocus               // application window lost input focus
} _appWindowState;

CapriceArgs args;

void ga_init_banking (t_MemBankConfig& membank_config, unsigned char RAM_bank)
{
   byte *romb0, *romb1, *romb2, *romb3, *romb4, *romb5, *romb6, *romb7;
   byte *pbRAMbank;

   romb0 = pbRAM;
   romb1 = pbRAM + 1*16384;
   romb2 = pbRAM + 2*16384;
   romb3 = pbRAM + 3*16384;

   // Check if this bank falls in the Silicon Disc range
   if (g_silicon_disc.owns_bank(RAM_bank)) {
      pbRAMbank = g_silicon_disc.bank_ptr(RAM_bank - SILICON_DISC_FIRST_BANK);
   } else {
      pbRAMbank = pbRAM + ((RAM_bank + 1) * 65536);
   }
   romb4 = pbRAMbank;
   romb5 = pbRAMbank + 1*16384;
   romb6 = pbRAMbank + 2*16384;
   romb7 = pbRAMbank + 3*16384;

   membank_config[0][0] = romb0;
   membank_config[0][1] = romb1;
   membank_config[0][2] = romb2;
   membank_config[0][3] = romb3;

   membank_config[1][0] = romb0;
   membank_config[1][1] = romb1;
   membank_config[1][2] = romb2;
   membank_config[1][3] = romb7;

   membank_config[2][0] = romb4;
   membank_config[2][1] = romb5;
   membank_config[2][2] = romb6;
   membank_config[2][3] = romb7;

   membank_config[3][0] = romb0;
   membank_config[3][1] = romb3;
   membank_config[3][2] = romb2;
   membank_config[3][3] = romb7;

   membank_config[4][0] = romb0;
   membank_config[4][1] = romb4;
   membank_config[4][2] = romb2;
   membank_config[4][3] = romb3;

   membank_config[5][0] = romb0;
   membank_config[5][1] = romb5;
   membank_config[5][2] = romb2;
   membank_config[5][3] = romb3;

   membank_config[6][0] = romb0;
   membank_config[6][1] = romb6;
   membank_config[6][2] = romb2;
   membank_config[6][3] = romb3;

   membank_config[7][0] = romb0;
   membank_config[7][1] = romb7;
   membank_config[7][2] = romb2;
   membank_config[7][3] = romb3;
}



void ga_memory_manager ()
{
   dword mem_bank;
   if (CPC.ram_size == 64) { // 64KB of RAM?
      mem_bank = 0; // no expansion memory
      GateArray.RAM_config = 0; // the only valid configuration is 0
   } else if (CPC.ram_size > 576) {
      // Yarek 4MB expansion: 6-bit bank number from data bits 5-3 (low) and
      // inverted port address bits 5-3 (high), stored in GateArray.RAM_ext
      mem_bank = (static_cast<dword>(GateArray.RAM_ext) << 3) | ((GateArray.RAM_config >> 3) & 7);
      if (((mem_bank+2)*64) > CPC.ram_size) { // selection is beyond available memory?
         mem_bank = 0; // force default mapping
      }
   } else {
      mem_bank = (GateArray.RAM_config >> 3) & 7; // extract expansion memory bank
      if (!g_silicon_disc.owns_bank(mem_bank) &&
          ((mem_bank+2)*64) > CPC.ram_size) { // selection is beyond available memory?
         mem_bank = 0; // force default mapping
      }
   }
   if (mem_bank != GateArray.RAM_bank) { // requested bank is different from the active one?
      GateArray.RAM_bank = mem_bank;
      ga_init_banking(membank_config, GateArray.RAM_bank);
   }
   for (int n = 0; n < 4; n++) { // remap active memory banks
      membank_read[n] = membank_config[GateArray.RAM_config & 7][n];
      membank_write[n] = membank_config[GateArray.RAM_config & 7][n];
   }
   if (!(GateArray.ROM_config & 0x04)) { // lower ROM is enabled?
      if (dwMF2Flags & MF2_ACTIVE) { // is the Multiface 2 paged in?
         // TODO: I think this is why the MF2 doesn't work properly:
         // ROM should be loaded R/O at 0x0000-0x1FFF (i.e not loaded in membank_write ?)
         // Writes should probably be disabled in membank_write (pointing to a dummy buffer, but not MF2 ROM ?)
         // MF2 also has a RAM (8kB) that should be loaded as R/W at 0x2000-0x3FFF
         membank_read[GateArray.lower_ROM_bank] = pbMF2ROM;
         membank_write[GateArray.lower_ROM_bank] = pbMF2ROM;
      } else {
         membank_read[GateArray.lower_ROM_bank] = pbROMlo; // 'page in' lower ROM
      }
   }
   if (CPC.model > 2 && GateArray.registerPageOn) {
      membank_read[1] = pbRegisterPage;
      membank_write[1] = pbRegisterPage;
   }
   if (!(GateArray.ROM_config & 0x08)) { // upper/expansion ROM is enabled?
      membank_read[3] = pbExpansionROM; // 'page in' upper/expansion ROM
   }
}


// ── MF2 I/O dispatch handler ────────────────────
// MF2 paging uses file-local dwMF2Flags and ga_memory_manager(),
// so its handler must live in this file.

static bool s_mf2_enabled = false;  // synced from CPC.mf2

static bool mf2_out_handler(reg_pair port, byte /*val*/)
{
   if (port.b.h != 0xFE) return false;
   if ((port.b.l == 0xE8) && (!(dwMF2Flags & MF2_INVISIBLE))) {
      dwMF2Flags |= MF2_ACTIVE;
      ga_memory_manager();
      return true;
   }
   if (port.b.l == 0xEA) {
      dwMF2Flags &= ~MF2_ACTIVE;
      ga_memory_manager();
      return true;
   }
   return false;
}

void mf2_register_io()
{
   s_mf2_enabled = CPC.mf2 != 0;
   io_register_out(0xFE, mf2_out_handler, &s_mf2_enabled, "Multiface II");
}


byte z80_IN_handler (reg_pair port)
{
   if (z80_check_io_breakpoint(port.w.l, IO_IN)) {
      z80.breakpoint_reached = 1;
   }
   byte ret_val;

   ret_val = 0xff; // default return value
// CRTC -----------------------------------------------------------------------
   if (!(port.b.h & 0x40)) { // CRTC chip select?
      byte crtc_rport = port.b.h & 3;
      bool is_reg_read = false;
      if (crtc_rport == 3) {
         // &BFxx: read register on all types
         is_reg_read = true;
      } else if (crtc_rport == 2) {
         if (CRTC.crtc_type == 1) {
            // Type 1 (UM6845R): &BExx reads status register
            ret_val = 0;
            if (CRTC.line_count >= CRTC.registers[6]) {
               ret_val |= 0x20; // bit 5: vertical blanking active
            }
            // bit 6: light pen strobe (not emulated, always 0)
         } else if (CRTC.crtc_type == 3) {
            // Type 3 (ASIC): &BExx also reads registers
            is_reg_read = true;
         }
         // Types 0/2: &BExx has no function, ret_val stays 0xff
      }
      if (is_reg_read) {
         byte reg = CRTC.reg_select;
         switch (CRTC.crtc_type) {
            case 0: // HD6845S: R12-R17 readable, rest returns 0
               if (reg >= 12 && reg <= 17) {
                  ret_val = CRTC.registers[reg];
               } else {
                  ret_val = 0;
               }
               break;
            case 1: // UM6845R: R14-R17 readable, R12-R13 write-only (return 0)
                     // R31 returns 0xFF, R18-30 return 0
               if (reg >= 14 && reg <= 17) {
                  ret_val = CRTC.registers[reg];
               } else if (reg == 31) {
                  ret_val = 0xff;
               } else {
                  ret_val = 0;
               }
               break;
            case 2: // MC6845: R14-R17 readable, rest returns 0
               if (reg >= 14 && reg <= 17) {
                  ret_val = CRTC.registers[reg];
               } else {
                  ret_val = 0;
               }
               break;
            case 3: // AMS40489 (ASIC): R12-R17 readable, R0-R11 write-only
            default:
               if (reg >= 12 && reg <= 17) {
                  ret_val = CRTC.registers[reg];
               } else {
                  ret_val = 0;
               }
               break;
         }
      }
   }
// PPI ------------------------------------------------------------------------
   else if (!(port.b.h & 0x08)) { // PPI chip select?
      byte ppi_port = port.b.h & 3;
      switch (ppi_port) {
         case 0: // read from port A?
            if (PPI.control & 0x10) { // port A set to input?
               if ((PSG.control & 0xc0) == 0x40) { // PSG control set to read?
                  if (PSG.reg_select < 16) { // within valid range?
                     if (PSG.reg_select == 14) { // PSG port A?
                        if (!(PSG.RegisterAY.Index[7] & 0x40)) { // port A in input mode?
                           ret_val = keyboard_matrix[CPC.keyboard_line & 0x0f]; // read keyboard matrix node status
                        } else {
                           ret_val = PSG.RegisterAY.Index[14] & (keyboard_matrix[CPC.keyboard_line & 0x0f]); // return last value w/ logic AND of input
                        }
                        ret_val &= io_fire_kbd_read_hooks(CPC.keyboard_line & 0x0f);
                        LOG_DEBUG("PPI read from portA (keyboard_line): " << CPC.keyboard_line << " - " << static_cast<int>(ret_val));
                     } else if (PSG.reg_select == 15) { // PSG port B?
                        if ((PSG.RegisterAY.Index[7] & 0x80)) { // port B in output mode?
                           ret_val = PSG.RegisterAY.Index[15]; // return stored value
                           LOG_DEBUG("PPI read from portA (PSG portB): " << CPC.keyboard_line << " - " << static_cast<int>(ret_val));
                        }
                     } else {
                        ret_val = PSG.RegisterAY.Index[PSG.reg_select]; // read PSG register
                        LOG_DEBUG("PPI read from portA (registers): " << CPC.keyboard_line << " - " << static_cast<int>(ret_val));
                     }
                  }
               }
            } else {
               ret_val = PPI.portA; // return last programmed value
               LOG_DEBUG("PPI read from portA (last value): " << CPC.keyboard_line << " - " << static_cast<int>(ret_val));
            }
            break;

         case 1: // read from port B?
            // 6128+: always use port B as input as this fixes Tintin on the moon.
            // This should always be the case anyway but do not activate it for other model for now, let's validate it before.
            // TODO: verify with CPC (non-plus) if we go in the else in some cases
            if (CPC.model > 2 || PPI.control & 2) { // port B set to input?
               LOG_DEBUG("PPI read from portB: bTapeLevel=" << static_cast<int>(bTapeLevel) << ", CPC.printer=" << CPC.printer << ", CPC.jumpers=" << CPC.jumpers << ", CRTC.flag_invsync=" << CRTC.flag_invsync)
               ret_val = bTapeLevel | // tape level when reading
                         (CPC.printer ? 0 : 0x40) | // ready line of connected printer
                         (CPC.jumpers & 0x7f) | // manufacturer + 50Hz
                         (CRTC.flag_invsync ? 1 : 0); // VSYNC status
            } else {
               LOG_DEBUG("PPI read from portB: " << static_cast<int>(PPI.portB))
               ret_val = PPI.portB; // return last programmed value
            }
            break;

         case 2: // read from port C?
            byte direction = PPI.control & 9; // isolate port C directions
            ret_val = PPI.portC; // default to last programmed value
            if (direction) { // either half set to input?
               if (direction & 8) { // upper half set to input?
                  ret_val &= 0x0f; // blank out upper half
                  byte val = PPI.portC & 0xc0; // isolate PSG control bits
                  if (val == 0xc0) { // PSG specify register?
                     val = 0x80; // change to PSG write register
                  }
                  ret_val |= val | 0x20; // casette write data is always set
                  if (CPC.tape_motor) {
                     ret_val |= 0x10; // set the bit if the tape motor is running
                  }
                  LOG_DEBUG("PPI read from portC (upper half): " << static_cast<int>(ret_val));
               }
               if (!(direction & 1)) { // lower half set to output?
                  ret_val |= 0x0f; // invalid - set all bits
                  LOG_DEBUG("PPI read from portC (lower half): " << static_cast<int>(ret_val));
               }
            }
            LOG_DEBUG("PPI read from portC: " << static_cast<int>(ret_val));
            break;
      }
   }
// ----------------------------------------------------------------------------
   else if (!(port.b.h & 0x04)) { // external peripheral?
      if ((port.b.h == 0xfb) && (!(port.b.l & 0x80))) { // FDC?
         if (!(port.b.l & 0x01)) { // FDC status register?
            ret_val = fdc_read_status();
         } else { // FDC data register
            ret_val = fdc_read_data();
         }
      }
   }
// Peripheral dispatch (Symbiface II, etc.) ------------------------------------
   ret_val = io_dispatch_in(port, ret_val);
   LOG_DEBUG("IN on port " << std::hex << static_cast<int>(port.w.l) << ", ret_val=" << static_cast<int>(ret_val) << std::dec);
   return ret_val;
}



void z80_OUT_handler (reg_pair port, byte val)
{
   if (z80_check_io_breakpoint(port.w.l, IO_OUT, val)) {
      z80.breakpoint_reached = 1;
   }
   LOG_DEBUG("OUT on port " << std::hex << static_cast<int>(port.w.l) << ", val=" << static_cast<int>(val) << std::dec);
// Gate Array -----------------------------------------------------------------
   if ((port.b.h & 0xc0) == 0x40) { // GA chip select?
      switch (val >> 6) {
         case 0: // select pen
            #ifdef DEBUG_GA
            if (dwDebugFlag) {
               fprintf(pfoDebug, "pen 0x%02x\r\n", val);
            }
            #endif
            GateArray.pen = val & 0x10 ? 0x10 : val & 0x0f; // if bit 5 is set, pen indexes the border colour
            LOG_DEBUG("Set pen value to " << static_cast<int>(GateArray.pen));
            if (CPC.mf2) { // MF2 enabled?
               *(pbMF2ROM + 0x03fcf) = val;
            }
            break;
         case 1: // set colour
            #ifdef DEBUG_GA
            if (dwDebugFlag) {
               fprintf(pfoDebug, "clr 0x%02x\r\n", val);
            }
            #endif
            {
               byte colour = val & 0x1f; // isolate colour value
               LOG_DEBUG("Set ink value " << static_cast<int>(GateArray.pen) << " to " << static_cast<int>(colour));
               GateArray.ink_values[GateArray.pen] = colour;
               GateArray.palette[GateArray.pen] = MapRGBSurface(back_surface, colours[colour].r, colours[colour].g, colours[colour].b);
               if (GateArray.pen < 2) {
                  byte r = (static_cast<dword>(colours[GateArray.ink_values[0]].r) + static_cast<dword>(colours[GateArray.ink_values[1]].r)) >> 1;
                  byte g = (static_cast<dword>(colours[GateArray.ink_values[0]].g) + static_cast<dword>(colours[GateArray.ink_values[1]].g)) >> 1;
                  byte b = (static_cast<dword>(colours[GateArray.ink_values[0]].b) + static_cast<dword>(colours[GateArray.ink_values[1]].b)) >> 1;
                  GateArray.palette[33] = MapRGBSurface(back_surface, r, g, b); // update the mode 2 'anti-aliasing' colour
               }
               // TODO: update pbRegisterPage
            }
            if (CPC.mf2) { // MF2 enabled?
               int iPen = *(pbMF2ROM + 0x03fcf);
               *(pbMF2ROM + (0x03f90 | ((iPen & 0x10) << 2) | (iPen & 0x0f))) = val;
            }
            break;
         case 2: // set mode
            if (!asic.locked && (val & 0x20)) {
               // 6128+ RMR2 register
               int membank = (val >> 3) & 3;
               if (membank == 3) { // Map register page at 0x4000
                  LOG_DEBUG("Register page on");
                  GateArray.registerPageOn = true;
                  membank = 0;
               } else {
                  LOG_DEBUG("Register page off");
                  GateArray.registerPageOn = false;
               }
               int page = (val & 0x7);
               LOG_DEBUG("RMR2: Low bank rom = 0x" << std::hex << (4*membank) << std::dec << "000 - page " << page);
               GateArray.lower_ROM_bank = membank;
               pbROMlo = pbCartridgePages[page];
               ga_memory_manager();
            } else {
               #ifdef DEBUG_GA
               if (dwDebugFlag) {
                  fprintf(pfoDebug, "rom 0x%02x\r\n", val);
               }
               #endif
               LOG_DEBUG("MRER: ROM config = " << std::hex << static_cast<int>(val) << std::dec << " - mode=" << static_cast<int>(val & 0x03));
               GateArray.ROM_config = val;
               GateArray.requested_scr_mode = val & 0x03; // request a new CPC screen mode
               ga_memory_manager();
               if (val & 0x10) { // delay Z80 interrupt?
                  z80.int_pending = 0; // clear pending interrupts
                  GateArray.sl_count = 0; // reset GA scanline counter
               }
               if (CPC.mf2) { // MF2 enabled?
                  *(pbMF2ROM + 0x03fef) = val;
               }
            }
            break;
         case 3:
            // Reading https://www.cpcwiki.eu/index.php/Gate_Array
            // suggests this should set memory configuration but actually this is contradicted by:
            //  - http://cpctech.cpc-live.com/docs/rampage.html
            //  - https://www.cpcwiki.eu/index.php/I/O_Port_Summary
            //  - https://www.cpcwiki.eu/index.php/Default_I/O_Port_Summary
            // which tell that this is controlled by address at %0xxxxxxx xxxxxxxx
            // so this is handled separately below
            break;
      }
   }
// Memory configuration -------------------------------------------------------
   if (!(port.b.h & 0x80) && (val & 0xc0) == 0xc0) {
     #ifdef DEBUG_GA
     if (dwDebugFlag) {
        fprintf(pfoDebug, "mem 0x%02x\r\n", val);
     }
     #endif
     LOG_DEBUG("RAM config: " << std::hex << static_cast<int>(val) << std::dec);
     GateArray.RAM_config = val;
     // Yarek 4MB: extract extended bank bits from inverted port address bits 5-3
     // Standard port #7F has bits 5-3 = 111, inverted = 000 (bank 0, backward compatible)
     GateArray.RAM_ext = (~port.b.h >> 3) & 7;
     ga_memory_manager();
     if (CPC.mf2) { // MF2 enabled?
        *(pbMF2ROM + 0x03fff) = val;
     }
   }
// CRTC -----------------------------------------------------------------------
   if (!(port.b.h & 0x40)) { // CRTC chip select?
      byte crtc_port = port.b.h & 3;
      if (crtc_port == 0) { // CRTC register select?
         // 6128+: this is where we detect the ASIC (un)locking sequence
         if (CPC.model > 2) {
           asic_poke_lock_sequence(val);
         }
         CRTC.reg_select = val;
         if (CPC.mf2) { // MF2 enabled?
            *(pbMF2ROM + 0x03cff) = val;
         }
      }
      else if (crtc_port == 1) { // CRTC write data?
         if (CRTC.reg_select < 16) { // only registers 0 - 15 can be written to
            LOG_DEBUG("CRTC write to register " << static_cast<int>(CRTC.reg_select) << ": " << static_cast<int>(val));
            switch (CRTC.reg_select) {
               case 0: // horizontal total
                  CRTC.registers[0] = val;
                  break;
               case 1: // horizontal displayed
                  CRTC.registers[1] = val;
                  update_skew();
                  break;
               case 2: // horizontal sync position
                  CRTC.registers[2] = val;
                  break;
               case 3: // sync width
                  CRTC.registers[3] = val;
                  CRTC.hsw = val & 0x0f; // isolate horizontal sync width
                  if (CRTC.crtc_type == 1 || CRTC.crtc_type == 2) {
                     // Types 1/2: VSYNC width fixed at 16 lines, R3 upper bits ignored
                     CRTC.vsw = 0; // 0 = 16 lines (counter wraps at 4 bits)
                  } else {
                     // Types 0/3: VSYNC width from R3 bits 7..4 (0 means 16)
                     CRTC.vsw = val >> 4;
                  }
                  // Type 0: HSYNC width 0 means no HSYNC (no interrupts)
                  // Type 2/3: HSYNC width 0 means 16
                  if (CRTC.hsw == 0 && (CRTC.crtc_type == 2 || CRTC.crtc_type == 3)) {
                     CRTC.hsw = 16; // treat 0 as 16 on types 2/3
                  }
                  break;
               case 4: // vertical total
                  CRTC.registers[4] = val & 0x7f;
                  if (CRTC.CharInstMR == CharMR2) {
                     if (CRTC.line_count == CRTC.registers[4]) { // matches vertical total?
                        if (CRTC.raster_count == CRTC.registers[9]) { // matches maximum raster address?
                           CRTC.flag_startvta = 1;
                        }
                     }
                  }
                  break;
               case 5: // vertical total adjust
                  CRTC.registers[5] = val & 0x1f;
                  break;
               case 6: // vertical displayed
                  CRTC.registers[6] = val & 0x7f;
                  if (CRTC.line_count == CRTC.registers[6]) { // matches vertical displayed?
                     new_dt.NewDISPTIMG = 0;
                  }
                  break;
               case 7: // vertical sync position
                  CRTC.registers[7] = val & 0x7f;
                  {
                     dword temp = 0;
                     if (CRTC.line_count == CRTC.registers[7]) { // matches vertical sync position?
                        temp++;
                        if (CRTC.r7match != temp) {
                           CRTC.r7match = temp;
                           if (CRTC.char_count >= 2) {
                              CRTC.flag_resvsync = 0;
                              if (!CRTC.flag_invsync) {
                                 CRTC.vsw_count = 0;
                                 CRTC.flag_invsync = 1;
                                 flags1.monVSYNC = 26;
                                 GateArray.hs_count = 2; // GA delays its VSYNC by two CRTC HSYNCs
                              }
                           }
                        }
                     }
                     else {
                        CRTC.r7match = 0;
                     }
                  }
                  break;
               case 8: // interlace and skew
                  if (CRTC.crtc_type == 1 || CRTC.crtc_type == 2) {
                     // Types 1/2: only bits 1..0 (interlace mode) are meaningful
                     CRTC.registers[8] = val & 0x03;
                  } else {
                     // Types 0/3: full register (skew + interlace)
                     CRTC.registers[8] = val;
                  }
                  update_skew();
                  break;
               case 9: // maximum raster count
                  CRTC.registers[9] = val & 0x1f;
                  {
                     dword temp = 0;
                     if (CRTC.raster_count == CRTC.registers[9]) { // matches maximum raster address?
                        temp = 1;
                        CRTC.flag_resscan = 1; // request a raster counter reset
                     }
                     if (CRTC.r9match != temp) {
                        CRTC.r9match = temp;
                        if (temp) {
                           CRTC.CharInstMR = CharMR1;
                        }
                     }
                     if (CRTC.raster_count == CRTC.registers[9]) { // matches maximum raster address?
                        if (CRTC.char_count == CRTC.registers[1]) {
                           CRTC.next_addr = CRTC.addr + CRTC.char_count;
                        }
                        if (CRTC.char_count == CRTC.registers[0]) { // matches horizontal total?
                           CRTC.flag_reschar = 1; // request a line count update
                        }
                        if (!CRTC.flag_startvta) {
                           CRTC.flag_resscan = 1;
                        }
                     } else {
                        if (!CRTC.flag_invta) { // not in vertical total adjust?
                           CRTC.flag_resscan = 0;
                        }
                     }
                  }
                  break;
               case 10: // cursor start raster
                  CRTC.registers[10] = val & 0x7f;
                  break;
               case 11: // cursor end raster
                  CRTC.registers[11] = val & 0x1f;
                  break;
               case 12: // start address high byte
                  CRTC.registers[12] = val & 0x3f;
                  CRTC.requested_addr = CRTC.registers[13] + (CRTC.registers[12] << 8);
                  // Type 1 (UM6845R): when VCC=0, R12/R13 re-read at start of each line
                  if (CRTC.crtc_type == 1 && CRTC.line_count == 0) {
                     CRTC.addr = CRTC.requested_addr;
                     CRTC.next_addr = CRTC.requested_addr;
                  }
                  break;
               case 13: // start address low byte
                  CRTC.registers[13] = val;
                  CRTC.requested_addr = CRTC.registers[13] + (CRTC.registers[12] << 8);
                  // Type 1 (UM6845R): when VCC=0, R12/R13 re-read at start of each line
                  if (CRTC.crtc_type == 1 && CRTC.line_count == 0) {
                     CRTC.addr = CRTC.requested_addr;
                     CRTC.next_addr = CRTC.requested_addr;
                  }
                  break;
               case 14: // cursor address high byte
                  CRTC.registers[14] = val & 0x3f;
                  break;
               case 15: // cursor address low byte
                  CRTC.registers[15] = val;
                  break;
            }
         }
         if (CPC.mf2) { // MF2 enabled?
            *(pbMF2ROM + (0x03db0 | (*(pbMF2ROM + 0x03cff) & 0x0f))) = val;
         }
         #ifdef DEBUG_CRTC
         if (dwDebugFlag) {
            fprintf(pfoDebug, "%02x = %02x\r\n", CRTC.reg_select, val);
         }
         #endif
      }
   }
// ROM select -----------------------------------------------------------------
   if (!(port.b.h & 0x20)) { // ROM select?
      if (CPC.model <= 2) {
         GateArray.upper_ROM = val;
         pbExpansionROM = memmap_ROM[val];
         if (pbExpansionROM == nullptr) { // selected expansion ROM not present?
            pbExpansionROM = pbROMhi; // revert to BASIC ROM
         }
      } else {
         uint32_t page = 1; // Default to basic page
         LOG_DEBUG("ROM select: " << static_cast<int>(val));
         if (val == 7) {
            page = 3;
         } else if (val >= 128) {
            page = val & 31;
         }
         GateArray.upper_ROM = page;
         pbExpansionROM = pbCartridgePages[page];
      }
      if (!(GateArray.ROM_config & 0x08)) { // upper/expansion ROM is enabled?
         membank_read[3] = pbExpansionROM; // 'page in' upper/expansion ROM
      }
      if (CPC.mf2) { // MF2 enabled?
         *(pbMF2ROM + 0x03aac) = val;
      }
   }
// printer port ---------------------------------------------------------------
   if (!(port.b.h & 0x10)) { // printer port?
      CPC.printer_port = val ^ 0x80; // invert bit 7
      if (pfoPrinter) {
         if (!(CPC.printer_port & 0x80)) { // only grab data bytes; ignore the strobe signal
            fputc(CPC.printer_port, pfoPrinter); // capture printer output to file
            fflush(pfoPrinter);
         }
      }
   }
// PPI ------------------------------------------------------------------------
   if (!(port.b.h & 0x08)) { // PPI chip select?
      switch (port.b.h & 3) {
         case 0: // write to port A?
            LOG_DEBUG("PPI write to portA: " << static_cast<int>(val));
            PPI.portA = val;
            if (!(PPI.control & 0x10)) { // port A set to output?
               LOG_DEBUG("PPI write to portA (PSG): " << static_cast<int>(val));
               byte psg_data = val;
               psg_write
            }
            break;
         case 1: // write to port B?
            LOG_DEBUG("PPI write to portB (upper half): " << static_cast<int>(val));
            PPI.portB = val;
            break;
         case 2: // write to port C?
            LOG_DEBUG("PPI write to portC: " << static_cast<int>(val));
            PPI.portC = val;
            if (!(PPI.control & 1)) { // output lower half?
               LOG_DEBUG("PPI write to portC (keyboard_line): " << static_cast<int>(val));
               CPC.keyboard_line = val;
               io_fire_kbd_line_hooks(CPC.keyboard_line & 0x0f);
            }
            if (!(PPI.control & 8)) { // output upper half?
               LOG_DEBUG("PPI write to portC (upper half): " << static_cast<int>(val));
               CPC.tape_motor = val & 0x10; // update tape motor control
               io_fire_tape_motor_hooks(CPC.tape_motor && CPC.tape_play_button);
               PSG.control = val; // change PSG control
               byte psg_data = PPI.portA;
               psg_write
            }
            break;
         case 3: // modify PPI control
            if (val & 0x80) { // change PPI configuration
               LOG_DEBUG("PPI.control " << static_cast<int>(PPI.control) << " => " << static_cast<int>(val));
               PPI.control = val; // update control byte
               PPI.portA = 0; // clear data for all ports
               PPI.portB = 0;
               PPI.portC = 0;
            } else { // bit manipulation of port C data
               LOG_DEBUG("PPI.portC update: " << static_cast<int>(val));
               byte bit = (val >> 1) & 7; // isolate bit to set
               if (val & 1) { // set bit?
                  PPI.portC |= bit_values[bit]; // set requested bit
               } else {
                  PPI.portC &= ~(bit_values[bit]); // reset requested bit
               }
               if (!(PPI.control & 1)) { // output lower half?
                  LOG_DEBUG("PPI.portC update (keyboard_line): " << static_cast<int>(PPI.portC));
                  CPC.keyboard_line = PPI.portC;
                  io_fire_kbd_line_hooks(CPC.keyboard_line & 0x0f);
               }
               if (!(PPI.control & 8)) { // output upper half?
                  LOG_DEBUG("PPI.portC update (upper half): " << static_cast<int>(PPI.portC));
                  CPC.tape_motor = PPI.portC & 0x10;
                  io_fire_tape_motor_hooks(CPC.tape_motor && CPC.tape_play_button);
                  PSG.control = PPI.portC; // change PSG control
                  byte psg_data = PPI.portA;
                  psg_write
               }
            }
            if (CPC.mf2) { // MF2 enabled?
               *(pbMF2ROM + 0x037ff) = val;
            }
            break;
      }
   }
// FDC ------------------------------------------------------------------------
   if ((port.b.h == 0xfa) && (!(port.b.l & 0x80))) { // floppy motor control?
      LOG_DEBUG("FDC motor control access: " << static_cast<int>(port.b.l) << " - " << static_cast<int>(val));
      FDC.motor = val & 0x01;
      io_fire_fdc_motor_hooks(FDC.motor != 0);
      #ifdef DEBUG_FDC
      fputs(FDC.motor ? "\r\n--- motor on" : "\r\n--- motor off", pfoDebug);
      #endif
      FDC.flags |= STATUSDRVA_flag | STATUSDRVB_flag;
   }
   else if ((port.b.h == 0xfb) && (!(port.b.l & 0x80))) { // FDC data register?
      fdc_write_data(val);
   }
// Peripheral dispatch (M4 Board, MF2, Symbiface II, AmDrum, Phazer) -----------
   io_dispatch_out(port, val);
}



void print (byte *pbAddr, const char *pchStr, bool bolColour)
{
   int iLen, iIdx;
   dword dwColour;
   word wColour;
   byte bRow, bColour;
   byte *pbLine, *pbPixel;

   iLen = strlen(pchStr); // number of characters to process
   switch (CPC.scr_bpp)
   {
      case 32:
         dwColour = bolColour ? 0xffffffff : 0;
         for (int n = 0; n < iLen; n++) {
            iIdx = static_cast<int>(pchStr[n]); // get the ASCII value
            if ((iIdx < FNT_MIN_CHAR) || (iIdx > FNT_MAX_CHAR)) { // limit it to the range of chars in the font
               iIdx = FNT_BAD_CHAR;
            }
            iIdx -= FNT_MIN_CHAR; // zero base the index
            pbLine = pbAddr; // keep a reference to the current screen position
            for (int iRow = 0; iRow < FNT_CHAR_HEIGHT; iRow++) { // loop for all rows in the font character
               pbPixel = pbLine;
               bRow = bFont[iIdx]; // get the bitmap information for one row
               for (int iCol = 0; iCol < FNT_CHAR_WIDTH; iCol++) { // loop for all columns in the font character
                  if (bRow & 0x80) { // is the bit set?
                     *(reinterpret_cast<dword*>(pbPixel)) = dwColour; // draw the character pixel
                     *(reinterpret_cast<dword*>(pbPixel+CPC.scr_bps)) = dwColour; // draw the second line in case dwYScale == 2 (will be overwritten by shadow otherwise)
                     *(reinterpret_cast<dword*>(pbPixel)+1) = 0; // draw the "shadow" on the right
                     *(reinterpret_cast<dword*>(pbPixel+CPC.scr_bps)+1) = 0; // second line of shadow on the right
                     *(reinterpret_cast<dword*>(pbPixel+CPC.scr_line_offs)) = 0; // shadow on the line below
                     *(reinterpret_cast<dword*>(pbPixel+CPC.scr_line_offs)+1) = 0; // shadow below & on the right
                  }
                  pbPixel += 4; // update the screen position
                  bRow <<= 1; // advance to the next bit
               }
               pbLine += CPC.scr_line_offs; // advance to next screen line
               iIdx += FNT_CHARS; // advance to next row in font data
            }
            pbAddr += FNT_CHAR_WIDTH*4; // set screen address to next character position
         }
         break;

      case 24:
         dwColour = bolColour ? 0x00ffffff : 0;
         for (int n = 0; n < iLen; n++) {
            iIdx = static_cast<int>(pchStr[n]); // get the ASCII value
            if ((iIdx < FNT_MIN_CHAR) || (iIdx > FNT_MAX_CHAR)) { // limit it to the range of chars in the font
               iIdx = FNT_BAD_CHAR;
            }
            iIdx -= FNT_MIN_CHAR; // zero base the index
            pbLine = pbAddr; // keep a reference to the current screen position
            for (int iRow = 0; iRow < FNT_CHAR_HEIGHT; iRow++) { // loop for all rows in the font character
               pbPixel = pbLine;
               bRow = bFont[iIdx]; // get the bitmap information for one row
               for (int iCol = 0; iCol < FNT_CHAR_WIDTH; iCol++) { // loop for all columns in the font character
                  if (bRow & 0x80) { // is the bit set?
                     *(reinterpret_cast<dword *>(pbPixel)) = dwColour; // draw the character pixel
                     *(reinterpret_cast<dword *>(pbPixel+CPC.scr_bps)) = dwColour; // draw the second line in case dwYScale == 2 (will be overwritten by shadow otherwise)
                     *(reinterpret_cast<dword *>(pbPixel+1)) = 0; // draw the "shadow" on the right
                     *(reinterpret_cast<dword *>(pbPixel+CPC.scr_bps)+1) = 0; // second line of shadow on the right
                     *(reinterpret_cast<dword *>(pbPixel+CPC.scr_line_offs)) = 0; // shadow on the line below
                     *(reinterpret_cast<dword *>(pbPixel+CPC.scr_line_offs)+1) = 0; // shadow below & on the right
                  }
                  pbPixel += 3; // update the screen position
                  bRow <<= 1; // advance to the next bit
               }
               pbLine += CPC.scr_line_offs; // advance to next screen line
               iIdx += FNT_CHARS; // advance to next row in font data
            }
            pbAddr += FNT_CHAR_WIDTH*3; // set screen address to next character position
         }
         break;

      case 15:
      case 16:
         wColour = bolColour ? 0xffff : 0;
         for (int n = 0; n < iLen; n++) {
            iIdx = static_cast<int>(pchStr[n]); // get the ASCII value
            if ((iIdx < FNT_MIN_CHAR) || (iIdx > FNT_MAX_CHAR)) { // limit it to the range of chars in the font
               iIdx = FNT_BAD_CHAR;
            }
            iIdx -= FNT_MIN_CHAR; // zero base the index
            pbLine = pbAddr; // keep a reference to the current screen position
            for (int iRow = 0; iRow < FNT_CHAR_HEIGHT; iRow++) { // loop for all rows in the font character
               pbPixel = pbLine;
               bRow = bFont[iIdx]; // get the bitmap information for one row
               for (int iCol = 0; iCol < FNT_CHAR_WIDTH; iCol++) { // loop for all columns in the font character
                  if (bRow & 0x80) { // is the bit set?
                     *(reinterpret_cast<word *>(pbPixel)) = wColour; // draw the character pixel
                     *(reinterpret_cast<word *>(pbPixel+CPC.scr_bps)) = wColour; // draw the second line in case dwYScale == 2 (will be overwritten by shadow otherwise)
                     *(reinterpret_cast<word *>(pbPixel)+1) = 0; // draw the "shadow" on the right
                     *(reinterpret_cast<word *>(pbPixel+CPC.scr_bps)+1) = 0; // second line of shadow on the right
                     *(reinterpret_cast<word *>(pbPixel+CPC.scr_line_offs)) = 0; // shadow on the line below
                     *(reinterpret_cast<word *>(pbPixel+CPC.scr_line_offs)+1) = 0; // shadow below & on the right
                  }
                  pbPixel += 2; // update the screen position
                  bRow <<= 1; // advance to the next bit
               }
               pbLine += CPC.scr_line_offs; // advance to next screen line
               iIdx += FNT_CHARS; // advance to next row in font data
            }
            pbAddr += FNT_CHAR_WIDTH*2; // set screen address to next character position
         }
         break;

      case 8:
         bColour = bolColour ? MapRGBSurface(back_surface, 255,255,255) : MapRGBSurface(back_surface, 0,0,0);
         for (int n = 0; n < iLen; n++) {
            iIdx = static_cast<int>(pchStr[n]); // get the ASCII value
            if ((iIdx < FNT_MIN_CHAR) || (iIdx > FNT_MAX_CHAR)) { // limit it to the range of chars in the font
               iIdx = FNT_BAD_CHAR;
            }
            iIdx -= FNT_MIN_CHAR; // zero base the index
            pbLine = pbAddr; // keep a reference to the current screen position
            for (int iRow = 0; iRow < FNT_CHAR_HEIGHT; iRow++) { // loop for all rows in the font character
               pbPixel = pbLine;
               bRow = bFont[iIdx]; // get the bitmap information for one row
               for (int iCol = 0; iCol < FNT_CHAR_WIDTH; iCol++) { // loop for all columns in the font character
                  if (bRow & 0x80) { // is the bit set?
                     *pbPixel = bColour; // draw the character pixel
                     *(pbPixel+CPC.scr_bps) = bColour; // draw the second line in case dwYScale == 2 (will be overwritten by shadow otherwise)
                     *(pbPixel+1) = 0; // draw the "shadow" on the right
                     *(pbPixel+CPC.scr_bps) = 0; // second line of shadow on the right
                     *(pbPixel+CPC.scr_line_offs) = 0; // shadow on the line below
                     *(pbPixel+CPC.scr_line_offs+1) = 0; // shadow below & on the right
                  }
                  pbPixel++; // update the screen position
                  bRow <<= 1; // advance to the next bit
               }
               pbLine += CPC.scr_line_offs; // advance to next screen line
               iIdx += FNT_CHARS; // advance to next row in font data
            }
            pbAddr += FNT_CHAR_WIDTH; // set screen address to next character position
         }
         break;
   }
}

int emulator_patch_ROM ()
{
   byte *pbPtr;

   if(CPC.model <= 2) { // Normal CPC range
      std::string romFilename = CPC.rom_path + "/" + chROMFile[CPC.model];
      if ((pfileObject = fopen(romFilename.c_str(), "rb")) != nullptr) { // load CPC OS + Basic
         if(fread(pbROM, 2*16384, 1, pfileObject) != 1) {
            fclose(pfileObject);
            LOG_ERROR("Couldn't read ROM file '" << romFilename << "'");
            return ERR_NOT_A_CPC_ROM;
         }
         pbROMlo = pbROM;
         fclose(pfileObject);
      } else {
         LOG_ERROR("Couldn't open ROM file '" << romFilename << "'");
         return ERR_CPC_ROM_MISSING;
      }
   } else { // Plus range
      if (pbCartridgePages[0] != nullptr) {
         pbROMlo = pbCartridgePages[0];
      }
   }

   // Patch ROM for non-english keyboards
   if (CPC.keyboard) {
      pbPtr = pbROMlo;
      switch(CPC.model) {
         case 0: // 464
            pbPtr += 0x1d69; // location of the keyboard translation table
            break;
         case 1: // 664
         case 2: // 6128
            pbPtr += 0x1eef; // location of the keyboard translation table
            break;
         case 3: // 6128+
            if(CPC.cartridge.file == CPC.rom_path + "/" + chROMFile[3]) { // Only patch system cartridge - we don't want to break another one by messing with it
               pbPtr += 0x1eef; // location of the keyboard translation table
            }
            break;
      }
      if (pbPtr != pbROMlo) {
         memcpy(pbPtr, cpc_keytrans[CPC.keyboard-1], 240); // patch the CPC OS ROM with the chosen keyboard layout
         pbPtr = pbROMlo + 0x3800;
         memcpy(pbPtr, cpc_charset[CPC.keyboard-1], 2048); // add the corresponding character set
      }
   }

   return 0;
}


void emulator_reset ()
{
   if (CPC.model > 2) {
      if (pbCartridgePages[0] != nullptr) {
         pbROMlo = pbCartridgePages[0];
      }
   }

// ASIC
   asic_reset();
   video_set_palette();

// Z80
   z80_reset();

// CPC
   CPC.cycle_count = CYCLE_COUNT_INIT;
   memset(keyboard_matrix, 0xff, sizeof(keyboard_matrix)); // clear CPC keyboard matrix
   CPC.tape_motor = 0;
   CPC.tape_play_button = 0;
   CPC.printer_port = 0xff;

// VDU
   memset(&VDU, 0, sizeof(VDU)); // clear VDU data structure
   VDU.flag_drawing = 1;

// CRTC
   CRTC.crtc_type = crtc_type_for_model(CPC.model);
   crtc_reset();

// Gate Array
   memset(&GateArray, 0, sizeof(GateArray)); // clear GA data structure
   GateArray.scr_mode =
   GateArray.requested_scr_mode = 1; // set to mode 1
   GateArray.registerPageOn = false;
   GateArray.lower_ROM_bank = 0;
   ga_init_banking(membank_config, GateArray.RAM_bank);

// PPI
   memset(&PPI, 0, sizeof(PPI)); // clear PPI data structure

// PSG
   PSG.control = 0;
   ResetAYChipEmulation();

// AmDrum
   amdrum_reset();

// SmartWatch
   smartwatch_reset();

// AMX Mouse
   amx_mouse_reset();

// Symbiface II
   symbiface_reset();

// M4 Board
   m4board_reset();

// FDC
   memset(&FDC, 0, sizeof(FDC)); // clear FDC data structure
   FDC.phase = CMD_PHASE;
   FDC.flags = STATUSDRVA_flag | STATUSDRVB_flag;

// memory
   memset(pbRAM, 0, CPC.ram_size*1024); // clear all memory used for CPC RAM
   if (pbMF2ROM) {
     memset(pbMF2ROM+8192, 0, 8192); // clear the MF2's RAM area
   }
   for (int n = 0; n < 4; n++) { // initialize active read/write bank configuration
      membank_read[n] = membank_config[0][n];
      membank_write[n] = membank_config[0][n];
   }
   membank_read[0] = pbROMlo; // 'page in' lower ROM
   membank_read[3] = pbROMhi; // 'page in' upper ROM

// Multiface 2
   dwMF2Flags = 0;
   dwMF2ExitAddr = 0xffffffff; // clear MF2 return address
   if ((pbMF2ROM) && (pbMF2ROMbackup)) {
      memcpy(pbMF2ROM, pbMF2ROMbackup, 8192); // copy the MF2 ROM to its proper place
   }
}

int input_init ()
{
   CPC.InputMapper->init();
   CPC.InputMapper->set_joystick_emulation();
   return 0;
}

int emulator_init ()
{
   if (input_init()) {
      fprintf(stderr, "input_init() failed. Aborting.\n");
      _exit(-1);
   }

   // Cartridge must be loaded before init as ROM needs to be present.
   cartridge_load();
   int iErr, iRomNum;
   byte *pchRomData;

   pbGPBuffer = new byte [128*1024]; // attempt to allocate the general purpose buffer
   pbRAMbuffer = new byte [CPC.ram_size*1024 + 1]; // allocate memory for desired amount of RAM
   // Ensure 1 byte is available before pbRAM as prerender_normal*_plus can read it
   pbRAM = pbRAMbuffer + 1;
   pbROM = new byte [32*1024]; // allocate memory for 32K of ROM
   pbRegisterPage = new byte [16*1024];
   pbROMlo = pbROM;
   pbROMhi =
   pbExpansionROM = pbROM + 16384;
   memset(memmap_ROM, 0, sizeof(memmap_ROM[0]) * 256); // clear the expansion ROM map
   ga_init_banking(membank_config, GateArray.RAM_bank); // init the CPC memory banking map
   if ((iErr = emulator_patch_ROM())) {
      LOG_ERROR("Failed patching the ROM");
      return iErr;
   }

   for (iRomNum = 0; iRomNum < MAX_ROM_SLOTS; iRomNum++) { // loop for ROMs 0-31
      if (!CPC.rom_file[iRomNum].empty()) { // is a ROM image specified for this slot?
         std::string rom_file = CPC.rom_file[iRomNum];
         if (rom_file == "DEFAULT") {
           // On 464, there's no AMSDOS by default.
           // We still allow users to override this if they want.
           // More details: https://github.com/ikari/konCePCja/issues/227
           if (CPC.model == 0) continue;
           rom_file = "amsdos.rom";
         }
         pchRomData = new byte [16384]; // allocate 16K
         memset(pchRomData, 0, 16384); // clear memory
         std::string romFilename = CPC.rom_path + "/" + rom_file;
         if ((pfileObject = fopen(romFilename.c_str(), "rb")) != nullptr) { // attempt to open the ROM image
            if(fread(pchRomData, 128, 1, pfileObject) != 1) { // read 128 bytes of ROM data
              fclose(pfileObject);
              return ERR_NOT_A_CPC_ROM;
            }
            word checksum = 0;
            for (int n = 0; n < 0x43; n++) {
               checksum += pchRomData[n];
            }

            // Check for Graduate Software ROM structure termination with $ in the header
            word gradcheck = 0;
            for (int n = 0; n < 0x43; n++) {
               if(pchRomData[n]==0x24) {
                 gradcheck = 1;
               }
            }
            if((pchRomData[0x38]==0xc9) && (gradcheck==1)) { // extra validation step ensure 0x38 has 0xc9 if a $ terminated string was in the header
              gradcheck = 1;
            } else {
              gradcheck = 0; // reset flag is there was a $ was found, but offset 0x38 wasn't 0xc9
            }
            // end of Graduate accessory ROM checks


            if (checksum == ((pchRomData[0x43] << 8) + pchRomData[0x44])) { // if the checksum matches, we got us an AMSDOS header
               if(fread(pchRomData, 128, 1, pfileObject) != 1) { // skip it
                 fclose(pfileObject);
                 return ERR_NOT_A_CPC_ROM;
               }
            }
            if (!(pchRomData[0] & 0xfc)) { // is it a valid CPC ROM image (0 = forground, 1 = background, 2 = extension)?
               if(fread(pchRomData+128, 16384-128, 1, pfileObject) != 1) { // read the rest of the ROM file
                 fclose(pfileObject);
                 return ERR_NOT_A_CPC_ROM;
               }
               memmap_ROM[iRomNum] = pchRomData; // update the ROM map
            } else if ((pchRomData[0] == 0x47) && (gradcheck==1)) { // Is it a Graduate CPM Accessory Rom? (ID="G")
            // Graduate Software Accessory Roms use a non standard format. Only the first byte is validated, and as long as
            // it's a "G" and terminated with a "$" it'll try to use it.
            // See https://www.cpcwiki.eu/index.php/Graduate_Software#Structure_of_a_utility_ROM for more details.
              if(fread(pchRomData+128, 16384-128, 1, pfileObject) != 1) { // read the rest of the ROM file
                fclose(pfileObject);
                return ERR_NOT_A_CPC_ROM;
              }
              memmap_ROM[iRomNum] = pchRomData; // update the ROM map
            } else { // not a valid ROM file
               fprintf(stderr, "ERROR: %s is not a CPC ROM file - clearing ROM slot %d.\n", rom_file.c_str(), iRomNum);
               delete [] pchRomData; // free memory on error
               CPC.rom_file[iRomNum] = "";
            }
            fclose(pfileObject);
         } else { // file not found
            fprintf(stderr, "ERROR: The %s file is missing - clearing ROM slot %d.\n", rom_file.c_str(), iRomNum);
            delete [] pchRomData; // free memory on error
            CPC.rom_file[iRomNum] = "";
         }
      }
   }
   if (CPC.mf2) { // Multiface 2 enabled?
      if (!pbMF2ROM) {
         pbMF2ROM = new byte [16384]; // allocate the space needed for the Multiface 2: 8K ROM + 8K RAM
         pbMF2ROMbackup = new byte [8192]; // allocate the space needed for the backup of the MF2 ROM
         memset(pbMF2ROM, 0, 16384); // clear memory
         std::string romFilename = CPC.rom_path + "/" + CPC.rom_mf2;
         bool MF2error = false;
         if ((pfileObject = fopen(romFilename.c_str(), "rb")) != nullptr) { // attempt to open the ROM image
            if((fread(pbMF2ROMbackup, 8192, 1, pfileObject) != 1) || (memcmp(pbMF2ROMbackup+0x0d32, "MULTIFACE 2", 11) != 0)) { // does it have the required signature?
               fprintf(stderr, "ERROR: The file selected as the MF2 ROM is either corrupt or invalid.\n");
               MF2error = true;
            }
            fclose(pfileObject);
         } else { // error opening file
            fprintf(stderr, "ERROR: The file selected as the MF2 ROM (%s) couldn't be opened.\n", romFilename.c_str());
            MF2error = true;
         }
         if(MF2error) {
           delete [] pbMF2ROMbackup;
           delete [] pbMF2ROM;
           pbMF2ROM = nullptr;
           pbMF2ROMbackup = nullptr;
           CPC.rom_mf2 = "";
           CPC.mf2 = 0; // disable MF2 support
         }
      }
   }

   // Auto-load M4 Board ROM if enabled and slot is free
   m4board_load_rom(memmap_ROM, CPC.rom_path, CPC.resources_path);

   // Register peripheral I/O handlers and core hooks
   io_dispatch_init();

   emulator_reset();
   CPC.paused = false;

      return 0;
}



void emulator_shutdown ()
{
   int iRomNum;

   delete [] pbMF2ROMbackup;
   delete [] pbMF2ROM;
   pbMF2ROM = nullptr;
   pbMF2ROMbackup = nullptr;
   m4board_unload_rom(memmap_ROM); // free auto-loaded M4 ROM before general cleanup
   for (iRomNum = 2; iRomNum < MAX_ROM_SLOTS; iRomNum++) // loop for ROMs 2-31
   {
      if (memmap_ROM[iRomNum] != nullptr) // was a ROM assigned to this slot?
         delete [] memmap_ROM[iRomNum]; // if so, release the associated memory
   }

   delete [] pbROM;
   delete [] pbRAMbuffer;
   delete [] pbGPBuffer;
}



void bin_load (const std::string& filename, const size_t offset)
{
  LOG_INFO("Load " << filename << " in memory at offset 0x" << std::hex << offset);
  FILE *file;
  if ((file = fopen(filename.c_str(), "rb")) == nullptr) {
    LOG_ERROR("File not found: " << filename);
    return;
  }

  auto closure = [&]() { fclose(file); };
  memutils::scope_exit<decltype(closure)> cs(closure); // TODO: when C++20, can become a one liner expression.

  size_t ram_size = 0XFFFF; // TODO: Find a way to have the real RAM size
  size_t max_size = ram_size - offset;
  size_t read = fread(&pbRAM[offset], 1, max_size, file);
  if (!feof(file)) {
    LOG_ERROR("Bin file too big to fit in memory");
    return;
  }
  if (ferror(file)) {
    LOG_ERROR("Error reading the bin file: " << ferror(file));
    return;
  }
  if (read == 0) {
    LOG_ERROR("Empty bin file");
    return;
  }
  // Jump at the beginning of the program
  z80.PC.w.l = offset;
  // Setup the stack the way it would be if we had launch it with run"
  z80_write_mem(--z80.SP.w.l, 0x0);
  z80_write_mem(--z80.SP.w.l, 0x98);
  z80_write_mem(--z80.SP.w.l, 0x7f);
  z80_write_mem(--z80.SP.w.l, 0x89);
  z80_write_mem(--z80.SP.w.l, 0xb9);
  z80_write_mem(--z80.SP.w.l, 0xa2);
}




int printer_start ()
{
   if (!pfoPrinter) {
      if(!(pfoPrinter = fopen(CPC.printer_file.c_str(), "wb"))) {
         return 0; // failed to open/create file
      }
   }
   return 1; // ready to capture printer output
}



void printer_stop ()
{
   if (pfoPrinter) {
      fclose(pfoPrinter);
   }
   pfoPrinter = nullptr;
}



void audio_update([[maybe_unused]] void *userdata, SDL_AudioStream *stream, int additional_amount, [[maybe_unused]] int total_amount)
{
  if (CPC.snd_ready) {
    int len = additional_amount;
    if (len > static_cast<int>(CPC.snd_buffersize)) len = static_cast<int>(CPC.snd_buffersize);
    if (len > 0) {
      if (CPC.paused) {
        // Send silence when paused to avoid buzzing from looped last buffer
        static std::vector<byte> silence;
        if (static_cast<int>(silence.size()) < len) silence.resize(len, 0);
        SDL_PutAudioStreamData(stream, silence.data(), len);
      } else {
        SDL_PutAudioStreamData(stream, pbSndBuffer.get(), len);
        if (g_wav_recorder.is_recording()) {
          g_wav_recorder.write_samples(pbSndBuffer.get(), static_cast<uint32_t>(len));
        }
        if (g_avi_recorder.is_recording()) {
          g_avi_recorder.capture_audio_samples(
             reinterpret_cast<const int16_t*>(pbSndBuffer.get()),
             static_cast<size_t>(len) / sizeof(int16_t));
        }
      }
    }
  } else {
    LOG_VERBOSE("Audio: audio_update: skipping audio: sound buffer not ready");
  }
}



int audio_align_samples (int given)
{
   int actual = 1;
   while (actual < given) {
      actual <<= 1;
   }
   return actual; // return the closest match as 2^n
}




int audio_init ()
{
   SDL_AudioSpec desired;

   if (!CPC.snd_enabled) {
      return 0;
   }

   CPC.snd_ready = false;

   desired.freq = freq_table[CPC.snd_playback_rate];
   desired.format = CPC.snd_bits ? SDL_AUDIO_S16LE : SDL_AUDIO_S8;
   desired.channels = CPC.snd_stereo + 1;

   int sample_frames = audio_align_samples(desired.freq * FRAME_PERIOD_MS / 1000);
   char frames_hint[32];
   snprintf(frames_hint, sizeof(frames_hint), "%d", sample_frames);
   SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames_hint);

   audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired, audio_update, nullptr);
   if (audio_stream == nullptr) {
      LOG_ERROR("Could not open audio: " << SDL_GetError());
      return 1;
   }
   SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(audio_stream));

   LOG_VERBOSE("Audio: Desired: Freq: " << desired.freq << ", Format: " << desired.format << ", Channels: " << static_cast<int>(desired.channels) << ", Frames: " << sample_frames);

   CPC.snd_buffersize = sample_frames * SDL_AUDIO_FRAMESIZE(desired);
   pbSndBuffer = std::make_unique<byte[]>(CPC.snd_buffersize);
   pbSndBufferEnd = pbSndBuffer.get() + CPC.snd_buffersize;
   memset(pbSndBuffer.get(), 0, CPC.snd_buffersize);
   CPC.snd_bufferptr = pbSndBuffer.get();
   CPC.snd_ready = true;
   LOG_VERBOSE("Audio: Sound buffer ready");

   InitAY();
   drive_sounds_init(desired.freq);

   for (int n = 0; n < 16; n++) {
      SetAYRegister(n, PSG.RegisterAY.Index[n]);
   }

      return 0;
}



void audio_shutdown ()
{
   if (audio_stream) { SDL_DestroyAudioStream(audio_stream); audio_stream = nullptr; }
}



void audio_pause ()
{
   if (CPC.snd_enabled && audio_stream) {
      SDL_PauseAudioDevice(SDL_GetAudioStreamDevice(audio_stream));
   }
}



void audio_resume ()
{
   if (CPC.snd_enabled && audio_stream) {
      SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(audio_stream));
   }
}

void cpc_pause()
{
   audio_pause();
   CPC.paused = true;
}

void cpc_resume()
{
   CPC.paused = false;
   audio_resume();
}

int video_set_palette ()
{
   if (!CPC.scr_tube) {
      for (int n = 0; n < 32; n++) {
         dword red = static_cast<dword>(colours_rgb[n][0] * (CPC.scr_intensity / 10.0) * 255);
         if (red > 255) { // limit to the maximum
            red = 255;
         }
         dword green = static_cast<dword>(colours_rgb[n][1] * (CPC.scr_intensity / 10.0) * 255);
         if (green > 255) {
            green = 255;
         }
         dword blue = static_cast<dword>(colours_rgb[n][2] * (CPC.scr_intensity / 10.0) * 255);
         if (blue > 255) {
            blue = 255;
         }
         colours[n].r = red;
         colours[n].g = green;
         colours[n].b = blue;
      }
   } else {
      for (int n = 0; n < 32; n++) {
         double *colours_green = video_get_green_palette(CPC.scr_green_mode);

         dword green = static_cast<dword>(colours_green[n] * (CPC.scr_intensity / 10.0) * 255);
         if (green > 255) {
             green = 255;
         }

         dword blue = static_cast<dword>(0.01 * CPC.scr_green_blue_percent * colours_green[n] * (CPC.scr_intensity / 10.0) * 255);

         // unlikely, but we care though
         if (blue > 255) {
             blue = 255;
         }

         colours[n].r = 0;
         colours[n].g = green;
         colours[n].b = blue;
      }
   }

   vid_plugin->set_palette(colours);

   for (int n = 0; n < 17; n++) { // loop for all colours + border
      int i=GateArray.ink_values[n];
      GateArray.palette[n] = MapRGBSurface(back_surface, colours[i].r,colours[i].g,colours[i].b);
   }

      return 0;
}



void video_set_style ()
{
   if (vid_plugin->half_pixels)
   {
      dwXScale = 1;
      dwYScale = 1;
   }
   else
   {
      dwXScale = 2;
      dwYScale = 2;
   }
   CPC.dwYScale = dwYScale;
   switch (dwXScale) {
      case 1:
         if (CPC.model > 2) {
            CPC.scr_prerendernorm = prerender_normal_half_plus;
         } else {
            CPC.scr_prerendernorm = prerender_normal_half;
         }
         CPC.scr_prerenderbord = prerender_border_half;
         CPC.scr_prerendersync = prerender_sync_half;
         break;
      case 2:
         if (CPC.model > 2) {
            CPC.scr_prerendernorm = prerender_normal_plus;
         } else {
            CPC.scr_prerendernorm = prerender_normal;
         }
         CPC.scr_prerenderbord = prerender_border;
         CPC.scr_prerendersync = prerender_sync;
         break;
   }

   switch(CPC.scr_bpp)
   {
      case 32:
               switch(dwYScale) {
                 case 1:
                   CPC.scr_render = render32bpp;
                   break;
                 case 2:
                   CPC.scr_render = render32bpp_doubleY;
                   break;
               }
               break;

      case 24:
               switch(dwYScale) {
                 case 1:
                   CPC.scr_render = render24bpp;
                   break;
                 case 2:
                   CPC.scr_render = render24bpp_doubleY;
                   break;
               }
               break;

      case 16:
      case 15:
               switch(dwYScale) {
                 case 1:
                   CPC.scr_render = render16bpp;
                   break;
                 case 2:
                   CPC.scr_render = render16bpp_doubleY;
                   break;
               }
               break;

      case 8:
               switch(dwYScale) {
                 case 1:
                   CPC.scr_render = render8bpp;
                   break;
                 case 2:
                   CPC.scr_render = render8bpp_doubleY;
                   break;
               }
               break;
   }
}


void mouse_init ()
{
  // hide the mouse cursor unless we emulate phazer
  set_cursor_visibility(CPC.phazer_emulation);
}


int video_init ()
{
   vid_plugin=&video_plugin_list[CPC.scr_style];
   LOG_DEBUG("video_init: vid_plugin = " << vid_plugin->name)

   back_surface=vid_plugin->init(vid_plugin, CPC.scr_scale, CPC.scr_window==0);

   if (!back_surface) {
      // OpenGL may be unavailable (e.g. SDL_VIDEODRIVER=dummy in CI).
      // Fall back to headless rendering so the emulator remains functional.
      LOG_ERROR("Could not set requested video mode: " << SDL_GetError()
                << " — falling back to headless");
      static video_plugin hp = video_headless_plugin();
      vid_plugin = &hp;
      g_headless = true;
      back_surface = vid_plugin->init(vid_plugin, CPC.scr_scale, false);
      if (!back_surface) {
         LOG_ERROR("Headless fallback also failed. Aborting.");
         return ERR_VIDEO_SET_MODE;
      }
   }

   {
      const SDL_PixelFormatDetails* fmt = SDL_GetPixelFormatDetails(back_surface->format);
      CPC.scr_bpp = fmt ? fmt->bits_per_pixel : 0; // bit depth of the surface
   }
   video_set_style(); // select rendering style

   int iErrCode = video_set_palette(); // init CPC colours
   if (iErrCode) {
      return iErrCode;
   }
   asic_set_palette();

   CPC.scr_bps = back_surface->pitch; // rendered screen line length in bytes
   CPC.scr_line_offs = CPC.scr_bps * dwYScale;
   CPC.scr_pos =
   CPC.scr_base = static_cast<byte *>(back_surface->pixels); // memory address of back buffer
   CPC.scr_gui_is_currently_on = false;

   crtc_init();

      return 0;
}



void video_shutdown ()
{
   vid_plugin->close();
}



void video_display ()
{
   vid_plugin->flip(vid_plugin);
}



int joysticks_init ()
{
   if(CPC.joysticks == 0) {
      return 0;
   }

   // Disable HIDAPI drivers known to crash inside SDL3 during device
   // negotiation (null-deref in SetEnhancedReportHint / WriteSubcommand).
   // The standard system joystick driver still works for these devices.
   // Users can override with env vars (e.g. SDL_JOYSTICK_HIDAPI_SWITCH=1).
   SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, "0");
   SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, "0");
   SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_NINTENDO_CLASSIC, "0");

   if (!SDL_InitSubSystem(SDL_INIT_JOYSTICK)) {
      fprintf(stderr, "Failed to initialize joystick subsystem. Error: %s\n", SDL_GetError());
      return ERR_JOYSTICKS_INIT;
   }

   int nbJoysticks = 0;
   SDL_JoystickID* ids = SDL_GetJoysticks(&nbJoysticks);
   if (!ids || nbJoysticks <= 0) {
      fprintf(stderr, "No joystick found.\n");
      if (ids) SDL_free(ids);
      return ERR_JOYSTICKS_INIT;
   }

   SDL_SetJoystickEventsEnabled(true);

   if(nbJoysticks > MAX_NB_JOYSTICKS) {
      nbJoysticks = MAX_NB_JOYSTICKS;
   }

   for(int i = 0; i < MAX_NB_JOYSTICKS; i++) {
      if(i < nbJoysticks) {
        const char* name = SDL_GetJoystickNameForID(ids[i]);
        fprintf(stderr, "Opening joystick %d: %s\n", i, name ? name : "(unknown)");
        joysticks[i] = SDL_OpenJoystick(ids[i]);
        if(joysticks[i] == nullptr) {
          fprintf(stderr, "Failed to open joystick %d. Error: %s\n", i, SDL_GetError());
        }
      } else {
        joysticks[i] = nullptr;
      }
   }

   SDL_free(ids);
   return 0;
}



void joysticks_shutdown ()
{
/* This cores for an unknown reason - anyway, SDL_QuitSubSystem will do the job
   for(int i = 0; i < MAX_NB_JOYSTICKS; i++) {
      if(joysticks[i] != nullptr) {
         SDL_CloseJoystick(joysticks[i]);
      }
   }
*/

   SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
}



void update_timings()
{
   dwTicksOffset = static_cast<int>(FRAME_PERIOD_MS / (CPC.speed/CPC_BASE_FREQUENCY_MHZ));
   dwTicksTarget = SDL_GetTicks();
   dwTicksTargetFPS = dwTicksTarget;
   dwTicksTarget += dwTicksOffset;
   // These are only used for frames timing if sound is disabled. Otherwise timing is controlled by the PSG.
   LOG_VERBOSE("Timing: First frame at " << dwTicksTargetFPS << " - next frame in " << dwTicksOffset << " ( " << FRAME_PERIOD_MS << "/(" << CPC.speed << "/" << CPC_BASE_FREQUENCY_MHZ << ") ) at " << dwTicksTarget);
}

// Recalculate emulation speed (to verify, seems to work reasonably well)
void update_cpc_speed()
{
   update_timings();
   InitAY();
}

std::string getConfigurationFilename(bool forWrite)
{
  int mode = R_OK | ( F_OK * forWrite );

  const char* PATH_OK = "";

  std::string binPathStr = binPath.string();
  std::vector<std::pair<const char*, std::string>> configPaths = {
    { PATH_OK, args.cfgFilePath}, // First look in any user supplied configuration file path
    { chAppPath, "/koncepcja.cfg" }, // koncepcja.cfg in the current working directory
    { binPathStr.c_str(), "/koncepcja.cfg" }, // koncepcja.cfg next to the binary (Finder launch)
    { getenv("XDG_CONFIG_HOME"), "/koncepcja/koncepcja.cfg" },
    { getenv("HOME"), "/.config/koncepcja/koncepcja.cfg" },
    { getenv("XDG_CONFIG_HOME"), "/koncepcja.cfg" }, // legacy flat paths
    { getenv("HOME"), "/.config/koncepcja.cfg" },
    { getenv("HOME"), "/.koncepcja.cfg" },
    { DESTDIR, "/etc/koncepcja.cfg" },
    { binPath.string().c_str(), "/../Resources/koncepcja.cfg" }, // To find the configuration from the bundle on MacOS
  };

  for(const auto& p: configPaths){
    // Skip paths using getenv if it returned NULL (i.e environment variable not defined)
    if (!p.first) continue;
    std::string s = std::string(p.first) + p.second;
    if (access(s.c_str(), mode) == 0) {
      std::cout << "Using configuration file" << (forWrite ? " to save" : "") << ": " << s << std::endl;
      // When config is found relative to the binary (not CWD), change
      // to the binary dir so relative paths in the config resolve correctly.
      // This handles Finder double-click (CWD=~) and macOS bundles.
      if (s == binPathStr + "/koncepcja.cfg") {
              std::filesystem::current_path(binPath);
              snprintf(chAppPath, sizeof(chAppPath), "%s", binPath.string().c_str());
      } else if (p.second == "/../Resources/koncepcja.cfg") {
              std::filesystem::current_path(binPath);
      }
      return s;
    }
  }

  std::cout << "No valid configuration file found, using empty config." << std::endl;
  return "";
}


void loadConfiguration (t_CPC &CPC, const std::string& configFilename)
{
   config::Config conf;
   conf.parseFile(configFilename);
   conf.setOverrides(args.cfgOverrides);

   std::string appPath = chAppPath;

   CPC.model = conf.getIntValue("system", "model", 2); // CPC 6128
   if (CPC.model > 3) {
      CPC.model = 2;
   }
   CPC.jumpers = conf.getIntValue("system", "jumpers", 0x1e) & 0x1e; // OEM is Amstrad, video refresh is 50Hz
   CPC.ram_size = conf.getIntValue("system", "ram_size", 128);
   if (!is_valid_ram_size(CPC.ram_size)) {
      CPC.ram_size = 128; // default to 128KB
   }
   if ((CPC.model >= 2) && (CPC.ram_size < 128)) {
      CPC.ram_size = 128; // minimum RAM size for CPC 6128 is 128KB
   }
   // Silicon Disc: battery-backed 256K RAM (banks 4-7)
   g_silicon_disc.enabled = conf.getIntValue("system", "silicon_disc", 0) != 0;
   if (g_silicon_disc.enabled) {
      silicon_disc_init(g_silicon_disc);
   }

   CPC.speed = conf.getIntValue("system", "speed", DEF_SPEED_SETTING); // original CPC speed
   if ((CPC.speed < MIN_SPEED_SETTING) || (CPC.speed > MAX_SPEED_SETTING)) {
      CPC.speed = DEF_SPEED_SETTING;
   }
   CPC.limit_speed = conf.getIntValue("system", "limit_speed", 1) & 1;
   CPC.auto_pause = conf.getIntValue("system", "auto_pause", 1) & 1;
   CPC.boot_time = conf.getIntValue("system", "boot_time", 5);
   CPC.printer = conf.getIntValue("system", "printer", 0) & 1;
   CPC.mf2 = conf.getIntValue("system", "mf2", 0) & 1;
   CPC.keyboard = conf.getIntValue("system", "keyboard", 0);
   if (CPC.keyboard > MAX_ROM_MODS) {
      CPC.keyboard = 0;
   }
   CPC.joystick_emulation = conf.getIntValue("system", "joystick_emulation", 0) & 1;
   CPC.joysticks = conf.getIntValue("system", "joysticks", 1) & 1;
   CPC.joystick_menu_button = conf.getIntValue("system", "joystick_menu_button", 9) - 1;
   CPC.joystick_vkeyboard_button = conf.getIntValue("system", "joystick_vkeyboard_button", 10) - 1;
   CPC.resources_path = conf.getStringValue("system", "resources_path", appPath + "/resources");

   CPC.devtools_scale = conf.getIntValue("devtools", "scale", 1);
   CPC.devtools_max_stack_size = conf.getIntValue("devtools", "max_stack_size", 50);

   CPC.workspace_layout = conf.getIntValue("ui", "workspace_layout", 0);
   if (CPC.workspace_layout < 0 || CPC.workspace_layout > 1) CPC.workspace_layout = 0;
   CPC.cpc_screen_scale = conf.getIntValue("ui", "cpc_screen_scale", 0);
   if (CPC.cpc_screen_scale < 0 || CPC.cpc_screen_scale > 3) CPC.cpc_screen_scale = 0;

   CPC.scr_scale = conf.getIntValue("video", "scr_scale", 2);
   CPC.scr_preserve_aspect_ratio = conf.getIntValue("video", "scr_preserve_aspect_ratio", 1);
   CPC.scr_style = conf.getIntValue("video", "scr_style", 1);
   if (CPC.scr_style >= video_plugin_list.size()) {
      CPC.scr_style = DEFAULT_VIDEO_PLUGIN;
      LOG_ERROR("Unsupported video plugin specified - defaulting to plugin " << video_plugin_list[DEFAULT_VIDEO_PLUGIN].name);
   }
   CPC.scr_oglfilter = conf.getIntValue("video", "scr_oglfilter", 1) & 1;
   CPC.scr_oglscanlines = conf.getIntValue("video", "scr_oglscanlines", 30);
   if (CPC.scr_oglscanlines > 100) {
      CPC.scr_oglscanlines = 30;
   }
   CPC.scr_led = conf.getIntValue("video", "scr_led", 1) & 1;
   CPC.scr_fps = conf.getIntValue("video", "scr_fps", 0) & 1;
   CPC.scr_tube = conf.getIntValue("video", "scr_tube", 0) & 1;
   CPC.scr_intensity = conf.getIntValue("video", "scr_intensity", 10);
   CPC.scr_remanency = conf.getIntValue("video", "scr_remanency", 0) & 1;
   if ((CPC.scr_intensity < 5) || (CPC.scr_intensity > 15)) {
      CPC.scr_intensity = 10;
   }
   CPC.scr_window = conf.getIntValue("video", "scr_window", 1) & 1;

   CPC.scr_green_mode = conf.getIntValue("video", "scr_green_mode", 0) & 1;
   CPC.scr_green_blue_percent = conf.getIntValue("video", "scr_green_blue_percent", 0);

   CPC.snd_enabled = conf.getIntValue("sound", "enabled", 1) & 1;
   CPC.snd_playback_rate = conf.getIntValue("sound", "playback_rate", 2);
   if (CPC.snd_playback_rate > (MAX_FREQ_ENTRIES-1)) {
      CPC.snd_playback_rate = 2;
   }
   CPC.snd_bits = conf.getIntValue("sound", "bits", 1) & 1;
   CPC.snd_stereo = conf.getIntValue("sound", "stereo", 1) & 1;
   CPC.snd_volume = conf.getIntValue("sound", "volume", 80);
   if (CPC.snd_volume > 100) {
      CPC.snd_volume = 80;
   }
   CPC.snd_pp_device = conf.getIntValue("sound", "pp_device", 0) & 1;
   g_amdrum.enabled = conf.getIntValue("sound", "amdrum", 0) & 1;
   g_drive_sounds.disk_enabled = conf.getIntValue("sound", "disk_sounds", 0) & 1;
   g_drive_sounds.tape_enabled = conf.getIntValue("sound", "tape_sounds", 0) & 1;
   g_smartwatch.enabled = conf.getIntValue("system", "smartwatch", 0) & 1;
   g_amx_mouse.enabled = conf.getIntValue("input", "amx_mouse", 0) & 1;

   g_symbiface.enabled = conf.getIntValue("peripheral", "symbiface", 0) & 1;
   g_m4board.enabled = conf.getIntValue("peripheral", "m4board", 0) & 1;
   g_m4board.sd_root_path = conf.getStringValue("peripheral", "m4_sd_path", "");
   g_m4board.rom_slot = conf.getIntValue("peripheral", "m4_rom_slot", 7);
   {
      std::string ide_path = conf.getStringValue("peripheral", "ide_master", "");
      if (!ide_path.empty() && g_symbiface.enabled) {
         symbiface_ide_attach(0, ide_path);
      }
      ide_path = conf.getStringValue("peripheral", "ide_slave", "");
      if (!ide_path.empty() && g_symbiface.enabled) {
         symbiface_ide_attach(1, ide_path);
      }
   }

   CPC.kbd_layout = conf.getStringValue("control", "kbd_layout", "keymap_us.map");

   CPC.max_tracksize = conf.getIntValue("file", "max_track_size", 6144-154);
   CPC.current_snap_path =
   CPC.snap_path = conf.getStringValue("file", "snap_path", appPath + "/snap/");
   CPC.current_cart_path =
   CPC.cart_path = conf.getStringValue("file", "cart_path", appPath + "/cart/");
   CPC.current_dsk_path =
   CPC.dsk_path = conf.getStringValue("file", "dsk_path", appPath + "/disk/");
   CPC.current_tape_path =
   CPC.tape_path = conf.getStringValue("file", "tape_path", appPath + "/tape/");

   int iFmt = FIRST_CUSTOM_DISK_FORMAT;
   for (int i = iFmt; i < MAX_DISK_FORMAT; i++) { // loop through all user definable disk formats
      char chFmtId[14];
      snprintf(chFmtId, sizeof(chFmtId), "fmt%02d", i); // build format ID
      std::string formatString = conf.getStringValue("file", chFmtId, "");
      disk_format[iFmt] = parseDiskFormat(formatString);
      if (!disk_format[iFmt].label.empty()) { // found format definition for this slot?
         iFmt++; // entry is valid
      }
   }
   CPC.printer_file = conf.getStringValue("file", "printer_file", appPath + "/printer.dat");
   CPC.sdump_dir = conf.getStringValue("file", "sdump_dir", appPath + "/screenshots");

   CPC.rom_path = conf.getStringValue("rom", "rom_path", appPath + "/rom/");
   for (int iRomNum = 0; iRomNum < MAX_ROM_SLOTS; iRomNum++) { // loop for ROMs 0-31
      char chRomId[14];
      snprintf(chRomId, sizeof(chRomId), "slot%02d", iRomNum); // build ROM ID
      CPC.rom_file[iRomNum] = conf.getStringValue("rom", chRomId, "");
   }
   CPC.rom_mf2 = conf.getStringValue("rom", "rom_mf2", "");

   CPC.cartridge.file = CPC.rom_path + "/system.cpr"; // Only default path defined. Needed for CPC6128+
}



bool saveConfiguration (t_CPC &CPC, const std::string& configFilename)
{
   config::Config conf;

   conf.setIntValue("system", "model", CPC.model);
   conf.setIntValue("system", "jumpers", CPC.jumpers);

   conf.setIntValue("system", "ram_size", CPC.ram_size); // 128KB RAM
   conf.setIntValue("system", "limit_speed", CPC.limit_speed);
   conf.setIntValue("system", "speed", CPC.speed); // original CPC speed
   conf.setIntValue("system", "auto_pause", CPC.auto_pause);
   conf.setIntValue("system", "printer", CPC.printer);
   conf.setIntValue("system", "mf2", CPC.mf2);
   conf.setIntValue("system", "keyboard", CPC.keyboard);
   conf.setIntValue("system", "boot_time", CPC.boot_time);
   conf.setIntValue("system", "joystick_emulation", CPC.joystick_emulation);
   conf.setIntValue("system", "joysticks", CPC.joysticks);
   conf.setIntValue("system", "joystick_menu_button", CPC.joystick_menu_button + 1);
   conf.setIntValue("system", "joystick_vkeyboard_button", CPC.joystick_vkeyboard_button + 1);
   conf.setStringValue("system", "resources_path", CPC.resources_path);

   conf.setIntValue("video", "scr_scale", CPC.scr_scale);
   conf.setIntValue("video", "scr_preserve_aspect_ratio", CPC.scr_preserve_aspect_ratio);
   conf.setIntValue("video", "scr_style", CPC.scr_style);
   conf.setIntValue("video", "scr_oglfilter", CPC.scr_oglfilter);
   conf.setIntValue("video", "scr_oglscanlines", CPC.scr_oglscanlines);
   conf.setIntValue("video", "scr_led", CPC.scr_led);
   conf.setIntValue("video", "scr_fps", CPC.scr_fps);
   conf.setIntValue("video", "scr_tube", CPC.scr_tube);
   conf.setIntValue("video", "scr_intensity", CPC.scr_intensity);
   conf.setIntValue("video", "scr_remanency", CPC.scr_remanency);
   conf.setIntValue("video", "scr_window", CPC.scr_window);

   conf.setIntValue("devtools", "scale", CPC.devtools_scale);

   conf.setIntValue("ui", "workspace_layout", CPC.workspace_layout);
   conf.setIntValue("ui", "cpc_screen_scale", CPC.cpc_screen_scale);

   conf.setIntValue("video", "scr_green_mode", CPC.scr_green_mode);
   conf.setIntValue("video", "scr_green_blue_percent", CPC.scr_green_blue_percent);

   conf.setIntValue("sound", "enabled", CPC.snd_enabled);
   conf.setIntValue("sound", "playback_rate", CPC.snd_playback_rate);
   conf.setIntValue("sound", "bits", CPC.snd_bits);
   conf.setIntValue("sound", "stereo", CPC.snd_stereo);
   conf.setIntValue("sound", "volume", CPC.snd_volume);
   conf.setIntValue("sound", "pp_device", CPC.snd_pp_device);
   conf.setIntValue("sound", "amdrum", g_amdrum.enabled ? 1 : 0);
   conf.setIntValue("sound", "disk_sounds", g_drive_sounds.disk_enabled ? 1 : 0);
   conf.setIntValue("sound", "tape_sounds", g_drive_sounds.tape_enabled ? 1 : 0);
   conf.setIntValue("system", "smartwatch", g_smartwatch.enabled ? 1 : 0);
   conf.setIntValue("input", "amx_mouse", g_amx_mouse.enabled ? 1 : 0);

   conf.setIntValue("peripheral", "symbiface", g_symbiface.enabled ? 1 : 0);
   conf.setStringValue("peripheral", "ide_master", g_symbiface.ide_master.image_path);
   conf.setStringValue("peripheral", "ide_slave", g_symbiface.ide_slave.image_path);
   conf.setIntValue("peripheral", "m4board", g_m4board.enabled ? 1 : 0);
   conf.setStringValue("peripheral", "m4_sd_path", g_m4board.sd_root_path);
   conf.setIntValue("peripheral", "m4_rom_slot", g_m4board.rom_slot);

   conf.setStringValue("control", "kbd_layout", CPC.kbd_layout);

   conf.setIntValue("file", "max_track_size", CPC.max_tracksize);
   conf.setStringValue("file", "snap_path", CPC.snap_path);
   conf.setStringValue("file", "cart_path", CPC.cart_path);
   conf.setStringValue("file", "dsk_path", CPC.dsk_path);
   conf.setStringValue("file", "tape_path", CPC.tape_path);

   for (int iFmt = FIRST_CUSTOM_DISK_FORMAT; iFmt < MAX_DISK_FORMAT; iFmt++) { // loop through all user definable disk formats
      char chFmtId[14];
      snprintf(chFmtId, sizeof(chFmtId), "fmt%02d", iFmt); // build format ID
      conf.setStringValue("file", chFmtId, serializeDiskFormat(disk_format[iFmt]));
   }
   conf.setStringValue("file", "printer_file", CPC.printer_file);
   conf.setStringValue("file", "sdump_dir", CPC.sdump_dir);

   conf.setStringValue("rom", "rom_path", CPC.rom_path);
   for (int iRomNum = 0; iRomNum < MAX_ROM_SLOTS; iRomNum++) { // loop for ROMs 0-31
      char chRomId[14];
      snprintf(chRomId, sizeof(chRomId), "slot%02d", iRomNum); // build ROM ID
      conf.setStringValue("rom", chRomId, CPC.rom_file[iRomNum]);
   }
   conf.setStringValue("rom", "rom_mf2", CPC.rom_mf2);

   return conf.saveToFile(configFilename);
}



// As long as a GUI is enabled, we must show the cursor.
// Because we can activate multiple GUIs at a time, we need to keep track of how
// many times we've been asked to show or hide cursor.
void set_cursor_visibility(bool show)
{
  static int shows_count = 1;
  if (show) {
    shows_count++;
  } else {
    shows_count--;
  }
  if (shows_count < 0) shows_count = 0;
  if (shows_count > 0) {
    SDL_ShowCursor();
  } else {
    SDL_HideCursor();
  }
}


static bool userConfirmsQuitWithoutSaving()
{
   auto result = pfd::message("Unsaved Changes",
     "You have unsaved changes to a disk. Quit anyway?",
     pfd::choice::yes_no, pfd::icon::warning).result();
   return result == pfd::button::yes;
}

void showGui();
void showVKeyboard();
void dumpSnapshot();
void loadSnapshot();
void showVKeyboard()
{
   imgui_state.show_vkeyboard = !imgui_state.show_vkeyboard;
}

void koncpc_queue_virtual_keys(const std::string& text)
{
   auto newEvents = CPC.InputMapper->StringToEvents(text);
   virtualKeyboardEvents.splice(virtualKeyboardEvents.end(), newEvents);
   nextVirtualEventFrameCount = dwFrameCountOverall;
}

void koncpc_menu_action(int action)
{
   switch (action) {
      case KONCPC_GUI:
        {
          showGui();
          break;
        }

      case KONCPC_VKBD:
        {
          showVKeyboard();
          break;
        }

      case KONCPC_DEVTOOLS:
        {
          imgui_state.show_devtools = true;
          break;
        }

      case KONCPC_FULLSCRN:
         audio_pause();
         SDL_Delay(20);
         video_shutdown();
         CPC.scr_window = CPC.scr_window ? 0 : 1;
         if (video_init()) {
            fprintf(stderr, "video_init() failed. Aborting.\n");
            cleanExit(-1);
         }
#ifdef __APPLE__
         koncpc_setup_macos_menu();
#endif
         audio_resume();
         break;

      case KONCPC_SCRNSHOT:
         // Delay taking the screenshot to ensure the frame is complete.
         g_take_screenshot = true;
         break;

      case KONCPC_DELAY:
         // Reuse boot_time as it is a reasonable wait time for Plus transition between the F1/F2 nag screen and the command line.
         // TODO: Support an argument to KONCPC_DELAY in autocmd instead.
         LOG_VERBOSE("Take into account KONCPC_DELAY");
         nextVirtualEventFrameCount = dwFrameCountOverall + CPC.boot_time;
         break;

      case KONCPC_WAITBREAK:
         breakPointsToSkipBeforeProceedingWithVirtualEvents++;
         LOG_INFO("Will skip " << breakPointsToSkipBeforeProceedingWithVirtualEvents << " before processing more virtual events.");
         LOG_VERBOSE("Setting z80.break_point=0 (was " << z80.break_point << ").");
         z80.break_point = 0; // set break point to address 0. FIXME would be interesting to change this via a parameter of KONCPC_WAITBREAK on command line.
         break;

      case KONCPC_SNAPSHOT:
         dumpSnapshot();
         break;

      case KONCPC_LD_SNAP:
         loadSnapshot();
         break;

      case KONCPC_TAPEPLAY:
         LOG_VERBOSE("Request to play tape");
         Tape_Rewind();
         if (!pbTapeImage.empty()) {
            if (CPC.tape_play_button) {
               LOG_VERBOSE("Play button released");
               CPC.tape_play_button = 0;
            } else {
               LOG_VERBOSE("Play button pushed");
               CPC.tape_play_button = 0x10;
            }
         }
         set_osd_message(std::string("Play tape: ") + (CPC.tape_play_button ? "on" : "off"));
         break;

      case KONCPC_MF2STOP:
         if(CPC.mf2 && !(dwMF2Flags & MF2_ACTIVE)) {
           reg_pair port;

           // Set mode to activate ROM_config
           //port.b.h = 0x40;
           //z80_OUT_handler(port, 128);

           // Attempt to load MF2 in lower ROM (can fail if lower ROM is not active)
           port.b.h = 0xfe;
           port.b.l = 0xe8;
           dwMF2Flags &= ~MF2_INVISIBLE;
           z80_OUT_handler(port, 0);

           // Stop execution if load succeeded
           if(dwMF2Flags & MF2_ACTIVE) {
             z80_mf2stop();
           }
         }
         break;

      case KONCPC_RESET:
         LOG_VERBOSE("User requested emulator reset");
         emulator_reset();
         break;

      case KONCPC_JOY:
         CPC.joystick_emulation = CPC.joystick_emulation ? 0 : 1;
         CPC.InputMapper->set_joystick_emulation();
         set_osd_message(std::string("Joystick emulation: ") + (CPC.joystick_emulation ? "on" : "off"));
         break;

      case KONCPC_PHAZER:
         CPC.phazer_emulation = CPC.phazer_emulation.Next();
         if (!CPC.phazer_emulation) CPC.phazer_pressed = false;
         mouse_init();
         set_osd_message(std::string("Phazer emulation: ") + CPC.phazer_emulation.ToString());
         break;

      case KONCPC_PASTE:
         set_osd_message("Pasting...");
         {
           auto content = std::string(SDL_GetClipboardText());
           LOG_VERBOSE("Pasting '" << content << "'");
           auto newEvents = CPC.InputMapper->StringToEvents(content);
           virtualKeyboardEvents.splice(virtualKeyboardEvents.end(), newEvents);
           nextVirtualEventFrameCount = dwFrameCountOverall;
           break;
         }

      case KONCPC_EXIT:
         cleanExit (0);
         break;

      case KONCPC_FPS:
         CPC.scr_fps = CPC.scr_fps ? 0 : 1; // toggle fps display on or off
         set_osd_message(std::string("Performances info: ") + (CPC.scr_fps ? "on" : "off"));
         break;

      case KONCPC_SPEED:
         CPC.limit_speed = CPC.limit_speed ? 0 : 1;
         set_osd_message(std::string("Limit speed: ") + (CPC.limit_speed ? "on" : "off"));
         break;

      case KONCPC_DEBUG:
         log_verbose = !log_verbose;
         #ifdef DEBUG
         dwDebugFlag = dwDebugFlag ? 0 : 1;
         #endif
         #ifdef DEBUG_CRTC
         if (dwDebugFlag) {
            for (int n = 0; n < 14; n++) {
               fprintf(pfoDebug, "%02x = %02x\r\n", n, CRTC.registers[n]);
            }
         }
         #endif
         set_osd_message(std::string("Debug mode: ") + (log_verbose ? "on" : "off"));
         break;

      case KONCPC_NEXTDISKA:
         CPC.driveA.zip_index += 1;
         file_load(CPC.driveA);
         break;
   }
}

void showGui()
{
   imgui_state.show_menu = true;
   imgui_state.menu_just_opened = true;
   CPC.paused = true;
}

// TODO: Dedupe with the version in CapriceDevTools
// TODO: Support watchpoints too
void loadBreakpoints()
{
  if (args.symFilePath.empty()) return;
  Symfile symfile(args.symFilePath);
  for (auto breakpoint : symfile.Breakpoints()) {
    if (std::find_if(breakpoints.begin(), breakpoints.end(),
          [&](const auto& bp) { return bp.address == breakpoint; } ) != breakpoints.end()) continue;
    breakpoints.emplace_back(breakpoint);
  }
  // Populate global symbol table from symfile
  for (const auto& [addr, name] : symfile.Symbols()) {
    g_symfile.addSymbol(addr, name);
  }
}

bool dumpScreenTo(const std::string& path) {
   if (!back_surface) return false;
   if (SDL_SavePNG(back_surface, path)) {
     LOG_ERROR("Could not write screenshot file to " + path);
     return false;
   }
   return true;
}

void dumpScreen() {
   std::string dir = CPC.sdump_dir;
   if (!is_directory(dir)) {
          LOG_ERROR("Unable to find or open directory " + CPC.sdump_dir + " when trying to take a screenshot. Defaulting to current directory.")
          dir = ".";
   }
   std::string dumpFile = "screenshot_" + getDateString() + ".png";
   std::string dumpPath = dir + "/" + dumpFile;
   LOG_INFO("Dumping screen to " + dumpPath);
   if (!dumpScreenTo(dumpPath)) {
     LOG_ERROR("Could not write screenshot file to " + dumpPath);
   }
   else {
     set_osd_message("Captured " + dumpFile);
   }
}

// Very similar to screenshot, but difficult to factorize :-)
void dumpSnapshot() {
   std::string dir = CPC.snap_path;
   if (!is_directory(dir)) {
          LOG_ERROR("Unable to find or open directory " + CPC.snap_path + " when trying to take a machine snapshot. Defaulting to current directory.")
          dir = ".";
   }
   std::string dumpFile = "snapshot_" + getDateString() + ".sna";
   std::string dumpPath = dir + "/" + dumpFile;
   LOG_INFO("Dumping machine snapshot to " + dumpPath);
   if (snapshot_save(dumpPath)) {
     LOG_ERROR("Could not write machine snapshot to " + dumpPath);
   }
   else {
     set_osd_message("Snapshotted " + dumpFile);
   }
   lastSavedSnapshot = dumpPath;
}

void loadSnapshot() {
   if (lastSavedSnapshot.empty()) return;
   LOG_INFO("Loading snapshot from " + lastSavedSnapshot);
   if (snapshot_load(lastSavedSnapshot)) {
     LOG_ERROR("Could not load machine snapshot from " + lastSavedSnapshot);
   }
   else {
     std::string dirname, filename;
     stringutils::splitPath(lastSavedSnapshot, dirname, filename);
     set_osd_message("Restored " + filename);
   }
}

bool driveAltered() {
  return driveA.altered || driveB.altered;
}

void doCleanUp ()
{
   printer_stop();
   emulator_shutdown();

   dsk_eject(&driveA);
   dsk_eject(&driveB);
   tape_eject();

   symbiface_cleanup();
   m4board_cleanup();
   joysticks_shutdown();
   audio_shutdown();
   video_clear_topbar();
   video_shutdown();

   #ifdef DEBUG
   if(pfoDebug) {
     fclose(pfoDebug);
   }
   #endif

   SDL_Quit();
}

void cleanExit(int returnCode, bool askIfUnsaved)
{
   if (!g_headless && askIfUnsaved && driveAltered() && !userConfirmsQuitWithoutSaving()) {
     return;
   }
   doCleanUp();
   _exit(returnCode);
}

// TODO(SDL2): Remove these 2 maps once not needed to debug keymaps anymore
#include <map>
std::map<SDL_Keycode, std::string> keycode_names = {
    {SDLK_UNKNOWN, "SDLK_UNKNOWN"},
    {SDLK_RETURN, "SDLK_RETURN"},
    {SDLK_ESCAPE, "SDLK_ESCAPE"},
    {SDLK_BACKSPACE, "SDLK_BACKSPACE"},
    {SDLK_TAB, "SDLK_TAB"},
    {SDLK_SPACE, "SDLK_SPACE"},
    {SDLK_EXCLAIM, "SDLK_EXCLAIM"},
    {SDLK_DBLAPOSTROPHE, "SDLK_DBLAPOSTROPHE"},
    {SDLK_HASH, "SDLK_HASH"},
    {SDLK_PERCENT, "SDLK_PERCENT"},
    {SDLK_DOLLAR, "SDLK_DOLLAR"},
    {SDLK_AMPERSAND, "SDLK_AMPERSAND"},
    {SDLK_APOSTROPHE, "SDLK_APOSTROPHE"},
    {SDLK_LEFTPAREN, "SDLK_LEFTPAREN"},
    {SDLK_RIGHTPAREN, "SDLK_RIGHTPAREN"},
    {SDLK_ASTERISK, "SDLK_ASTERISK"},
    {SDLK_PLUS, "SDLK_PLUS"},
    {SDLK_COMMA, "SDLK_COMMA"},
    {SDLK_MINUS, "SDLK_MINUS"},
    {SDLK_PERIOD, "SDLK_PERIOD"},
    {SDLK_SLASH, "SDLK_SLASH"},
    {SDLK_0, "SDLK_0"},
    {SDLK_1, "SDLK_1"},
    {SDLK_2, "SDLK_2"},
    {SDLK_3, "SDLK_3"},
    {SDLK_4, "SDLK_4"},
    {SDLK_5, "SDLK_5"},
    {SDLK_6, "SDLK_6"},
    {SDLK_7, "SDLK_7"},
    {SDLK_8, "SDLK_8"},
    {SDLK_9, "SDLK_9"},
    {SDLK_COLON, "SDLK_COLON"},
    {SDLK_SEMICOLON, "SDLK_SEMICOLON"},
    {SDLK_LESS, "SDLK_LESS"},
    {SDLK_EQUALS, "SDLK_EQUALS"},
    {SDLK_GREATER, "SDLK_GREATER"},
    {SDLK_QUESTION, "SDLK_QUESTION"},
    {SDLK_AT, "SDLK_AT"},
    {SDLK_LEFTBRACKET, "SDLK_LEFTBRACKET"},
    {SDLK_BACKSLASH, "SDLK_BACKSLASH"},
    {SDLK_RIGHTBRACKET, "SDLK_RIGHTBRACKET"},
    {SDLK_CARET, "SDLK_CARET"},
    {SDLK_UNDERSCORE, "SDLK_UNDERSCORE"},
    {SDLK_GRAVE, "SDLK_GRAVE"},
    {SDLK_A, "SDLK_A"},
    {SDLK_B, "SDLK_B"},
    {SDLK_C, "SDLK_C"},
    {SDLK_D, "SDLK_D"},
    {SDLK_E, "SDLK_E"},
    {SDLK_F, "SDLK_F"},
    {SDLK_G, "SDLK_G"},
    {SDLK_H, "SDLK_H"},
    {SDLK_I, "SDLK_I"},
    {SDLK_J, "SDLK_J"},
    {SDLK_K, "SDLK_K"},
    {SDLK_L, "SDLK_L"},
    {SDLK_M, "SDLK_M"},
    {SDLK_N, "SDLK_N"},
    {SDLK_O, "SDLK_O"},
    {SDLK_P, "SDLK_P"},
    {SDLK_Q, "SDLK_Q"},
    {SDLK_R, "SDLK_R"},
    {SDLK_S, "SDLK_S"},
    {SDLK_T, "SDLK_T"},
    {SDLK_U, "SDLK_U"},
    {SDLK_V, "SDLK_V"},
    {SDLK_W, "SDLK_W"},
    {SDLK_X, "SDLK_X"},
    {SDLK_Y, "SDLK_Y"},
    {SDLK_Z, "SDLK_Z"},
    {SDLK_CAPSLOCK, "SDLK_CAPSLOCK"},
    {SDLK_F1, "SDLK_F1"},
    {SDLK_F2, "SDLK_F2"},
    {SDLK_F3, "SDLK_F3"},
    {SDLK_F4, "SDLK_F4"},
    {SDLK_F5, "SDLK_F5"},
    {SDLK_F6, "SDLK_F6"},
    {SDLK_F7, "SDLK_F7"},
    {SDLK_F8, "SDLK_F8"},
    {SDLK_F9, "SDLK_F9"},
    {SDLK_F10, "SDLK_F10"},
    {SDLK_F11, "SDLK_F11"},
    {SDLK_F12, "SDLK_F12"},
    {SDLK_PRINTSCREEN, "SDLK_PRINTSCREEN"},
    {SDLK_SCROLLLOCK, "SDLK_SCROLLLOCK"},
    {SDLK_PAUSE, "SDLK_PAUSE"},
    {SDLK_INSERT, "SDLK_INSERT"},
    {SDLK_HOME, "SDLK_HOME"},
    {SDLK_PAGEUP, "SDLK_PAGEUP"},
    {SDLK_DELETE, "SDLK_DELETE"},
    {SDLK_END, "SDLK_END"},
    {SDLK_PAGEDOWN, "SDLK_PAGEDOWN"},
    {SDLK_RIGHT, "SDLK_RIGHT"},
    {SDLK_LEFT, "SDLK_LEFT"},
    {SDLK_DOWN, "SDLK_DOWN"},
    {SDLK_UP, "SDLK_UP"},
    {SDLK_NUMLOCKCLEAR, "SDLK_NUMLOCKCLEAR"},
    {SDLK_KP_DIVIDE, "SDLK_KP_DIVIDE"},
    {SDLK_KP_MULTIPLY, "SDLK_KP_MULTIPLY"},
    {SDLK_KP_MINUS, "SDLK_KP_MINUS"},
    {SDLK_KP_PLUS, "SDLK_KP_PLUS"},
    {SDLK_KP_ENTER, "SDLK_KP_ENTER"},
    {SDLK_KP_1, "SDLK_KP_1"},
    {SDLK_KP_2, "SDLK_KP_2"},
    {SDLK_KP_3, "SDLK_KP_3"},
    {SDLK_KP_4, "SDLK_KP_4"},
    {SDLK_KP_5, "SDLK_KP_5"},
    {SDLK_KP_6, "SDLK_KP_6"},
    {SDLK_KP_7, "SDLK_KP_7"},
    {SDLK_KP_8, "SDLK_KP_8"},
    {SDLK_KP_9, "SDLK_KP_9"},
    {SDLK_KP_0, "SDLK_KP_0"},
    {SDLK_KP_PERIOD, "SDLK_KP_PERIOD"},
    {SDLK_APPLICATION, "SDLK_APPLICATION"},
    {SDLK_POWER, "SDLK_POWER"},
    {SDLK_KP_EQUALS, "SDLK_KP_EQUALS"},
    {SDLK_F13, "SDLK_F13"},
    {SDLK_F14, "SDLK_F14"},
    {SDLK_F15, "SDLK_F15"},
    {SDLK_F16, "SDLK_F16"},
    {SDLK_F17, "SDLK_F17"},
    {SDLK_F18, "SDLK_F18"},
    {SDLK_F19, "SDLK_F19"},
    {SDLK_F20, "SDLK_F20"},
    {SDLK_F21, "SDLK_F21"},
    {SDLK_F22, "SDLK_F22"},
    {SDLK_F23, "SDLK_F23"},
    {SDLK_F24, "SDLK_F24"},
    {SDLK_EXECUTE, "SDLK_EXECUTE"},
    {SDLK_HELP, "SDLK_HELP"},
    {SDLK_MENU, "SDLK_MENU"},
    {SDLK_SELECT, "SDLK_SELECT"},
    {SDLK_STOP, "SDLK_STOP"},
    {SDLK_AGAIN, "SDLK_AGAIN"},
    {SDLK_UNDO, "SDLK_UNDO"},
    {SDLK_CUT, "SDLK_CUT"},
    {SDLK_COPY, "SDLK_COPY"},
    {SDLK_PASTE, "SDLK_PASTE"},
    {SDLK_FIND, "SDLK_FIND"},
    {SDLK_MUTE, "SDLK_MUTE"},
    {SDLK_VOLUMEUP, "SDLK_VOLUMEUP"},
    {SDLK_VOLUMEDOWN, "SDLK_VOLUMEDOWN"},
    {SDLK_KP_COMMA, "SDLK_KP_COMMA"},
    {SDLK_KP_EQUALSAS400, "SDLK_KP_EQUALSAS400"},
    {SDLK_ALTERASE, "SDLK_ALTERASE"},
    {SDLK_SYSREQ, "SDLK_SYSREQ"},
    {SDLK_CANCEL, "SDLK_CANCEL"},
    {SDLK_CLEAR, "SDLK_CLEAR"},
    {SDLK_PRIOR, "SDLK_PRIOR"},
    {SDLK_RETURN2, "SDLK_RETURN2"},
    {SDLK_SEPARATOR, "SDLK_SEPARATOR"},
    {SDLK_OUT, "SDLK_OUT"},
    {SDLK_OPER, "SDLK_OPER"},
    {SDLK_CLEARAGAIN, "SDLK_CLEARAGAIN"},
    {SDLK_CRSEL, "SDLK_CRSEL"},
    {SDLK_EXSEL, "SDLK_EXSEL"},
    {SDLK_KP_00, "SDLK_KP_00"},
    {SDLK_KP_000, "SDLK_KP_000"},
    {SDLK_THOUSANDSSEPARATOR, "SDLK_THOUSANDSSEPARATOR"},
    {SDLK_DECIMALSEPARATOR, "SDLK_DECIMALSEPARATOR"},
    {SDLK_CURRENCYUNIT, "SDLK_CURRENCYUNIT"},
    {SDLK_CURRENCYSUBUNIT, "SDLK_CURRENCYSUBUNIT"},
    {SDLK_KP_LEFTPAREN, "SDLK_KP_LEFTPAREN"},
    {SDLK_KP_RIGHTPAREN, "SDLK_KP_RIGHTPAREN"},
    {SDLK_KP_LEFTBRACE, "SDLK_KP_LEFTBRACE"},
    {SDLK_KP_RIGHTBRACE, "SDLK_KP_RIGHTBRACE"},
    {SDLK_KP_TAB, "SDLK_KP_TAB"},
    {SDLK_KP_BACKSPACE, "SDLK_KP_BACKSPACE"},
    {SDLK_KP_A, "SDLK_KP_A"},
    {SDLK_KP_B, "SDLK_KP_B"},
    {SDLK_KP_C, "SDLK_KP_C"},
    {SDLK_KP_D, "SDLK_KP_D"},
    {SDLK_KP_E, "SDLK_KP_E"},
    {SDLK_KP_F, "SDLK_KP_F"},
    {SDLK_KP_XOR, "SDLK_KP_XOR"},
    {SDLK_KP_POWER, "SDLK_KP_POWER"},
    {SDLK_KP_PERCENT, "SDLK_KP_PERCENT"},
    {SDLK_KP_LESS, "SDLK_KP_LESS"},
    {SDLK_KP_GREATER, "SDLK_KP_GREATER"},
    {SDLK_KP_AMPERSAND, "SDLK_KP_AMPERSAND"},
    {SDLK_KP_DBLAMPERSAND, "SDLK_KP_DBLAMPERSAND"},
    {SDLK_KP_VERTICALBAR, "SDLK_KP_VERTICALBAR"},
    {SDLK_KP_DBLVERTICALBAR, "SDLK_KP_DBLVERTICALBAR"},
    {SDLK_KP_COLON, "SDLK_KP_COLON"},
    {SDLK_KP_HASH, "SDLK_KP_HASH"},
    {SDLK_KP_SPACE, "SDLK_KP_SPACE"},
    {SDLK_KP_AT, "SDLK_KP_AT"},
    {SDLK_KP_EXCLAM, "SDLK_KP_EXCLAM"},
    {SDLK_KP_MEMSTORE, "SDLK_KP_MEMSTORE"},
    {SDLK_KP_MEMRECALL, "SDLK_KP_MEMRECALL"},
    {SDLK_KP_MEMCLEAR, "SDLK_KP_MEMCLEAR"},
    {SDLK_KP_MEMADD, "SDLK_KP_MEMADD"},
    {SDLK_KP_MEMSUBTRACT, "SDLK_KP_MEMSUBTRACT"},
    {SDLK_KP_MEMMULTIPLY, "SDLK_KP_MEMMULTIPLY"},
    {SDLK_KP_MEMDIVIDE, "SDLK_KP_MEMDIVIDE"},
    {SDLK_KP_PLUSMINUS, "SDLK_KP_PLUSMINUS"},
    {SDLK_KP_CLEAR, "SDLK_KP_CLEAR"},
    {SDLK_KP_CLEARENTRY, "SDLK_KP_CLEARENTRY"},
    {SDLK_KP_BINARY, "SDLK_KP_BINARY"},
    {SDLK_KP_OCTAL, "SDLK_KP_OCTAL"},
    {SDLK_KP_DECIMAL, "SDLK_KP_DECIMAL"},
    {SDLK_KP_HEXADECIMAL, "SDLK_KP_HEXADECIMAL"},
    {SDLK_LCTRL, "SDLK_LCTRL"},
    {SDLK_LSHIFT, "SDLK_LSHIFT"},
    {SDLK_LALT, "SDLK_LALT"},
    {SDLK_LGUI, "SDLK_LGUI"},
    {SDLK_RCTRL, "SDLK_RCTRL"},
    {SDLK_RSHIFT, "SDLK_RSHIFT"},
    {SDLK_RALT, "SDLK_RALT"},
    {SDLK_RGUI, "SDLK_RGUI"},
    {SDLK_MODE, "SDLK_MODE"},
    {SDLK_MEDIA_NEXT_TRACK, "SDLK_MEDIA_NEXT_TRACK"},
    {SDLK_MEDIA_PREVIOUS_TRACK, "SDLK_MEDIA_PREVIOUS_TRACK"},
    {SDLK_MEDIA_STOP, "SDLK_MEDIA_STOP"},
    {SDLK_MEDIA_PLAY, "SDLK_MEDIA_PLAY"},
    {SDLK_MUTE, "SDLK_MUTE"},
    {SDLK_MEDIA_SELECT, "SDLK_MEDIA_SELECT"},
    {SDLK_AC_SEARCH, "SDLK_AC_SEARCH"},
    {SDLK_AC_HOME, "SDLK_AC_HOME"},
    {SDLK_AC_BACK, "SDLK_AC_BACK"},
    {SDLK_AC_FORWARD, "SDLK_AC_FORWARD"},
    {SDLK_AC_STOP, "SDLK_AC_STOP"},
    {SDLK_AC_REFRESH, "SDLK_AC_REFRESH"},
    {SDLK_AC_BOOKMARKS, "SDLK_AC_BOOKMARKS"},
    {SDLK_MEDIA_EJECT, "SDLK_MEDIA_EJECT"},
    {SDLK_SLEEP, "SDLK_SLEEP"},
    #if SDL_VERSION_ATLEAST(2, 0, 6)
    {SDLK_MEDIA_REWIND, "SDLK_MEDIA_REWIND"},
    {SDLK_MEDIA_FAST_FORWARD, "SDLK_MEDIA_FAST_FORWARD"},
    #endif
};

std::map<SDL_Scancode, std::string> scancode_names = {
    {SDL_SCANCODE_UNKNOWN, "SDL_SCANCODE_UNKNOWN"},
    {SDL_SCANCODE_A, "SDL_SCANCODE_A"},
    {SDL_SCANCODE_B, "SDL_SCANCODE_B"},
    {SDL_SCANCODE_C, "SDL_SCANCODE_C"},
    {SDL_SCANCODE_D, "SDL_SCANCODE_D"},
    {SDL_SCANCODE_E, "SDL_SCANCODE_E"},
    {SDL_SCANCODE_F, "SDL_SCANCODE_F"},
    {SDL_SCANCODE_G, "SDL_SCANCODE_G"},
    {SDL_SCANCODE_H, "SDL_SCANCODE_H"},
    {SDL_SCANCODE_I, "SDL_SCANCODE_I"},
    {SDL_SCANCODE_J, "SDL_SCANCODE_J"},
    {SDL_SCANCODE_K, "SDL_SCANCODE_K"},
    {SDL_SCANCODE_L, "SDL_SCANCODE_L"},
    {SDL_SCANCODE_M, "SDL_SCANCODE_M"},
    {SDL_SCANCODE_N, "SDL_SCANCODE_N"},
    {SDL_SCANCODE_O, "SDL_SCANCODE_O"},
    {SDL_SCANCODE_P, "SDL_SCANCODE_P"},
    {SDL_SCANCODE_Q, "SDL_SCANCODE_Q"},
    {SDL_SCANCODE_R, "SDL_SCANCODE_R"},
    {SDL_SCANCODE_S, "SDL_SCANCODE_S"},
    {SDL_SCANCODE_T, "SDL_SCANCODE_T"},
    {SDL_SCANCODE_U, "SDL_SCANCODE_U"},
    {SDL_SCANCODE_V, "SDL_SCANCODE_V"},
    {SDL_SCANCODE_W, "SDL_SCANCODE_W"},
    {SDL_SCANCODE_X, "SDL_SCANCODE_X"},
    {SDL_SCANCODE_Y, "SDL_SCANCODE_Y"},
    {SDL_SCANCODE_Z, "SDL_SCANCODE_Z"},
    {SDL_SCANCODE_1, "SDL_SCANCODE_1"},
    {SDL_SCANCODE_2, "SDL_SCANCODE_2"},
    {SDL_SCANCODE_3, "SDL_SCANCODE_3"},
    {SDL_SCANCODE_4, "SDL_SCANCODE_4"},
    {SDL_SCANCODE_5, "SDL_SCANCODE_5"},
    {SDL_SCANCODE_6, "SDL_SCANCODE_6"},
    {SDL_SCANCODE_7, "SDL_SCANCODE_7"},
    {SDL_SCANCODE_8, "SDL_SCANCODE_8"},
    {SDL_SCANCODE_9, "SDL_SCANCODE_9"},
    {SDL_SCANCODE_0, "SDL_SCANCODE_0"},
    {SDL_SCANCODE_RETURN, "SDL_SCANCODE_RETURN"},
    {SDL_SCANCODE_ESCAPE, "SDL_SCANCODE_ESCAPE"},
    {SDL_SCANCODE_BACKSPACE, "SDL_SCANCODE_BACKSPACE"},
    {SDL_SCANCODE_TAB, "SDL_SCANCODE_TAB"},
    {SDL_SCANCODE_SPACE, "SDL_SCANCODE_SPACE"},
    {SDL_SCANCODE_MINUS, "SDL_SCANCODE_MINUS"},
    {SDL_SCANCODE_EQUALS, "SDL_SCANCODE_EQUALS"},
    {SDL_SCANCODE_LEFTBRACKET, "SDL_SCANCODE_LEFTBRACKET"},
    {SDL_SCANCODE_RIGHTBRACKET, "SDL_SCANCODE_RIGHTBRACKET"},
    {SDL_SCANCODE_BACKSLASH, "SDL_SCANCODE_BACKSLASH"},
    {SDL_SCANCODE_NONUSHASH, "SDL_SCANCODE_NONUSHASH"},
    {SDL_SCANCODE_SEMICOLON, "SDL_SCANCODE_SEMICOLON"},
    {SDL_SCANCODE_APOSTROPHE, "SDL_SCANCODE_APOSTROPHE"},
    {SDL_SCANCODE_GRAVE, "SDL_SCANCODE_GRAVE"},
    {SDL_SCANCODE_COMMA, "SDL_SCANCODE_COMMA"},
    {SDL_SCANCODE_PERIOD, "SDL_SCANCODE_PERIOD"},
    {SDL_SCANCODE_SLASH, "SDL_SCANCODE_SLASH"},
    {SDL_SCANCODE_CAPSLOCK, "SDL_SCANCODE_CAPSLOCK"},
    {SDL_SCANCODE_F1, "SDL_SCANCODE_F1"},
    {SDL_SCANCODE_F2, "SDL_SCANCODE_F2"},
    {SDL_SCANCODE_F3, "SDL_SCANCODE_F3"},
    {SDL_SCANCODE_F4, "SDL_SCANCODE_F4"},
    {SDL_SCANCODE_F5, "SDL_SCANCODE_F5"},
    {SDL_SCANCODE_F6, "SDL_SCANCODE_F6"},
    {SDL_SCANCODE_F7, "SDL_SCANCODE_F7"},
    {SDL_SCANCODE_F8, "SDL_SCANCODE_F8"},
    {SDL_SCANCODE_F9, "SDL_SCANCODE_F9"},
    {SDL_SCANCODE_F10, "SDL_SCANCODE_F10"},
    {SDL_SCANCODE_F11, "SDL_SCANCODE_F11"},
    {SDL_SCANCODE_F12, "SDL_SCANCODE_F12"},
    {SDL_SCANCODE_PRINTSCREEN, "SDL_SCANCODE_PRINTSCREEN"},
    {SDL_SCANCODE_SCROLLLOCK, "SDL_SCANCODE_SCROLLLOCK"},
    {SDL_SCANCODE_PAUSE, "SDL_SCANCODE_PAUSE"},
    {SDL_SCANCODE_INSERT, "SDL_SCANCODE_INSERT"},
    {SDL_SCANCODE_HOME, "SDL_SCANCODE_HOME"},
    {SDL_SCANCODE_PAGEUP, "SDL_SCANCODE_PAGEUP"},
    {SDL_SCANCODE_DELETE, "SDL_SCANCODE_DELETE"},
    {SDL_SCANCODE_END, "SDL_SCANCODE_END"},
    {SDL_SCANCODE_PAGEDOWN, "SDL_SCANCODE_PAGEDOWN"},
    {SDL_SCANCODE_RIGHT, "SDL_SCANCODE_RIGHT"},
    {SDL_SCANCODE_LEFT, "SDL_SCANCODE_LEFT"},
    {SDL_SCANCODE_DOWN, "SDL_SCANCODE_DOWN"},
    {SDL_SCANCODE_UP, "SDL_SCANCODE_UP"},
    {SDL_SCANCODE_NUMLOCKCLEAR, "SDL_SCANCODE_NUMLOCKCLEAR"},
    {SDL_SCANCODE_KP_DIVIDE, "SDL_SCANCODE_KP_DIVIDE"},
    {SDL_SCANCODE_KP_MULTIPLY, "SDL_SCANCODE_KP_MULTIPLY"},
    {SDL_SCANCODE_KP_MINUS, "SDL_SCANCODE_KP_MINUS"},
    {SDL_SCANCODE_KP_PLUS, "SDL_SCANCODE_KP_PLUS"},
    {SDL_SCANCODE_KP_ENTER, "SDL_SCANCODE_KP_ENTER"},
    {SDL_SCANCODE_KP_1, "SDL_SCANCODE_KP_1"},
    {SDL_SCANCODE_KP_2, "SDL_SCANCODE_KP_2"},
    {SDL_SCANCODE_KP_3, "SDL_SCANCODE_KP_3"},
    {SDL_SCANCODE_KP_4, "SDL_SCANCODE_KP_4"},
    {SDL_SCANCODE_KP_5, "SDL_SCANCODE_KP_5"},
    {SDL_SCANCODE_KP_6, "SDL_SCANCODE_KP_6"},
    {SDL_SCANCODE_KP_7, "SDL_SCANCODE_KP_7"},
    {SDL_SCANCODE_KP_8, "SDL_SCANCODE_KP_8"},
    {SDL_SCANCODE_KP_9, "SDL_SCANCODE_KP_9"},
    {SDL_SCANCODE_KP_0, "SDL_SCANCODE_KP_0"},
    {SDL_SCANCODE_KP_PERIOD, "SDL_SCANCODE_KP_PERIOD"},
    {SDL_SCANCODE_NONUSBACKSLASH, "SDL_SCANCODE_NONUSBACKSLASH"},
    {SDL_SCANCODE_APPLICATION, "SDL_SCANCODE_APPLICATION"},
    {SDL_SCANCODE_POWER, "SDL_SCANCODE_POWER"},
    {SDL_SCANCODE_KP_EQUALS, "SDL_SCANCODE_KP_EQUALS"},
    {SDL_SCANCODE_F13, "SDL_SCANCODE_F13"},
    {SDL_SCANCODE_F14, "SDL_SCANCODE_F14"},
    {SDL_SCANCODE_F15, "SDL_SCANCODE_F15"},
    {SDL_SCANCODE_F16, "SDL_SCANCODE_F16"},
    {SDL_SCANCODE_F17, "SDL_SCANCODE_F17"},
    {SDL_SCANCODE_F18, "SDL_SCANCODE_F18"},
    {SDL_SCANCODE_F19, "SDL_SCANCODE_F19"},
    {SDL_SCANCODE_F20, "SDL_SCANCODE_F20"},
    {SDL_SCANCODE_F21, "SDL_SCANCODE_F21"},
    {SDL_SCANCODE_F22, "SDL_SCANCODE_F22"},
    {SDL_SCANCODE_F23, "SDL_SCANCODE_F23"},
    {SDL_SCANCODE_F24, "SDL_SCANCODE_F24"},
    {SDL_SCANCODE_EXECUTE, "SDL_SCANCODE_EXECUTE"},
    {SDL_SCANCODE_HELP, "SDL_SCANCODE_HELP"},
    {SDL_SCANCODE_MENU, "SDL_SCANCODE_MENU"},
    {SDL_SCANCODE_SELECT, "SDL_SCANCODE_SELECT"},
    {SDL_SCANCODE_STOP, "SDL_SCANCODE_STOP"},
    {SDL_SCANCODE_AGAIN, "SDL_SCANCODE_AGAIN"},
    {SDL_SCANCODE_UNDO, "SDL_SCANCODE_UNDO"},
    {SDL_SCANCODE_CUT, "SDL_SCANCODE_CUT"},
    {SDL_SCANCODE_COPY, "SDL_SCANCODE_COPY"},
    {SDL_SCANCODE_PASTE, "SDL_SCANCODE_PASTE"},
    {SDL_SCANCODE_FIND, "SDL_SCANCODE_FIND"},
    {SDL_SCANCODE_MUTE, "SDL_SCANCODE_MUTE"},
    {SDL_SCANCODE_VOLUMEUP, "SDL_SCANCODE_VOLUMEUP"},
    {SDL_SCANCODE_VOLUMEDOWN, "SDL_SCANCODE_VOLUMEDOWN"},
/*     {SDL_SCANCODE_LOCKINGCAPSLOCK, "SDL_SCANCODE_LOCKINGCAPSLOCK"}, */
/*     {SDL_SCANCODE_LOCKINGNUMLOCK, "SDL_SCANCODE_LOCKINGNUMLOCK"}, */
/*     {SDL_SCANCODE_LOCKINGSCROLLLOCK, "SDL_SCANCODE_LOCKINGSCROLLLOCK"}, */
    {SDL_SCANCODE_KP_COMMA, "SDL_SCANCODE_KP_COMMA"},
    {SDL_SCANCODE_KP_EQUALSAS400, "SDL_SCANCODE_KP_EQUALSAS400"},
    {SDL_SCANCODE_INTERNATIONAL1, "SDL_SCANCODE_INTERNATIONAL1"},
    {SDL_SCANCODE_INTERNATIONAL2, "SDL_SCANCODE_INTERNATIONAL2"},
    {SDL_SCANCODE_INTERNATIONAL3, "SDL_SCANCODE_INTERNATIONAL3"},
    {SDL_SCANCODE_INTERNATIONAL4, "SDL_SCANCODE_INTERNATIONAL4"},
    {SDL_SCANCODE_INTERNATIONAL5, "SDL_SCANCODE_INTERNATIONAL5"},
    {SDL_SCANCODE_INTERNATIONAL6, "SDL_SCANCODE_INTERNATIONAL6"},
    {SDL_SCANCODE_INTERNATIONAL7, "SDL_SCANCODE_INTERNATIONAL7"},
    {SDL_SCANCODE_INTERNATIONAL8, "SDL_SCANCODE_INTERNATIONAL8"},
    {SDL_SCANCODE_INTERNATIONAL9, "SDL_SCANCODE_INTERNATIONAL9"},
    {SDL_SCANCODE_LANG1, "SDL_SCANCODE_LANG1"},
    {SDL_SCANCODE_LANG2, "SDL_SCANCODE_LANG2"},
    {SDL_SCANCODE_LANG3, "SDL_SCANCODE_LANG3"},
    {SDL_SCANCODE_LANG4, "SDL_SCANCODE_LANG4"},
    {SDL_SCANCODE_LANG5, "SDL_SCANCODE_LANG5"},
    {SDL_SCANCODE_LANG6, "SDL_SCANCODE_LANG6"},
    {SDL_SCANCODE_LANG7, "SDL_SCANCODE_LANG7"},
    {SDL_SCANCODE_LANG8, "SDL_SCANCODE_LANG8"},
    {SDL_SCANCODE_LANG9, "SDL_SCANCODE_LANG9"},
    {SDL_SCANCODE_ALTERASE, "SDL_SCANCODE_ALTERASE"},
    {SDL_SCANCODE_SYSREQ, "SDL_SCANCODE_SYSREQ"},
    {SDL_SCANCODE_CANCEL, "SDL_SCANCODE_CANCEL"},
    {SDL_SCANCODE_CLEAR, "SDL_SCANCODE_CLEAR"},
    {SDL_SCANCODE_PRIOR, "SDL_SCANCODE_PRIOR"},
    {SDL_SCANCODE_RETURN2, "SDL_SCANCODE_RETURN2"},
    {SDL_SCANCODE_SEPARATOR, "SDL_SCANCODE_SEPARATOR"},
    {SDL_SCANCODE_OUT, "SDL_SCANCODE_OUT"},
    {SDL_SCANCODE_OPER, "SDL_SCANCODE_OPER"},
    {SDL_SCANCODE_CLEARAGAIN, "SDL_SCANCODE_CLEARAGAIN"},
    {SDL_SCANCODE_CRSEL, "SDL_SCANCODE_CRSEL"},
    {SDL_SCANCODE_EXSEL, "SDL_SCANCODE_EXSEL"},
    {SDL_SCANCODE_KP_00, "SDL_SCANCODE_KP_00"},
    {SDL_SCANCODE_KP_000, "SDL_SCANCODE_KP_000"},
    {SDL_SCANCODE_THOUSANDSSEPARATOR, "SDL_SCANCODE_THOUSANDSSEPARATOR"},
    {SDL_SCANCODE_DECIMALSEPARATOR, "SDL_SCANCODE_DECIMALSEPARATOR"},
    {SDL_SCANCODE_CURRENCYUNIT, "SDL_SCANCODE_CURRENCYUNIT"},
    {SDL_SCANCODE_CURRENCYSUBUNIT, "SDL_SCANCODE_CURRENCYSUBUNIT"},
    {SDL_SCANCODE_KP_LEFTPAREN, "SDL_SCANCODE_KP_LEFTPAREN"},
    {SDL_SCANCODE_KP_RIGHTPAREN, "SDL_SCANCODE_KP_RIGHTPAREN"},
    {SDL_SCANCODE_KP_LEFTBRACE, "SDL_SCANCODE_KP_LEFTBRACE"},
    {SDL_SCANCODE_KP_RIGHTBRACE, "SDL_SCANCODE_KP_RIGHTBRACE"},
    {SDL_SCANCODE_KP_TAB, "SDL_SCANCODE_KP_TAB"},
    {SDL_SCANCODE_KP_BACKSPACE, "SDL_SCANCODE_KP_BACKSPACE"},
    {SDL_SCANCODE_KP_A, "SDL_SCANCODE_KP_A"},
    {SDL_SCANCODE_KP_B, "SDL_SCANCODE_KP_B"},
    {SDL_SCANCODE_KP_C, "SDL_SCANCODE_KP_C"},
    {SDL_SCANCODE_KP_D, "SDL_SCANCODE_KP_D"},
    {SDL_SCANCODE_KP_E, "SDL_SCANCODE_KP_E"},
    {SDL_SCANCODE_KP_F, "SDL_SCANCODE_KP_F"},
    {SDL_SCANCODE_KP_XOR, "SDL_SCANCODE_KP_XOR"},
    {SDL_SCANCODE_KP_POWER, "SDL_SCANCODE_KP_POWER"},
    {SDL_SCANCODE_KP_PERCENT, "SDL_SCANCODE_KP_PERCENT"},
    {SDL_SCANCODE_KP_LESS, "SDL_SCANCODE_KP_LESS"},
    {SDL_SCANCODE_KP_GREATER, "SDL_SCANCODE_KP_GREATER"},
    {SDL_SCANCODE_KP_AMPERSAND, "SDL_SCANCODE_KP_AMPERSAND"},
    {SDL_SCANCODE_KP_DBLAMPERSAND, "SDL_SCANCODE_KP_DBLAMPERSAND"},
    {SDL_SCANCODE_KP_VERTICALBAR, "SDL_SCANCODE_KP_VERTICALBAR"},
    {SDL_SCANCODE_KP_DBLVERTICALBAR, "SDL_SCANCODE_KP_DBLVERTICALBAR"},
    {SDL_SCANCODE_KP_COLON, "SDL_SCANCODE_KP_COLON"},
    {SDL_SCANCODE_KP_HASH, "SDL_SCANCODE_KP_HASH"},
    {SDL_SCANCODE_KP_SPACE, "SDL_SCANCODE_KP_SPACE"},
    {SDL_SCANCODE_KP_AT, "SDL_SCANCODE_KP_AT"},
    {SDL_SCANCODE_KP_EXCLAM, "SDL_SCANCODE_KP_EXCLAM"},
    {SDL_SCANCODE_KP_MEMSTORE, "SDL_SCANCODE_KP_MEMSTORE"},
    {SDL_SCANCODE_KP_MEMRECALL, "SDL_SCANCODE_KP_MEMRECALL"},
    {SDL_SCANCODE_KP_MEMCLEAR, "SDL_SCANCODE_KP_MEMCLEAR"},
    {SDL_SCANCODE_KP_MEMADD, "SDL_SCANCODE_KP_MEMADD"},
    {SDL_SCANCODE_KP_MEMSUBTRACT, "SDL_SCANCODE_KP_MEMSUBTRACT"},
    {SDL_SCANCODE_KP_MEMMULTIPLY, "SDL_SCANCODE_KP_MEMMULTIPLY"},
    {SDL_SCANCODE_KP_MEMDIVIDE, "SDL_SCANCODE_KP_MEMDIVIDE"},
    {SDL_SCANCODE_KP_PLUSMINUS, "SDL_SCANCODE_KP_PLUSMINUS"},
    {SDL_SCANCODE_KP_CLEAR, "SDL_SCANCODE_KP_CLEAR"},
    {SDL_SCANCODE_KP_CLEARENTRY, "SDL_SCANCODE_KP_CLEARENTRY"},
    {SDL_SCANCODE_KP_BINARY, "SDL_SCANCODE_KP_BINARY"},
    {SDL_SCANCODE_KP_OCTAL, "SDL_SCANCODE_KP_OCTAL"},
    {SDL_SCANCODE_KP_DECIMAL, "SDL_SCANCODE_KP_DECIMAL"},
    {SDL_SCANCODE_KP_HEXADECIMAL, "SDL_SCANCODE_KP_HEXADECIMAL"},
    {SDL_SCANCODE_LCTRL, "SDL_SCANCODE_LCTRL"},
    {SDL_SCANCODE_LSHIFT, "SDL_SCANCODE_LSHIFT"},
    {SDL_SCANCODE_LALT, "SDL_SCANCODE_LALT"},
    {SDL_SCANCODE_LGUI, "SDL_SCANCODE_LGUI"},
    {SDL_SCANCODE_RCTRL, "SDL_SCANCODE_RCTRL"},
    {SDL_SCANCODE_RSHIFT, "SDL_SCANCODE_RSHIFT"},
    {SDL_SCANCODE_RALT, "SDL_SCANCODE_RALT"},
    {SDL_SCANCODE_RGUI, "SDL_SCANCODE_RGUI"},
    {SDL_SCANCODE_MODE, "SDL_SCANCODE_MODE"},
    {SDL_SCANCODE_MEDIA_NEXT_TRACK, "SDL_SCANCODE_MEDIA_NEXT_TRACK"},
    {SDL_SCANCODE_MEDIA_PREVIOUS_TRACK, "SDL_SCANCODE_MEDIA_PREVIOUS_TRACK"},
    {SDL_SCANCODE_MEDIA_STOP, "SDL_SCANCODE_MEDIA_STOP"},
    {SDL_SCANCODE_MEDIA_PLAY, "SDL_SCANCODE_MEDIA_PLAY"},
    {SDL_SCANCODE_MUTE, "SDL_SCANCODE_MUTE"},
    {SDL_SCANCODE_MEDIA_SELECT, "SDL_SCANCODE_MEDIA_SELECT"},
    {SDL_SCANCODE_AC_SEARCH, "SDL_SCANCODE_AC_SEARCH"},
    {SDL_SCANCODE_AC_HOME, "SDL_SCANCODE_AC_HOME"},
    {SDL_SCANCODE_AC_BACK, "SDL_SCANCODE_AC_BACK"},
    {SDL_SCANCODE_AC_FORWARD, "SDL_SCANCODE_AC_FORWARD"},
    {SDL_SCANCODE_AC_STOP, "SDL_SCANCODE_AC_STOP"},
    {SDL_SCANCODE_AC_REFRESH, "SDL_SCANCODE_AC_REFRESH"},
    {SDL_SCANCODE_AC_BOOKMARKS, "SDL_SCANCODE_AC_BOOKMARKS"},
    {SDL_SCANCODE_MEDIA_EJECT, "SDL_SCANCODE_MEDIA_EJECT"},
    {SDL_SCANCODE_SLEEP, "SDL_SCANCODE_SLEEP"},
    #if SDL_VERSION_ATLEAST(2, 0, 6)
    {SDL_SCANCODE_MEDIA_REWIND, "SDL_SCANCODE_MEDIA_REWIND"},
    {SDL_SCANCODE_MEDIA_FAST_FORWARD, "SDL_SCANCODE_MEDIA_FAST_FORWARD"},
    #endif
    {SDL_SCANCODE_COUNT, "SDL_SCANCODE_COUNT"},
};

int koncpc_main (int argc, char **argv)
{
   int iExitCondition;
   bool bin_loaded = false;
   SDL_Event event;
   std::vector<std::string> slot_list;

   try {
     binPath = std::filesystem::absolute(std::filesystem::path(argv[0]).parent_path());
   } catch(const std::filesystem::filesystem_error&) {
     // Fallback in case argv[0] is unresolvable (e.g. found via PATH).
     // binPath is only used for bundles anyway.
     binPath = std::filesystem::absolute(".");
   }
   parseArguments(argc, argv, slot_list, args);
   g_headless = args.headless;
   g_exit_on_break = args.exitOnBreak;

   // Parse --exit-after spec: Nf (frames), Ns (seconds), Nms (milliseconds)
   if (!args.exitAfter.empty()) {
      const std::string& spec = args.exitAfter;
      if (spec.size() > 2 && spec.substr(spec.size()-2) == "ms") {
         g_exit_mode = EXIT_MS;
         g_exit_target = std::stoul(spec.substr(0, spec.size()-2));
      } else if (spec.back() == 's') {
         g_exit_mode = EXIT_MS;
         g_exit_target = std::stoul(spec.substr(0, spec.size()-1)) * 1000;
      } else if (spec.back() == 'f') {
         g_exit_mode = EXIT_FRAMES;
         g_exit_target = std::stoul(spec.substr(0, spec.size()-1));
      } else {
         // Default: treat bare number as frames
         g_exit_mode = EXIT_FRAMES;
         g_exit_target = std::stoul(spec);
      }
   }

   if (g_headless) {
      // SDL3: timer is always available, init core only for headless
      if (!SDL_Init(0)) {
         fprintf(stderr, "SDL_Init(0) failed: %s\n", SDL_GetError());
         _exit(-1);
      }
   } else {
      if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
         fprintf(stderr, "SDL_Init() failed: %s\n", SDL_GetError());
         _exit(-1);
      }
   }

   // PNG loader uses libpng; no SDL_image init required

   // konCePCja IPC server (stub)
   g_ipc->start();

   #ifndef APP_PATH
   if(getcwd(chAppPath, sizeof(chAppPath)-1) == nullptr) {
      fprintf(stderr, "getcwd failed: %s\n", strerror(errno));
      cleanExit(-1);
   }
   #else
      snprintf(chAppPath, sizeof(chAppPath), "%s", APP_PATH);
   #endif

   loadConfiguration(CPC, getConfigurationFilename()); // retrieve the emulator configuration
   if (CPC.printer) {
      if (!printer_start()) { // start capturing printer output, if enabled
         CPC.printer = 0;
      }
   }

   z80_init_tables(); // init Z80 emulation

   if (g_headless) {
      // In headless mode, force the headless video plugin (offscreen surface only)
      static video_plugin hp = video_headless_plugin();
      vid_plugin = &hp;
      back_surface = vid_plugin->init(vid_plugin, CPC.scr_scale, false);
      if (!back_surface) {
         fprintf(stderr, "headless video_init() failed. Aborting.\n");
         _exit(-1);
      }
      {
         const SDL_PixelFormatDetails* fmt = SDL_GetPixelFormatDetails(back_surface->format);
         CPC.scr_bpp = fmt ? fmt->bits_per_pixel : 0;
      }
      video_set_style();
      if (video_set_palette()) {
         fprintf(stderr, "headless video_set_palette() failed. Aborting.\n");
         _exit(-1);
      }
      asic_set_palette();
      CPC.scr_bps = back_surface->pitch;
      CPC.scr_line_offs = CPC.scr_bps * dwYScale;
      CPC.scr_pos = CPC.scr_base = static_cast<byte *>(back_surface->pixels);
      CPC.scr_gui_is_currently_on = false;
      crtc_init();
      // No audio in headless mode
      CPC.snd_enabled = 0;
   } else {
      if (video_init()) {
         fprintf(stderr, "video_init() failed. Aborting.\n");
         cleanExit(-1);
      }
#ifdef __APPLE__
      koncpc_setup_macos_menu();
      koncpc_disable_app_nap();
#endif
      topbar_height_px = imgui_topbar_height();
      video_set_topbar(nullptr, topbar_height_px);
      mouse_init();

      if (audio_init()) {
         fprintf(stderr, "audio_init() failed. Disabling sound.\n");
         CPC.snd_enabled = 0;
      }

      if (joysticks_init()) {
         fprintf(stderr, "joysticks_init() failed. Joysticks won't work.\n");
      }
   }

#ifdef DEBUG
   pfoDebug = fopen("./debug.txt", "wt");
#endif

   // Extract files to be loaded from the command line args
   fillSlots(slot_list, CPC);

   // Must be done before emulator_init()
   CPC.InputMapper = new InputMapper(&CPC);

   // emulator_init must be called before loading files as they require
   // pbGPBuffer to be initialized.
   if (emulator_init()) {
      fprintf(stderr, "emulator_init() failed. Aborting.\n");
      cleanExit(-1);
   }

   // Really load the various drives, if needed
   loadSlots();

   // Fill the buffer with autocmd if provided
   virtualKeyboardEvents = CPC.InputMapper->StringToEvents(args.autocmd);
   // Give some time to the CPC to start before sending any command
   nextVirtualEventFrameCount = dwFrameCountOverall + CPC.boot_time;

// ----------------------------------------------------------------------------

   update_timings();
   if (!g_headless) audio_resume();

   loadBreakpoints();

   g_exit_start_ticks = SDL_GetTicks();
   iExitCondition = EC_FRAME_COMPLETE;

   while (true) {
      // We can only load bin files after the CPC finished the init
      if (!bin_loaded &&
          dwFrameCountOverall > CPC.boot_time) {
          bin_loaded = true;
          if (!args.binFile.empty()) bin_load(args.binFile, args.binOffset);
      }

      if(!virtualKeyboardEvents.empty()
         && (nextVirtualEventFrameCount < dwFrameCountOverall)
         && (breakPointsToSkipBeforeProceedingWithVirtualEvents == 0)) {

         auto nextVirtualEvent = &virtualKeyboardEvents.front();
         if (!g_headless) SDL_PushEvent(nextVirtualEvent);

         auto key = nextVirtualEvent->key.key;
         auto mod = static_cast<SDL_Keymod>(nextVirtualEvent->key.mod);
         auto evtype = nextVirtualEvent->key.type;
         LOG_DEBUG("Inserted virtual event key=" << int(key) << " (" << evtype << ")");

         CPCScancode scancode = CPC.InputMapper->CPCscancodeFromKeysym(key, mod);
         if (!(scancode & MOD_EMU_KEY)) {
            LOG_DEBUG("The virtual event is a keypress (not a command), so introduce a pause.");
            nextVirtualEventFrameCount = dwFrameCountOverall
               + ((evtype == SDL_EVENT_KEY_DOWN || evtype == SDL_EVENT_KEY_UP)?1:0);
         }

         // In headless mode, directly process keyboard events
         if (g_headless) {
            if (!(scancode & MOD_EMU_KEY)) {
               bool press = (evtype == SDL_EVENT_KEY_DOWN);
               applyKeypress(scancode, keyboard_matrix, press);
            } else if (evtype == SDL_EVENT_KEY_DOWN) {
               // Handle emulator commands (no SDL event loop in headless mode)
               switch (scancode) {
                  case KONCPC_EXIT:
                     cleanExit(0);
                     break;
                  case KONCPC_RESET:
                     emulator_reset();
                     break;
                  case KONCPC_WAITBREAK:
                     breakPointsToSkipBeforeProceedingWithVirtualEvents++;
                     LOG_INFO("Will skip " << breakPointsToSkipBeforeProceedingWithVirtualEvents << " before processing more virtual events.");
                     z80.break_point = 0;
                     break;
                  case KONCPC_DELAY:
                     nextVirtualEventFrameCount = dwFrameCountOverall + CPC.boot_time;
                     break;
                  case KONCPC_SNAPSHOT:
                     dumpSnapshot();
                     break;
                  case KONCPC_TAPEPLAY:
                     Tape_Rewind();
                     if (!pbTapeImage.empty()) {
                        CPC.tape_play_button = CPC.tape_play_button ? 0 : 0x10;
                     }
                     break;
                  case KONCPC_SPEED:
                     CPC.limit_speed = CPC.limit_speed ? 0 : 1;
                     break;
                  case KONCPC_DEBUG:
                     log_verbose = !log_verbose;
                     break;
                  default:
                     LOG_DEBUG("Ignoring emulator key " << scancode << " in headless mode");
                     break;
               }
            }
         }

         virtualKeyboardEvents.pop_front();
      }

      while (!g_headless && SDL_PollEvent(&event)) {
         // Handle main window close before ImGui consumes the event
         if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
           SDL_WindowID main_id = mainSDLWindow ? SDL_GetWindowID(mainSDLWindow) : 0;
           if (event.window.windowID == main_id) {
             cleanExit(0);
           }
         }

         // Feed event to Dear ImGui
         ImGui_ImplSDL3_ProcessEvent(&event);

         // Check for command palette shortcut (Cmd+K / Ctrl+K)
         if (event.type == SDL_EVENT_KEY_DOWN) {
           bool ctrl = (event.key.mod & SDL_KMOD_CTRL) != 0;
           bool cmd_key = (event.key.mod & SDL_KMOD_GUI) != 0;
           if (g_command_palette.handle_key(event.key.key, ctrl, cmd_key)) {
             continue;
           }
         }

         // If ImGui wants input, skip emulator processing.
         // Exception: virtual keyboard events (windowID=0) always reach the emulator.
         {
           ImGuiIO& io = ImGui::GetIO();
           bool is_key_event = (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_TEXT_INPUT);
           bool is_mouse_event_imgui = (event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_MOUSE_WHEEL);
           bool is_virtual_key = is_key_event && event.key.windowID == 0;
           if ((is_key_event && !is_virtual_key && io.WantCaptureKeyboard) || (is_mouse_event_imgui && io.WantCaptureMouse)) {
             continue;
           }
         }

         switch (event.type) {
            case SDL_EVENT_KEY_DOWN:
               {
                  CPCScancode scancode = CPC.InputMapper->CPCscancodeFromKeysym(event.key.key, static_cast<SDL_Keymod>(event.key.mod));
                  LOG_VERBOSE("Keyboard: pressed: " << SDL_GetKeyName(event.key.key) << " - keycode: " << keycode_names[event.key.key] << " (" << event.key.key << ") - scancode: " << scancode_names[event.key.scancode] << " (" << event.key.scancode << ") - CPC key: " << CPC.InputMapper->CPCkeyToString(CPC.InputMapper->CPCkeyFromKeysym(event.key.key, static_cast<SDL_Keymod>(event.key.mod))) << " - CPC scancode: " << scancode);
                  if (!(scancode & MOD_EMU_KEY)) {
                     applyKeypress(scancode, keyboard_matrix, true);
                  }
               }
               break;

            case SDL_EVENT_KEY_UP:
               {
                  CPCScancode scancode = CPC.InputMapper->CPCscancodeFromKeysym(event.key.key, static_cast<SDL_Keymod>(event.key.mod));
                  if (!(scancode & MOD_EMU_KEY)) {
                     applyKeypress(scancode, keyboard_matrix, false);
                  }
                  else { // process emulator specific keys
                     switch (scancode) {
                        case KONCPC_GUI:
                          {
                            showGui();
                            break;
                          }

                        case KONCPC_VKBD:
                          {
                            showVKeyboard();
                            break;
                          }

                        case KONCPC_DEVTOOLS:
                          {
                            imgui_state.show_devtools = true;
                            break;
                          }

                        case KONCPC_FULLSCRN:
                           audio_pause();
                           SDL_Delay(20);
                           video_shutdown();
                           CPC.scr_window = CPC.scr_window ? 0 : 1;
                           if (video_init()) {
                              fprintf(stderr, "video_init() failed. Aborting.\n");
                              cleanExit(-1);
                           }
#ifdef __APPLE__
                           koncpc_setup_macos_menu();
#endif
                           audio_resume();
                           break;

                        case KONCPC_SCRNSHOT:
                           // Delay taking the screenshot to ensure the frame is complete.
                           g_take_screenshot = true;
                           break;

                        case KONCPC_DELAY:
                           // Reuse boot_time as it is a reasonable wait time for Plus transition between the F1/F2 nag screen and the command line.
                           // TODO: Support an argument to KONCPC_DELAY in autocmd instead.
                           LOG_VERBOSE("Take into account KONCPC_DELAY");
                           nextVirtualEventFrameCount = dwFrameCountOverall + CPC.boot_time;
                           break;

                        case KONCPC_WAITBREAK:
                           breakPointsToSkipBeforeProceedingWithVirtualEvents++;
                           LOG_INFO("Will skip " << breakPointsToSkipBeforeProceedingWithVirtualEvents << " before processing more virtual events.");
                           LOG_VERBOSE("Setting z80.break_point=0 (was " << z80.break_point << ").");
                           z80.break_point = 0; // set break point to address 0. FIXME would be interesting to change this via a parameter of KONCPC_WAITBREAK on command line.
                           break;

                        case KONCPC_SNAPSHOT:
                           dumpSnapshot();
                           break;

                        case KONCPC_LD_SNAP:
                           loadSnapshot();
                           break;

                        case KONCPC_TAPEPLAY:
                           LOG_VERBOSE("Request to play tape");
                           Tape_Rewind();
                           if (!pbTapeImage.empty()) {
                              if (CPC.tape_play_button) {
                                 LOG_VERBOSE("Play button released");
                                 CPC.tape_play_button = 0;
                              } else {
                                 LOG_VERBOSE("Play button pushed");
                                 CPC.tape_play_button = 0x10;
                              }
                           }
                           set_osd_message(std::string("Play tape: ") + (CPC.tape_play_button ? "on" : "off"));
                           break;

                        case KONCPC_MF2STOP:
                           if(CPC.mf2 && !(dwMF2Flags & MF2_ACTIVE)) {
                             reg_pair port;

                             // Set mode to activate ROM_config
                             //port.b.h = 0x40;
                             //z80_OUT_handler(port, 128);

                             // Attempt to load MF2 in lower ROM (can fail if lower ROM is not active)
                             port.b.h = 0xfe;
                             port.b.l = 0xe8;
                             dwMF2Flags &= ~MF2_INVISIBLE;
                             z80_OUT_handler(port, 0);

                             // Stop execution if load succeeded
                             if(dwMF2Flags & MF2_ACTIVE) {
                               z80_mf2stop();
                             }
                           }
                           break;

                        case KONCPC_RESET:
                           LOG_VERBOSE("User requested emulator reset");
                           emulator_reset();
                           break;

                        case KONCPC_JOY:
                           CPC.joystick_emulation = CPC.joystick_emulation ? 0 : 1;
                           CPC.InputMapper->set_joystick_emulation();
                           set_osd_message(std::string("Joystick emulation: ") + (CPC.joystick_emulation ? "on" : "off"));
                           break;

                        case KONCPC_PHAZER:
                           CPC.phazer_emulation = CPC.phazer_emulation.Next();
                           if (!CPC.phazer_emulation) CPC.phazer_pressed = false;
                           mouse_init();
                           set_osd_message(std::string("Phazer emulation: ") + CPC.phazer_emulation.ToString());
                           break;

                        case KONCPC_PASTE:
                           set_osd_message("Pasting...");
                           {
                             auto content = std::string(SDL_GetClipboardText());
                             LOG_VERBOSE("Pasting '" << content << "'");
                             auto newEvents = CPC.InputMapper->StringToEvents(content);
                             virtualKeyboardEvents.splice(virtualKeyboardEvents.end(), newEvents);
                             nextVirtualEventFrameCount = dwFrameCountOverall;
                             break;
                           }

                        case KONCPC_EXIT:
                           cleanExit (0);
                           break;

                        case KONCPC_FPS:
                           CPC.scr_fps = CPC.scr_fps ? 0 : 1; // toggle fps display on or off
                           set_osd_message(std::string("Performances info: ") + (CPC.scr_fps ? "on" : "off"));
                           break;

                        case KONCPC_SPEED:
                           CPC.limit_speed = CPC.limit_speed ? 0 : 1;
                           set_osd_message(std::string("Limit speed: ") + (CPC.limit_speed ? "on" : "off"));
                           break;

                        case KONCPC_DEBUG:
                           log_verbose = !log_verbose;
                           #ifdef DEBUG
                           dwDebugFlag = dwDebugFlag ? 0 : 1;
                           #endif
                           #ifdef DEBUG_CRTC
                           if (dwDebugFlag) {
                              for (int n = 0; n < 14; n++) {
                                 fprintf(pfoDebug, "%02x = %02x\r\n", n, CRTC.registers[n]);
                              }
                           }
                           #endif
                           set_osd_message(std::string("Debug mode: ") + (log_verbose ? "on" : "off"));
                           break;

                        case KONCPC_NEXTDISKA:
                           CPC.driveA.zip_index += 1;
                           file_load(CPC.driveA);
                           break;
                     }
                  }
               }
               break;

            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            {
                CPCScancode scancode = CPC.InputMapper->CPCscancodeFromJoystickButton(event.jbutton);
                if (scancode == 0xff) {
                  if (event.jbutton.button == CPC.joystick_menu_button)
                  {
                    showGui();
                  }
                  if (event.jbutton.button == CPC.joystick_vkeyboard_button)
                  {
                    showVKeyboard();
                  }
                }
                applyKeypress(scancode, keyboard_matrix, true);
            }
            break;

            case SDL_EVENT_JOYSTICK_BUTTON_UP:
            {
              CPCScancode scancode = CPC.InputMapper->CPCscancodeFromJoystickButton(event.jbutton);
              applyKeypress(scancode, keyboard_matrix, false);
            }
            break;

            case SDL_EVENT_JOYSTICK_AXIS_MOTION:
            {
              CPCScancode scancodes[2] = {0xff, 0xff};
              bool release = false;
              CPC.InputMapper->CPCscancodeFromJoystickAxis(event.jaxis, scancodes, release);
              applyKeypress(scancodes[0], keyboard_matrix, !release);
              if (release && scancodes[0] != 0xff) {
                 applyKeypress(scancodes[1], keyboard_matrix, !release);
              }
            }
            break;

            case SDL_EVENT_MOUSE_MOTION:
            {
              {
                SDL_WindowID main_wid = mainSDLWindow ? SDL_GetWindowID(mainSDLWindow) : 0;
                bool on_main = (event.motion.windowID == main_wid);
                bool over_topbar = on_main && event.motion.y < topbar_height_px;
                static bool topbar_cursor_visible = false;
                if (over_topbar && !topbar_cursor_visible) {
                  set_cursor_visibility(true);
                  topbar_cursor_visible = true;
                } else if (!over_topbar && topbar_cursor_visible && !CPC.scr_gui_is_currently_on && !CPC.phazer_emulation) {
                  set_cursor_visibility(false);
                  topbar_cursor_visible = false;
                }
              }
              CPC.phazer_x = (event.motion.x-vid_plugin->x_offset) * vid_plugin->x_scale;
              CPC.phazer_y = (event.motion.y-vid_plugin->y_offset) * vid_plugin->y_scale;
              if (g_amx_mouse.enabled) {
                amx_mouse_update(event.motion.xrel, event.motion.yrel, SDL_GetMouseState(nullptr, nullptr));
              }
              if (g_symbiface.enabled) {
                symbiface_mouse_update(event.motion.xrel, event.motion.yrel, SDL_GetMouseState(nullptr, nullptr));
              }
            }
            break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            {
              SDL_WindowID main_wid = mainSDLWindow ? SDL_GetWindowID(mainSDLWindow) : 0;
              if (event.button.windowID == main_wid && event.button.y < topbar_height_px) {
                if (!CPC.scr_gui_is_currently_on) showGui();
                break;
              }
              if (CPC.phazer_emulation) {
                // Trojan Light Phazer uses Joystick Fire for the trigger button:
                // https://www.cpcwiki.eu/index.php/Trojan_Light_Phazer
                if (CPC.phazer_emulation == PhazerType::TrojanLightPhazer) {
                auto scancode = CPC.InputMapper->CPCscancodeFromCPCkey(CPC_J0_FIRE1);
                  applyKeypress(scancode, keyboard_matrix, true);
                }
                CPC.phazer_pressed = true;
              }
              if (g_amx_mouse.enabled) {
                amx_mouse_update(0, 0, SDL_GetMouseState(nullptr, nullptr));
              }
            }
            break;

            case SDL_EVENT_MOUSE_BUTTON_UP:
            {
              if (CPC.phazer_emulation) {
                if (CPC.phazer_emulation == PhazerType::TrojanLightPhazer) {
                  auto scancode = CPC.InputMapper->CPCscancodeFromCPCkey(CPC_J0_FIRE1);
                  applyKeypress(scancode, keyboard_matrix, false);
                }
                CPC.phazer_pressed = false;
              }
              if (g_amx_mouse.enabled) {
                amx_mouse_update(0, 0, SDL_GetMouseState(nullptr, nullptr));
              }
            }
            break;

            // TODO: What if we were paused because of other reason than losing focus and then only lost focus
            //       the right thing to do here is to restore focus but keep paused... implementing this require
            //       keeping track of pause source, which will be a pain.
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
            case SDL_EVENT_WINDOW_MOUSE_ENTER:
              if (CPC.auto_pause) {
                cpc_resume();
              }
              break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            case SDL_EVENT_WINDOW_MINIMIZED:
              if (CPC.auto_pause) {
                cpc_pause();
              }
              break;

            case SDL_EVENT_QUIT:
               cleanExit(0);
         }
      }

      if (!CPC.paused) { // run the emulation, as long as the user doesn't pause it
         dwTicks = SDL_GetTicks();
         if (dwTicks >= dwTicksTargetFPS) { // update FPS counter?
            dwFPS = dwFrameCount;
            dwFrameCount = 0;
            dwTicksTargetFPS = dwTicks + 1000; // prep counter for the next run
         }

         if (CPC.limit_speed) { // limit to original CPC speed?
            if (iExitCondition == EC_CYCLE_COUNT) {
               dwTicks = SDL_GetTicks();
               if (dwTicks < dwTicksTarget) { // limit speed ?
                  if (dwTicksTarget - dwTicks > POLL_INTERVAL_MS) { // No need to burn cycles if next event is far away
                     std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
                  }
                  continue; // delay emulation
               }
               dwTicksTarget = dwTicks + dwTicksOffset; // prep counter for the next run
            }
         }

         dword dwOffset = CPC.scr_pos - CPC.scr_base; // offset in current surface row
         if (VDU.scrln > 0) {
            CPC.scr_base = static_cast<byte *>(back_surface->pixels) + (VDU.scrln * CPC.scr_line_offs); // determine current position
         } else {
            CPC.scr_base = static_cast<byte *>(back_surface->pixels); // reset to surface start
         }
         CPC.scr_pos = CPC.scr_base + dwOffset; // update current rendering position

         iExitCondition = z80_execute(); // run the emulation until an exit condition is met

         // Sample tape level into waveform ring buffer (sub-frame rate)
         if (CPC.tape_motor && CPC.tape_play_button) {
            imgui_state.tape_wave_buf[imgui_state.tape_wave_head] = bTapeLevel;
            imgui_state.tape_wave_head = (imgui_state.tape_wave_head + 1) % ImGuiUIState::TAPE_WAVE_SAMPLES;

         }

         if (iExitCondition == EC_BREAKPOINT) {
            if (z80.breakpoint_reached || z80.watchpoint_reached) {
              g_trace.dump_if_crash();
              if (g_exit_on_break) {
                cleanExit(1, false);
              }
              // This is a breakpoint from DevTools or symbol file
              imgui_state.show_devtools = true;
              CPC.paused = true;
              z80.step_in = 0;
              z80.step_out = 0;
              z80.step_out_addresses.clear();
            } else if (z80.step_in >= 2) {
              // Step In completed (one instruction) or Step Out completed (RET reached)
              CPC.paused = true;
              z80.step_in = 0;
              z80.step_out = 0;
              z80.step_out_addresses.clear();
            } else {
              // This is an old flavour breakpoint
              // We have to clear breakpoint to let the z80 emulator move on.
              z80.break_point = 0xffffffff; // clear break point
              z80.trace = 1; // make sure we'll be here to rearm break point at the next z80 instruction.

              if (breakPointsToSkipBeforeProceedingWithVirtualEvents>0) {
                breakPointsToSkipBeforeProceedingWithVirtualEvents--;
                LOG_DEBUG("Decremented breakpoint skip counter to " << breakPointsToSkipBeforeProceedingWithVirtualEvents);
              }
            }
         } else {
            if (z80.break_point == 0xffffffff) { // TODO(cpcitor) clean up 0xffffffff into a value like Z80_BREAKPOINT_NONE
               LOG_DEBUG("Rearming EC_BREAKPOINT.");
               z80.break_point = 0; // set break point for next time
            }
         }

         if (iExitCondition == EC_FRAME_COMPLETE) { // emulation finished rendering a complete frame?
            dwFrameCountOverall++;
            dwFrameCount++;

            // Check --exit-after condition
            if (g_exit_mode == EXIT_FRAMES && dwFrameCountOverall >= g_exit_target) {
               cleanExit(0, false);
            }
            if (g_exit_mode == EXIT_MS && (SDL_GetTicks() - g_exit_start_ticks) >= g_exit_target) {
               cleanExit(0, false);
            }

            // Check IPC VBL events
            ipc_check_vbl_events();

            // YM register recording: capture PSG state once per VBL
            if (g_ym_recorder.is_recording()) {
               g_ym_recorder.capture_frame(PSG.RegisterAY.Index);
            }

            // AVI video recording: capture frame once per VBL
            if (g_avi_recorder.is_recording()) {
               g_avi_recorder.capture_video_frame(
                  static_cast<const uint8_t*>(back_surface->pixels),
                  back_surface->w, back_surface->h, back_surface->pitch);
            }

            // Session recording: capture keyboard state per frame
            if (g_session.state() == SessionState::RECORDING) {
               // Record changed keyboard matrix bytes as key events
               static uint8_t prev_matrix[16] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
                                                   0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
               for (int row = 0; row < 16; row++) {
                  if (keyboard_matrix[row] != prev_matrix[row]) {
                     // Encode as row in high byte, value in low byte
                     uint16_t data = static_cast<uint16_t>((row << 8) | keyboard_matrix[row]);
                     g_session.record_event(SessionEventType::KEY_DOWN, data);
                     prev_matrix[row] = keyboard_matrix[row];
                  }
               }
               g_session.record_frame_sync();
            }

            // Session playback: replay events for this frame
            if (g_session.state() == SessionState::PLAYING) {
               SessionEvent evt;
               while (g_session.next_event(evt)) {
                  if (evt.type == SessionEventType::KEY_DOWN) {
                     int row = (evt.data >> 8) & 0x0F;
                     keyboard_matrix[row] = static_cast<byte>(evt.data & 0xFF);
                  }
               }
               if (!g_session.advance_frame()) {
                  // Recording finished, session goes back to IDLE
               }
            }

            // Auto-type: drain queue one action per frame
            if (g_autotype_queue.is_active()) {
               g_autotype_queue.tick([](uint16_t cpc_key, bool pressed) {
                  CPCScancode scancode = CPC.InputMapper->CPCscancodeFromCPCkey(static_cast<CPC_KEYS>(cpc_key));
                  // Direct matrix manipulation (same as ipc_apply_keypress)
                  if (static_cast<byte>(scancode) == 0xff) return;
                  if (pressed) {
                     keyboard_matrix[static_cast<byte>(scancode) >> 4] &= ~bit_values[static_cast<byte>(scancode) & 7];
                     if (scancode & MOD_CPC_SHIFT) {
                        keyboard_matrix[0x25 >> 4] &= ~bit_values[0x25 & 7];
                     } else {
                        keyboard_matrix[0x25 >> 4] |= bit_values[0x25 & 7];
                     }
                     if (scancode & MOD_CPC_CTRL) {
                        keyboard_matrix[0x27 >> 4] &= ~bit_values[0x27 & 7];
                     } else {
                        keyboard_matrix[0x27 >> 4] |= bit_values[0x27 & 7];
                     }
                  } else {
                     keyboard_matrix[static_cast<byte>(scancode) >> 4] |= bit_values[static_cast<byte>(scancode) & 7];
                     keyboard_matrix[0x25 >> 4] |= bit_values[0x25 & 7];
                     keyboard_matrix[0x27 >> 4] |= bit_values[0x27 & 7];
                  }
               });
            }

            // Handle IPC "step frame" — decrement remaining, pause when done
            if (g_ipc->frame_step_active.load()) {
               int remaining = g_ipc->frame_step_remaining.fetch_sub(1) - 1;
               if (remaining <= 0) {
                  cpc_pause();
                  g_ipc->notify_frame_step_done();
               }
            }

            if (!g_headless) {
               if (SDL_GetTicks() < osd_timing) {
                  print(static_cast<byte *>(back_surface->pixels) + CPC.scr_line_offs, osd_message.c_str(), true);
               }
               std::string fpsText;
               if (CPC.scr_fps) {
                  char chStr[15];
                  snprintf(chStr, sizeof(chStr), "%3dFPS %3d%%", static_cast<int>(dwFPS), static_cast<int>(dwFPS) * 100 / (1000 / static_cast<int>(FRAME_PERIOD_MS)));
                  fpsText = chStr;
               }
               imgui_state.topbar_fps = fpsText;
               imgui_state.drive_a_led = FDC.led && (FDC.command[1] & 1) == 0;
               imgui_state.drive_b_led = FDC.led && (FDC.command[1] & 1) == 1;
            }
            asic_draw_sprites();
            if (!g_headless) {
              video_display();
              video_take_pending_window_screenshot();
            }
            if (g_take_screenshot) {
              dumpScreen();
              g_take_screenshot = false;
            }
         }
      }
      else { // We are paused — still render ImGui UI overlay
         if (!g_headless) {
            video_display();
            video_take_pending_window_screenshot();
         }
         std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
      }
   }

   g_ipc->stop();
   return 0;
}
