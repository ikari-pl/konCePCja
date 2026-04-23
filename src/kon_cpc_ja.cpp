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
#include <cctype>
#include <climits>
#include <iostream>
#include <sstream>
#include <chrono>
#include <string>
#include <thread>
#include <filesystem>
#ifndef _MSC_VER
#include <unistd.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

#include "SDL3/SDL.h"

static inline Uint32 MapRGBSurface(SDL_Surface* surface, Uint8 r, Uint8 g, Uint8 b) {
  const SDL_PixelFormatDetails* fmt = SDL_GetPixelFormatDetails(surface->format);
  SDL_Palette* pal = SDL_GetSurfacePalette(surface);
  return SDL_MapRGB(fmt, pal, r, g, b);
}

#include "koncepcja.h"
#include "koncepcja_ipc_server.h"
#include "telnet_console.h"
#include "autotype.h"
#include "crtc.h"
#include "symfile.h"
#include "disk.h"
#include "tape.h"
#include "video.h"
#include "video_gpu.h"
#include "z80.h"
#include "configuration.h"
#include "memutils.h"
#include "stringutils.h"
#include "zip.h"
#include "keyboard.h"
#include "keyboard_manager.h"
#include "trace.h"
#include "wav_recorder.h"
#include "amdrum.h"
#include "smartwatch.h"
#include "amx_mouse.h"
#include "drive_sounds.h"
#include "symbiface.h"
#include "m4board.h"
#include "m4board_http.h"
#include "serial_interface.h"
#include "io_dispatch.h"
#include "ym_recorder.h"
#include "avi_recorder.h"
#include "macos_menu.h"
#include "cpc_machine.h"
#include "memory_bus.h"
#include "io_bus.h"

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
bool g_debug = false;
static bool g_exit_on_break = false;
static enum { EXIT_NONE, EXIT_FRAMES, EXIT_MS } g_exit_mode = EXIT_NONE;
static dword g_exit_target = 0;
static dword g_exit_start_ticks = 0;

// Autotype keyboard-scan synchronization:
// Set when the Z80 reads the keyboard matrix via PPI Port A (PSG reg 14).
// The autotype tick is gated on this flag so key changes happen between scan cycles.
static bool g_keyboard_scanned = false;
static int g_keyboard_scan_timeout = 0; // frames since last scan, for fallback
static const int kAutotypeScanTimeoutFrames = 10; // inject anyway after N frames without a scan

static int topbar_height_px = 24;

extern t_CPC CPC;
SDL_Joystick* joysticks[MAX_NB_JOYSTICKS];

// Emulation/render thread split (P1.2a)
// g_emu_paused: authoritative pause flag shared between threads.
// cpc_pause()/cpc_resume() keep CPC.paused and g_emu_paused in sync.
// Z80 thread reads g_emu_paused; render thread reads it for the paused-overlay path.
std::atomic<bool> g_emu_paused{false};
// Set true when main() wants the Z80 thread to exit cleanly.
static std::atomic<bool> g_z80_thread_quit{false};
// Exit code to use when the Z80 thread requests quit via SDL_EVENT_QUIT.
static std::atomic<int> g_z80_requested_exit_code{0};
// Handle for the Z80 emulation thread (non-headless mode only). Stored so
// doCleanUp() can join it instead of letting it run past global destruction.
static std::thread g_z80_thread;
// Protects the imgui_state stats fields written by the Z80 thread and read by
// the render thread (frame_time_avg_us, z80_time_avg_us, audio_*, etc.).
std::mutex g_imgui_stats_mutex;
// True when the Z80 thread is NOT inside z80_execute() (i.e. safe to touch Z80 state
// from another thread).  Starts true because the thread hasn't spawned yet.
std::atomic<bool> g_z80_quiescent{true};
// Frame handoff: Z80 signals after asic_draw_sprites(); render signals after Phase A.
FrameSignal g_frame_signal;

// High-resolution timing using SDL_GetPerformanceCounter (nanosecond-class)
static uint64_t perfFreq;          // SDL_GetPerformanceFrequency() — ticks per second
static uint64_t perfTicksOffset;   // frame period in perf-counter ticks
static uint64_t perfTicksTarget;   // next frame deadline in perf-counter ticks
static uint64_t perfTicksTargetFPS; // next 1-second FPS sample point
dword dwFPS, dwFrameCount;
dword dwXScale, dwYScale;


// Frame timing measurement (1-second reporting window).
// frameTimeAccum/z80TimeAccum/sleepTimeAccum are Z80-thread-only (no atomic needed).
// displayTimeAccum is written by the render thread and read+reset by the Z80 thread's
// FPS publisher — use atomic to avoid a data race.
static uint64_t frameTimeAccum = 0;
static uint64_t frameTimeMin = UINT64_MAX;
static uint64_t frameTimeMax = 0;
static std::atomic<uint64_t> displayTimeAccum{0};
static uint64_t sleepTimeAccum = 0;
static uint64_t z80TimeAccum = 0;
static uint32_t frameTimeSamples = 0;
static uint64_t lastFrameStart = 0;  // perf counter at EC_FRAME_COMPLETE (for frame-to-frame stats)

dword osd_timing;
std::string osd_message;

std::string lastSavedSnapshot;

dword dwBreakPoint, dwTrace, dwMF2ExitAddr;
dword dwMF2Flags = 0;
// Audio buffer: PSG writes samples here, pushed to SDL stream on EC_SOUND_BUFFER.
std::unique_ptr<byte[]> pbSndBuffer;      // PSG write buffer
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
std::atomic<byte> keyboard_matrix[16];

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

JoystickEmulation nextJoystickEmulation(JoystickEmulation current) {
  return static_cast<JoystickEmulation>((static_cast<int>(current)+1) % static_cast<int>(JoystickEmulation::Last));
}

std::string JoystickEmulationToString(JoystickEmulation value) {
  switch (value) {
    case JoystickEmulation::None:
      return "off";
    case JoystickEmulation::Keyboard:
      return "keyboard";
    case JoystickEmulation::Mouse:
      return "mouse";
    case JoystickEmulation::Last:
      return "<invalid joystick emulation: last>";
  }
  return "<invalid joystick emulation>";
}

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

// Phase 1: non-owning aggregate of core globals. Behavior is unchanged;
// this simply provides a structured view for future refactors.
CpcMachine g_machine{
  &CPC,
  &CRTC,
  &GateArray,
  &FDC,
  &PPI,
  &PSG,
  &VDU,
  &driveA,
  &driveB,
  &z80,
};

// Phase 2: non-owning bus views over existing banking / IO dispatch.
// Memory hot paths use g_memory_bus (wrapping membank_*); IO still goes via the same dispatch.
MemoryBus g_memory_bus{ membank_read, membank_write };
IoBus g_io_bus{};

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
      memory_set_read_bank(n, membank_config[GateArray.RAM_config & 7][n]);
      memory_set_write_bank(n, membank_config[GateArray.RAM_config & 7][n]);
   }
   if (!(GateArray.ROM_config & 0x04)) { // lower ROM is enabled?
      if (dwMF2Flags & MF2_ACTIVE) { // is the Multiface 2 paged in?
         // MF2 ROM (8K) at 0x0000-0x1FFF: read-only overlay
         // MF2 RAM (8K) at 0x2000-0x3FFF: read-write (intercepted in z80.cpp write_mem)
         // Writes to 0x0000-0x1FFF fall through to CPC RAM (membank_write unchanged)
         memory_set_read_bank(GateArray.lower_ROM_bank, pbMF2ROM);
      } else {
         memory_set_read_bank(GateArray.lower_ROM_bank, pbROMlo); // 'page in' lower ROM
      }
   }
   if (CPC.model > 2 && GateArray.registerPageOn) {
      memory_set_read_bank(1, pbRegisterPage);
      memory_set_write_bank(1, pbRegisterPage);
   }
   if (!(GateArray.ROM_config & 0x08)) { // upper/expansion ROM is enabled?
      memory_set_read_bank(3, pbExpansionROM); // 'page in' upper/expansion ROM
   }
}

void memory_set_read_bank(int slot, byte* ptr) {
  membank_read[slot] = ptr;
}

void memory_set_write_bank(int slot, byte* ptr) {
  membank_write[slot] = ptr;
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
                           ret_val = keyboard_matrix[CPC.keyboard_line & 0x0f].load(std::memory_order_relaxed); // read keyboard matrix node status
                        } else {
                           ret_val = PSG.RegisterAY.Index[14] & (keyboard_matrix[CPC.keyboard_line & 0x0f].load(std::memory_order_relaxed)); // return last value w/ logic AND of input
                        }
                        ret_val &= io_fire_kbd_read_hooks(CPC.keyboard_line & 0x0f);
                        g_keyboard_scanned = true; // signal autotype that firmware has scanned
                        g_keyboard_manager.notify_scanned(CPC.keyboard_line & 0x0f);
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
   ret_val = g_io_bus.in(port, ret_val);
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
               video_update_palette_entry(GateArray.pen, colours[colour].r, colours[colour].g, colours[colour].b);
               if (GateArray.pen < 2) {
                  byte r = (static_cast<dword>(colours[GateArray.ink_values[0]].r) + static_cast<dword>(colours[GateArray.ink_values[1]].r)) >> 1;
                  byte g = (static_cast<dword>(colours[GateArray.ink_values[0]].g) + static_cast<dword>(colours[GateArray.ink_values[1]].g)) >> 1;
                  byte b = (static_cast<dword>(colours[GateArray.ink_values[0]].b) + static_cast<dword>(colours[GateArray.ink_values[1]].b)) >> 1;
                  video_update_palette_entry(33, r, g, b); // update the mode 2 'anti-aliasing' colour
               }               // TODO: update pbRegisterPage
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
         memory_set_read_bank(3, pbExpansionROM); // 'page in' upper/expansion ROM
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
   g_io_bus.out(port, val);
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
   for (auto& row : keyboard_matrix) row.store(0xff, std::memory_order_relaxed); // clear CPC keyboard matrix
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
      memory_set_read_bank(n, membank_config[0][n]);
      memory_set_write_bank(n, membank_config[0][n]);
   }
   memory_set_read_bank(0, pbROMlo); // 'page in' lower ROM
   memory_set_read_bank(3, pbROMhi); // 'page in' upper ROM

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
   SDL_SetWindowRelativeMouseMode(mainSDLWindow, CPC.joystick_emulation == JoystickEmulation::Mouse);
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
              LOG_ERROR("Invalid ROM '" << romFilename << "': less than 128 bytes. Not a CPC ROM?");
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

            bool has_amsdos_header = false;
            if (checksum == ((pchRomData[0x43] << 8) + pchRomData[0x44])) { // if the checksum matches, we got us an AMSDOS header
               has_amsdos_header = true;
               if(fread(pchRomData, 128, 1, pfileObject) != 1) { // skip it
                 LOG_ERROR("Invalid ROM '" << romFilename << "': couldn't read the 128 bytes of the AMSDOS header. Not a CPC ROM?");
                 fclose(pfileObject);
                 return ERR_NOT_A_CPC_ROM;
               }
            }

            auto rom_file_size = file_size(fileno(pfileObject));
            int max_rom_size = has_amsdos_header ? 16384 + 128 : 16384;
            if (rom_file_size > max_rom_size) {
                 fclose(pfileObject);
                 LOG_ERROR("Invalid ROM '" << romFilename << "': total ROM size is greater than 16kB. Not a CPC ROM?");
                 return ERR_NOT_A_CPC_ROM;
            }
            if (!(pchRomData[0] & 0xfc)) { // is it a valid CPC ROM image (0 = foreground, 1 = background, 2 = extension)?
               if(fread(pchRomData+128, rom_file_size-128, 1, pfileObject) != 1) { // read the rest of the ROM file
                 fclose(pfileObject);
                 LOG_ERROR("Internal error: couldn't read the expected ROM size from " << romFilename);
                 return ERR_NOT_A_CPC_ROM;
               }
               memmap_ROM[iRomNum] = pchRomData; // update the ROM map
            } else if ((pchRomData[0] == 0x47) && (gradcheck==1)) { // Is it a Graduate CPM Accessory Rom? (ID="G")
            // Graduate Software Accessory Roms use a non standard format. Only the first byte is validated, and as long as
            // it's a "G" and terminated with a "$" it'll try to use it.
            // See https://www.cpcwiki.eu/index.php/Graduate_Software#Structure_of_a_utility_ROM for more details.
              if(fread(pchRomData+128, rom_file_size-128, 1, pfileObject) != 1) { // read the rest of the ROM file
                fclose(pfileObject);
                LOG_ERROR("Internal error: couldn't read the expected ROM size from " << romFilename);
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

   // Auto-load Serial Interface ROM if enabled and slot is free
   if (g_serial_interface.get_config().enabled) {
      g_si_rom.load(memmap_ROM, CPC.rom_path);
   }

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
   g_si_rom.unload(memmap_ROM); // free auto-loaded SI ROM before general cleanup
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



// ── Audio diagnostics ──
static uint64_t audio_last_push_tick = 0;   // perf counter of last push
static int audio_underrun_count = 0;        // underruns: queue was empty
static int audio_near_underrun_count = 0;   // near-underruns: queue < half buffer
static int audio_push_count = 0;            // successful pushes this reporting period
static double audio_queue_sum_bytes = 0;    // sum of queue depths (for average)
static int audio_queue_min_bytes = INT_MAX; // min queue depth this period
static uint64_t audio_push_interval_max = 0; // longest gap between pushes (perf ticks)

// Push completed audio buffer into SDL stream (called from main loop on EC_SOUND_BUFFER).
// SDL handles internal queuing and feeds the hardware at the correct rate.
static void audio_push_buffer(const byte* data, int len)
{
   if (!audio_stream || !CPC.snd_ready || len <= 0) return;

   uint64_t now = SDL_GetPerformanceCounter();

   // Measure queue depth BEFORE pushing.
   int queued = SDL_GetAudioStreamQueued(audio_stream);
   if (queued < 0) queued = 0; // SDL error — treat as empty

   // Detect underruns (skip if we have no previous push timestamp to
   // compute an interval from — e.g. the very first push after init).
   if (audio_last_push_tick > 0) {
      double interval_ms = static_cast<double>(now - audio_last_push_tick) * 1000.0 / perfFreq;
      if (queued == 0) {
         audio_underrun_count++;
         LOG_DEBUG("Audio UNDERRUN: queue empty, interval " << interval_ms << "ms");
      } else if (queued < len / 2) {
         // Below half a buffer — real danger of audible artifact
         audio_near_underrun_count++;
         LOG_DEBUG("Audio near-underrun: queue " << queued << "B (< " << len / 2
                   << "B), interval " << interval_ms << "ms");
      }
   }
   audio_queue_sum_bytes += queued;
   if (queued < audio_queue_min_bytes) audio_queue_min_bytes = queued;

   // Push to SDL — only update timing/count on success
   if (!SDL_PutAudioStreamData(audio_stream, data, len)) {
      LOG_DEBUG("Audio: SDL_PutAudioStreamData failed: " << SDL_GetError());
      return;
   }

   // Maintain minimum queue depth to absorb macOS compositor stalls
   // (observed up to ~70ms). Target: 4 frames = 80ms headroom.
   // Top-up with silence so the hardware never runs dry mid-stall.
   // Adds at most 80ms audio latency — acceptable for emulation.
   {
      // Use snd_buffersize (one full CPC frame) as the chunk unit, not the
      // current push size which can be a small partial frame at end-of-frame.
      const int frame_bytes = static_cast<int>(CPC.snd_buffersize);
      const int target_bytes = frame_bytes * 4; // ~80ms at 50Hz
      int queued_now = SDL_GetAudioStreamQueued(audio_stream);
      if (queued_now >= 0 && queued_now < target_bytes) {
         static std::vector<byte> silence_frame;
         if (static_cast<int>(silence_frame.size()) != frame_bytes)
            silence_frame.assign(static_cast<size_t>(frame_bytes), 0);
         int deficit = target_bytes - queued_now;
         while (deficit >= frame_bytes) {
            SDL_PutAudioStreamData(audio_stream, silence_frame.data(), frame_bytes);
            deficit -= frame_bytes;
         }
      }
   }

   // Measure push interval (after successful push)
   if (audio_last_push_tick > 0) {
      uint64_t interval = now - audio_last_push_tick;
      if (interval > audio_push_interval_max) audio_push_interval_max = interval;
   }
   audio_last_push_tick = now;
   audio_push_count++;
   if (g_wav_recorder.is_recording()) {
      g_wav_recorder.write_samples(data, static_cast<uint32_t>(len));
   }
   if (g_avi_recorder.is_recording()) {
      g_avi_recorder.capture_audio_samples(
         reinterpret_cast<const int16_t*>(data),
         static_cast<size_t>(len) / sizeof(int16_t));
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

   audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired, nullptr, nullptr);
   if (audio_stream == nullptr) {
      LOG_ERROR("Could not open audio: " << SDL_GetError());
      return 1;
   }

   LOG_VERBOSE("Audio: Desired: Freq: " << desired.freq << ", Format: " << desired.format << ", Channels: " << static_cast<int>(desired.channels) << ", Frames: " << sample_frames);

   CPC.snd_buffersize = sample_frames * SDL_AUDIO_FRAMESIZE(desired);
   pbSndBuffer = std::make_unique<byte[]>(CPC.snd_buffersize);
   pbSndBufferEnd = pbSndBuffer.get() + CPC.snd_buffersize;
   CPC.snd_bufferptr = pbSndBuffer.get();

   // Pre-buffer 2 silent buffers (~46ms at 44100Hz) so the SDL queue has
   // enough margin to absorb occasional compositor stalls (~69ms observed).
   // Done BEFORE resuming the device so SDL doesn't start draining immediately.
   // Adds ~46ms of audio latency — acceptable for an emulator.
   {
      std::vector<byte> silence(CPC.snd_buffersize, 0);
      for (int i = 0; i < 2; i++) {
         if (!SDL_PutAudioStreamData(audio_stream, silence.data(), static_cast<int>(CPC.snd_buffersize))) {
            LOG_ERROR("Audio: pre-buffer failed: " << SDL_GetError());
         }
      }
   }
   SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(audio_stream));
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
   g_emu_paused.store(true, std::memory_order_relaxed);
}

void cpc_resume()
{
   CPC.paused = false;
   g_emu_paused.store(false, std::memory_order_relaxed);
   lastFrameStart = 0; // reset so first frame after resume isn't measured as huge
   audio_resume();
}

void cpc_pause_and_wait()
{
   cpc_pause();
   // Spin until the Z80 thread has exited z80_execute() and entered its sleep loop.
   // g_z80_quiescent is set true by z80_thread_main before sleeping, false before
   // entering z80_execute().  In headless mode the Z80 runs on the calling thread,
   // so g_z80_quiescent stays true and we return immediately.
   while (!g_z80_quiescent.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
   }
}

void video_update_palette_entry(int index, uint8_t r, uint8_t g, uint8_t b) {
  if (index < 0 || index >= 34) return;
  if (!back_surface) return;
  const SDL_PixelFormatDetails* fmt = SDL_GetPixelFormatDetails(back_surface->format);
  SDL_Palette* pal = SDL_GetSurfacePalette(back_surface);
  GateArray.palette[index] = SDL_MapRGB(fmt, pal, r, g, b);

  unsigned int clamped = std::clamp(CPC.scr_oglscanlines, 0u, 100u);
  float factor = (100 - clamped) / 100.0f;
  GateArray.dark_palette[index] = SDL_MapRGB(fmt, pal,
      static_cast<uint8_t>(r * factor),
      static_cast<uint8_t>(g * factor),
      static_cast<uint8_t>(b * factor));
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
         colours[n].r = static_cast<Uint8>(red);
         colours[n].g = static_cast<Uint8>(green);
         colours[n].b = static_cast<Uint8>(blue);
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
         colours[n].g = static_cast<Uint8>(green);
         colours[n].b = static_cast<Uint8>(blue);
      }
   }

   vid_plugin->set_palette(colours);

   for (int n = 0; n < 17; n++) { // loop for all colours + border
      int i=GateArray.ink_values[n];
      video_update_palette_entry(n, colours[i].r, colours[i].g, colours[i].b);
   }

   return 0;
}



void video_set_style ()
{
   // Always render at native Mode 2 width (768px). dwXScale=2 selects full
   // ModeMap tables in crtc_init(). dwYScale controls scanline doubling only.
   dwXScale = 2;
   dwYScale = vid_plugin->half_pixels ? 1 : 2;
   CPC.dwYScale = dwYScale;

   if (CPC.model > 2) {
      CPC.scr_prerendernorm = prerender_normal_plus;
   } else {
      CPC.scr_prerendernorm = prerender_normal;
   }
   CPC.scr_prerenderbord = prerender_border;
   CPC.scr_prerendersync = prerender_sync;

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
   int original_scr_style = CPC.scr_style;
   vid_plugin=&video_plugin_list[CPC.scr_style];
   LOG_DEBUG("video_init: vid_plugin = " << vid_plugin->name)

   // Always init at scale=2 for best surface quality (768×540, doubled scanlines).
   // The actual display size is controlled by scr_scale via compute_scale.
   back_surface=vid_plugin->init(vid_plugin, 2, CPC.scr_window==0);

   if (!back_surface) {
      // OpenGL may be unavailable (e.g. Intel HD 3000 only exposes GL 1.1).
      // Try the SDL_Renderer backend which uses D3D11/Metal instead of OpenGL.
      LOG_ERROR("Could not set requested video mode: " << SDL_GetError()
                << " — trying SDL_Renderer fallback");
      for (size_t i = 0; i < video_plugin_list.size(); i++) {
         if (std::string(video_plugin_list[i].name).find("(SDL)") != std::string::npos) {
            vid_plugin = &video_plugin_list[i];
            LOG_INFO("Falling back to: " << vid_plugin->name);
            back_surface = vid_plugin->init(vid_plugin, 2, CPC.scr_window==0);
            if (back_surface) {
               CPC.scr_style = static_cast<int>(i);
               break;
            }
         }
      }
   }
   if (!back_surface) {
      // Last resort: headless rendering (no window).
      LOG_ERROR("SDL_Renderer fallback also failed — falling back to headless");
      static video_plugin hp = video_headless_plugin();
      vid_plugin = &hp;
      g_headless = true;
      back_surface = vid_plugin->init(vid_plugin, CPC.scr_scale, false);
      if (!back_surface) {
         CPC.scr_style = original_scr_style;
         LOG_ERROR("Headless fallback also failed. Aborting.");
         return ERR_VIDEO_SET_MODE;
      }
   }

   // Phase 4: the GPU device is initialised inside gpu_direct_init() (the
   // "Direct (GPU)" plugin) — only when that plugin is selected.  Other
   // plugins never touch the GPU device.  video_shutdown() still calls
   // video_gpu_shutdown() as a safety net (no-op when device is null).

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

   crtc_init();

   // Resize window to match user's chosen scale (init always creates at 2x)
   if (CPC.scr_scale > 0 && mainSDLWindow) {
      static const float sf[] = { 0.f, 1.f, 1.5f, 2.f, 3.f };
      if (CPC.scr_scale < sizeof(sf)/sizeof(sf[0])) {
         float f = sf[CPC.scr_scale];
         int new_w = static_cast<int>(CPC_RENDER_WIDTH * f);
         int new_h = CPC.scr_crt_aspect
                   ? static_cast<int>(new_w * 3.f / 4.f)
                   : static_cast<int>(CPC_VISIBLE_SCR_HEIGHT * f);
         new_h += video_get_topbar_height() + video_get_bottombar_height();
         SDL_SetWindowSize(mainSDLWindow, new_w, new_h);
      }
   }

      return 0;
}



void video_shutdown ()
{
   // Plugin close must run first so the GPU plugin can tear down ImGui
   // SDLGPU3 and other device-dependent state before the GPU device
   // itself is destroyed.  For non-GPU plugins the order is irrelevant
   // (video_gpu_shutdown is a no-op when the device was never created).
   vid_plugin->close();
   video_gpu_shutdown();   // safety net — idempotent no-op after plugin close
}



void video_display ()
{
   vid_plugin->flip(vid_plugin);
}

// Phase B: floating viewports + window swap. Call after pushing audio.
static void video_display_b()
{
   if (vid_plugin->flip_b)
      vid_plugin->flip_b(vid_plugin);
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
   perfFreq = SDL_GetPerformanceFrequency();
   // Frame period in perf-counter ticks: (FRAME_PERIOD_MS / 1000) * freq / speed_ratio
   double speed_ratio = CPC.speed / CPC_BASE_FREQUENCY_MHZ;
   perfTicksOffset = static_cast<uint64_t>((FRAME_PERIOD_MS / 1000.0) * perfFreq / speed_ratio);
   uint64_t now = SDL_GetPerformanceCounter();
   perfTicksTarget = now + perfTicksOffset;
   perfTicksTargetFPS = now + perfFreq; // 1 second from now
   LOG_VERBOSE("Timing: perfFreq=" << perfFreq << " perfTicksOffset=" << perfTicksOffset
               << " (" << (perfTicksOffset * 1000.0 / perfFreq) << "ms/frame)"
               << " speed_ratio=" << speed_ratio);
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
   CPC.frameskip = conf.getIntValue("system", "frameskip", 0) & 1;
   CPC.auto_pause = conf.getIntValue("system", "auto_pause", 1) & 1;
   CPC.boot_time = conf.getIntValue("system", "boot_time", 5);
   CPC.printer = conf.getIntValue("system", "printer", 0) & 1;
   CPC.mf2 = conf.getIntValue("system", "mf2", 0) & 1;
   CPC.keyboard = conf.getIntValue("system", "keyboard", 0);
   if (CPC.keyboard > MAX_ROM_MODS) {
      CPC.keyboard = 0;
   }
   
   int ksm = conf.getIntValue("system", "keyboard_support_mode", 0);
   if (ksm < 0 || ksm >= static_cast<int>(KeyboardSupportMode::Last)) {
       ksm = 0;
   }
   CPC.keyboard_support_mode = static_cast<KeyboardSupportMode>(ksm);

   {
      int joy_emu_val = conf.getIntValue("system", "joystick_emulation", 0);
      if (joy_emu_val < 0 || joy_emu_val >= static_cast<int>(JoystickEmulation::Last)) {
         LOG_WARNING("Invalid joystick_emulation value " << joy_emu_val << " in configuration. Defaulting to 'off'.");
         joy_emu_val = static_cast<int>(JoystickEmulation::None);
      }
      CPC.joystick_emulation = static_cast<JoystickEmulation>(joy_emu_val);
   }
   CPC.joysticks = conf.getIntValue("system", "joysticks", 1) & 1;
   CPC.joystick_menu_button = conf.getIntValue("system", "joystick_menu_button", 9) - 1;
   CPC.joystick_vkeyboard_button = conf.getIntValue("system", "joystick_vkeyboard_button", 10) - 1;
   CPC.resources_path = conf.getStringValue("system", "resources_path", appPath + "/resources");

   CPC.devtools_scale = conf.getIntValue("devtools", "scale", 1);
   CPC.devtools_max_stack_size = conf.getIntValue("devtools", "max_stack_size", 50);

   {
      int wl = conf.getIntValue("ui", "workspace_layout", 0);
      CPC.workspace_layout = (wl == 1) ? t_CPC::WorkspaceLayoutMode::Docked
                                       : t_CPC::WorkspaceLayoutMode::Classic;
      int ss = conf.getIntValue("ui", "cpc_screen_scale", 0);
      CPC.cpc_screen_scale = (ss >= 0 && ss <= 3)
          ? static_cast<t_CPC::ScreenScale>(ss)
          : t_CPC::ScreenScale::Fit;
   }

   CPC.scr_scale = conf.getIntValue("video", "scr_scale", 2);
   CPC.scr_preserve_aspect_ratio = conf.getIntValue("video", "scr_preserve_aspect_ratio", 1);
   CPC.scr_crt_aspect = conf.getIntValue("video", "scr_crt_aspect", 1);
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
   CPC.scr_scanlines = conf.getIntValue("video", "scr_scanlines", 0) & 1;
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
   g_m4board.rom_slot = conf.getIntValue("peripheral", "m4_rom_slot", 6);
   CPC.m4_http_port = conf.getIntValue("peripheral", "m4_http_port", 8080);
   CPC.m4_bind_ip = conf.getStringValue("peripheral", "m4_bind_ip", "127.0.0.1");
   // Load port mappings (m4_port_map_N = cpc_port:host_port:user_override)
   for (int i = 0; i < 16; i++) {
      char key[32];
      snprintf(key, sizeof(key), "m4_port_map_%d", i);
      std::string val = conf.getStringValue("peripheral", key, "");
      if (val.empty()) break;
      // Parse "cpc:host:override"
      uint16_t cpc_port = 0, host_port = 0;
      int user_override = 0;
      if (sscanf(val.c_str(), "%hu:%hu:%d", &cpc_port, &host_port, &user_override) >= 2) {
         g_m4_http.set_port_mapping(cpc_port, host_port, user_override != 0);
      }
   }
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

   // Serial Interface config
   {
      SerialConfig scfg;
      scfg.enabled = conf.getIntValue("peripheral", "serial_enabled", 0) != 0;
      scfg.backend_type = static_cast<SerialBackendType>(
         conf.getIntValue("peripheral", "serial_backend", 0));
      scfg.input_file = conf.getStringValue("peripheral", "serial_input_file", "");
      scfg.output_file = conf.getStringValue("peripheral", "serial_output_file", "");
      scfg.device_path = conf.getStringValue("peripheral", "serial_device", "");
      scfg.tcp_host = conf.getStringValue("peripheral", "serial_tcp_host", "127.0.0.1");
      scfg.tcp_port = static_cast<uint16_t>(
         conf.getIntValue("peripheral", "serial_tcp_port", 23));
      scfg.baud_rate = conf.getIntValue("peripheral", "serial_baud", 9600);
      g_serial_interface.set_config(scfg);
      g_serial_interface.apply_config();
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

   // Load MRU (recent files) lists
   for (int i = 0; i < t_CPC::MRU_MAX; i++) {
      char key[16];
      snprintf(key, sizeof(key), "mru_disk_%02d", i);
      std::string val = conf.getStringValue("file", key, "");
      if (!val.empty()) CPC.mru_disks.push_back(val);
      snprintf(key, sizeof(key), "mru_tape_%02d", i);
      val = conf.getStringValue("file", key, "");
      if (!val.empty()) CPC.mru_tapes.push_back(val);
      snprintf(key, sizeof(key), "mru_snap_%02d", i);
      val = conf.getStringValue("file", key, "");
      if (!val.empty()) CPC.mru_snaps.push_back(val);
      snprintf(key, sizeof(key), "mru_cart_%02d", i);
      val = conf.getStringValue("file", key, "");
      if (!val.empty()) CPC.mru_carts.push_back(val);
   }

   CPC.cartridge.file = CPC.rom_path + "/system.cpr"; // Only default path defined. Needed for CPC6128+
}



bool saveConfiguration (t_CPC &CPC, const std::string& configFilename)
{
   config::Config conf;

   conf.setIntValue("system", "model", CPC.model);
   conf.setIntValue("system", "jumpers", CPC.jumpers);

   conf.setIntValue("system", "ram_size", CPC.ram_size); // 128KB RAM
   conf.setIntValue("system", "limit_speed", CPC.limit_speed);
   conf.setIntValue("system", "frameskip", CPC.frameskip);
   conf.setIntValue("system", "speed", CPC.speed); // original CPC speed
   conf.setIntValue("system", "auto_pause", CPC.auto_pause);
   conf.setIntValue("system", "printer", CPC.printer);
   conf.setIntValue("system", "mf2", CPC.mf2);
   conf.setIntValue("system", "keyboard", CPC.keyboard);
   conf.setIntValue("system", "keyboard_support_mode", static_cast<int>(CPC.keyboard_support_mode));
   conf.setIntValue("system", "boot_time", CPC.boot_time);
   conf.setIntValue("system", "joystick_emulation", static_cast<int>(CPC.joystick_emulation));
   conf.setIntValue("system", "joysticks", CPC.joysticks);
   conf.setIntValue("system", "joystick_menu_button", CPC.joystick_menu_button + 1);
   conf.setIntValue("system", "joystick_vkeyboard_button", CPC.joystick_vkeyboard_button + 1);
   conf.setStringValue("system", "resources_path", CPC.resources_path);

   conf.setIntValue("video", "scr_scale", CPC.scr_scale);
   conf.setIntValue("video", "scr_preserve_aspect_ratio", CPC.scr_preserve_aspect_ratio);
   conf.setIntValue("video", "scr_crt_aspect", CPC.scr_crt_aspect);
   conf.setIntValue("video", "scr_style", CPC.scr_style);
   conf.setIntValue("video", "scr_oglfilter", CPC.scr_oglfilter);
   conf.setIntValue("video", "scr_oglscanlines", CPC.scr_oglscanlines);
   conf.setIntValue("video", "scr_scanlines", CPC.scr_scanlines);
   conf.setIntValue("video", "scr_led", CPC.scr_led);
   conf.setIntValue("video", "scr_fps", CPC.scr_fps);
   conf.setIntValue("video", "scr_tube", CPC.scr_tube);
   conf.setIntValue("video", "scr_intensity", CPC.scr_intensity);
   conf.setIntValue("video", "scr_remanency", CPC.scr_remanency);
   conf.setIntValue("video", "scr_window", CPC.scr_window);

   conf.setIntValue("devtools", "scale", CPC.devtools_scale);

   conf.setIntValue("ui", "workspace_layout", static_cast<int>(CPC.workspace_layout));
   conf.setIntValue("ui", "cpc_screen_scale", static_cast<int>(CPC.cpc_screen_scale));

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
   conf.setIntValue("peripheral", "m4_http_port", CPC.m4_http_port);
   conf.setStringValue("peripheral", "m4_bind_ip", CPC.m4_bind_ip);

   // Serial Interface config
   {
      auto cfg = g_serial_interface.get_config();
      conf.setIntValue("peripheral", "serial_enabled", cfg.enabled ? 1 : 0);
      conf.setIntValue("peripheral", "serial_backend", static_cast<int>(cfg.backend_type));
      conf.setStringValue("peripheral", "serial_input_file", cfg.input_file);
      conf.setStringValue("peripheral", "serial_output_file", cfg.output_file);
      conf.setStringValue("peripheral", "serial_device", cfg.device_path);
      conf.setStringValue("peripheral", "serial_tcp_host", cfg.tcp_host);
      conf.setIntValue("peripheral", "serial_tcp_port", cfg.tcp_port);
      conf.setIntValue("peripheral", "serial_baud", cfg.baud_rate);
   }

   // Save port mappings and clear stale keys
   {
      auto mappings = g_m4_http.get_port_mappings_snapshot();
      size_t count = mappings.size() < 16 ? mappings.size() : 16;
      for (size_t i = 0; i < count; i++) {
         char key[48], val[64];
         snprintf(key, sizeof(key), "m4_port_map_%zu", i);
         snprintf(val, sizeof(val), "%d:%d:%d",
                  mappings[i].cpc_port, mappings[i].host_port,
                  mappings[i].user_override ? 1 : 0);
         conf.setStringValue("peripheral", key, val);
      }
      // Clear remaining keys up to 15 to remove stale entries
      for (size_t i = count; i < 16; i++) {
         char key[32];
         snprintf(key, sizeof(key), "m4_port_map_%zu", i);
         conf.setStringValue("peripheral", key, "");
      }
   }

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

   // Save MRU (recent files) lists
   for (int i = 0; i < t_CPC::MRU_MAX; i++) {
      char key[16];
      snprintf(key, sizeof(key), "mru_disk_%02d", i);
      conf.setStringValue("file", key, i < (int)CPC.mru_disks.size() ? CPC.mru_disks[i] : "");
      snprintf(key, sizeof(key), "mru_tape_%02d", i);
      conf.setStringValue("file", key, i < (int)CPC.mru_tapes.size() ? CPC.mru_tapes[i] : "");
      snprintf(key, sizeof(key), "mru_snap_%02d", i);
      conf.setStringValue("file", key, i < (int)CPC.mru_snaps.size() ? CPC.mru_snaps[i] : "");
      snprintf(key, sizeof(key), "mru_cart_%02d", i);
      conf.setStringValue("file", key, i < (int)CPC.mru_carts.size() ? CPC.mru_carts[i] : "");
   }

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
         CPC.joystick_emulation = nextJoystickEmulation(CPC.joystick_emulation);
         CPC.InputMapper->set_joystick_emulation();
         SDL_SetWindowRelativeMouseMode(mainSDLWindow, CPC.joystick_emulation == JoystickEmulation::Mouse);
         set_osd_message(std::string("Joystick emulation: ") + JoystickEmulationToString(CPC.joystick_emulation));
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
   cpc_pause(); // keep CPC.paused and g_emu_paused in sync
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
#ifdef _WIN32
   timeEndPeriod(1);
#endif
   // Shutdown ordering — three constraints that together force this dance:
   //
   //  1. Z80 thread reads pbRAM/pbROM/MF2ROM and disk buffers from inside
   //     z80_execute() and z80_OUT_handler.  Freeing those before the thread
   //     stops causes a segfault (KERN_INVALID_ADDRESS in z80_OUT_handler).
   //  2. Z80 thread writes pfoPrinter from the printer-port hook until its
   //     very last instruction.  Closing the file before the thread stops
   //     truncates final printer output (style 11 regression).
   //  3. Setting g_z80_thread_quit alone is not enough: the Z80 may be deep
   //     inside z80_execute() and won't observe the flag until it returns
   //     at the next frame/sound boundary.  On macOS CI this can take long
   //     enough to hit the 15-minute job timeout.
   //
   // Solution: pause-and-wait FIRST, but ONLY if the quit flag isn't already
   // set.  When KONCPC_EXIT is processed inside the Z80 thread, cleanExit()
   // sets g_z80_thread_quit and pushes SDL_EVENT_QUIT before returning; by
   // the time the render thread reaches doCleanUp(), the Z80 has typically
   // already exited its loop.  In that case cpc_pause_and_wait() would block
   // forever (it spins until g_z80_quiescent goes true, which the now-dead
   // Z80 thread will never set).  Skip it and join directly.
   //
   // For the "render thread initiated quit" path (e.g. SDL_QUIT from the
   // window close button or F10 menu), the Z80 is still actively running
   // inside z80_execute() and we DO need pause+quiescence before join.
   //
   //  4. Plain cpc_pause_and_wait() is NOT sufficient: the Z80 thread sets
   //     g_z80_quiescent=false before z80_execute() and only re-enters the
   //     paused/quiescent branch at the top of its loop.  After EC_FRAME_COMPLETE
   //     it calls signal_ready() then blocks in wait_consumed() — it never
   //     observes g_emu_paused there.  If SDL_QUIT is processed while the Z80
   //     is in wait_consumed(), pause_and_wait would spin forever.  Abort the
   //     frame handshake after cpc_pause() so wait_consumed() returns and the
   //     Z80 thread can reach the quiescent paused branch.
   if (g_z80_thread.joinable() &&
       std::this_thread::get_id() != g_z80_thread.get_id()) {
      if (!g_z80_thread_quit.load(std::memory_order_relaxed)) {
         cpc_pause();
         g_frame_signal.abort(); // unblock Z80 if stuck in wait_consumed()
         while (!g_z80_quiescent.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
         }
         g_z80_thread_quit.store(true, std::memory_order_relaxed);
         cpc_resume();
      }
      g_frame_signal.abort(); // belt-and-suspenders for any pending wait_consumed
      g_z80_thread.join();
   } else if (g_z80_thread.joinable()) {
      // Self-join from the Z80 thread itself — can't pause-wait, just signal
      // and detach; _exit() in the caller handles the rest.
      g_z80_thread_quit.store(true, std::memory_order_relaxed);
      g_frame_signal.abort();
      g_z80_thread.detach();
   }

   printer_stop();
   emulator_shutdown();
   dsk_eject(&driveA);
   dsk_eject(&driveB);
   tape_eject();

   g_m4_http.stop();
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

   // If we are on the Z80 thread, we must not call doCleanUp() directly: the
   // render (main) thread may be concurrently inside video_display_b() using
   // SDL resources.  Instead, signal the Z80 loop to exit and push SDL_EVENT_QUIT
   // so the render thread handles orderly teardown when it is next safe to do so.
   if (g_z80_thread.joinable() &&
       std::this_thread::get_id() == g_z80_thread.get_id()) {
      g_z80_requested_exit_code.store(returnCode, std::memory_order_relaxed);
      g_z80_thread_quit.store(true, std::memory_order_relaxed);
      SDL_Event qe = {};
      qe.type = SDL_EVENT_QUIT;
      SDL_PushEvent(&qe);
      return; // Z80 thread loop exits on next iteration via g_z80_thread_quit
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

static void handle_mouse_joystick_button(const SDL_MouseButtonEvent& event, std::atomic<byte> keyboard_matrix[], bool pressed) {
   if (CPC.joystick_emulation == JoystickEmulation::Mouse) {
      if (event.button == 1)
         applyKeypress(CPC.InputMapper->CPCscancodeFromCPCkey(CPC_J0_FIRE1), keyboard_matrix, pressed);
      if (event.button == 3)
         applyKeypress(CPC.InputMapper->CPCscancodeFromCPCkey(CPC_J0_FIRE2), keyboard_matrix, pressed);
   }
}

// Z80 emulation thread — runs z80_execute() and handles all emulation side effects.
// Used only in non-headless (GUI) mode; headless runs the original single-threaded path.
//
// At EC_FRAME_COMPLETE:
//   1. Completes per-frame work (autotype, session, IPC, etc.)
//   2. Calls asic_draw_sprites() — finalises back_surface pixels
//   3. Calls g_frame_signal.signal_ready() — hands back_surface to render thread
//   4. Blocks in g_frame_signal.wait_consumed() while render does Phase A (~3ms)
//   5. Immediately starts the next frame on return — concurrent with render's Phase B
static void z80_thread_main()
{
   dword iExitCondition = EC_FRAME_COMPLETE;
   static int consecutive_skips = 0;

   while (!g_z80_thread_quit.load(std::memory_order_relaxed)) {
      if (g_emu_paused.load(std::memory_order_relaxed)) {
         // Mark quiescent so cpc_pause_and_wait() callers know we are safe to inspect.
         g_z80_quiescent.store(true, std::memory_order_release);
         std::this_thread::sleep_for(std::chrono::milliseconds(1));
         continue;
      }
      // About to enter z80_execute() — mark non-quiescent.
      g_z80_quiescent.store(false, std::memory_order_release);

      // FPS counter: publish stats once per second
      {
         uint64_t perfNow = SDL_GetPerformanceCounter();
         if (perfNow >= perfTicksTargetFPS) {
            dwFPS = dwFrameCount;
            dwFrameCount = 0;
            perfTicksTargetFPS = perfNow + perfFreq;

            {
               std::lock_guard<std::mutex> stats_lock(g_imgui_stats_mutex);
               if (frameTimeSamples > 0) {
                  double ticksToUs = 1000000.0 / static_cast<double>(perfFreq);
                  imgui_state.frame_time_avg_us = static_cast<float>(static_cast<double>(frameTimeAccum) / frameTimeSamples * ticksToUs);
                  imgui_state.frame_time_min_us = static_cast<float>(static_cast<double>(frameTimeMin) * ticksToUs);
                  imgui_state.frame_time_max_us = static_cast<float>(static_cast<double>(frameTimeMax) * ticksToUs);
                  imgui_state.display_time_avg_us = static_cast<float>(static_cast<double>(displayTimeAccum.exchange(0, std::memory_order_relaxed)) / frameTimeSamples * ticksToUs);
                  imgui_state.sleep_time_avg_us = static_cast<float>(static_cast<double>(sleepTimeAccum) / frameTimeSamples * ticksToUs);
                  imgui_state.z80_time_avg_us = static_cast<float>(static_cast<double>(z80TimeAccum) / frameTimeSamples * ticksToUs);
               }
               frameTimeAccum = 0;
               frameTimeMin = UINT64_MAX;
               frameTimeMax = 0;
               sleepTimeAccum = 0;
               z80TimeAccum = 0;
               frameTimeSamples = 0;

               imgui_state.audio_underruns = audio_underrun_count;
               imgui_state.audio_near_underruns = audio_near_underrun_count;
               imgui_state.audio_pushes = audio_push_count;
               if (audio_push_count == 0) {
                  imgui_state.audio_queue_avg_ms = 0;
                  imgui_state.audio_queue_min_ms = 0;
                  imgui_state.audio_push_interval_max_us = 0;
               } else {
                  double avg_bytes = audio_queue_sum_bytes / audio_push_count;
                  int frame_size = CPC.snd_stereo ? 4 : 2;
                  if (CPC.snd_bits == 0) frame_size /= 2;
                  int sample_rate = freq_table[CPC.snd_playback_rate];
                  double bytes_per_ms = sample_rate * frame_size / 1000.0;
                  imgui_state.audio_queue_avg_ms = static_cast<float>(avg_bytes / bytes_per_ms);
                  imgui_state.audio_queue_min_ms = static_cast<float>(audio_queue_min_bytes / bytes_per_ms);
                  imgui_state.audio_push_interval_max_us = static_cast<float>(
                     static_cast<double>(audio_push_interval_max) * 1000000.0 / perfFreq);
               }
               audio_underrun_count = 0;
               audio_near_underrun_count = 0;
               audio_push_count = 0;
               audio_queue_sum_bytes = 0;
               audio_queue_min_bytes = INT_MAX;
               audio_push_interval_max = 0;
            } // g_imgui_stats_mutex
         }
      }

      // Speed limiter: spin/sleep until deadline on audio-driven cycle boundaries
      static constexpr int MAX_CONSECUTIVE_SKIPS = 5;
      if (CPC.limit_speed && iExitCondition == EC_CYCLE_COUNT) {
         uint64_t sleepStart = SDL_GetPerformanceCounter();
         if (sleepStart < perfTicksTarget) {
            uint64_t remaining_ticks = perfTicksTarget - sleepStart;
            uint64_t remaining_ms = (remaining_ticks * 1000) / perfFreq;
            if (remaining_ms > 2) {
               SDL_Delay(static_cast<Uint32>(remaining_ms - 2));
            }
            while (SDL_GetPerformanceCounter() < perfTicksTarget) { SDL_Delay(0); }
         }
         sleepTimeAccum += SDL_GetPerformanceCounter() - sleepStart;
         perfTicksTarget += perfTicksOffset;
         uint64_t now = SDL_GetPerformanceCounter();
         if (!CPC.frameskip && perfTicksTarget + 3 * perfTicksOffset < now) {
            perfTicksTarget = now + perfTicksOffset;
         }
      } else if (iExitCondition != EC_CYCLE_COUNT) {
         CPC.skip_rendering = false;
         consecutive_skips = 0;
      }

      // Frameskip decision at frame boundaries
      if (iExitCondition == EC_FRAME_COMPLETE && CPC.limit_speed) {
         uint64_t now = SDL_GetPerformanceCounter();
         if (CPC.frameskip && now > perfTicksTarget) {
            if (consecutive_skips < MAX_CONSECUTIVE_SKIPS) {
               CPC.skip_rendering = true;
               consecutive_skips++;
            } else {
               CPC.skip_rendering = false;
               consecutive_skips = 0;
               perfTicksTarget = now + perfTicksOffset;
            }
         } else {
            CPC.skip_rendering = false;
            consecutive_skips = 0;
         }
      } else if (iExitCondition == EC_FRAME_COMPLETE && !CPC.limit_speed) {
         CPC.skip_rendering = false;
         consecutive_skips = 0;
      }

      // Update screen buffer base pointer for this frame's scanline
      {
         dword dwOffset = CPC.scr_pos - CPC.scr_base;
         if (VDU.scrln > 0) {
            CPC.scr_base = static_cast<byte *>(back_surface->pixels) + (VDU.scrln * CPC.scr_line_offs);
         } else {
            CPC.scr_base = static_cast<byte *>(back_surface->pixels);
         }
         CPC.scr_pos = CPC.scr_base + dwOffset;
      }

      {
         uint64_t z80Start = SDL_GetPerformanceCounter();
         iExitCondition = z80_execute();
         z80TimeAccum += SDL_GetPerformanceCounter() - z80Start;
      }

      // Tape wave sample (sub-frame resolution, render thread reads this under condvar)
      if (CPC.tape_motor && CPC.tape_play_button) {
         imgui_state.tape_wave_buf[imgui_state.tape_wave_head] = bTapeLevel;
         imgui_state.tape_wave_head = (imgui_state.tape_wave_head + 1) % ImGuiUIState::TAPE_WAVE_SAMPLES;
      }

      // Audio: PSG filled buffer — push to SDL
      if (iExitCondition == EC_SOUND_BUFFER) {
         if (!g_emu_paused.load(std::memory_order_relaxed)) {
            audio_push_buffer(pbSndBuffer.get(), static_cast<int>(CPC.snd_buffersize));
         }
         CPC.snd_bufferptr = pbSndBuffer.get();
      }

      // Breakpoint / step
      if (iExitCondition == EC_BREAKPOINT) {
         if (z80.breakpoint_reached || z80.watchpoint_reached) {
            g_trace.dump_if_crash();
            if (g_exit_on_break) {
               cleanExit(1, false);
            }
            imgui_state.show_devtools = true;
            cpc_pause();
            // Mid-frame pause: the render thread may be waiting in wait_ready() for a
            // frame that will never arrive (we stopped before EC_FRAME_COMPLETE).
            // Send a skip signal so it unblocks, calls signal_consumed(), then sees
            // g_emu_paused=true and shows the paused overlay on its next iteration.
            g_frame_signal.signal_ready(true);
            z80.step_in = 0;
            z80.step_out = 0;
            z80.step_out_addresses.clear();
         } else if (z80.step_in >= 2) {
            cpc_pause();
            g_frame_signal.signal_ready(true); // same: unblock render thread
            z80.step_in = 0;
            z80.step_out = 0;
            z80.step_out_addresses.clear();
         } else {
            z80.break_point = 0xffffffff;
            z80.trace = 1;
            if (breakPointsToSkipBeforeProceedingWithVirtualEvents > 0) {
               breakPointsToSkipBeforeProceedingWithVirtualEvents--;
               LOG_DEBUG("Decremented breakpoint skip counter to " << breakPointsToSkipBeforeProceedingWithVirtualEvents);
            }
         }
      } else {
         if (z80.break_point == 0xffffffff) {
            LOG_DEBUG("Rearming EC_BREAKPOINT.");
            z80.break_point = 0;
         }
      }

      if (iExitCondition == EC_FRAME_COMPLETE) {
         dwFrameCountOverall++;
         dwFrameCount++;

         g_keyboard_manager.update(keyboard_matrix, dwFrameCountOverall);

         // Frame-to-frame timing
         {
            uint64_t now = SDL_GetPerformanceCounter();
            if (lastFrameStart > 0) {
               uint64_t elapsed = now - lastFrameStart;
               frameTimeAccum += elapsed;
               if (elapsed < frameTimeMin) frameTimeMin = elapsed;
               if (elapsed > frameTimeMax) frameTimeMax = elapsed;
               frameTimeSamples++;
            }
            lastFrameStart = now;
         }

         // Exit-after checks (--exit-after N frames or N ms)
         if (g_exit_mode == EXIT_FRAMES && dwFrameCountOverall >= g_exit_target) {
            cleanExit(0, false);
         }
         if (g_exit_mode == EXIT_MS && (SDL_GetTicks() - g_exit_start_ticks) >= g_exit_target) {
            cleanExit(0, false);
         }

         ipc_check_vbl_events();

         if (g_m4board.activity_frames > 0) g_m4board.activity_frames--;

         if (g_ym_recorder.is_recording()) {
            g_ym_recorder.capture_frame(PSG.RegisterAY.Index);
         }

         // AVI video capture — reads back_surface; must be before signal_ready()
         if (!CPC.skip_rendering && g_avi_recorder.is_recording()) {
            g_avi_recorder.capture_video_frame(
               static_cast<const uint8_t*>(back_surface->pixels),
               back_surface->w, back_surface->h, back_surface->pitch);
         }

         // Session recording: keyboard snapshot per frame
         if (g_session.state() == SessionState::RECORDING) {
            static uint8_t prev_matrix[16] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
                                               0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
            for (int row = 0; row < 16; row++) {
               byte cur = keyboard_matrix[row].load(std::memory_order_relaxed);
               if (cur != prev_matrix[row]) {
                  g_session.record_event(SessionEventType::KEY_DOWN,
                     static_cast<uint16_t>((row << 8) | cur));
                  prev_matrix[row] = cur;
               }
            }
            g_session.record_frame_sync();
         }

         // Session playback: replay frame events
         if (g_session.state() == SessionState::PLAYING) {
            SessionEvent evt;
            while (g_session.next_event(evt)) {
               if (evt.type == SessionEventType::KEY_DOWN) {
                  int row = (evt.data >> 8) & 0x0F;
                  keyboard_matrix[row].store(static_cast<byte>(evt.data & 0xFF),
                     std::memory_order_relaxed);
               }
            }
            if (!g_session.advance_frame()) { /* recording finished */ }
         }

         // Auto-type: drain queue, synchronized with CPC keyboard scans
         if (g_autotype_queue.is_active()) {
            bool do_tick = false;
            if (g_keyboard_scanned) {
               g_keyboard_scanned = false;
               g_keyboard_scan_timeout = 0;
               do_tick = true;
            } else if (++g_keyboard_scan_timeout >= kAutotypeScanTimeoutFrames) {
               g_keyboard_scan_timeout = 0;
               do_tick = true;
            }
            if (do_tick) {
               g_autotype_queue.tick([](uint16_t cpc_key, bool pressed) {
                  CPCScancode scancode = CPC.InputMapper->CPCscancodeFromCPCkey(
                     static_cast<CPC_KEYS>(cpc_key));
                  if (static_cast<byte>(scancode) == 0xff) return;
                  if (pressed) {
                     keyboard_matrix[static_cast<byte>(scancode) >> 4].fetch_and(
                        ~bit_values[static_cast<byte>(scancode) & 7], std::memory_order_relaxed);
                     if (scancode & MOD_CPC_SHIFT)
                        keyboard_matrix[0x25 >> 4].fetch_and(~bit_values[0x25 & 7], std::memory_order_relaxed);
                     else
                        keyboard_matrix[0x25 >> 4].fetch_or(bit_values[0x25 & 7], std::memory_order_relaxed);
                     if (scancode & MOD_CPC_CTRL)
                        keyboard_matrix[0x27 >> 4].fetch_and(~bit_values[0x27 & 7], std::memory_order_relaxed);
                     else
                        keyboard_matrix[0x27 >> 4].fetch_or(bit_values[0x27 & 7], std::memory_order_relaxed);
                  } else {
                     keyboard_matrix[static_cast<byte>(scancode) >> 4].fetch_or(
                        bit_values[static_cast<byte>(scancode) & 7], std::memory_order_relaxed);
                     keyboard_matrix[0x25 >> 4].fetch_or(bit_values[0x25 & 7], std::memory_order_relaxed);
                     keyboard_matrix[0x27 >> 4].fetch_or(bit_values[0x27 & 7], std::memory_order_relaxed);
                  }
               });
            }
         }

         g_telnet.drain_input();

         // IPC frame step
         if (g_ipc->frame_step_active.load()) {
            int remaining = g_ipc->frame_step_remaining.fetch_sub(1) - 1;
            if (remaining <= 0) {
               cpc_pause();
               g_ipc->notify_frame_step_done();
            }
         }

         // Drive LED state and FPS text — written before signal_ready() so the condvar's
         // happens-before ensures render thread sees them after wait_ready() returns.
         imgui_state.drive_a_led = FDC.led && (FDC.command[1] & 1) == 0;
         imgui_state.drive_b_led = FDC.led && (FDC.command[1] & 1) == 1;
         if (CPC.scr_fps) {
            char chStr[15];
            snprintf(chStr, sizeof(chStr), "%3dFPS %3d%%",
               static_cast<int>(dwFPS),
               static_cast<int>(dwFPS) * 100 / static_cast<int>(1000.0 / FRAME_PERIOD_MS));
            imgui_state.topbar_fps = chStr;
         } else {
            imgui_state.topbar_fps.clear();
         }

         // Finalise back_surface (ASIC sprites must be drawn before handoff to render)
         if (!CPC.skip_rendering) {
            asic_draw_sprites();
         }

         // Hand back_surface to render thread; block until Phase A (texture upload) done.
         // Phase B (SDL_GL_SwapWindow, 0-60ms) runs concurrently with the next Z80 frame.
         g_frame_signal.signal_ready(CPC.skip_rendering);
         g_frame_signal.wait_consumed();
      }
   }
}

int koncpc_main (int argc, char **argv)
{
#ifdef _WIN32
   // Set Windows timer resolution to 1ms for accurate SDL_Delay() in the speed limiter.
   // Without this, SDL_Delay(1) actually sleeps ~15.6ms (default 64Hz timer).
   struct Win32TimerGuard {
      Win32TimerGuard()  { timeBeginPeriod(1); }
      ~Win32TimerGuard() { timeEndPeriod(1);   }
   } win32TimerGuard;
#endif
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
   g_debug = args.debug;
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

   // Telnet console — CPC text mirror on IPC+1
   g_telnet.start();

   // M4 Board HTTP server — started later after config is loaded (see below)

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
      back_surface = vid_plugin->init(vid_plugin, 2, false);
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
      // Set the Dock icon (fixes generic icon when running outside .app bundle).
      // koncepcja-icon.png serves as both the icon and CRT overlay — its screen
      // area is translucent (~alpha 40) so the live CPC screen shows through.
      {
         std::string icon_path = CPC.resources_path + "/koncepcja-icon.png";
         koncpc_set_dock_icon(icon_path.c_str());
      }
#endif
      topbar_height_px = imgui_topbar_height();
      video_set_topbar(nullptr, topbar_height_px);
      // video_set_topbar handles the window resize using compute_window_size()
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

   // M4 Board HTTP server — start after config is loaded and M4 is initialized
   if (g_m4board.enabled && !g_m4board.sd_root_path.empty()) {
      g_m4_http.start(CPC.m4_http_port, CPC.m4_bind_ip);
   }

   // Fill the buffer with autocmd if provided.
   // Two paths:
   // - If the string contains ~KEY~ syntax (tilde-delimited), use AutoTypeQueue
   //   which parses ~ENTER~, ~PAUSE 50~, etc.
   // - Otherwise, use StringToEvents which handles \a (CPC keys) and \f (emulator
   //   commands like KONCPC_WAITBREAK/KONCPC_EXIT) from replaceKoncpcKeys().
   if (!args.autocmd.empty()) {
      if (args.autocmd.find('~') != std::string::npos) {
         std::string cmd = "~PAUSE " + std::to_string(CPC.boot_time) + "~" + args.autocmd;
         auto err = g_autotype_queue.enqueue(cmd);
         if (!err.empty()) {
            LOG_ERROR("--autocmd parse error: " << err);
         }
      } else {
         virtualKeyboardEvents = CPC.InputMapper->StringToEvents(args.autocmd);
         nextVirtualEventFrameCount = dwFrameCountOverall + CPC.boot_time;
      }
   }

// ----------------------------------------------------------------------------

   update_timings();
   if (!g_headless) audio_resume();

   loadBreakpoints();

   g_exit_start_ticks = SDL_GetTicks();
   iExitCondition = EC_FRAME_COMPLETE;

   // Spawn Z80 emulation thread for non-headless mode.
   // The render loop (below) becomes render-only; the Z80 thread signals each
   // completed frame via g_frame_signal so Phase A/B can overlap with the next frame.
   if (!g_headless) {
      g_z80_thread = std::thread(z80_thread_main);
   }

   dword nextMouseReset = 0;
   // Whether this loop of emulation should release the joystick axis for mouse emulation.
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

      // Mouse-as-joystick: release all joystick axes periodically so they don't stick
      if (dwFrameCountOverall >= nextMouseReset && CPC.joystick_emulation == JoystickEmulation::Mouse) {
        // We set release_modifiers = false because otherwise, this somehow breaks some keys, like | on a french keyboard!
        applyKeypress(CPC.InputMapper->CPCscancodeFromCPCkey(CPC_J0_RIGHT), keyboard_matrix, false, false);
        applyKeypress(CPC.InputMapper->CPCscancodeFromCPCkey(CPC_J0_LEFT), keyboard_matrix, false, false);
        applyKeypress(CPC.InputMapper->CPCscancodeFromCPCkey(CPC_J0_DOWN), keyboard_matrix, false, false);
        applyKeypress(CPC.InputMapper->CPCscancodeFromCPCkey(CPC_J0_UP), keyboard_matrix, false, false);
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

         // ── Drag-and-drop file loading ──
         if (event.type == SDL_EVENT_DROP_FILE) {
           const char* dropped = event.drop.data;
           if (dropped) {
             std::string drop_path(dropped);
             std::string ext = std::filesystem::path(drop_path).extension().string();
             std::transform(ext.begin(), ext.end(), ext.begin(),
                            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
             auto drop_fname = std::filesystem::path(drop_path).filename().string();

             if (ext == ".dsk" || ext == ".ipf" || ext == ".raw") {
               CPC.driveA.file = drop_path;
               if (file_load(CPC.driveA) == 0) {
                 imgui_toast_success("Drive A: " + drop_fname);
                 imgui_mru_push(CPC.mru_disks, drop_path);
               } else
                 imgui_toast_error("Failed to load disk: " + drop_fname);
             } else if (ext == ".cdt" || ext == ".voc") {
               CPC.tape.file = drop_path;
               if (file_load(CPC.tape) == 0) {
                 imgui_toast_success("Tape loaded: " + drop_fname);
                 imgui_mru_push(CPC.mru_tapes, drop_path);
                 tape_scan_blocks();
               } else
                 imgui_toast_error("Failed to load tape: " + drop_fname);
             } else if (ext == ".sna") {
               CPC.snapshot.file = drop_path;
               if (file_load(CPC.snapshot) == 0) {
                 imgui_toast_success("Snapshot loaded: " + drop_fname);
                 imgui_mru_push(CPC.mru_snaps, drop_path);
               } else
                 imgui_toast_error("Failed to load snapshot: " + drop_fname);
             } else if (ext == ".cpr") {
               CPC.cartridge.file = drop_path;
               if (file_load(CPC.cartridge) == 0) {
                 imgui_toast_success("Cartridge loaded: " + drop_fname);
                 imgui_mru_push(CPC.mru_carts, drop_path);
                 emulator_reset();
               } else {
                 imgui_toast_error("Failed to load cartridge: " + drop_fname);
               }
             } else if (ext == ".zip") {
               // Try as disk first (most common zip content)
               CPC.driveA.file = drop_path;
               if (file_load(CPC.driveA) == 0) {
                 imgui_toast_success("Drive A: " + drop_fname);
                 imgui_mru_push(CPC.mru_disks, drop_path);
               } else
                 imgui_toast_error("Unsupported ZIP content: " + drop_fname);
             } else {
               imgui_toast_error("Unknown file type: " + drop_fname);
             }
           }
           continue;
         }

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
         //
         // WantCaptureKeyboard blocks when menus, dropdowns, devtools, or any ImGui
         // window has focus. Special case: the virtual keyboard only uses mouse clicks,
         // so when it's the sole reason WantCaptureKeyboard is set, let physical keys
         // reach the CPC. Any other UI (menus, text fields, devtools) takes priority.
         {
           ImGuiIO& io = ImGui::GetIO();
           bool is_key_event = (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP);
           bool is_text_event = (event.type == SDL_EVENT_TEXT_INPUT);
           bool is_mouse_event_imgui = (event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_MOUSE_WHEEL);
           bool is_virtual_key = is_key_event && event.key.windowID == 0;

           // Use our own policy, not ImGui's WantCaptureKeyboard — ImGui's
           // NewFrame() sets it based on internal focus state which can stay
           // stuck after native file dialogs or menu interactions.
           // imgui_any_keyboard_ui_active() checks actual dialog/menu/devtools
           // state and is the single source of truth.
           bool imgui_wants_kbd = imgui_any_keyboard_ui_active();

           if (((is_key_event && !is_virtual_key) && imgui_wants_kbd) || (is_text_event && imgui_wants_kbd) || (is_mouse_event_imgui && io.WantCaptureMouse)) {
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
                           CPC.joystick_emulation = nextJoystickEmulation(CPC.joystick_emulation);
                           CPC.InputMapper->set_joystick_emulation();
                           SDL_SetWindowRelativeMouseMode(mainSDLWindow, CPC.joystick_emulation == JoystickEmulation::Mouse);
                           set_osd_message(std::string("Joystick emulation: ") + JoystickEmulationToString(CPC.joystick_emulation));
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
                bool over_topbar = on_main && event.motion.y < imgui_topbar_height();
                static bool topbar_cursor_visible = false;
                if (over_topbar && !topbar_cursor_visible) {
                  set_cursor_visibility(true);
                  topbar_cursor_visible = true;
                } else if (!over_topbar && topbar_cursor_visible && !CPC.phazer_emulation) {
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
              if (CPC.joystick_emulation == JoystickEmulation::Mouse) {
                int threshold = 2;
                if (event.motion.yrel > threshold) {
                  applyKeypress(CPC.InputMapper->CPCscancodeFromCPCkey(CPC_J0_DOWN), keyboard_matrix, true);
                }
                if (event.motion.yrel < -threshold) {
                  applyKeypress(CPC.InputMapper->CPCscancodeFromCPCkey(CPC_J0_UP), keyboard_matrix, true);
                }
                if (event.motion.xrel > threshold) {
                  applyKeypress(CPC.InputMapper->CPCscancodeFromCPCkey(CPC_J0_RIGHT), keyboard_matrix, true);
                }
                if (event.motion.xrel < -threshold) {
                  applyKeypress(CPC.InputMapper->CPCscancodeFromCPCkey(CPC_J0_LEFT), keyboard_matrix, true);
                }
                nextMouseReset = dwFrameCountOverall + 2;
              }
            }
            break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            {
              // Topbar clicks (Menu button, drive LEDs, etc.) are handled by ImGui's
              // imgui_render_topbar(). No legacy showGui() handler needed here.
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
              handle_mouse_joystick_button(event.button, keyboard_matrix, true);
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
              handle_mouse_joystick_button(event.button, keyboard_matrix, false);
            }
            break;

            // TODO: What if we were paused because of other reason than losing focus and then only lost focus
            //       the right thing to do here is to restore focus but keep paused... implementing this require
            //       keeping track of pause source, which will be a pain.
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
              if (CPC.auto_pause) {
                cpc_resume();
              }
              // In docked mode, refocus CPC Screen so keyboard routes to emulator
              // Only on app focus gain — not mouse enter, which fires during
              // normal UI interaction and would steal focus from popups/menus.
              if (CPC.workspace_layout == t_CPC::WorkspaceLayoutMode::Docked) {
                imgui_state.request_cpc_screen_focus = true;
              }
              break;
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
               cleanExit(g_z80_requested_exit_code.load(std::memory_order_relaxed));
         }
      }
      // ---- Non-headless: Z80 thread (z80_thread_main) handles emulation ----
      // Render thread waits for frame signal, does Phase A, releases Z80, then Phase B.
      if (!g_headless) {
         if (g_emu_paused.load(std::memory_order_relaxed)) {
            // Paused overlay: render ImGui without waiting for a frame signal
            video_display();
            video_display_b();
            video_take_pending_window_screenshot();
            if (g_m4_http.is_running()) g_m4_http.drain_pending();
            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
         } else {
            // Poll for the Z80 frame signal, pumping SDL events between attempts.
            // On macOS+Metal the Cocoa run loop must stay alive for CADisplayLink
            // and drawable-completion callbacks to fire; a bare condvar wait starves
            // it, causing SDL_RenderPresent / SDL_GL_SwapWindow to hang indefinitely
            // for video styles that spend time in Phase A before the GPU call
            // (CRT Basic/Full with GL shaders, SDL swscale with pixel filters).
            bool skip = false;
            while (!g_frame_signal.try_wait_ready_for(1, skip)) {
                SDL_PumpEvents(); // keep macOS/Metal run loop alive
            }
            if (!skip) {
               // OSD text — render thread owns osd_message/osd_timing, no race
               if (SDL_GetTicks() < osd_timing) {
                  print(static_cast<byte *>(back_surface->pixels) + CPC.scr_line_offs,
                        osd_message.c_str(), true);
               }
               uint64_t displayStart = SDL_GetPerformanceCounter();
               video_display(); // Phase A: texture upload + ImGui render (~3ms)
               // Partial audio push before Phase B stall
               {
                  int partial = static_cast<int>(CPC.snd_bufferptr - pbSndBuffer.get());
                  if (!g_emu_paused.load() && partial > 0) {
                     audio_push_buffer(pbSndBuffer.get(), partial);
                     CPC.snd_bufferptr = pbSndBuffer.get();
                  }
               }
               g_frame_signal.signal_consumed(); // Z80 starts next frame concurrently
               // Main-thread-only housekeeping (after releasing back_surface to Z80)
               if (g_m4_http.is_running()) g_m4_http.drain_pending();
#ifdef __APPLE__
               // Capture while Z80 is still blocked (between signal_consumed and next
               // wait_ready) — safe without atomics due to condvar happens-before.
               dword frame_count_snap = dwFrameCountOverall;
               if (back_surface && (frame_count_snap % 50) == 0) {
                  koncpc_update_dock_icon_preview(
                     back_surface->pixels, back_surface->w, back_surface->h,
                     back_surface->pitch, 0, 0, back_surface->w, back_surface->h);
               }
#endif
               // If quit was requested (e.g. KONCPC_EXIT from the Z80 thread),
               // skip video_display_b() which hangs indefinitely for OpenGL
               // styles (7-19).  signal_consumed() was already sent so the Z80
               // loop has seen the quit flag and will exit; on the next main-loop
               // iteration SDL_EVENT_QUIT will reach the event handler above and
               // doCleanUp() will join the (already-exited) Z80 thread cleanly.
               if (g_z80_thread_quit.load(std::memory_order_relaxed)) {
                  continue;
               }
               video_display_b(); // Phase B: 0-60ms, Z80 runs concurrently!
               uint64_t displayEnd = SDL_GetPerformanceCounter();
               displayTimeAccum.fetch_add(displayEnd - displayStart, std::memory_order_relaxed);
               if (audio_stream && CPC.snd_ready) {
                  int queued = SDL_GetAudioStreamQueued(audio_stream);
                  if (queued < 0) queued = 0;
                  if (queued < audio_queue_min_bytes) audio_queue_min_bytes = queued;
                  if (queued < static_cast<int>(CPC.snd_buffersize) / 2 && audio_push_count > 0) {
                     double display_ms = static_cast<double>(displayEnd - displayStart) * 1000.0 / perfFreq;
                     LOG_DEBUG("Audio low queue after display: " << queued
                               << "B, display took " << display_ms << "ms");
                  }
               }
               video_take_pending_window_screenshot();
               if (g_take_screenshot) {
                  dumpScreen();
                  g_take_screenshot = false;
               }
            } else {
               // Skipped frame: service screenshot requests before releasing Z80
               video_take_pending_window_screenshot();
               if (g_take_screenshot) {
                  dumpScreen();
                  g_take_screenshot = false;
               }
               g_frame_signal.signal_consumed();
            }
         }
      }

      // ---- Headless: original single-threaded emulation (unchanged) ----
      if (g_headless && !CPC.paused) { // run the emulation
         uint64_t perfNow = SDL_GetPerformanceCounter();

         if (perfNow >= perfTicksTargetFPS) { // update FPS counter every second
            dwFPS = dwFrameCount;
            dwFrameCount = 0;
            perfTicksTargetFPS = perfNow + perfFreq; // next sample in 1 second

            // Publish frame timing stats (use double to avoid integer division precision loss)
            if (frameTimeSamples > 0) {
               double ticksToUs = 1000000.0 / static_cast<double>(perfFreq);
               imgui_state.frame_time_avg_us = static_cast<float>(static_cast<double>(frameTimeAccum) / frameTimeSamples * ticksToUs);
               imgui_state.frame_time_min_us = static_cast<float>(static_cast<double>(frameTimeMin) * ticksToUs);
               imgui_state.frame_time_max_us = static_cast<float>(static_cast<double>(frameTimeMax) * ticksToUs);
               imgui_state.display_time_avg_us = static_cast<float>(static_cast<double>(displayTimeAccum.load(std::memory_order_relaxed)) / frameTimeSamples * ticksToUs);
               imgui_state.sleep_time_avg_us = static_cast<float>(static_cast<double>(sleepTimeAccum) / frameTimeSamples * ticksToUs);
               imgui_state.z80_time_avg_us = static_cast<float>(static_cast<double>(z80TimeAccum) / frameTimeSamples * ticksToUs);
            }
            frameTimeAccum = 0;
            frameTimeMin = UINT64_MAX;
            frameTimeMax = 0;
            displayTimeAccum.store(0, std::memory_order_relaxed);
            sleepTimeAccum = 0;
            z80TimeAccum = 0;
            frameTimeSamples = 0;

            // Publish audio diagnostics
            imgui_state.audio_underruns = audio_underrun_count;
            imgui_state.audio_near_underruns = audio_near_underrun_count;
            imgui_state.audio_pushes = audio_push_count;
            if (audio_push_count == 0) {
               imgui_state.audio_queue_avg_ms = 0;
               imgui_state.audio_queue_min_ms = 0;
               imgui_state.audio_push_interval_max_us = 0;
            } else if (audio_push_count > 0) {
               // Convert queue depth from bytes to milliseconds
               double avg_bytes = audio_queue_sum_bytes / audio_push_count;
               int frame_size = CPC.snd_stereo ? 4 : 2;  // 16-bit stereo=4, mono=2
               if (CPC.snd_bits == 0) frame_size /= 2;    // 8-bit halves it
               int sample_rate = freq_table[CPC.snd_playback_rate];
               double bytes_per_ms = sample_rate * frame_size / 1000.0;
               imgui_state.audio_queue_avg_ms = static_cast<float>(avg_bytes / bytes_per_ms);
               imgui_state.audio_queue_min_ms = static_cast<float>(audio_queue_min_bytes / bytes_per_ms);
               imgui_state.audio_push_interval_max_us = static_cast<float>(
                  static_cast<double>(audio_push_interval_max) * 1000000.0 / perfFreq);
            }
            audio_underrun_count = 0;
            audio_near_underrun_count = 0;
            audio_push_count = 0;
            audio_queue_sum_bytes = 0;
            audio_queue_min_bytes = INT_MAX;
            audio_push_interval_max = 0;
         }

         static constexpr int MAX_CONSECUTIVE_SKIPS = 5;
         static int consecutive_skips = 0;
         if (CPC.limit_speed && iExitCondition == EC_CYCLE_COUNT) {
            // Absolute deadline: sleep until perfTicksTarget, then advance by one frame.
            // Multiple EC_CYCLE_COUNTs may fire per frame (audio-driven cycle boundaries);
            // only the first one sleeps — subsequent ones see the deadline already passed.
            uint64_t sleepStart = SDL_GetPerformanceCounter();
            if (sleepStart < perfTicksTarget) {
               uint64_t remaining_ticks = perfTicksTarget - sleepStart;
               uint64_t remaining_ms = (remaining_ticks * 1000) / perfFreq;
               if (remaining_ms > 2) {
                  SDL_Delay(static_cast<Uint32>(remaining_ms - 2));
               }
               while (SDL_GetPerformanceCounter() < perfTicksTarget) { SDL_Delay(0); }
            }
            sleepTimeAccum += SDL_GetPerformanceCounter() - sleepStart;
            perfTicksTarget += perfTicksOffset;
            // Catch-up: if more than 3 frames behind, reset the deadline.
            uint64_t now = SDL_GetPerformanceCounter();
            if (!CPC.frameskip && perfTicksTarget + 3 * perfTicksOffset < now) {
               perfTicksTarget = now + perfTicksOffset;
            }
         } else if (iExitCondition != EC_CYCLE_COUNT) {
            // Speed limiter not active and not a mid-frame audio slice.
            CPC.skip_rendering = false;
            consecutive_skips = 0;
         }

         // Frameskip decision: only on frame boundaries to avoid mid-frame toggles.
         if (iExitCondition == EC_FRAME_COMPLETE && CPC.limit_speed) {
            uint64_t now = SDL_GetPerformanceCounter();
            if (CPC.frameskip && now > perfTicksTarget) {
               if (consecutive_skips < MAX_CONSECUTIVE_SKIPS) {
                  CPC.skip_rendering = true;
                  consecutive_skips++;
               } else {
                  CPC.skip_rendering = false;
                  consecutive_skips = 0;
                  perfTicksTarget = now + perfTicksOffset;
               }
            } else {
               CPC.skip_rendering = false;
               consecutive_skips = 0;
            }
         } else if (iExitCondition == EC_FRAME_COMPLETE && !CPC.limit_speed) {
            CPC.skip_rendering = false;
            consecutive_skips = 0;
         }

         dword dwOffset = CPC.scr_pos - CPC.scr_base; // offset in current surface row
         if (VDU.scrln > 0) {
            CPC.scr_base = static_cast<byte *>(back_surface->pixels) + (VDU.scrln * CPC.scr_line_offs); // determine current position
         } else {
            CPC.scr_base = static_cast<byte *>(back_surface->pixels); // reset to surface start
         }
         CPC.scr_pos = CPC.scr_base + dwOffset; // update current rendering position

         {
            uint64_t z80Start = SDL_GetPerformanceCounter();
            iExitCondition = z80_execute(); // run the emulation until an exit condition is met
            z80TimeAccum += SDL_GetPerformanceCounter() - z80Start;
         }

         // Sample tape level into waveform ring buffer (sub-frame rate)
         if (CPC.tape_motor && CPC.tape_play_button) {
            imgui_state.tape_wave_buf[imgui_state.tape_wave_head] = bTapeLevel;
            imgui_state.tape_wave_head = (imgui_state.tape_wave_head + 1) % ImGuiUIState::TAPE_WAVE_SAMPLES;

         }

         // Audio push: PSG finished filling the back buffer — push it to SDL.
         if (iExitCondition == EC_SOUND_BUFFER) {
            if (!CPC.paused) {
               audio_push_buffer(pbSndBuffer.get(), static_cast<int>(CPC.snd_buffersize));
            }
            CPC.snd_bufferptr = pbSndBuffer.get(); // reset write position
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

            g_keyboard_manager.update(keyboard_matrix, dwFrameCountOverall);

            // Measure frame-to-frame time (only on actual completed frames)
            {
               uint64_t now = SDL_GetPerformanceCounter();
               if (lastFrameStart > 0) {
                  uint64_t elapsed = now - lastFrameStart;
                  frameTimeAccum += elapsed;
                  if (elapsed < frameTimeMin) frameTimeMin = elapsed;
                  if (elapsed > frameTimeMax) frameTimeMax = elapsed;
                  frameTimeSamples++;
               }
               lastFrameStart = now;
            }

            // Check --exit-after condition
            if (g_exit_mode == EXIT_FRAMES && dwFrameCountOverall >= g_exit_target) {
               cleanExit(0, false);
            }
            if (g_exit_mode == EXIT_MS && (SDL_GetTicks() - g_exit_start_ticks) >= g_exit_target) {
               cleanExit(0, false);
            }

            // Check IPC VBL events
            ipc_check_vbl_events();

            // M4 Board activity LED countdown (1 per frame at 50fps)
            if (g_m4board.activity_frames > 0) g_m4board.activity_frames--;

            // M4 HTTP server — drain deferred actions (reset, pause toggle)
            if (g_m4_http.is_running()) g_m4_http.drain_pending();

#ifdef __APPLE__
            // Update Dock icon with CPC screen preview (~1fps at 50fps emulation)
            // back_surface is already sized to CPC_VISIBLE_SCR_WIDTH/HEIGHT * scale
            if (back_surface && (dwFrameCountOverall % 50) == 0) {
               koncpc_update_dock_icon_preview(
                  back_surface->pixels, back_surface->w, back_surface->h, back_surface->pitch,
                  0, 0, back_surface->w, back_surface->h);
            }
#endif

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
                  byte cur = keyboard_matrix[row].load(std::memory_order_relaxed);
                  if (cur != prev_matrix[row]) {
                     // Encode as row in high byte, value in low byte
                     uint16_t data = static_cast<uint16_t>((row << 8) | cur);
                     g_session.record_event(SessionEventType::KEY_DOWN, data);
                     prev_matrix[row] = cur;
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
                     keyboard_matrix[row].store(static_cast<byte>(evt.data & 0xFF), std::memory_order_relaxed);
                  }
               }
               if (!g_session.advance_frame()) {
                  // Recording finished, session goes back to IDLE
               }
            }

            // Auto-type: drain queue synchronized with keyboard scans.
            // Only inject key changes after the firmware has read the matrix,
            // so keys aren't changed mid-scan. Fallback after 10 frames for
            // programs that don't scan the keyboard (e.g. during loading).
            if (g_autotype_queue.is_active()) {
               bool do_tick = false;
               if (g_keyboard_scanned) {
                  g_keyboard_scanned = false;
                  g_keyboard_scan_timeout = 0;
                  do_tick = true;
               } else if (++g_keyboard_scan_timeout >= kAutotypeScanTimeoutFrames) {
                  g_keyboard_scan_timeout = 0;
                  do_tick = true;
               }
               if (do_tick) {
                  g_autotype_queue.tick([](uint16_t cpc_key, bool pressed) {
                     CPCScancode scancode = CPC.InputMapper->CPCscancodeFromCPCkey(static_cast<CPC_KEYS>(cpc_key));
                     // Direct matrix manipulation (same as ipc_apply_keypress)
                     if (static_cast<byte>(scancode) == 0xff) return;
                     if (pressed) {
                        keyboard_matrix[static_cast<byte>(scancode) >> 4].fetch_and(~bit_values[static_cast<byte>(scancode) & 7], std::memory_order_relaxed);
                        if (scancode & MOD_CPC_SHIFT) {
                           keyboard_matrix[0x25 >> 4].fetch_and(~bit_values[0x25 & 7], std::memory_order_relaxed);
                        } else {
                           keyboard_matrix[0x25 >> 4].fetch_or(bit_values[0x25 & 7], std::memory_order_relaxed);
                        }
                        if (scancode & MOD_CPC_CTRL) {
                           keyboard_matrix[0x27 >> 4].fetch_and(~bit_values[0x27 & 7], std::memory_order_relaxed);
                        } else {
                           keyboard_matrix[0x27 >> 4].fetch_or(bit_values[0x27 & 7], std::memory_order_relaxed);
                        }
                     } else {
                        keyboard_matrix[static_cast<byte>(scancode) >> 4].fetch_or(bit_values[static_cast<byte>(scancode) & 7], std::memory_order_relaxed);
                        keyboard_matrix[0x25 >> 4].fetch_or(bit_values[0x25 & 7], std::memory_order_relaxed);
                        keyboard_matrix[0x27 >> 4].fetch_or(bit_values[0x27 & 7], std::memory_order_relaxed);
                     }
                  });
               }
            }

            // Telnet console: drain input into autotype queue
            g_telnet.drain_input();

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
                  snprintf(chStr, sizeof(chStr), "%3dFPS %3d%%",
                     static_cast<int>(dwFPS),
                     static_cast<int>(dwFPS) * 100 / static_cast<int>(1000.0 / FRAME_PERIOD_MS));
                  fpsText = chStr;
               }
               imgui_state.topbar_fps = fpsText;
               imgui_state.drive_a_led = FDC.led && (FDC.command[1] & 1) == 0;
               imgui_state.drive_b_led = FDC.led && (FDC.command[1] & 1) == 1;
            }
            if (!CPC.skip_rendering) {
               asic_draw_sprites();
            }
            if (!g_headless) {
              if (!CPC.skip_rendering) {
                uint64_t displayStart = SDL_GetPerformanceCounter();

                video_display(); // phase A: texture upload + ImGui render (~3ms)

                // Push any partial audio buffer accumulated since the last EC_SOUND_BUFFER.
                // This tops up the audio queue before the expensive phase B stall
                // (floating viewports + GL context switches, 0-60ms).
                {
                   int partial = static_cast<int>(CPC.snd_bufferptr - pbSndBuffer.get());
                   if (!CPC.paused && partial > 0) {
                      audio_push_buffer(pbSndBuffer.get(), partial);
                      CPC.snd_bufferptr = pbSndBuffer.get();
                   }
                }

                video_display_b(); // phase B: floating viewports + window swap

                uint64_t displayEnd = SDL_GetPerformanceCounter();
                displayTimeAccum.fetch_add(displayEnd - displayStart, std::memory_order_relaxed);

                // Check audio queue after display (GL viewport stalls drain it)
                // Sample audio queue depth after display — catches GL stalls.
                // Only updates min (underrun counting is done in audio_push_buffer
                // to avoid double-counting).
                if (audio_stream && CPC.snd_ready) {
                   int queued = SDL_GetAudioStreamQueued(audio_stream);
                   if (queued < 0) queued = 0;
                   if (queued < audio_queue_min_bytes)
                      audio_queue_min_bytes = queued;
                   if (queued < static_cast<int>(CPC.snd_buffersize) / 2 && audio_push_count > 0) {
                      double display_ms = static_cast<double>(displayEnd - displayStart) * 1000.0 / perfFreq;
                      LOG_DEBUG("Audio low queue after display: " << queued
                                << "B, display took " << display_ms << "ms");
                   }
                }
              }
              video_take_pending_window_screenshot();
            }


            if (g_take_screenshot) {
              dumpScreen();
              g_take_screenshot = false;
            }
         }
      }
      else if (g_headless) { // Headless paused: sleep (non-headless handled above)
         // Drain HTTP deferred actions even while paused (otherwise resume won't work)
         if (g_m4_http.is_running()) g_m4_http.drain_pending();
         std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
      }

      // Deferred video plugin switch (triggered by Options combo).
      // Lightweight path swaps CRT resources only; full path tears down window/GL.
      if (imgui_state.video_reinit_pending) {
         imgui_state.video_reinit_pending = false;
         if (!video_try_lightweight_switch()) {
            // Full reinit — preserve window geometry so bars don't shrink the image
            int saved_w = 0, saved_h = 0, saved_x = 0, saved_y = 0;
            if (mainSDLWindow) {
               SDL_GetWindowSize(mainSDLWindow, &saved_w, &saved_h);
               SDL_GetWindowPosition(mainSDLWindow, &saved_x, &saved_y);
            }
            audio_pause();
            SDL_Delay(20);
            video_shutdown();
            if (video_init()) {
               fprintf(stderr, "video_init() failed after plugin change. Aborting.\n");
               cleanExit(-1);
            }
            // Only restore window geometry if the output size didn't change
            // (i.e. only the plugin changed, not scale/fullscreen)
            bool size_changed = (CPC.scr_scale != imgui_state.old_cpc_settings.scr_scale)
                             || (CPC.scr_window != imgui_state.old_cpc_settings.scr_window);
            if (saved_w > 0 && mainSDLWindow && !size_changed) {
               SDL_SetWindowSize(mainSDLWindow, saved_w, saved_h);
               SDL_SetWindowPosition(mainSDLWindow, saved_x, saved_y);
            }
#ifdef __APPLE__
            koncpc_setup_macos_menu();
#endif
            audio_resume();
         }
      }

      // Handle IPC "repaint" — re-render frame from RAM without Z80 advancement
      // Checked every loop (paused or unpaused)
      if (g_repaint_pending.load()) {
         std::string shot_path;
         {
            std::lock_guard<std::mutex> lock(g_repaint_mutex);
            shot_path = g_repaint_screenshot_path;
            g_repaint_screenshot_path.clear();
         }
         
         video_repaint_from_ram();
         
         if (!shot_path.empty()) {
            if (SDL_SavePNG(back_surface, shot_path)) {
               std::lock_guard<std::mutex> lock(g_repaint_mutex);
               g_repaint_error = "SDL_SavePNG failed for " + shot_path;
            } else {
               LOG_INFO("Repaint screenshot saved to " + shot_path);
            }
         }
         
         video_display(); // Force update UI
         video_display_b();
         g_repaint_done.store(true);
         g_repaint_pending.store(false);
      }
   }

   g_m4_http.stop();
   g_telnet.stop();
   g_ipc->stop();
   return 0;
}
