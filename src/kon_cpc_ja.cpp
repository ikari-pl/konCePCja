// konCePCja — host shell: emulator entry point and main loop, Z80/render
// thread orchestration, ROM setup and gate-array banking view, SDL event
// dispatch, audio push, and lifecycle (pause/reset/cleanup).

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#ifndef _MSC_VER
#include <unistd.h>
#endif

#ifdef _WIN32
/* clang-format off */
#include <windows.h>
#include <timeapi.h>
/* clang-format on */
#endif

#include "SDL3/SDL.h"

namespace {
inline Uint32 MapRGBSurface(SDL_Surface* surface, Uint8 r, Uint8 g, Uint8 b) {
  const SDL_PixelFormatDetails* fmt =
      SDL_GetPixelFormatDetails(surface->format);
  SDL_Palette const* pal = SDL_GetSurfacePalette(surface);
  return SDL_MapRGB(fmt, pal, r, g, b);
}
}  // namespace

#include "amdrum.h"
#include "amx_mouse.h"
#include "autotype.h"
#include "avi_recorder.h"
#include "configuration.h"
#include "cpc_key_tables.h"
#include "cpc_machine.h"
#include "crtc_types.h"
#include "data_areas.h"
#include "devtools_ui.h"
#include "drive_sounds.h"
#include "hw_views.h"
#include "io_bus.h"
#include "io_dispatch.h"
#include "keyboard.h"
#include "keyboard_manager.h"
#include "koncepcja.h"
#include "koncepcja_ipc_server.h"
#include "m4board.h"
#include "m4board_http.h"
#include "macos_menu.h"
#include "memory_bus.h"
#include "memutils.h"
#include "serial_interface.h"
#include "smartwatch.h"
#include "stringutils.h"
#include "symbiface.h"
#include "symfile.h"
#include "telnet_console.h"
#include "trace.h"
#include "video_gpu.h"
#include "video_host.h"
#include "vjoystick_map.h"
#include "wav_recorder.h"
#include "window_rescue.h"
#include "ym_recorder.h"
#include "z80_view.h"
#include "zip_archive.h"

// imgui.h / imgui_impl_sdl3.h are intentionally NOT included here.
// The main loop talks to the UI through IUiHost only (P1.5.1).
// imgui_ui.h is still included because the global `imgui_state` struct
// (telemetry / flag bus) is defined there and is a public data
// contract — see iui_host.h header for why imgui_state stays free-
// standing instead of being absorbed into the host interface.
#include "command_palette.h"
#include "imgui_ui.h"
#include "iui_host.h"
#include "menu_actions.h"

Symfile g_symfile;

namespace {
KoncepcjaIpcServer* g_ipc = new KoncepcjaIpcServer();
}
#include <errno.h>

#include <cstring>

#include "argparse.h"
#include "cartridge.h"
#include "errors.h"
#include "fileutils.h"
#include "imgui_ui_testable.h"
#include "log.h"
#include "portable-file-dialogs.h"
#include "savepng.h"
#include "session_recording.h"
#include "silicon_disc.h"
#include "slotshandler.h"
#include "subcycle/machine.h"
#include "subcycle_bridge.h"
#include "tape_line_in.h"

inline constexpr int MAX_NB_JOYSTICKS = 2;
inline constexpr int POLL_INTERVAL_MS = 1;

#ifndef DESTDIR
#define DESTDIR ""
#endif

extern t_z80regs z80;
extern std::vector<Breakpoint> breakpoints;

extern t_disk_format disk_format[];

extern byte* pbCartridgePages[];

extern SDL_Window* mainSDLWindow;
extern SDL_Renderer* renderer;

namespace {
SDL_AudioStream* audio_stream = nullptr;
}  // namespace
// Independent host cosmetic-sound stream (drive/tape SFX). It owns its own
// logical audio device on the default output, so SDL/the OS mixes it with the
// emulated AY device at the output layer — physically decoupled from the AY
// stream (audio_stream), never summed into the AY buffer. SDL's own audio
// thread pulls samples from it via drive_sounds_audio_callback.
namespace {
SDL_AudioStream* drive_audio_stream = nullptr;
}  // namespace
extern SDL_Surface* back_surface;
SDL_Surface* back_surface = nullptr;
extern video_plugin* vid_plugin;
video_plugin* vid_plugin;

namespace {
bool g_take_screenshot = false;
}  // namespace
bool g_headless = false;
extern bool g_debug;
bool g_debug = false;
namespace {
bool g_log_fps = false;  // --fps: log once-per-second FPS to stdout
}  // namespace
namespace {
bool g_exit_on_break = false;
}  // namespace
namespace {
enum : std::uint8_t { EXIT_NONE, EXIT_FRAMES, EXIT_MS } g_exit_mode = EXIT_NONE;
}  // namespace
namespace {
dword g_exit_target = 0;
}  // namespace
namespace {
dword g_exit_start_ticks = 0;
}  // namespace

// Autotype keyboard-scan synchronization:
// Set when the Z80 reads the keyboard matrix via PPI Port A (PSG reg 14).
// The autotype tick is gated on this flag so key changes happen between scan
// cycles.
namespace {
bool g_keyboard_scanned = false;
// Engine=1: the rows the firmware actually read this frame (machine
// take_key_scanned_rows(), read-and-clear — captured ONCE per frame here and
// shared by the autotype scan gate and the KeyboardManager relay).
uint16_t g_engine1_scanned_rows = 0;
// A break fired while a KONCPC_WAITBREAK was still queued (e.g. `-a 'call 0'
// -a KONCPC_WAITBREAK` where BASIC executes the CALL before the queue reaches
// the WAITBREAK). The WAITBREAK consumes the latch instead of blocking on a
// breakpoint that already came and went.
bool g_waitbreak_latch = false;
}  // namespace

bool autotype_waitbreak_in_flight() {
  return g_autotype_queue.is_blocked() ||
         g_autotype_queue.has_pending_command(KONCPC_WAITBREAK);
}
namespace {
int g_keyboard_scan_timeout = 0;  // frames since last scan, for fallback
}  // namespace
namespace {
const int kAutotypeScanTimeoutFrames =
    10;  // inject anyway after N frames without a scan
}  // namespace

namespace {
int topbar_height_px = 24;
}  // namespace

extern t_CPC CPC;
// Two device slots -> CPC joystick 0 and 1.  A slot holds EITHER a high-level
// SDL_Gamepad (preferred) OR a raw SDL_Joystick (fallback for non-gamepad
// devices); device_instance[] maps an SDL event's instance id back to its slot
// for hotplug and per-slot input routing.
namespace {
SDL_Joystick* joysticks[MAX_NB_JOYSTICKS];
}  // namespace
namespace {
SDL_Gamepad* gamepads[MAX_NB_JOYSTICKS] = {nullptr};
}  // namespace
namespace {
SDL_JoystickID device_instance[MAX_NB_JOYSTICKS] = {0};
}  // namespace

// Emulation/render thread split (P1.2a)
// g_emu_paused: authoritative pause flag shared between threads.
// cpc_pause()/cpc_resume() keep CPC.paused and g_emu_paused in sync.
// Z80 thread reads g_emu_paused; render thread reads it for the paused-overlay
// path.
std::atomic<bool> g_emu_paused{false};
// Set true when main() wants the Z80 thread to exit cleanly.
namespace {
std::atomic<bool> g_z80_thread_quit{false};
}  // namespace
// Exit code to use when the Z80 thread requests quit via SDL_EVENT_QUIT.
namespace {
std::atomic<int> g_z80_requested_exit_code{0};
}  // namespace
// Captured at the top of koncpc_main() so cleanExit() can tell whether it's
// running on the main (render) thread vs. an auxiliary thread (IPC / HTTP /
// telnet).  Only the main thread owns SDL teardown; anything else must push
// SDL_EVENT_QUIT and let the main loop handle the teardown.
namespace {
std::thread::id g_main_thread_id{};
}  // namespace
// Handle for the Z80 emulation thread (non-headless mode only). Stored so
// doCleanUp() can join it instead of letting it run past global destruction.
namespace {
std::thread g_z80_thread;
}  // namespace
// Protects the imgui_state stats fields written by the Z80 thread and read by
// the render thread (frame_time_avg_us, z80_time_avg_us, audio_*, etc.).
std::mutex g_imgui_stats_mutex;
// True when the Z80 thread is NOT inside z80_execute() (i.e. safe to touch Z80
// state from another thread).  Starts true because the thread hasn't spawned
// yet.
std::atomic<bool> g_z80_quiescent{true};
// Frame handoff: Z80 signals after asic_draw_sprites(); render signals after
// Phase A.
FrameSignal g_frame_signal;

// High-resolution timing using SDL_GetPerformanceCounter (nanosecond-class)
namespace {
uint64_t perfFreq;  // SDL_GetPerformanceFrequency() — ticks per second
}  // namespace
namespace {
uint64_t perfTicksOffset;  // frame period in perf-counter ticks
}  // namespace
namespace {
uint64_t perfTicksTarget;  // next frame deadline in perf-counter ticks
}  // namespace
namespace {
uint64_t perfTicksTargetFPS;  // next 1-second FPS sample point
}  // namespace
// NOLINTNEXTLINE(misc-use-internal-linkage): dwFPS is referenced cross-TU
// (tape_line_in.cpp); per-file check can't see the other TU
dword dwFPS, dwFrameCount;
namespace {
dword dwXScale, dwYScale;
}  // namespace

// Frame timing measurement (1-second reporting window).
// frameTimeAccum/z80TimeAccum/sleepTimeAccum are Z80-thread-only (no atomic
// needed). displayTimeAccum is written by the render thread and read+reset by
// the Z80 thread's FPS publisher — use atomic to avoid a data race.
namespace {
uint64_t frameTimeAccum = 0;
}  // namespace
namespace {
uint64_t frameTimeMin = UINT64_MAX;
}  // namespace
namespace {
uint64_t frameTimeMax = 0;
}  // namespace
namespace {
std::atomic<uint64_t> displayTimeAccum{0};
}  // namespace
namespace {
uint64_t sleepTimeAccum = 0;
}  // namespace
namespace {
uint64_t z80TimeAccum = 0;
}  // namespace
namespace {
uint32_t frameTimeSamples = 0;
}  // namespace
namespace {
uint64_t lastFrameStart =
    0;  // perf counter at EC_FRAME_COMPLETE (for frame-to-frame stats)
}  // namespace

namespace {
dword osd_timing;
}  // namespace
namespace {
std::string osd_message;
}  // namespace

namespace {
std::string lastSavedSnapshot;
}  // namespace

namespace {
dword dwMF2ExitAddr;
}  // namespace
extern dword dwMF2Flags;
dword dwMF2Flags = 0;
// Audio buffer: PSG writes samples here, pushed to SDL stream on
// EC_SOUND_BUFFER.
namespace {
std::unique_ptr<byte[]> pbSndBuffer;  // PSG write buffer
}  // namespace
extern byte* pbGPBuffer;
byte* pbGPBuffer = nullptr;
namespace {
byte* pbSndBufferEnd = nullptr;
}  // namespace
extern byte *membank_read[4], *membank_write[4], *memmap_ROM[256];
byte *membank_read[4], *membank_write[4], *memmap_ROM[256];
extern byte* pbRAM;
byte* pbRAM = nullptr;
namespace {
byte* pbRAMbuffer = nullptr;
}  // namespace
namespace {
byte* pbROM = nullptr;
}  // namespace
extern byte* pbROMlo;
byte* pbROMlo = nullptr;
extern byte* pbROMhi;
byte* pbROMhi = nullptr;
extern byte* pbExpansionROM;
byte* pbExpansionROM = nullptr;
namespace {
byte* pbMF2ROMbackup = nullptr;
}  // namespace
namespace {
byte* pbMF2ROM = nullptr;
}  // namespace
extern std::vector<byte> pbTapeImage;
// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other
// translation units/tests; internal linkage would break the link
std::vector<byte> pbTapeImage;
// `keyboard_matrix` is the pending/authoritative key state: every writer (main
// thread SDL/virtual keys, IPC thread, Z80-thread autotype) mutates it.
// `keyboard_matrix_live` is the snapshot the CPC firmware actually scans; the
// Z80 thread copies pending->live once per frame, before z80_execute(), so the
// firmware never observes a partially-applied (multi-line) keypress.
// `g_kbd_matrix_mutex` makes each writer's key+shift+ctrl write sequence atomic
// with respect to that snapshot copy — without it, a scan could read a shifted
// key's digit line before its SHIFT line and decode the unshifted glyph
// (e.g. '1'->'&' on shifted-digit layouts).  See beads-2qg / beads-d1n.
std::atomic<byte> keyboard_matrix[16];
std::atomic<byte> keyboard_matrix_live[16];
std::mutex g_kbd_matrix_mutex;

extern dword dwFrameCountOverall;
dword dwFrameCountOverall = 0;

namespace {
t_MemBankConfig membank_config;
}  // namespace

// External linkage: the engine=1 bridge drains the printer Device's
// strobe-clocked bytes into this capture file (see koncepcja.h).
FILE* pfoPrinter;

#ifdef DEBUG
dword dwDebugFlag = 0;
FILE* pfoDebug = nullptr;
#endif

enum : std::uint8_t { MAX_FREQ_ENTRIES = 5 };
namespace {
dword freq_table[MAX_FREQ_ENTRIES] = {11025, 22050, 44100, 48000, 96000};
}  // namespace

#include "font.h"

void set_osd_message(const std::string& message, uint32_t for_milliseconds) {
  osd_timing = SDL_GetTicks() + for_milliseconds;
  osd_message = " " + message;
  // Unify feedback channels (beads-49l): in GUI mode also surface the message
  // as a toast, so the same action gives consistent feedback whether it was
  // triggered by a shortcut/menu (this OSD path) or a file dialog (which
  // toasts directly).  Headless keeps the OSD-only behavior.
  if (!g_headless) {
    ui_host().toast(UiToastLevel::Info, message);
  }
}

extern double colours_rgb[32][3];
double colours_rgb[32][3] = {
    {0.5, 0.5, 0.5}, {0.5, 0.5, 0.5}, {0.0, 1.0, 0.5}, {1.0, 1.0, 0.5},
    {0.0, 0.0, 0.5}, {1.0, 0.0, 0.5}, {0.0, 0.5, 0.5}, {1.0, 0.5, 0.5},
    {1.0, 0.0, 0.5}, {1.0, 1.0, 0.5}, {1.0, 1.0, 0.0}, {1.0, 1.0, 1.0},
    {1.0, 0.0, 0.0}, {1.0, 0.0, 1.0}, {1.0, 0.5, 0.0}, {1.0, 0.5, 1.0},
    {0.0, 0.0, 0.5}, {0.0, 1.0, 0.5}, {0.0, 1.0, 0.0}, {0.0, 1.0, 1.0},
    {0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, {0.0, 0.5, 0.0}, {0.0, 0.5, 1.0},
    {0.5, 0.0, 0.5}, {0.5, 1.0, 0.5}, {0.5, 1.0, 0.0}, {0.5, 1.0, 1.0},
    {0.5, 0.0, 0.0}, {0.5, 0.0, 1.0}, {0.5, 0.5, 0.0}, {0.5, 0.5, 1.0}};

// original RGB color to GREEN LUMA converted by Ulrich Doewich
// unknown formula.
namespace {
double colours_green_classic[32] = {
    0.5647, 0.5647, 0.7529, 0.9412, 0.1882, 0.3765, 0.4706, 0.6588,
    0.3765, 0.9412, 0.9098, 0.9725, 0.3451, 0.4078, 0.6275, 0.6902,
    0.1882, 0.7529, 0.7216, 0.7843, 0.1569, 0.2196, 0.4392, 0.5020,
    0.2824, 0.8471, 0.8157, 0.8784, 0.2510, 0.3137, 0.5333, 0.5961};
}  // namespace

// added by a proposal from libretro project,
// see https://github.com/ikari/konCePCja/issues/135

namespace {
double colours_green_libretro[32] = {
    0.5755, 0.5755, 0.7534, 0.9718, 0.1792, 0.3976, 0.4663, 0.6847,
    0.3976, 0.9718, 0.9136, 1.0300, 0.3394, 0.4558, 0.6265, 0.7429,
    0.1792, 0.7534, 0.6952, 0.8116, 0.1210, 0.2374, 0.4081, 0.5245,
    0.2884, 0.8626, 0.8044, 0.9208, 0.2302, 0.3466, 0.5173, 0.6337};
}  // namespace

// interface to use the palette also from tests
double* video_get_green_palette(int mode) {
  if (!mode) return colours_green_classic;
  return colours_green_libretro;
}

double* video_get_rgb_color(int color) { return colours_rgb[color]; }

namespace {
SDL_Color colours[32];
}  // namespace

extern byte bit_values[8];
byte bit_values[8] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};

#include "rom_mods.h"

extern char chAppPath[_MAX_PATH + 1];
char chAppPath[_MAX_PATH + 1];
namespace {
std::filesystem::path binPath;  // Where the binary is
}  // namespace
// NOLINTNEXTLINE(misc-use-internal-linkage): chROMFile is referenced cross-TU
// (subcycle_bridge.cpp); per-file check can't see the other TU
std::string chROMFile[4] = {"cpc464.rom", "cpc664.rom", "cpc6128.rom",
                            "system.cpr"};

JoystickEmulation nextJoystickEmulation(JoystickEmulation current) {
  return static_cast<JoystickEmulation>(
      (static_cast<int>(current) + 1) %
      static_cast<int>(JoystickEmulation::Last));
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
extern t_CRTC CRTC;
t_CRTC CRTC;
extern t_FDC FDC;
t_FDC FDC;
extern t_GateArray GateArray;
t_GateArray GateArray;
namespace {
t_PPI PPI;
}  // namespace
extern t_PSG PSG;
t_PSG PSG;
namespace {
t_VDU VDU;
}  // namespace

extern t_drive driveA;
t_drive driveA;
extern t_drive driveB;
t_drive driveB;

// Phase 1: non-owning aggregate of core globals. Behavior is unchanged;
// this simply provides a structured view for future refactors.
CpcMachine g_machine{
    &CPC, &CRTC, &GateArray, &FDC, &PPI, &PSG, &VDU, &driveA, &driveB, &z80,
};

// Phase 2: non-owning bus views over existing banking / IO dispatch.
// Memory hot paths use g_memory_bus (wrapping membank_*); IO still goes via the
// same dispatch.
MemoryBus g_memory_bus{membank_read, membank_write};
IoBus g_io_bus{};

namespace {
enum ApplicationWindowState : std::uint8_t {
  Minimized,    // application window has been iconified
  Restored,     // application window has been restored
  GainedFocus,  // application window got input focus
  LostFocus     // application window lost input focus
};
}  // namespace

namespace {
CapriceArgs args;
}  // namespace

void ga_init_banking(t_MemBankConfig& membank_config, unsigned char RAM_bank) {
  // Eight 16K banks feed the decode: 0-3 are base RAM, 4-7 the selected
  // expansion bank (or a Silicon Disc bank when the index falls in its
  // range).
  byte* expansion =
      g_silicon_disc.owns_bank(RAM_bank)
          ? g_silicon_disc.bank_ptr(RAM_bank - SILICON_DISC_FIRST_BANK)
          : pbRAM + ((RAM_bank + 1) * 65536);
  byte* banks[8];
  for (int i = 0; i < 4; ++i) {
    banks[i] = pbRAM + (i * 16384);
    banks[4 + i] = expansion + (i * 16384);
  }

  // Gate-array RAM configurations 0-7: which bank appears in each 16K slot
  // (the classic PAL decode table from the 6128 service manual).
  static constexpr int kBankLayout[8][4] = {
      {0, 1, 2, 3}, {0, 1, 2, 7}, {4, 5, 6, 7}, {0, 3, 2, 7},
      {0, 4, 2, 3}, {0, 5, 2, 3}, {0, 6, 2, 3}, {0, 7, 2, 3},
  };
  for (int config = 0; config < 8; ++config) {
    for (int slot = 0; slot < 4; ++slot) {
      membank_config[config][slot] = banks[kBankLayout[config][slot]];
    }
  }
}

void ga_memory_manager() {
  dword mem_bank;
  if (CPC.ram_size == 64) {    // 64KB of RAM?
    mem_bank = 0;              // no expansion memory
    GateArray.RAM_config = 0;  // the only valid configuration is 0
  } else if (CPC.ram_size > 576) {
    // Yarek 4MB expansion: 6-bit bank number from data bits 5-3 (low) and
    // inverted port address bits 5-3 (high), stored in GateArray.RAM_ext
    mem_bank = (static_cast<dword>(GateArray.RAM_ext) << 3) |
               ((GateArray.RAM_config >> 3) & 7);
    if (((mem_bank + 2) * 64) >
        CPC.ram_size) {  // selection is beyond available memory?
      mem_bank = 0;      // force default mapping
    }
  } else {
    mem_bank =
        (GateArray.RAM_config >> 3) & 7;  // extract expansion memory bank
    if (!g_silicon_disc.owns_bank(mem_bank) &&
        ((mem_bank + 2) * 64) >
            CPC.ram_size) {  // selection is beyond available memory?
      mem_bank = 0;          // force default mapping
    }
  }
  if (mem_bank !=
      GateArray.RAM_bank) {  // requested bank is different from the active one?
    GateArray.RAM_bank = mem_bank;
    ga_init_banking(membank_config, GateArray.RAM_bank);
  }
  for (int n = 0; n < 4; n++) {  // remap active memory banks
    memory_set_read_bank(n, membank_config[GateArray.RAM_config & 7][n]);
    memory_set_write_bank(n, membank_config[GateArray.RAM_config & 7][n]);
  }
  if (!(GateArray.ROM_config & 0x04)) {  // lower ROM is enabled?
    if (dwMF2Flags & MF2_ACTIVE) {       // is the Multiface 2 paged in?
      // MF2 ROM (8K) at 0x0000-0x1FFF: read-only overlay
      // MF2 RAM (8K) at 0x2000-0x3FFF: read-write (intercepted in z80.cpp
      // write_mem) Writes to 0x0000-0x1FFF fall through to CPC RAM
      // (membank_write unchanged)
      memory_set_read_bank(GateArray.lower_ROM_bank, pbMF2ROM);
    } else {
      memory_set_read_bank(GateArray.lower_ROM_bank,
                           pbROMlo);  // 'page in' lower ROM
    }
  }
  if (CPC.model > 2 && GateArray.registerPageOn) {
    memory_set_read_bank(1, pbRegisterPage);
    memory_set_write_bank(1, pbRegisterPage);
  }
  if (!(GateArray.ROM_config & 0x08)) {       // upper/expansion ROM is enabled?
    memory_set_read_bank(3, pbExpansionROM);  // 'page in' upper/expansion ROM
  }
}

void memory_set_read_bank(int slot, byte* ptr) { membank_read[slot] = ptr; }

void memory_set_write_bank(int slot, byte* ptr) { membank_write[slot] = ptr; }

// ── MF2 I/O dispatch handler ────────────────────
// MF2 paging uses file-local dwMF2Flags and ga_memory_manager(),
// so its handler must live in this file.

namespace {
bool s_mf2_enabled = false;  // synced from CPC.mf2
}  // namespace

namespace {
bool mf2_out_handler(reg_pair port, byte /*val*/) {
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
}  // namespace

void mf2_register_io() {  // NOLINT(misc-use-internal-linkage): registered from
                          // io_dispatch (cross-TU)
  s_mf2_enabled = CPC.mf2 != 0;
  io_register_out(0xFE, mf2_out_handler, &s_mf2_enabled, "Multiface II");
}

// OSD text renderer: draws `pchStr` at pbAddr in the back surface with the
// built-in 8x8 font (font.h). Each glyph pixel is doubled vertically
// (scr_bps offsets the second half-line) and casts a one-pixel black drop
// shadow right and below; bolColour selects white or black text. One
// depth-generic implementation — the pixel width in bytes derives from
// CPC.scr_bpp (RGBA32 and RGB565 are the live plugin formats), replacing
// four hand-unrolled copies whose 8-bit variant clobbered its own second
// scanline.
namespace {
void print(byte* pbAddr, const char* pchStr, bool bolColour) {
  const unsigned int px = (CPC.scr_bpp + 7) / 8;  // bytes per pixel
  if (px == 0 || px > 4) return;                  // no surface / unknown depth

  // White truncates to all-ones at every depth; 8-bit palettized surfaces
  // ask SDL for the palette entry instead.
  // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated
  // (out-param/compound-assign/loop/reference)
  uint32_t colour = bolColour ? 0xffffffffu : 0u;
  if (CPC.scr_bpp == 8) {
    colour = bolColour ? MapRGBSurface(back_surface, 255, 255, 255)
                       : MapRGBSurface(back_surface, 0, 0, 0);
  }

  // Little-endian partial store: paints one pixel of any byte width.
  const auto put = [px](byte* p, uint32_t v) {
    for (unsigned int i = 0; i < px; i++)
      p[i] = static_cast<byte>(v >> (8 * i));
  };

  const size_t len = strlen(pchStr);
  for (size_t n = 0; n < len; n++) {
    int idx = static_cast<unsigned char>(pchStr[n]);
    if (idx < FNT_MIN_CHAR || idx >= FNT_MAX_CHAR) idx = FNT_BAD_CHAR;
    idx -= FNT_MIN_CHAR;  // zero-base into the font strip
    byte* line = pbAddr;
    for (int row = 0; row < FNT_CHAR_HEIGHT; row++) {
      byte* pixel = line;
      byte bits = bFont[idx];  // one glyph scanline, MSB first
      for (int col = 0; col < FNT_CHAR_WIDTH; col++) {
        if (bits & 0x80) {
          put(pixel, colour);                      // glyph pixel
          put(pixel + CPC.scr_bps, colour);        // doubled half-line
          put(pixel + px, 0);                      // shadow right
          put(pixel + CPC.scr_bps + px, 0);        //  ... doubled
          put(pixel + CPC.scr_line_offs, 0);       // shadow below
          put(pixel + CPC.scr_line_offs + px, 0);  // shadow below-right
        }
        pixel += px;
        bits <<= 1;
      }
      line += CPC.scr_line_offs;  // next surface line
      idx += FNT_CHARS;           // next row in the font strip
    }
    pbAddr += FNT_CHAR_WIDTH * px;  // next character cell
  }
}
}  // namespace

namespace {

// RAII stdio handle for the ROM loaders below.
struct FileCloser {
  void operator()(std::FILE* f) const { std::fclose(f); }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

FilePtr open_file(const std::string& path, const char* mode) {
  return FilePtr(std::fopen(path.c_str(), mode));
}

}  // namespace

// Load the system ROM and apply the configured keyboard-layout firmware
// patch. Models 0-2 read a plain 32K OS+BASIC image from rom_path; the Plus
// boots from cartridge bank 0 (already extracted by cartridge_load()). For a
// non-English layout the firmware's keyboard-translation table and character
// set are patched in place — on a Plus only when the system cartridge is
// mounted, never a game cartridge.
namespace {
int emulator_patch_ROM() {
  if (CPC.model <= 2) {
    const std::string rom_file = CPC.rom_path + "/" + chROMFile[CPC.model];
    FilePtr const f = open_file(rom_file, "rb");
    if (!f) {
      LOG_ERROR("Couldn't open ROM file '" << rom_file << "'");
      return ERR_CPC_ROM_MISSING;
    }
    if (fread(pbROM, 2 * 16384, 1, f.get()) != 1) {
      LOG_ERROR("Couldn't read ROM file '" << rom_file << "'");
      return ERR_NOT_A_CPC_ROM;
    }
    pbROMlo = pbROM;
  } else if (pbCartridgePages[0] != nullptr) {
    pbROMlo = pbCartridgePages[0];
  }

  if (CPC.keyboard == 0) return 0;  // English firmware ships as-is

  size_t table_offset = 0;  // 0 = leave this firmware untouched
  switch (CPC.model) {
    case 0:  // 464
      table_offset = 0x1d69;
      break;
    case 1:  // 664
    case 2:  // 6128
      table_offset = 0x1eef;
      break;
    case 3:  // 6128+
      if (CPC.cartridge.file == CPC.rom_path + "/" + chROMFile[3])
        table_offset = 0x1eef;
      break;
    default:
      break;
  }
  if (table_offset != 0) {
    memcpy(pbROMlo + table_offset, cpc_keytrans[CPC.keyboard - 1], 240);
    memcpy(pbROMlo + 0x3800, cpc_charset[CPC.keyboard - 1], 2048);
  }
  return 0;
}
}  // namespace

void emulator_reset() {
  // Reset mutates the entire board. Under engine=1 the Z80 runs on its own
  // thread, so board_reset() must not race a concurrent run_frame() — a torn
  // reset can leave the machine unable to complete a frame, and the Fast tier
  // then free-runs the char clock (observed: tens of GB of audio, PC frozen at
  // 0x0000, the emulator wedged). Most reset callers on the render thread (the
  // Machine-menu button, F5, drag-drop cartridge load) reach here without
  // quiescing; the IPC path already does. Quiesce here so every path is safe,
  // and restore the caller's pause state: a paused caller (menu/IPC) stays
  // paused and resumes itself; a running caller (F5) keeps running. Cheap and
  // idempotent — in headless/single-threaded mode g_z80_quiescent is always
  // true, and at init (before the Z80 thread exists) it is too.
  const bool was_paused = g_emu_paused.load(std::memory_order_relaxed);
  cpc_pause_and_wait();
  subcycle_bridge_reset();  // no-op unless the sub-cycle engine is active
  if (CPC.model > 2) {
    if (pbCartridgePages[0] != nullptr) {
      pbROMlo = pbCartridgePages[0];
    }
  }

  video_set_palette();

  // CPC
  CPC.cycle_count = CYCLE_COUNT_INIT;
  for (auto& row : keyboard_matrix)
    row.store(0xff, std::memory_order_relaxed);  // clear CPC keyboard matrix
  for (auto& row : keyboard_matrix_live)
    row.store(0xff, std::memory_order_relaxed);  // and its per-frame snapshot
  CPC.tape_motor = 0;
  CPC.tape_play_button = 0;
  CPC.printer_port = 0xff;

  // Chip views: value-reset to power-on state (the Devices themselves reset
  // with the machine; these are the host-side mirrors the tools read).
  VDU = {};
  VDU.flag_drawing = 1;

  CRTC.crtc_type = crtc_type_for_model(CPC.model);

  GateArray = {};
  GateArray.scr_mode = GateArray.requested_scr_mode = 1;  // mode 1 at power-on
  ga_init_banking(membank_config, GateArray.RAM_bank);

  PPI = {};

  PSG.control = 0;

  // Peripheral expansions
  amdrum_reset();
  smartwatch_reset();
  amx_mouse_reset();
  symbiface_reset();
  m4board_reset();

  FDC = {};
  FDC.phase = CMD_PHASE;
  FDC.flags = STATUSDRVA_flag | STATUSDRVB_flag;

  // memory
  memset(pbRAM, 0, CPC.ram_size * 1024);  // clear all memory used for CPC RAM
  if (pbMF2ROM) {
    memset(pbMF2ROM + 8192, 0, 8192);  // clear the MF2's RAM area
  }
  for (int n = 0; n < 4;
       n++) {  // initialize active read/write bank configuration
    memory_set_read_bank(n, membank_config[0][n]);
    memory_set_write_bank(n, membank_config[0][n]);
  }
  memory_set_read_bank(0, pbROMlo);  // 'page in' lower ROM
  memory_set_read_bank(3, pbROMhi);  // 'page in' upper ROM

  // Multiface 2
  dwMF2Flags = 0;
  dwMF2ExitAddr = 0xffffffff;  // clear MF2 return address
  if (pbMF2ROM && pbMF2ROMbackup) {
    memcpy(pbMF2ROM, pbMF2ROMbackup,
           8192);  // copy the MF2 ROM to its proper place
  }

  // The first reset happens during emulator_init() (boot), where an OSD
  // message would be wrong / premature. Suppress feedback on that very first
  // call; every subsequent (user-initiated) reset shows confirmation.
  static bool first_reset = true;
  if (first_reset) {
    first_reset = false;
  } else {
    set_osd_message("Machine reset");
  }

  if (!was_paused) cpc_resume();  // a running caller keeps running post-reset
}

namespace {
int input_init() {
  CPC.InputMapper->init();
  CPC.InputMapper->set_joystick_emulation();
  SDL_SetWindowRelativeMouseMode(
      mainSDLWindow, CPC.joystick_emulation == JoystickEmulation::Mouse);
  return 0;
}
}  // namespace

namespace {

// Read one expansion-ROM image (a 16K CPC ROM, optionally prefixed by a
// 128-byte AMSDOS header) into memmap_ROM[slot].
//
// Mirrors the classic slot semantics: a missing file or a non-CPC image is a
// SOFT failure — the slot is cleared with a message and boot continues; a
// file that identifies as a ROM but cannot be read in full (truncated,
// oversized, I/O error) is a HARD failure that aborts emulator_init.
// Returns 0 on success or soft failure, an ERR_* code on hard failure.
int load_expansion_rom_slot(int slot, const std::string& rom_file) {
  const std::string path = CPC.rom_path + "/" + rom_file;
  FilePtr const f = open_file(path, "rb");
  if (!f) {
    fprintf(stderr, "ERROR: The %s file is missing - clearing ROM slot %d.\n",
            rom_file.c_str(), slot);
    CPC.rom_file[slot] = "";
    return 0;
  }

  auto rom = std::make_unique<byte[]>(16384);  // value-initialized (zeroed)
  if (fread(rom.get(), 128, 1, f.get()) != 1) {
    LOG_ERROR("Invalid ROM '" << path
                              << "': less than 128 bytes. Not a CPC ROM?");
    return ERR_NOT_A_CPC_ROM;
  }

  // Graduate Software accessory ROMs use a non-standard format: a '$'-
  // terminated string in the first 0x43 bytes plus a RET (0xc9) at 0x38.
  // https://www.cpcwiki.eu/index.php/Graduate_Software#Structure_of_a_utility_ROM
  bool graduate = rom[0x38] == 0xc9;
  if (graduate) {
    graduate = false;
    for (int n = 0; n < 0x43; n++) graduate = graduate || rom[n] == 0x24;
  }

  // AMSDOS header detection: 16-bit checksum of bytes 0x00-0x42 stored
  // big-endian at 0x43/0x44. When present, drop the header and reload the
  // first 128 bytes of the ROM body over it.
  word checksum = 0;
  for (int n = 0; n < 0x43; n++) checksum += rom[n];
  const bool has_amsdos_header = checksum == ((rom[0x43] << 8) + rom[0x44]);
  if (has_amsdos_header && fread(rom.get(), 128, 1, f.get()) != 1) {
    LOG_ERROR("Invalid ROM '" << path
                              << "': couldn't read the 128 bytes of the "
                                 "AMSDOS header. Not a CPC ROM?");
    return ERR_NOT_A_CPC_ROM;
  }

  const long total_size = file_size(fileno(f.get()));
  const long header_bytes = has_amsdos_header ? 128 : 0;
  if (total_size > 16384 + header_bytes) {
    LOG_ERROR("Invalid ROM '"
              << path
              << "': total ROM size is greater than 16kB. Not a CPC ROM?");
    return ERR_NOT_A_CPC_ROM;
  }

  // ROM type byte: 0 = foreground, 1 = background, 2 = extension — or 'G'
  // for a Graduate accessory ROM.
  const bool valid_cpc_rom = !(rom[0] & 0xfc);
  if (!valid_cpc_rom && (rom[0] != 0x47 || !graduate)) {
    fprintf(stderr, "ERROR: %s is not a CPC ROM file - clearing ROM slot %d.\n",
            rom_file.c_str(), slot);
    CPC.rom_file[slot] = "";
    return 0;
  }

  // 128 body bytes are already in the buffer; header_bytes more were
  // consumed from the file when the AMSDOS header was dropped.
  const long remaining = total_size - 128 - header_bytes;
  if (remaining > 0 && fread(rom.get() + 128, remaining, 1, f.get()) != 1) {
    LOG_ERROR("Internal error: couldn't read the expected ROM size from "
              << path);
    return ERR_NOT_A_CPC_ROM;
  }
  memmap_ROM[slot] = rom.release();
  return 0;
}

// Load the Multiface II ROM: 8K firmware image kept twice — a pristine
// backup (pbMF2ROMbackup, restored on every reset) and the live 8K ROM + 8K
// RAM window (pbMF2ROM). A bad image disables MF2 support for the session.
void load_mf2_rom() {
  if (pbMF2ROM != nullptr) return;  // already resident
  const std::string path = CPC.rom_path + "/" + CPC.rom_mf2;
  auto backup = std::make_unique<byte[]>(8192);
  bool ok = false;
  if (FilePtr const f = open_file(path, "rb")) {
    ok = fread(backup.get(), 8192, 1, f.get()) == 1 &&
         memcmp(backup.get() + 0x0d32, "MULTIFACE 2", 11) == 0;
    if (!ok) {
      fprintf(stderr,
              "ERROR: The file selected as the MF2 ROM is either corrupt "
              "or invalid.\n");
    }
  } else {
    fprintf(stderr,
            "ERROR: The file selected as the MF2 ROM (%s) couldn't be "
            "opened.\n",
            path.c_str());
  }
  if (ok) {
    pbMF2ROMbackup = backup.release();
    pbMF2ROM = new byte[16384]();  // 8K ROM window + 8K RAM, zeroed
  } else {
    CPC.rom_mf2 = "";
    CPC.mf2 = 0;  // disable MF2 support
  }
}

}  // namespace

int emulator_init() {
  if (input_init()) {
    fprintf(stderr, "input_init() failed. Aborting.\n");
    _exit(-1);
  }

  // Cartridge must be loaded before init as ROM needs to be present.
  cartridge_load();

  // Host-side memory: all value-initialized (zeroed).
  pbGPBuffer = new byte[128 * 1024]();  // general-purpose scratch buffer
  // One guard byte sits before pbRAM: prerender_normal*_plus may read it.
  pbRAMbuffer = new byte[(CPC.ram_size * 1024) + 1]();
  pbRAM = pbRAMbuffer + 1;
  pbROM = new byte[32 * 1024]();  // system ROM: OS + BASIC
  pbRegisterPage = new byte[16 * 1024]();
  pbROMlo = pbROM;
  pbROMhi = pbExpansionROM = pbROM + 16384;
  std::fill(std::begin(memmap_ROM), std::end(memmap_ROM), nullptr);
  ga_init_banking(membank_config,
                  GateArray.RAM_bank);  // init the CPC memory banking map
  if (int const err = emulator_patch_ROM()) {
    LOG_ERROR("Failed patching the ROM");
    return err;
  }

  for (int slot = 0; slot < MAX_ROM_SLOTS; slot++) {
    if (CPC.rom_file[slot].empty()) continue;
    std::string rom_file = CPC.rom_file[slot];
    if (rom_file == "DEFAULT") {
      // On 464, there's no AMSDOS by default.
      // We still allow users to override this if they want.
      // More details: https://github.com/ikari/konCePCja/issues/227
      if (CPC.model == 0) continue;
      rom_file = "amsdos.rom";
    }
    if (int const err = load_expansion_rom_slot(slot, rom_file)) return err;
  }
  if (CPC.mf2) load_mf2_rom();  // Multiface 2 enabled?

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

namespace {
void emulator_shutdown() {
  delete[] pbMF2ROMbackup;
  delete[] pbMF2ROM;
  pbMF2ROM = nullptr;
  pbMF2ROMbackup = nullptr;
  g_si_rom.unload(
      memmap_ROM);  // free auto-loaded SI ROM before general cleanup
  m4board_unload_rom(
      memmap_ROM);  // free auto-loaded M4 ROM before general cleanup
  for (int slot = 2; slot < MAX_ROM_SLOTS; slot++)  // expansion ROMs 2-31
    delete[] memmap_ROM[slot];

  delete[] pbROM;
  delete[] pbRAMbuffer;
  delete[] pbGPBuffer;
}
}  // namespace

void bin_load(const std::string& filename, const size_t offset) {
  LOG_INFO("Load " << filename << " in memory at offset 0x" << std::hex
                   << offset);
  FILE* file;
  if ((file = fopen(filename.c_str(), "rb")) == nullptr) {
    LOG_ERROR("File not found: " << filename);
    return;
  }

  auto closure = [&]() { fclose(file); };
  memutils::scope_exit<decltype(closure)> const cs(closure);

  size_t const ram_size = static_cast<size_t>(CPC.ram_size) * 1024;
  size_t const max_size = ram_size - offset;
  std::vector<uint8_t> chunk(max_size);
  size_t const read = fread(chunk.data(), 1, max_size, file);
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
  // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated
  // (out-param/compound-assign/loop/reference)
  if (subcycle::Machine* m = subcycle_bridge_machine()) {
    for (size_t i = 0; i < read; ++i) m->ram_write(offset + i, chunk[i]);
  } else {
    std::memcpy(&pbRAM[offset], chunk.data(), read);
  }
  // Jump at the beginning of the program
  z80.PC.w.l = static_cast<word>(offset);
  // Setup the stack the way it would be if we had launch it with run"
  z80_write_mem(--z80.SP.w.l, 0x0);
  z80_write_mem(--z80.SP.w.l, 0x98);
  z80_write_mem(--z80.SP.w.l, 0x7f);
  z80_write_mem(--z80.SP.w.l, 0x89);
  z80_write_mem(--z80.SP.w.l, 0xb9);
  z80_write_mem(--z80.SP.w.l, 0xa2);
  if (subcycle_bridge_active()) subcycle_bridge_regs_to_machine();
}

int printer_start() {
  if (!pfoPrinter) {
    if (!(pfoPrinter = fopen(CPC.printer_file.c_str(), "wb"))) {
      return 0;  // failed to open/create file
    }
  }
  return 1;  // ready to capture printer output
}

void printer_stop() {
  if (pfoPrinter != nullptr && fclose(pfoPrinter) != 0) {
    LOG_ERROR("Error closing printer output file '" << CPC.printer_file << "'");
  }
  pfoPrinter = nullptr;
}

// ── Audio diagnostics ──
namespace {
uint64_t audio_last_push_tick = 0;  // perf counter of last push
}  // namespace
namespace {
int audio_underrun_count = 0;  // underruns: queue was empty
}  // namespace
namespace {
int audio_near_underrun_count = 0;  // near-underruns: queue < half buffer
}  // namespace
namespace {
int audio_push_count = 0;  // successful pushes this reporting period
}  // namespace
namespace {
double audio_queue_sum_bytes = 0;  // sum of queue depths (for average)
}  // namespace
namespace {
int audio_queue_min_bytes = INT_MAX;  // min queue depth this period
}  // namespace
namespace {
uint64_t audio_push_interval_max =
    0;  // longest gap between pushes (perf ticks)
}  // namespace

// Push completed audio buffer into SDL stream (called from main loop on
// EC_SOUND_BUFFER). SDL handles internal queuing and feeds the hardware at the
// correct rate.
namespace {
void audio_push_buffer(const byte* data, int len) {
  if (!audio_stream || !CPC.snd_ready || len <= 0) return;

  uint64_t const now = SDL_GetPerformanceCounter();

  // Measure queue depth BEFORE pushing.
  int queued = SDL_GetAudioStreamQueued(audio_stream);
  queued = std::max(queued, 0);  // SDL error — treat as empty

  // Cap total latency (beads-49ir). With the speed limiter off the emulation is
  // unpaced and produces audio hundreds of times faster than the 44.1kHz sink
  // drains it; with no ceiling the SDL queue grows without bound and audio lags
  // the picture by seconds. Drop the buffer once the queue already holds more
  // than ~160ms. That is well above the 80ms underrun floor topped up below, so
  // paced playback (queue ~80ms) never trips it; unpaced audio then tracks
  // realtime with bounded latency (skipping ahead) instead of backlogging.
  const int max_queue_bytes = static_cast<int>(CPC.snd_buffersize) * 8;
  if (queued >= max_queue_bytes) return;

  // Detect underruns (skip if we have no previous push timestamp to
  // compute an interval from — e.g. the very first push after init).
  if (audio_last_push_tick > 0) {
    // [[maybe_unused]]: consumed only by LOG_DEBUG, which release compiles out.
    [[maybe_unused]] double const interval_ms =
        static_cast<double>(now - audio_last_push_tick) * 1000.0 / perfFreq;
    if (queued == 0) {
      audio_underrun_count++;
      LOG_DEBUG("Audio UNDERRUN: queue empty, interval " << interval_ms
                                                         << "ms");
    } else if (queued < len / 2) {
      // Below half a buffer — real danger of audible artifact
      audio_near_underrun_count++;
      LOG_DEBUG("Audio near-underrun: queue " << queued << "B (< " << len / 2
                                              << "B), interval " << interval_ms
                                              << "ms");
    }
  }
  audio_queue_sum_bytes += queued;
  audio_queue_min_bytes = std::min(queued, audio_queue_min_bytes);

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
    const int target_bytes = frame_bytes * 4;  // ~80ms at 50Hz
    int const queued_now = SDL_GetAudioStreamQueued(audio_stream);
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
    uint64_t const interval = now - audio_last_push_tick;
    audio_push_interval_max = std::max(interval, audio_push_interval_max);
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
}  // namespace

namespace {
int audio_align_samples(int given) {
  int actual = 1;
  while (actual < given) {
    actual <<= 1;
  }
  return actual;  // return the closest match as 2^n
}
}  // namespace

// SDL get-callback for the independent drive/tape SFX stream. Runs on SDL's
// audio thread: renders the requested amount of interleaved-stereo int16 SFX
// and hands it to SDL, which mixes it with the AY stream at the device. The
// generator self-gates on its enable flags, so this is silence when the effects
// are off. `additional_amount` is in the stream's source format (stereo S16 =>
// 4 bytes/frame).
namespace {
void SDLCALL drive_sounds_audio_callback(void* /*userdata*/,
                                         SDL_AudioStream* stream,
                                         int additional_amount,
                                         int /*total_amount*/) {
  if (additional_amount <= 0) return;
  constexpr int kBytesPerFrame = 2 * static_cast<int>(sizeof(int16_t));
  int frames_needed = additional_amount / kBytesPerFrame;
  int16_t chunk[512 * 2];  // 512 stereo frames per iteration
  while (frames_needed > 0) {
    int const n = frames_needed < 512 ? frames_needed : 512;
    drive_sounds_fill_stereo(chunk, n);
    SDL_PutAudioStreamData(stream, chunk, n * kBytesPerFrame);
    frames_needed -= n;
  }
}
}  // namespace

int audio_init() {
  SDL_AudioSpec desired;

  if (!CPC.snd_enabled) {
    return 0;
  }

  CPC.snd_ready = false;

  desired.freq = freq_table[CPC.snd_playback_rate];
  desired.format = CPC.snd_bits ? SDL_AUDIO_S16LE : SDL_AUDIO_S8;
  desired.channels = CPC.snd_stereo + 1;

  int const sample_frames =
      audio_align_samples(desired.freq * FRAME_PERIOD_MS / 1000);
  char frames_hint[32];
  snprintf(frames_hint, sizeof(frames_hint), "%d", sample_frames);
  SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames_hint);

  audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                           &desired, nullptr, nullptr);
  if (audio_stream == nullptr) {
    LOG_ERROR("Could not open audio: " << SDL_GetError());
    return 1;
  }

  LOG_VERBOSE("Audio: Desired: Freq: "
              << desired.freq << ", Format: " << desired.format
              << ", Channels: " << static_cast<int>(desired.channels)
              << ", Frames: " << sample_frames);

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
      if (!SDL_PutAudioStreamData(audio_stream, silence.data(),
                                  static_cast<int>(CPC.snd_buffersize))) {
        LOG_ERROR("Audio: pre-buffer failed: " << SDL_GetError());
      }
    }
  }
  SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(audio_stream));
  CPC.snd_ready = true;
  LOG_VERBOSE("Audio: Sound buffer ready");

  drive_sounds_init(desired.freq);

  // Open the independent drive/tape SFX path as its OWN audio device stream on
  // the default output. SDL's audio thread pulls samples via the get-callback,
  // and SDL/the OS mixes this device with the AY device at the output layer —
  // the emulated AY buffer is never perturbed. (A device opened via
  // SDL_OpenAudioDeviceStream cannot accept extra bound streams, so the SFX get
  // their own logical device rather than sharing the AY one.) Done AFTER
  // drive_sounds_init so the generator and its lock are ready before the audio
  // thread starts pulling.
  {
    SDL_AudioSpec drive_spec;
    drive_spec.freq = desired.freq;
    drive_spec.format = SDL_AUDIO_S16;  // generator emits int16
    drive_spec.channels = 2;            // stereo so we can pan (right bias)
    drive_audio_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &drive_spec,
        drive_sounds_audio_callback, nullptr);
    if (drive_audio_stream) {
      SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(drive_audio_stream));
      LOG_DEBUG("Audio: drive/tape SFX stream ready (independent of AY)");
    } else {
      LOG_ERROR(
          "Audio: could not open drive/tape SFX stream: " << SDL_GetError());
    }
  }

  return 0;
}

void audio_shutdown() {
  // Each stream came from SDL_OpenAudioDeviceStream, so destroying it also
  // closes its own logical device. Tear down the SFX stream first for tidiness.
  if (drive_audio_stream) {
    SDL_DestroyAudioStream(drive_audio_stream);
    drive_audio_stream = nullptr;
  }
  if (audio_stream) {
    SDL_DestroyAudioStream(audio_stream);
    audio_stream = nullptr;
  }
}

void audio_pause() {
  if (CPC.snd_enabled && audio_stream) {
    SDL_PauseAudioDevice(SDL_GetAudioStreamDevice(audio_stream));
    // Halt the host SFX device too, so drive/tape sounds stop with emulation.
    if (drive_audio_stream) {
      SDL_PauseAudioDevice(SDL_GetAudioStreamDevice(drive_audio_stream));
    }
  }
}

void audio_resume() {
  if (CPC.snd_enabled && audio_stream) {
    SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(audio_stream));
    if (drive_audio_stream) {
      SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(drive_audio_stream));
    }
  }
}

void cpc_pause() {
  audio_pause();
  CPC.paused = true;
  g_emu_paused.store(true, std::memory_order_relaxed);
}

void cpc_resume() {
  CPC.paused = false;
  g_emu_paused.store(false, std::memory_order_relaxed);
  lastFrameStart =
      0;  // reset so first frame after resume isn't measured as huge
  audio_resume();
}

void cpc_pause_and_wait() {
  cpc_pause();
  // Spin until the Z80 thread has exited z80_execute() and entered its sleep
  // loop. g_z80_quiescent is set true by z80_thread_main before sleeping, false
  // before entering z80_execute().  In headless mode the Z80 runs on the
  // calling thread, so g_z80_quiescent stays true and we return immediately.
  while (!g_z80_quiescent.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
}

void video_update_palette_entry(int index, uint8_t r, uint8_t g, uint8_t b) {
  if (index < 0 || index >= 34) return;
  if (!back_surface) return;
  const SDL_PixelFormatDetails* fmt =
      SDL_GetPixelFormatDetails(back_surface->format);
  SDL_Palette const* pal = SDL_GetSurfacePalette(back_surface);
  GateArray.palette[index] = SDL_MapRGB(fmt, pal, r, g, b);

  unsigned int const clamped = std::clamp(CPC.scr_oglscanlines, 0u, 100u);
  float const factor = (100 - clamped) / 100.0f;
  GateArray.dark_palette[index] = SDL_MapRGB(
      fmt, pal, static_cast<uint8_t>(r * factor),
      static_cast<uint8_t>(g * factor), static_cast<uint8_t>(b * factor));
}

int video_set_palette() {
  if (!CPC.scr_tube) {
    for (int n = 0; n < 32; n++) {
      dword red = static_cast<dword>(colours_rgb[n][0] *
                                     (CPC.scr_intensity / 10.0) * 255);
      red = std::min<dword>(red, 255);  // limit to the maximum

      dword green = static_cast<dword>(colours_rgb[n][1] *
                                       (CPC.scr_intensity / 10.0) * 255);
      green = std::min<dword>(green, 255);
      dword blue = static_cast<dword>(colours_rgb[n][2] *
                                      (CPC.scr_intensity / 10.0) * 255);
      blue = std::min<dword>(blue, 255);
      colours[n].r = static_cast<Uint8>(red);
      colours[n].g = static_cast<Uint8>(green);
      colours[n].b = static_cast<Uint8>(blue);
    }
  } else {
    for (int n = 0; n < 32; n++) {
      double const* colours_green = video_get_green_palette(CPC.scr_green_mode);

      dword green = static_cast<dword>(colours_green[n] *
                                       (CPC.scr_intensity / 10.0) * 255);
      green = std::min<dword>(green, 255);

      dword blue = static_cast<dword>(0.01 * CPC.scr_green_blue_percent *
                                      colours_green[n] *
                                      (CPC.scr_intensity / 10.0) * 255);

      // unlikely, but we care though
      blue = std::min<dword>(blue, 255);

      colours[n].r = 0;
      colours[n].g = static_cast<Uint8>(green);
      colours[n].b = static_cast<Uint8>(blue);
    }
  }

  vid_plugin->set_palette(colours);

  for (int n = 0; n < 17; n++) {  // loop for all colours + border
    int const i = GateArray.ink_values[n];
    video_update_palette_entry(n, colours[i].r, colours[i].g, colours[i].b);
  }

  return 0;
}

namespace {
void video_set_style() {
  // Always render at native Mode 2 width (768px). dwXScale=2 selects full
  // ModeMap tables in crtc_init(). dwYScale controls scanline doubling only.
  dwXScale = 2;
  dwYScale = vid_plugin->half_pixels ? 1 : 2;
  CPC.dwYScale = dwYScale;
}
}  // namespace

void mouse_init() {
  // hide the mouse cursor unless we emulate phazer
  set_cursor_visibility(CPC.phazer_emulation);
}

// Pull `win` back onto a display if it has ended up (almost) entirely off every
// monitor's usable area — windows drift off-screen from a saved position on a
// now-disconnected display, or from OS window-management nudges. `min_visible`
// is how many points must overlap a display in BOTH axes to count as "on
// screen": pass a titlebar-ish value (~48) at startup so the window is
// grabbable, or 1 on live moves so we only rescue a truly-lost window and never
// fight an intentional partial drag. The window is only re-positioned (never
// resized), so a large window on a small display keeps its size with its
// top-left made visible.
namespace {
void koncpc_rescue_window_onscreen(SDL_Window* win, int min_visible) {
  if (!win) return;
  koncpc::Rect w{};
  SDL_GetWindowPosition(win, &w.x, &w.y);
  SDL_GetWindowSize(win, &w.w, &w.h);

  int count = 0;
  SDL_DisplayID* ids = SDL_GetDisplays(&count);
  if (!ids || count <= 0) {
    if (ids) SDL_free(ids);
    return;
  }
  std::vector<koncpc::Rect> displays;
  displays.reserve(count);
  for (int i = 0; i < count; ++i) {
    SDL_Rect b;
    if (SDL_GetDisplayUsableBounds(ids[i], &b)) {
      displays.push_back({b.x, b.y, b.w, b.h});
    }
  }
  SDL_free(ids);

  int nx = 0, ny = 0;
  if (koncpc::compute_onscreen_position(w, displays.data(),
                                        static_cast<int>(displays.size()),
                                        min_visible, nx, ny)) {
    SDL_SetWindowPosition(win, nx, ny);
    LOG_DEBUG("Rescued off-screen window from " << w.x << "," << w.y << " to "
                                                << nx << "," << ny);
  }
}
}  // namespace

int video_init() {
  int const original_scr_style = CPC.scr_style;
  vid_plugin = &video_plugin_list[CPC.scr_style];
  LOG_DEBUG("video_init: vid_plugin = " << vid_plugin->name)

  // Always init at scale=2 for best surface quality (768×540, doubled
  // scanlines). The actual display size is controlled by scr_scale via
  // compute_scale.
  back_surface = vid_plugin->init(vid_plugin, 2, CPC.scr_window == 0);

  if (!back_surface) {
    // OpenGL may be unavailable (e.g. Intel HD 3000 only exposes GL 1.1).
    // Try the SDL_Renderer backend which uses D3D11/Metal instead of OpenGL.
    LOG_ERROR("Could not set requested video mode: "
              << SDL_GetError() << " — trying SDL_Renderer fallback");
    for (size_t i = 0; i < video_plugin_list.size(); i++) {
      if (std::string(video_plugin_list[i].name).find("(SDL)") !=
          std::string::npos) {
        vid_plugin = &video_plugin_list[i];
        LOG_INFO("Falling back to: " << vid_plugin->name);
        back_surface = vid_plugin->init(vid_plugin, 2, CPC.scr_window == 0);
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

  // Triple-buffer the CPC frame so the Z80 thread never blocks on render.  Only
  // the threaded (GUI) path uses the ring; the headless fallback is
  // single-threaded and keeps writing the plugin's single surface directly.
  if (!g_headless) {
    back_surface = video_ring_init(back_surface);
  }

  {
    const SDL_PixelFormatDetails* fmt =
        SDL_GetPixelFormatDetails(back_surface->format);
    CPC.scr_bpp = fmt ? fmt->bits_per_pixel : 0;  // bit depth of the surface
  }
  video_set_style();  // select rendering style

  int const iErrCode = video_set_palette();  // init CPC colours
  if (iErrCode) {
    return iErrCode;
  }

  CPC.scr_bps = back_surface->pitch;  // rendered screen line length in bytes
  CPC.scr_line_offs = CPC.scr_bps * dwYScale;
  CPC.scr_pos = CPC.scr_base = static_cast<byte*>(
      back_surface->pixels);  // memory address of back buffer

  // Resize window to match user's chosen scale (init always creates at 2x)
  if (CPC.scr_scale > 0 && mainSDLWindow) {
    static const float sf[] = {0.f, 1.f, 1.5f, 2.f, 3.f};
    if (CPC.scr_scale < sizeof(sf) / sizeof(sf[0])) {
      float const f = sf[CPC.scr_scale];
      int const new_w = static_cast<int>(CPC_RENDER_WIDTH * f);
      int new_h = CPC.scr_crt_aspect
                      ? static_cast<int>(new_w * 3.f / 4.f)
                      : static_cast<int>(CPC_VISIBLE_SCR_HEIGHT * f);
      new_h += video_get_topbar_height() + video_get_bottombar_height();
      SDL_SetWindowSize(mainSDLWindow, new_w, new_h);
    }
  }

  // A saved/derived position may land on a display that no longer exists (or is
  // arranged differently); make sure the freshly-sized window is reachable.
  koncpc_rescue_window_onscreen(mainSDLWindow, 48);

  return 0;
}

void video_shutdown() {
  // Plugin close must run first so the GPU plugin can tear down ImGui
  // SDLGPU3 and other device-dependent state before the GPU device
  // itself is destroyed.  For non-GPU plugins the order is irrelevant
  // (video_gpu_shutdown is a no-op when the device was never created).
  video_ring_shutdown();  // free ring write buffers before the plugin frees its
                          // front-end surface (restyle re-allocates the ring)
  vid_plugin->close();
  video_gpu_shutdown();  // safety net — idempotent no-op after plugin close
}

void video_display() {  // NOLINT(misc-use-internal-linkage): called from
                        // video_host (cross-TU)
  vid_plugin->flip(vid_plugin);
}

// Phase B: floating viewports + window swap. Call after pushing audio.
namespace {
void video_display_b() {
  if (vid_plugin->flip_b) vid_plugin->flip_b(vid_plugin);
}
}  // namespace

// ── Controller device tracking (gamepad + raw joystick, hotplug-aware) ──────
// Return the slot (0/1) a given SDL instance id is mapped to, or -1 if unknown.
namespace {
int controller_slot_of_instance(SDL_JoystickID id) {
  if (id == 0) return -1;
  for (int i = 0; i < MAX_NB_JOYSTICKS; i++) {
    if (device_instance[i] == id) return i;
  }
  return -1;
}
}  // namespace

// First free slot, or -1 when both CPC joysticks are already assigned.
namespace {
int controller_free_slot() {
  for (int i = 0; i < MAX_NB_JOYSTICKS; i++) {
    if (device_instance[i] == 0) return i;
  }
  return -1;
}
}  // namespace

// Open a newly-seen device into a free slot.  Prefers the high-level
// SDL_Gamepad API (auto-mapped buttons/axes); falls back to raw SDL_Joystick
// otherwise.
namespace {
void controller_open(SDL_JoystickID id) {
  if (id == 0 || controller_slot_of_instance(id) >= 0) return;  // already open
  int const slot = controller_free_slot();
  if (slot < 0) return;  // only two CPC joysticks — ignore extra devices
  if (SDL_IsGamepad(id)) {
    SDL_Gamepad* gp = SDL_OpenGamepad(id);
    if (gp != nullptr) {
      gamepads[slot] = gp;
      device_instance[slot] = id;
      const char* name = SDL_GetGamepadNameForID(id);
      fprintf(stderr, "Opened gamepad in slot %d: %s\n", slot,
              name ? name : "(unknown)");
      return;
    }
    fprintf(stderr, "Failed to open gamepad %u: %s\n",
            static_cast<unsigned>(id), SDL_GetError());
  }
  SDL_Joystick* js = SDL_OpenJoystick(id);
  if (js != nullptr) {
    joysticks[slot] = js;
    device_instance[slot] = id;
    const char* name = SDL_GetJoystickNameForID(id);
    fprintf(stderr, "Opened joystick in slot %d: %s\n", slot,
            name ? name : "(unknown)");
  } else {
    fprintf(stderr, "Failed to open joystick %u: %s\n",
            static_cast<unsigned>(id), SDL_GetError());
  }
}
}  // namespace

// Close a device and free its slot, releasing any matrix bits it still held.
namespace {
void controller_close(SDL_JoystickID id) {
  int const slot = controller_slot_of_instance(id);
  if (slot < 0) return;
  if (gamepads[slot] != nullptr) {
    SDL_CloseGamepad(gamepads[slot]);
    gamepads[slot] = nullptr;
  }
  if (joysticks[slot] != nullptr) {
    SDL_CloseJoystick(joysticks[slot]);
    joysticks[slot] = nullptr;
  }
  device_instance[slot] = 0;
  // Release every direction/fire this slot could have been holding down.
  vjoy::VJoyKeys const all = vjoy::vjoy_all_keys(slot);
  for (int i = 0; i < all.count; i++) {
    applyKeypressDirect(CPC.InputMapper->CPCscancodeFromCPCkey(all.keys[i]),
                        keyboard_matrix, false);
  }
  fprintf(stderr, "Closed controller in slot %d\n", slot);
}
}  // namespace

namespace {
int joysticks_init() {
  if (CPC.joysticks == 0) {
    return 0;
  }

  // Disable HIDAPI drivers known to crash inside SDL3 during device
  // negotiation (null-deref in SetEnhancedReportHint / WriteSubcommand).
  // The standard system joystick driver still works for these devices.
  // Users can override with env vars (e.g. SDL_JOYSTICK_HIDAPI_SWITCH=1).
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, "0");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, "0");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_NINTENDO_CLASSIC, "0");

  // GAMEPAD implies JOYSTICK, but request both explicitly for clarity.
  if (!SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
    fprintf(stderr, "Failed to initialize joystick subsystem. Error: %s\n",
            SDL_GetError());
    return ERR_JOYSTICKS_INIT;
  }

  SDL_SetJoystickEventsEnabled(true);
  SDL_SetGamepadEventsEnabled(true);

  // Open whatever is already present.  SDL_GetJoysticks() lists every device
  // (gamepad-capable or not); controller_open() picks the right API per device.
  int nbJoysticks = 0;
  SDL_JoystickID* ids = SDL_GetJoysticks(&nbJoysticks);
  if (ids != nullptr) {
    for (int i = 0; i < nbJoysticks && controller_free_slot() >= 0; i++) {
      controller_open(ids[i]);
    }
    SDL_free(ids);
  }

  // Do NOT fail when nothing is plugged in — hotplug (SDL_EVENT_*_ADDED) will
  // pick devices up at runtime.
  if (device_instance[0] == 0 && device_instance[1] == 0) {
    fprintf(stderr, "No controller yet; hotplug enabled.\n");
  }
  return 0;
}
}  // namespace

namespace {
void joysticks_shutdown() {
  for (int i = 0; i < MAX_NB_JOYSTICKS; i++) {
    if (gamepads[i] != nullptr) {
      SDL_CloseGamepad(gamepads[i]);
      gamepads[i] = nullptr;
    }
    if (joysticks[i] != nullptr) {
      SDL_CloseJoystick(joysticks[i]);
      joysticks[i] = nullptr;
    }
    device_instance[i] = 0;
  }
  SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK);
}
}  // namespace

// Press/release one direction/fire (single bit) for a slot, via the shared
// vjoystick mapping — the same keyboard-matrix row-9/row-6 path the on-screen
// virtual joystick and IPC `input joy` drive.  applyKeypress() respects the
// pause guard, matching the historic raw-joystick handlers.
namespace {
void controller_apply_bit(int slot, unsigned bit, bool pressed) {
  vjoy::VJoyKeys const k = vjoy::vjoy_active_keys(slot, bit);
  if (k.count == 1) {
    applyKeypress(CPC.InputMapper->CPCscancodeFromCPCkey(k.keys[0]),
                  keyboard_matrix, pressed);
  }
}
}  // namespace

namespace {
void update_timings() {
  perfFreq = SDL_GetPerformanceFrequency();
  // Frame period in perf-counter ticks: (FRAME_PERIOD_MS / 1000) * freq /
  // speed_ratio
  double const speed_ratio = CPC.speed / CPC_BASE_FREQUENCY_MHZ;
  perfTicksOffset = static_cast<uint64_t>((FRAME_PERIOD_MS / 1000.0) *
                                          perfFreq / speed_ratio);
  uint64_t const now = SDL_GetPerformanceCounter();
  perfTicksTarget = now + perfTicksOffset;
  perfTicksTargetFPS = now + perfFreq;  // 1 second from now
  LOG_VERBOSE("Timing: perfFreq="
              << perfFreq << " perfTicksOffset=" << perfTicksOffset << " ("
              << (perfTicksOffset * 1000.0 / perfFreq) << "ms/frame)"
              << " speed_ratio=" << speed_ratio);
}
}  // namespace

// Recalculate emulation speed (to verify, seems to work reasonably well)
void update_cpc_speed() { update_timings(); }

std::string getConfigurationFilename(bool forWrite) {
  int const mode = R_OK | (F_OK * forWrite);

  const char* PATH_OK = "";

  std::string const binPathStr = binPath.string();
  std::vector<std::pair<const char*, std::string>> const configPaths = {
      {PATH_OK, args.cfgFilePath},  // First look in any user supplied
                                    // configuration file path
      {chAppPath,
       "/koncepcja.cfg"},  // koncepcja.cfg in the current working directory
      {binPathStr.c_str(),
       "/koncepcja.cfg"},  // koncepcja.cfg next to the binary (Finder launch)
      {getenv("XDG_CONFIG_HOME"), "/koncepcja/koncepcja.cfg"},
      {getenv("HOME"), "/.config/koncepcja/koncepcja.cfg"},
      {getenv("XDG_CONFIG_HOME"), "/koncepcja.cfg"},  // legacy flat paths
      {getenv("HOME"), "/.config/koncepcja.cfg"},
      {getenv("HOME"), "/.koncepcja.cfg"},
      {DESTDIR, "/etc/koncepcja.cfg"},
      {binPath.string().c_str(),
       "/../Resources/koncepcja.cfg"},  // To find the configuration from the
                                        // bundle on MacOS
  };

  for (const auto& p : configPaths) {
    // Skip paths using getenv if it returned NULL (i.e environment variable not
    // defined)
    if (!p.first) continue;
    std::string s = std::string(p.first) + p.second;
    if (access(s.c_str(), mode) == 0) {
      std::cout << "Using configuration file" << (forWrite ? " to save" : "")
                << ": " << s << '\n';
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

  std::cout << "No valid configuration file found, using empty config." << '\n';
  return "";
}

void loadConfiguration(t_CPC& CPC, const std::string& configFilename) {
  config::Config conf;
  conf.parseFile(configFilename);
  conf.setOverrides(args.cfgOverrides);

  std::string const appPath = chAppPath;

  // Read helpers: read_flag keeps the classic bit-0 semantics of the on/off
  // options (any even value reads as off); read_clamped falls back to the
  // default whenever the stored value is outside the legal range — including
  // negatives, which the old unsigned-wrap checks also rejected.
  const auto read_flag = [&conf](const char* sect, const char* key, int def) {
    return conf.getIntValue(sect, key, def) & 1;
  };
  const auto read_clamped = [&conf](const char* sect, const char* key, int def,
                                    int lo, int hi) {
    const int v = conf.getIntValue(sect, key, def);
    return (v >= lo && v <= hi) ? v : def;
  };

  CPC.model = read_clamped("system", "model", 2, 0, 3);  // default: CPC 6128
  CPC.jumpers = conf.getIntValue("system", "jumpers", 0x1e) &
                0x1e;  // OEM is Amstrad, video refresh is 50Hz
  CPC.ram_size = conf.getIntValue("system", "ram_size", 128);
  if (!is_valid_ram_size(CPC.ram_size)) {
    CPC.ram_size = 128;  // default to 128KB
  }
  if ((CPC.model >= 2) && (CPC.ram_size < 128)) {
    CPC.ram_size = 128;  // minimum RAM size for CPC 6128 is 128KB
  }
  // Silicon Disc: battery-backed 256K RAM (banks 4-7)
  g_silicon_disc.enabled = conf.getIntValue("system", "silicon_disc", 0) != 0;
  if (g_silicon_disc.enabled) {
    silicon_disc_init(g_silicon_disc);
  }

  CPC.speed = read_clamped("system", "speed", DEF_SPEED_SETTING,
                           MIN_SPEED_SETTING, MAX_SPEED_SETTING);
  CPC.limit_speed = read_flag("system", "limit_speed", 1);
  CPC.frameskip = read_flag("system", "frameskip", 0);
  CPC.auto_pause = read_flag("system", "auto_pause", 1);
  CPC.boot_time = conf.getIntValue("system", "boot_time", 5);
  CPC.printer = read_flag("system", "printer", 0);
  CPC.mf2 = read_flag("system", "mf2", 0);
  CPC.keyboard = read_clamped("system", "keyboard", 0, 0, MAX_ROM_MODS);

  // The sub-cycle board scans the keyboard matrix per-cycle, so a Direct
  // release can drop a key pressed after the frame-start snapshot but before
  // its row is scanned; BufferedUntilRead holds each key until the firmware
  // actually reads its row. An explicit value in the config or a
  // `-O system.keyboard_support_mode=N` override always wins (getIntValue
  // only returns the default when the key is absent).
  const int ksm_default =
      static_cast<int>(KeyboardSupportMode::BufferedUntilRead);
  int ksm = conf.getIntValue("system", "keyboard_support_mode", ksm_default);
  if (ksm < 0 || ksm >= static_cast<int>(KeyboardSupportMode::Last)) {
    ksm = ksm_default;
  }
  CPC.keyboard_support_mode = static_cast<KeyboardSupportMode>(ksm);

  {
    int joy_emu_val = conf.getIntValue("system", "joystick_emulation", 0);
    if (joy_emu_val < 0 ||
        joy_emu_val >= static_cast<int>(JoystickEmulation::Last)) {
      LOG_WARNING("Invalid joystick_emulation value "
                  << joy_emu_val << " in configuration. Defaulting to 'off'.");
      joy_emu_val = static_cast<int>(JoystickEmulation::None);
    }
    CPC.joystick_emulation = static_cast<JoystickEmulation>(joy_emu_val);
  }
  CPC.joysticks = read_flag("system", "joysticks", 1);
  CPC.joystick_menu_button =
      conf.getIntValue("system", "joystick_menu_button", 9) - 1;
  CPC.joystick_vkeyboard_button =
      conf.getIntValue("system", "joystick_vkeyboard_button", 10) - 1;
  CPC.resources_path =
      conf.getStringValue("system", "resources_path", appPath + "/resources");

  CPC.devtools_scale = conf.getIntValue("devtools", "scale", 1);
  CPC.devtools_max_stack_size =
      conf.getIntValue("devtools", "max_stack_size", 50);

  {
    int const wl = conf.getIntValue("ui", "workspace_layout", 0);
    CPC.workspace_layout = (wl == 1) ? t_CPC::WorkspaceLayoutMode::Docked
                                     : t_CPC::WorkspaceLayoutMode::Classic;
    int const ss = conf.getIntValue("ui", "cpc_screen_scale", 0);
    CPC.cpc_screen_scale = (ss >= 0 && ss <= 3)
                               ? static_cast<t_CPC::ScreenScale>(ss)
                               : t_CPC::ScreenScale::Fit;
  }

  // Default 1x (index 1). NOTE: scr_scale indexes sf[]={0,1,1.5,2,3}, so "2"
  // means 1.5x, not 2x — the old default started every fresh launch at 1.5x.
  CPC.scr_scale = conf.getIntValue("video", "scr_scale", 1);
  CPC.scr_preserve_aspect_ratio =
      conf.getIntValue("video", "scr_preserve_aspect_ratio", 1);
  CPC.scr_crt_aspect = conf.getIntValue("video", "scr_crt_aspect", 1);
  CPC.scr_vsync = conf.getIntValue("video", "vsync", 1);
  CPC.scr_style = conf.getIntValue("video", "scr_style", 1);
  // Phase 7c.1b scr_style remap — GL plugins removed and their slots
  // (0-10) replaced in-place by the SDL3 GPU implementations.  The old
  // explicit "(GPU)"-suffixed entries that used to sit at 17-27 are
  // gone; the CRT (GPU) trio moved to the freed 17-19 indices.  The
  // SDL_Renderer plugins at 11-16 are unchanged.
  //
  // Pre-Phase-7b configs may also reference 28/29/30 (the old CRT GPU
  // positions before Phase 7b shifted them down to 25/26/27).
  //
  // Remap table:
  //   17        -> 0   (old Direct (GPU)            -> Direct)
  //   18..24    -> 4..10 (old swscale (GPU) family  -> swscale)
  //   25..27    -> 17..19 (old CRT (GPU) trio       -> CRT)
  //   28..30    -> 17..19 (pre-7b CRT (GPU) trio    -> CRT)
  //
  // Indices 0-16 in older configs already point at the right plugin
  // (just GPU-backed now instead of GL-backed) so no remap needed.
  {
    unsigned int const s = CPC.scr_style;
    unsigned int remapped = s;
    // Indices 0..19 are ALL valid CURRENT plugins (incl. the CRT shaders at
    // 17/18/19), so never remap an in-range value — it is the user's real
    // choice. Only migrate genuinely-stale configs whose index is out of the
    // current range (>= 20), left over from the pre-7c.1b GL era. (Remapping
    // in-range CRT indices silently swapped CRT Lottes for Scale2x and broke
    // multi-viewport, since swscale plugins don't render detached windows.)
    if (s >= 20 && s <= 24)
      remapped = s - 14;  // old swscale (GPU) -> swscale (6..10)
    else if (s >= 25 && s <= 27)
      remapped = s - 8;  // old CRT (GPU)    -> CRT (17..19)
    else if (s >= 28 && s <= 30)
      remapped = s - 11;  // pre-7b CRT (GPU) -> CRT (17..19)
    if (remapped != s && remapped < video_plugin_list.size()) {
      LOG_INFO("scr_style=" << s
                            << " is obsolete after Phase 7c.1b — "
                               "remapped to "
                            << remapped << " ("
                            << video_plugin_list[remapped].name << ")");
      CPC.scr_style = remapped;
    }
  }
  if (CPC.scr_style >= video_plugin_list.size()) {
    CPC.scr_style = DEFAULT_VIDEO_PLUGIN;
    LOG_ERROR("Unsupported video plugin specified - defaulting to plugin "
              << video_plugin_list[DEFAULT_VIDEO_PLUGIN].name);
  }
  CPC.scr_oglfilter = read_flag("video", "scr_oglfilter", 1);
  CPC.scr_oglscanlines = read_clamped("video", "scr_oglscanlines", 30, 0, 100);
  CPC.scr_scanlines = read_flag("video", "scr_scanlines", 0);
  CPC.scr_led = read_flag("video", "scr_led", 1);
  CPC.scr_fps = read_flag("video", "scr_fps", 0);
  CPC.scr_tube = read_flag("video", "scr_tube", 0);
  CPC.scr_intensity = read_clamped("video", "scr_intensity", 10, 5, 15);
  CPC.scr_remanency = read_flag("video", "scr_remanency", 0);
  CPC.scr_window = read_flag("video", "scr_window", 1);

  CPC.scr_green_mode = read_flag("video", "scr_green_mode", 0);
  CPC.scr_green_blue_percent =
      conf.getIntValue("video", "scr_green_blue_percent", 0);

  CPC.snd_enabled = read_flag("sound", "enabled", 1);
  CPC.snd_playback_rate =
      read_clamped("sound", "playback_rate", 2, 0, MAX_FREQ_ENTRIES - 1);
  CPC.snd_bits = read_flag("sound", "bits", 1);
  CPC.snd_stereo = read_flag("sound", "stereo", 1);
  // Sub-cycle engine (Milestone B): its audio contract is fixed at stereo
  // s16 44.1 kHz, so force the SDL stream format to match before audio init.
  // The sub-cycle pin-level board is THE engine (Gate C retired the legacy
  // core; the [system] engine flag is gone with it — an `engine=` line in an
  // old config is simply never read).
  {  // [system] run_tier: 0=auto (Fast; Wake while debugging — the default,
     // user decision 2026-07-10), 1=fast, 2=wake, 3=soldered, 4=faithful.
    int pol = conf.getIntValue("system", "run_tier", 0);
    if (pol < 0 || pol > 4) pol = 0;
    subcycle_bridge_set_tier_policy(static_cast<BridgeTierPolicy>(pol));
  }
  CPC.snd_playback_rate = 2;  // 44100 — the board's fixed output format
  CPC.snd_bits = 1;
  CPC.snd_stereo = 1;
  CPC.snd_volume = read_clamped("sound", "volume", 80, 0, 100);
  CPC.snd_pp_device = read_flag("sound", "pp_device", 0);
  g_amdrum.enabled = read_flag("sound", "amdrum", 0) != 0;
  g_drive_sounds.disk_enabled = read_flag("sound", "disk_sounds", 0) != 0;
  g_drive_sounds.tape_enabled = read_flag("sound", "tape_sounds", 0) != 0;
  tape_line_out_set_volume(conf.getIntValue("sound", "tape_data_volume", 35) /
                           100.0f);
  // Drive Sound Lab tuning (params + volume/pan). Applied to
  // g_drive_sounds.params here; drive_sounds_init() re-bakes from them later,
  // in audio_init().
  drive_sounds_params_from_string(
      conf.getStringValue("sound", "drivesnd_params", ""));
  g_smartwatch.enabled = read_flag("system", "smartwatch", 0) != 0;
  g_amx_mouse.enabled = read_flag("input", "amx_mouse", 0) != 0;
  // Light gun selection (0=off, 1=Amstrad Magnum Phaser, 2=Trojan Light
  // Phazer). Mirrors the F-key toggle; lets headless/CI runs and config files
  // enable a gun (the IPC 'input gun' contract keys off phazer_emulation).
  // Clamp to the valid enum range: a bad config value (e.g. 99) must not
  // produce an out-of-range PhazerType.
  CPC.phazer_emulation = static_cast<PhazerType::Value>(
      std::clamp(conf.getIntValue("input", "lightgun", 0), 0,
                 static_cast<int>(PhazerType::TrojanLightPhazer)));
  if (!CPC.phazer_emulation) CPC.phazer_pressed = false;

  g_symbiface.enabled = read_flag("peripheral", "symbiface", 0) != 0;
  g_m4board.enabled = read_flag("peripheral", "m4board", 0) != 0;
  g_m4board.sd_root_path = conf.getStringValue("peripheral", "m4_sd_path", "");
  g_m4board.rom_slot = conf.getIntValue("peripheral", "m4_rom_slot", 6);
  CPC.m4_http_port = conf.getIntValue("peripheral", "m4_http_port", 8080);
  CPC.m4_bind_ip = conf.getStringValue("peripheral", "m4_bind_ip", "127.0.0.1");
  // Load port mappings (m4_port_map_N = cpc_port:host_port:user_override)
  for (int i = 0; i < 16; i++) {
    char key[32];
    snprintf(key, sizeof(key), "m4_port_map_%d", i);
    std::string const val = conf.getStringValue("peripheral", key, "");
    if (val.empty()) break;
    // Parse "cpc:host:override"
    uint16_t cpc_port = 0, host_port = 0;
    int user_override = 0;
    if (sscanf(val.c_str(), "%hu:%hu:%d", &cpc_port, &host_port,
               &user_override) >= 2) {
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
    scfg.input_file =
        conf.getStringValue("peripheral", "serial_input_file", "");
    scfg.output_file =
        conf.getStringValue("peripheral", "serial_output_file", "");
    scfg.device_path = conf.getStringValue("peripheral", "serial_device", "");
    scfg.tcp_host =
        conf.getStringValue("peripheral", "serial_tcp_host", "127.0.0.1");
    scfg.tcp_port = static_cast<uint16_t>(
        conf.getIntValue("peripheral", "serial_tcp_port", 23));
    scfg.baud_rate = conf.getIntValue("peripheral", "serial_baud", 9600);
    g_serial_interface.set_config(scfg);
    g_serial_interface.apply_config();
  }

  CPC.kbd_layout =
      conf.getStringValue("control", "kbd_layout", "keymap_us.map");

  CPC.max_tracksize = conf.getIntValue("file", "max_track_size", 6144 - 154);
  CPC.current_snap_path = CPC.snap_path =
      conf.getStringValue("file", "snap_path", appPath + "/snap/");
  CPC.current_cart_path = CPC.cart_path =
      conf.getStringValue("file", "cart_path", appPath + "/cart/");
  CPC.current_dsk_path = CPC.dsk_path =
      conf.getStringValue("file", "dsk_path", appPath + "/disk/");
  CPC.current_tape_path = CPC.tape_path =
      conf.getStringValue("file", "tape_path", appPath + "/tape/");

  int iFmt = FIRST_CUSTOM_DISK_FORMAT;
  for (int i = iFmt; i < MAX_DISK_FORMAT;
       i++) {  // loop through all user definable disk formats
    char chFmtId[14];
    snprintf(chFmtId, sizeof(chFmtId), "fmt%02d", i);  // build format ID
    std::string const formatString = conf.getStringValue("file", chFmtId, "");
    disk_format[iFmt] = parseDiskFormat(formatString);
    if (!disk_format[iFmt]
             .label.empty()) {  // found format definition for this slot?
      iFmt++;                   // entry is valid
    }
  }
  CPC.printer_file =
      conf.getStringValue("file", "printer_file", appPath + "/printer.dat");
  CPC.sdump_dir =
      conf.getStringValue("file", "sdump_dir", appPath + "/screenshots");

  CPC.rom_path = conf.getStringValue("rom", "rom_path", appPath + "/rom/");
  for (int iRomNum = 0; iRomNum < MAX_ROM_SLOTS;
       iRomNum++) {  // loop for ROMs 0-31
    char chRomId[14];
    snprintf(chRomId, sizeof(chRomId), "slot%02d", iRomNum);  // build ROM ID
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

  CPC.cartridge.file =
      CPC.rom_path +
      "/system.cpr";  // Only default path defined. Needed for CPC6128+
}

// Serialize the live settings back into the INI model and write it out.
// Mirror image of loadConfiguration: every key written here is read there,
// same section, same name, same encoding (flags as 0/1 ints).
bool saveConfiguration(t_CPC& CPC, const std::string& configFilename) {
  config::Config conf;

  conf.setIntValue("system", "model", CPC.model);
  conf.setIntValue("system", "jumpers", CPC.jumpers);
  conf.setIntValue("system", "ram_size", CPC.ram_size);
  conf.setIntValue("system", "limit_speed", CPC.limit_speed);
  conf.setIntValue("system", "frameskip", CPC.frameskip);
  conf.setIntValue("system", "speed", CPC.speed);
  conf.setIntValue("system", "auto_pause", CPC.auto_pause);
  conf.setIntValue("system", "printer", CPC.printer);
  conf.setIntValue("system", "mf2", CPC.mf2);
  conf.setIntValue("system", "keyboard", CPC.keyboard);
  conf.setIntValue("system", "keyboard_support_mode",
                   static_cast<int>(CPC.keyboard_support_mode));
  conf.setIntValue("system", "boot_time", CPC.boot_time);
  conf.setIntValue("system", "joystick_emulation",
                   static_cast<int>(CPC.joystick_emulation));
  conf.setIntValue("system", "joysticks", CPC.joysticks);
  conf.setIntValue("system", "joystick_menu_button",
                   CPC.joystick_menu_button + 1);
  conf.setIntValue("system", "joystick_vkeyboard_button",
                   CPC.joystick_vkeyboard_button + 1);
  conf.setStringValue("system", "resources_path", CPC.resources_path);

  conf.setIntValue("video", "scr_scale", CPC.scr_scale);
  conf.setIntValue("video", "scr_preserve_aspect_ratio",
                   CPC.scr_preserve_aspect_ratio);
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

  conf.setIntValue("ui", "workspace_layout",
                   static_cast<int>(CPC.workspace_layout));
  conf.setIntValue("ui", "cpc_screen_scale",
                   static_cast<int>(CPC.cpc_screen_scale));

  conf.setIntValue("video", "scr_green_mode", CPC.scr_green_mode);
  conf.setIntValue("video", "scr_green_blue_percent",
                   CPC.scr_green_blue_percent);

  conf.setIntValue("sound", "enabled", CPC.snd_enabled);
  conf.setIntValue("sound", "playback_rate", CPC.snd_playback_rate);
  conf.setIntValue("sound", "bits", CPC.snd_bits);
  conf.setIntValue("sound", "stereo", CPC.snd_stereo);
  conf.setIntValue("sound", "volume", CPC.snd_volume);
  conf.setIntValue("sound", "pp_device", CPC.snd_pp_device);
  conf.setIntValue("sound", "amdrum", g_amdrum.enabled ? 1 : 0);
  conf.setIntValue("sound", "disk_sounds", g_drive_sounds.disk_enabled ? 1 : 0);
  conf.setIntValue("sound", "tape_sounds", g_drive_sounds.tape_enabled ? 1 : 0);
  conf.setIntValue("sound", "tape_data_volume",
                   static_cast<int>((tape_line_out_volume() * 100.0f) + 0.5f));
  conf.setStringValue("sound", "drivesnd_params",
                      drive_sounds_params_to_string());
  conf.setIntValue("system", "smartwatch", g_smartwatch.enabled ? 1 : 0);
  conf.setIntValue("input", "amx_mouse", g_amx_mouse.enabled ? 1 : 0);

  conf.setIntValue("peripheral", "symbiface", g_symbiface.enabled ? 1 : 0);
  conf.setStringValue("peripheral", "ide_master",
                      g_symbiface.ide_master.image_path);
  conf.setStringValue("peripheral", "ide_slave",
                      g_symbiface.ide_slave.image_path);
  conf.setIntValue("peripheral", "m4board", g_m4board.enabled ? 1 : 0);
  conf.setStringValue("peripheral", "m4_sd_path", g_m4board.sd_root_path);
  conf.setIntValue("peripheral", "m4_rom_slot", g_m4board.rom_slot);
  conf.setIntValue("peripheral", "m4_http_port", CPC.m4_http_port);
  conf.setStringValue("peripheral", "m4_bind_ip", CPC.m4_bind_ip);

  // Serial Interface config
  {
    auto cfg = g_serial_interface.get_config();
    conf.setIntValue("peripheral", "serial_enabled", cfg.enabled ? 1 : 0);
    conf.setIntValue("peripheral", "serial_backend",
                     static_cast<int>(cfg.backend_type));
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
    size_t const count = mappings.size() < 16 ? mappings.size() : 16;
    for (size_t i = 0; i < count; i++) {
      char key[48], val[64];
      snprintf(key, sizeof(key), "m4_port_map_%zu", i);
      snprintf(val, sizeof(val), "%d:%d:%d", mappings[i].cpc_port,
               mappings[i].host_port, mappings[i].user_override ? 1 : 0);
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

  for (int iFmt = FIRST_CUSTOM_DISK_FORMAT; iFmt < MAX_DISK_FORMAT;
       iFmt++) {  // loop through all user definable disk formats
    char chFmtId[14];
    snprintf(chFmtId, sizeof(chFmtId), "fmt%02d", iFmt);  // build format ID
    conf.setStringValue("file", chFmtId,
                        serializeDiskFormat(disk_format[iFmt]));
  }
  conf.setStringValue("file", "printer_file", CPC.printer_file);
  conf.setStringValue("file", "sdump_dir", CPC.sdump_dir);

  conf.setStringValue("rom", "rom_path", CPC.rom_path);
  for (int iRomNum = 0; iRomNum < MAX_ROM_SLOTS;
       iRomNum++) {  // loop for ROMs 0-31
    char chRomId[14];
    snprintf(chRomId, sizeof(chRomId), "slot%02d", iRomNum);  // build ROM ID
    conf.setStringValue("rom", chRomId, CPC.rom_file[iRomNum]);
  }
  conf.setStringValue("rom", "rom_mf2", CPC.rom_mf2);

  // Save MRU (recent files) lists
  for (int i = 0; i < t_CPC::MRU_MAX; i++) {
    char key[16];
    snprintf(key, sizeof(key), "mru_disk_%02d", i);
    conf.setStringValue(
        "file", key,
        i < static_cast<int>(CPC.mru_disks.size()) ? CPC.mru_disks[i] : "");
    snprintf(key, sizeof(key), "mru_tape_%02d", i);
    conf.setStringValue(
        "file", key,
        i < static_cast<int>(CPC.mru_tapes.size()) ? CPC.mru_tapes[i] : "");
    snprintf(key, sizeof(key), "mru_snap_%02d", i);
    conf.setStringValue(
        "file", key,
        i < static_cast<int>(CPC.mru_snaps.size()) ? CPC.mru_snaps[i] : "");
    snprintf(key, sizeof(key), "mru_cart_%02d", i);
    conf.setStringValue(
        "file", key,
        i < static_cast<int>(CPC.mru_carts.size()) ? CPC.mru_carts[i] : "");
  }

  return conf.saveToFile(configFilename);
}

// As long as a GUI is enabled, we must show the cursor.
// Because we can activate multiple GUIs at a time, we need to keep track of how
// many times we've been asked to show or hide cursor.
void set_cursor_visibility(bool show) {
  static int shows_count = 1;
  if (show) {
    shows_count++;
  } else {
    shows_count--;
  }
  shows_count = std::max(shows_count, 0);
  if (shows_count > 0) {
    SDL_ShowCursor();
  } else {
    SDL_HideCursor();
  }
}

namespace {
bool userConfirmsQuitWithoutSaving() {
  auto result = pfd::message("Unsaved Changes",
                             "You have unsaved changes to a disk. Quit anyway?",
                             pfd::choice::yes_no, pfd::icon::warning)
                    .result();
  return result == pfd::button::yes;
}
}  // namespace

namespace {
void showGui();
}  // namespace
namespace {
void showVKeyboard();
}  // namespace
namespace {
void showVJoystick();
}  // namespace
namespace {
void dumpSnapshot();
}  // namespace
namespace {
void loadSnapshot();
}  // namespace
namespace {
void showVKeyboard() {
  imgui_state.show_vkeyboard = !imgui_state.show_vkeyboard;
}
}  // namespace
namespace {
void showVJoystick() {
  imgui_state.show_vjoystick = !imgui_state.show_vjoystick;
}
}  // namespace

void koncpc_queue_virtual_keys(const std::string& text) {
  // Single scan-synced path: feed the legacy \a/\f encoding into the
  // AutoTypeQueue, which the Z80 thread drains in sync with the firmware's
  // keyboard scans (speed-independent).
  g_autotype_queue.enqueue_legacy(text);
}

void koncpc_menu_action(int action) {
  switch (action) {
    case KONCPC_GUI: {
      showGui();
      break;
    }

    case KONCPC_VKBD: {
      showVKeyboard();
      break;
    }

    case KONCPC_VJOY: {
      showVJoystick();
      break;
    }

    case KONCPC_DEVTOOLS: {
      imgui_state.show_devtools = !imgui_state.show_devtools;
      log_verbose = imgui_state.show_devtools;
      if (imgui_state.show_devtools) {
        set_osd_message("Debug mode: on");
      } else {
        g_devtools_ui.close_all_windows();
        set_osd_message("Debug mode: off");
      }
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
      // Pause the autotype queue for boot_time frames — a reasonable wait for
      // the Plus transition between the F1/F2 nag screen and the command line.
      // Prefer the ~PAUSE n~ autocmd syntax for explicit control.
      LOG_VERBOSE("Take into account KONCPC_DELAY");
      g_autotype_queue.enqueue("~PAUSE " + std::to_string(CPC.boot_time) + "~");
      break;

    case KONCPC_WAITBREAK:
      // Arm a breakpoint; the autotype queue blocks (apply_cmd returns true)
      // until it fires and the Z80 thread calls g_autotype_queue.resume().
      LOG_INFO("Autotype: waiting for next breakpoint before continuing.");
      LOG_VERBOSE("Setting z80.break_point=0 (was " << z80.break_point << ").");
      z80.break_point = 0;
      break;

    case KONCPC_SNAPSHOT:
      dumpSnapshot();
      break;

    case KONCPC_LD_SNAP:
      loadSnapshot();
      break;

    case KONCPC_TAPEPLAY:
      LOG_VERBOSE("Request to play tape");
      if (!pbTapeImage.empty()) {
        if (CPC.tape_play_button) {
          LOG_VERBOSE("Play button released");
          CPC.tape_play_button = 0;
        } else {
          LOG_VERBOSE("Play button pushed");
          CPC.tape_play_button = 0x10;
        }
      }
      set_osd_message(std::string("Play tape: ") +
                      (CPC.tape_play_button ? "on" : "off"));
      break;

    case KONCPC_MF2STOP:
      if (CPC.mf2) subcycle_bridge_mf2_stop();
      break;

    case KONCPC_RESET:
      LOG_VERBOSE("User requested emulator reset");
      emulator_reset();
      break;

    case KONCPC_JOY:
      CPC.joystick_emulation = nextJoystickEmulation(CPC.joystick_emulation);
      CPC.InputMapper->set_joystick_emulation();
      SDL_SetWindowRelativeMouseMode(
          mainSDLWindow, CPC.joystick_emulation == JoystickEmulation::Mouse);
      set_osd_message(std::string("Joystick emulation: ") +
                      JoystickEmulationToString(CPC.joystick_emulation));
      break;

    case KONCPC_PHAZER:
      CPC.phazer_emulation = CPC.phazer_emulation.Next();
      if (!CPC.phazer_emulation) CPC.phazer_pressed = false;
      mouse_init();
      set_osd_message(std::string("Phazer emulation: ") +
                      CPC.phazer_emulation.ToString());
      break;

    case KONCPC_PASTE:
      set_osd_message("Pasting...");
      {
        auto content = std::string(SDL_GetClipboardText());
        LOG_VERBOSE("Pasting '" << content << "'");
        koncpc_queue_virtual_keys(content);
        break;
      }

    case KONCPC_EXIT:
      cleanExit(0);
      break;

    case KONCPC_FPS:
      CPC.scr_fps = CPC.scr_fps ? 0 : 1;  // toggle fps display on or off
      set_osd_message(std::string("Performances info: ") +
                      (CPC.scr_fps ? "on" : "off"));
      break;

    case KONCPC_SPEED: {
      // koncpc_menu_action() runs on the main thread and the IPC thread, so the
      // debounce timestamp must be atomic to avoid a data race.
      static std::atomic<uint64_t> last_speed_toggle{0};
      uint64_t const now = SDL_GetPerformanceCounter();
      if (now - last_speed_toggle.load(std::memory_order_relaxed) >
          SDL_GetPerformanceFrequency() / 10) {
        CPC.limit_speed = CPC.limit_speed ? 0 : 1;
        set_osd_message(std::string("Limit speed: ") +
                        (CPC.limit_speed ? "on" : "off"));
        last_speed_toggle.store(now, std::memory_order_relaxed);
      }
      break;
    }

    case KONCPC_TIER: {
      // Cycle the sub-cycle engine's run-tier policy (Shift+F9). Only
      // meaningful while the pin-level engine runs; env-pinned tiers stay
      // pinned (the OSD says which applies).
      if (!subcycle_bridge_active()) {
        set_osd_message("Run tier: board not running");
        break;
      }
      if (subcycle_bridge_tier_env_pinned()) {
        set_osd_message(std::string("Run tier: pinned by env (") +
                        subcycle_bridge_effective_tier_label() + ")");
        break;
      }
      BridgeTierPolicy next = BridgeTierPolicy::Auto;
      const char* label = "Automatic (Performance; Balanced while debugging)";
      switch (subcycle_bridge_tier_policy()) {
        case BridgeTierPolicy::Auto:
          next = BridgeTierPolicy::Fast;
          label = "Performance — instruction-stepped";
          break;
        case BridgeTierPolicy::Fast:
          next = BridgeTierPolicy::Wake;
          label = "Balanced — cycle-stepped, event-driven";
          break;
        default:  // Balanced / Microscope tiers cycle back to Automatic
          next = BridgeTierPolicy::Auto;
          label = "Automatic (Performance; Balanced while debugging)";
          break;
      }
      subcycle_bridge_set_tier_policy(next);
      set_osd_message(std::string("Run tier: ") + label);
      break;
    }

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
      set_osd_message(std::string("Debug mode: ") +
                      (log_verbose ? "on" : "off"));
      break;

    case KONCPC_NEXTDISKA:
      CPC.driveA.zip_index += 1;
      file_load(CPC.driveA);
      break;
  }
}

namespace {
void showGui() {
  imgui_state.show_menu = true;
  imgui_state.menu_just_opened = true;
  cpc_pause();  // keep CPC.paused and g_emu_paused in sync
}
}  // namespace

// TODO: Dedupe with the version in CapriceDevTools
// TODO: Support watchpoints too
namespace {
void loadBreakpoints() {
  if (args.symFilePath.empty()) return;
  Symfile const symfile(args.symFilePath);
  for (auto breakpoint : symfile.Breakpoints()) {
    if (std::find_if(breakpoints.begin(), breakpoints.end(),
                     [&](const auto& bp) {
                       return bp.address == breakpoint;
                     }) != breakpoints.end())
      continue;
    breakpoints.emplace_back(breakpoint);
  }
  // Populate global symbol table from symfile
  for (const auto& [addr, name] : symfile.Symbols()) {
    g_symfile.addSymbol(addr, name);
  }
}
}  // namespace

bool dumpScreenTo(const std::string& path) {
  // Read the latest PUBLISHED frame — the emulation's actual output. The
  // presented surface (video_render_surface) can go stale when the present
  // path stalls (occluded macOS window / remote desktop) even though the
  // emulation keeps producing frames; screenshots must not photograph that.
  // Falls back to the presented surface, then back_surface (headless).
  SDL_Surface* surf = video_ring_published_peek();
  if (!surf) surf = video_render_surface();
  if (!surf) surf = back_surface;
  if (!surf) return false;
  if (SDL_SavePNG(surf, path)) {
    LOG_ERROR("Could not write screenshot file to " + path);
    return false;
  }
  return true;
}

namespace {

// Build "<dir>/<prefix>_<timestamp><ext>", falling back to the current
// directory when the configured target directory is missing. Shared by the
// screenshot and machine-snapshot dumpers.
std::string timestamped_dump_path(const std::string& dir, const char* what,
                                  const char* prefix, const char* ext) {
  std::string target = dir;
  if (!is_directory(target)) {
    LOG_ERROR("Unable to find or open directory " + dir +
              " when trying to take a " + what +
              ". Defaulting to current directory.")
    target = ".";
  }
  return target + "/" + prefix + "_" + getDateString() + ext;
}

}  // namespace

void dumpScreen() {
  const std::string path =
      timestamped_dump_path(CPC.sdump_dir, "screenshot", "screenshot", ".png");
  LOG_INFO("Dumping screen to " + path);
  if (!dumpScreenTo(path)) {
    LOG_ERROR("Could not write screenshot file to " + path);
    set_osd_message("Screenshot failed");
  } else {
    set_osd_message("Captured " + path.substr(path.find_last_of('/') + 1));
  }
}

namespace {
void dumpSnapshot() {
  const std::string path = timestamped_dump_path(
      CPC.snap_path, "machine snapshot", "snapshot", ".sna");
  LOG_INFO("Dumping machine snapshot to " + path);
  if (snapshot_save(path)) {
    LOG_ERROR("Could not write machine snapshot to " + path);
    set_osd_message("Snapshot save failed");
  } else {
    set_osd_message("Snapshotted " + path.substr(path.find_last_of('/') + 1));
  }
  lastSavedSnapshot = path;
}
}  // namespace

namespace {
void loadSnapshot() {
  if (lastSavedSnapshot.empty()) return;
  LOG_INFO("Loading snapshot from " + lastSavedSnapshot);
  if (snapshot_load(lastSavedSnapshot)) {
    LOG_ERROR("Could not load machine snapshot from " + lastSavedSnapshot);
    std::string dirname, filename;
    stringutils::splitPath(lastSavedSnapshot, dirname, filename);
    set_osd_message("Snapshot load failed: " + filename);
  } else {
    std::string dirname, filename;
    stringutils::splitPath(lastSavedSnapshot, dirname, filename);
    set_osd_message("Restored " + filename);
  }
}
}  // namespace

bool driveAltered() { return driveA.altered || driveB.altered; }

namespace {
void doCleanUp() {
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
  //     paused/quiescent branch at the top of its loop.  abort() makes
  //     signal_ready a no-op and releases the render thread's wait so neither
  //     thread can be left spinning on the frame signal during teardown; then
  //     cpc_pause_and_wait() drives the Z80 to its quiescent paused branch.
  if (g_z80_thread.joinable() &&
      std::this_thread::get_id() != g_z80_thread.get_id()) {
    if (!g_z80_thread_quit.load(std::memory_order_relaxed)) {
      cpc_pause();
      g_frame_signal
          .abort();  // make signal_ready a no-op + release render wait
      cpc_pause_and_wait();
      g_z80_thread_quit.store(true, std::memory_order_relaxed);
      cpc_resume();
    }
    g_frame_signal.abort();  // belt-and-suspenders during teardown
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
  if (pfoDebug) {
    fclose(pfoDebug);
  }
#endif

  SDL_Quit();
}
}  // namespace

void cleanExit(int returnCode, bool askIfUnsaved) {
  // Only the main (render) thread is allowed to run SDL_Quit(): it's the
  // thread that owns the video subsystem and lives inside SDL_PumpEvents().
  // Any other thread calling doCleanUp() directly races with the pump loop
  // and can segfault at PC=0 when SDL nulls the video driver's PumpEvents
  // function pointer out from under it.  Macos UI ops (the confirm-quit
  // dialog below) are also main-thread-only.  So we check the thread FIRST,
  // before any UI, and route off-main callers through SDL_EVENT_QUIT:
  //
  //   - Z80 thread  → push SDL_EVENT_QUIT; loop exits via g_z80_thread_quit
  //   - IPC / HTTP / telnet / any other aux thread  → push SDL_EVENT_QUIT
  //   - Main thread → safe to run the confirm dialog + doCleanUp() + _exit()
  //
  // The render thread's SDL_EVENT_QUIT handler calls cleanExit() recursively,
  // at which point we land in the main-thread branch and do the real
  // teardown (including the confirm dialog if askIfUnsaved was set — the
  // returnCode carries through via g_z80_requested_exit_code).
  //
  // `is_not_main` is framed defensively: if g_main_thread_id hasn't been
  // captured yet (early-init path), we *assume* we're on main.  That
  // preserves the pre-fix behaviour during startup and keeps us from
  // infinitely pushing SDL_EVENT_QUIT to ourselves before the main loop
  // has a chance to spin.
  const auto tid = std::this_thread::get_id();
  const bool is_z80_self =
      g_z80_thread.joinable() && tid == g_z80_thread.get_id();
  const bool is_not_main =
      (g_main_thread_id != std::thread::id{}) && (tid != g_main_thread_id);

  if (is_z80_self || is_not_main) {
    g_z80_requested_exit_code.store(returnCode, std::memory_order_relaxed);
    if (is_z80_self) {
      // Z80 self-quit also has to break its own loop.  Aux threads don't
      // need this bit — the main thread handles shutdown orchestration.
      g_z80_thread_quit.store(true, std::memory_order_relaxed);
    }
    SDL_Event qe = {};
    qe.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&qe);
    return;
  }

  // Main thread (or early-init pre-capture): safe to prompt and tear down.
  if (!g_headless && askIfUnsaved && driveAltered() &&
      !userConfirmsQuitWithoutSaving()) {
    return;
  }
  doCleanUp();
  _exit(returnCode);
}

namespace {
void handle_mouse_joystick_button(const SDL_MouseButtonEvent& event,
                                  std::atomic<byte> keyboard_matrix[],
                                  bool pressed) {
  if (CPC.joystick_emulation == JoystickEmulation::Mouse) {
    if (event.button == 1)
      applyKeypress(CPC.InputMapper->CPCscancodeFromCPCkey(CPC_J0_FIRE1),
                    keyboard_matrix, pressed);
    if (event.button == 3)
      applyKeypress(CPC.InputMapper->CPCscancodeFromCPCkey(CPC_J0_FIRE2),
                    keyboard_matrix, pressed);
  }
}
}  // namespace

// Publish a consistent snapshot of the pending keyboard matrix into the live
// matrix that the CPC firmware actually scans.  Called once per frame, before
// z80_execute(), on whichever thread runs the emulation (the Z80 thread in GUI
// mode, the main thread in headless mode).  Holding g_kbd_matrix_mutex makes
// the 16-byte copy atomic with respect to any in-progress key+shift write on
// the main/IPC threads, so the firmware never scans a shifted key without its
// SHIFT line set ('1'->'&' on shifted-digit layouts).  See beads-2qg/d1n.
namespace {
void publish_keyboard_snapshot() {
  std::scoped_lock const matrix_lock(g_kbd_matrix_mutex);
  for (int i = 0; i < 16; i++) {
    keyboard_matrix_live[i].store(
        keyboard_matrix[i].load(std::memory_order_relaxed),
        std::memory_order_relaxed);
  }
}
}  // namespace

// Z80 emulation thread — runs z80_execute() and handles all emulation side
// effects. Used only in non-headless (GUI) mode; headless runs the original
// single-threaded path.
//
// At EC_FRAME_COMPLETE:
//   1. Completes per-frame work (autotype, session, IPC, etc.)
//   2. Calls asic_draw_sprites() — finalises the write buffer's pixels
//   3. Calls video_ring_publish() — publishes the frame + advances back_surface
//   4. Calls g_frame_signal.signal_ready() — wakes the render thread (no wait)
//   5. Immediately starts the next frame — the render thread reads the latest
//   published buffer independently and never blocks the Z80.
namespace {
void z80_thread_main() {
  dword iExitCondition = EC_FRAME_COMPLETE;
  static int consecutive_skips = 0;
  // Residual "render-wait" metric (from the U1 baseline): time the Z80 spends
  // in signal_ready() — now just a mutex+notify, so the [fps] log reads ~0,
  // confirming the decouple holds (it was 15-45% before the ring).
  static uint64_t s_render_wait_accum = 0;

  while (!g_z80_thread_quit.load(std::memory_order_relaxed)) {
    if (g_emu_paused.load(std::memory_order_relaxed)) {
      // Mark quiescent so cpc_pause_and_wait() callers know we are safe to
      // inspect.
      g_z80_quiescent.store(true, std::memory_order_release);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    // About to enter z80_execute() — mark non-quiescent.
    g_z80_quiescent.store(false, std::memory_order_release);

    // Publish a consistent snapshot of the pending keyboard state for this
    // frame's firmware scan (see publish_keyboard_snapshot).
    publish_keyboard_snapshot();

    // FPS counter: publish stats once per second
    {
      uint64_t const perfNow = SDL_GetPerformanceCounter();
      if (perfNow >= perfTicksTargetFPS) {
        dwFPS = dwFrameCount;
        dwFrameCount = 0;
        perfTicksTargetFPS = perfNow + perfFreq;

        // U1 baseline: average time/frame the Z80 was blocked in
        // wait_consumed() waiting for render, and the % of wall-clock spent
        // blocked. Both fall to ~0 once the Z80 no longer waits on render.
        double const render_wait_ms =
            dwFPS ? static_cast<double>(s_render_wait_accum) * 1000.0 /
                        static_cast<double>(perfFreq) / dwFPS
                  : 0.0;
        double const render_wait_pct =
            static_cast<double>(s_render_wait_accum) * 100.0 /
            static_cast<double>(perfFreq);
        s_render_wait_accum = 0;

        // Opt-in once-per-second FPS log (run with --fps, or --debug) — a
        // passive, tail-able steady-FPS readout that doesn't perturb the
        // emulation the way an IPC `wait vbl` probe does.
        if (g_log_fps || g_debug) {
          // Split the frame: machine = subcycle_bridge_frame (Z80+devices+blit+
          // debug_sync, the z80Start..z80End region); total = whole emu-loop
          // iteration. machine≈total localizes the cost inside the machine
          // frame; total>>machine points at the wrapper
          // (audio/keyboard/signal/timing).
          double const machine_ms =
              frameTimeSamples
                  ? static_cast<double>(z80TimeAccum) * 1000.0 /
                        static_cast<double>(perfFreq) / frameTimeSamples
                  : 0.0;
          double const total_ms =
              frameTimeSamples
                  ? static_cast<double>(frameTimeAccum) * 1000.0 /
                        static_cast<double>(perfFreq) / frameTimeSamples
                  : 0.0;
          printf(
              "[fps] %3u FPS  %3u%% speed  | render-wait %.1f ms/f (%2.0f%%)  "
              "| "
              "machine %.1f ms/f  total %.1f ms/f\n",
              dwFPS,
              dwFPS * 100u / static_cast<unsigned>(1000.0 / FRAME_PERIOD_MS),
              render_wait_ms, render_wait_pct, machine_ms, total_ms);
          fflush(stdout);
        }

        {
          std::scoped_lock const stats_lock(g_imgui_stats_mutex);
          if (frameTimeSamples > 0) {
            double const ticksToUs = 1000000.0 / static_cast<double>(perfFreq);
            imgui_state.frame_time_avg_us =
                static_cast<float>(static_cast<double>(frameTimeAccum) /
                                   frameTimeSamples * ticksToUs);
            imgui_state.frame_time_min_us = static_cast<float>(
                static_cast<double>(frameTimeMin) * ticksToUs);
            imgui_state.frame_time_max_us = static_cast<float>(
                static_cast<double>(frameTimeMax) * ticksToUs);
            imgui_state.display_time_avg_us = static_cast<float>(
                static_cast<double>(
                    displayTimeAccum.exchange(0, std::memory_order_relaxed)) /
                frameTimeSamples * ticksToUs);
            imgui_state.sleep_time_avg_us =
                static_cast<float>(static_cast<double>(sleepTimeAccum) /
                                   frameTimeSamples * ticksToUs);
            imgui_state.z80_time_avg_us =
                static_cast<float>(static_cast<double>(z80TimeAccum) /
                                   frameTimeSamples * ticksToUs);
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
            double const avg_bytes = audio_queue_sum_bytes / audio_push_count;
            int frame_size = CPC.snd_stereo ? 4 : 2;
            if (CPC.snd_bits == 0) frame_size /= 2;
            int const sample_rate = freq_table[CPC.snd_playback_rate];
            double const bytes_per_ms = sample_rate * frame_size / 1000.0;
            imgui_state.audio_queue_avg_ms =
                static_cast<float>(avg_bytes / bytes_per_ms);
            imgui_state.audio_queue_min_ms =
                static_cast<float>(audio_queue_min_bytes / bytes_per_ms);
            imgui_state.audio_push_interval_max_us = static_cast<float>(
                static_cast<double>(audio_push_interval_max) * 1000000.0 /
                perfFreq);
          }
          audio_underrun_count = 0;
          audio_near_underrun_count = 0;
          audio_push_count = 0;
          audio_queue_sum_bytes = 0;
          audio_queue_min_bytes = INT_MAX;
          audio_push_interval_max = 0;
        }  // g_imgui_stats_mutex
      }
    }

    // Speed limiter: spin/sleep until deadline on audio-driven cycle
    // boundaries. Forced on while autotype is active so the Z80 stays at real
    // time: the autotype is paced by the firmware's keyboard scans (with a
    // frame-based timeout fallback), which is only meaningful at real-time
    // speed — an uncapped Z80 would drain the queue during a busy CPU stretch
    // (e.g. while BASIC executes a typed command) and drop the keys it isn't
    // reading.
    static constexpr int MAX_CONSECUTIVE_SKIPS = 5;
    const bool limit_now = CPC.limit_speed || g_autotype_queue.is_active();
    if (limit_now && iExitCondition == EC_CYCLE_COUNT) {
      uint64_t const sleepStart = SDL_GetPerformanceCounter();
      if (sleepStart < perfTicksTarget) {
        uint64_t const remaining_ticks = perfTicksTarget - sleepStart;
        uint64_t const remaining_ms = (remaining_ticks * 1000) / perfFreq;
        if (remaining_ms > 2) {
          SDL_Delay(static_cast<Uint32>(remaining_ms - 2));
        }
        while (SDL_GetPerformanceCounter() < perfTicksTarget) {
          SDL_Delay(0);
        }
      }
      sleepTimeAccum += SDL_GetPerformanceCounter() - sleepStart;
      perfTicksTarget += perfTicksOffset;
      uint64_t const now = SDL_GetPerformanceCounter();
      if (!CPC.frameskip && perfTicksTarget + (3 * perfTicksOffset) < now) {
        perfTicksTarget = now + perfTicksOffset;
      }
    } else if (iExitCondition != EC_CYCLE_COUNT) {
      CPC.skip_rendering = false;
      consecutive_skips = 0;
    }

    // Frameskip decision at frame boundaries
    if (iExitCondition == EC_FRAME_COMPLETE && limit_now) {
      uint64_t const now = SDL_GetPerformanceCounter();
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
      dword const dwOffset = CPC.scr_pos - CPC.scr_base;
      if (VDU.scrln > 0) {
        CPC.scr_base = static_cast<byte*>(back_surface->pixels) +
                       (VDU.scrln * CPC.scr_line_offs);
      } else {
        CPC.scr_base = static_cast<byte*>(back_surface->pixels);
      }
      CPC.scr_pos = CPC.scr_base + dwOffset;
    }

    {
      uint64_t const z80Start = SDL_GetPerformanceCounter();
      if (subcycle_bridge_active()) {
        subcycle_bridge_sync_probe();  // list edits made while paused reach
                                       // the probe before this frame runs
        // Sub-cycle engine: one whole frame of the pin-level board. Keyboard
        // comes from the published matrix (so autotype/IPC/session all work);
        // audio uses the existing SDL push path; pacing is the bridge's own
        // 50 Hz deadline (the legacy limiter below only acts on
        // EC_CYCLE_COUNT, which this engine never emits).
        uint8_t rows[16];
        for (int i = 0; i < 16; i++)
          rows[i] = keyboard_matrix_live[i].load(std::memory_order_relaxed);
        if (tape_line_in_active())  // mic -> Schmitt -> the deck's line queue
          tape_line_in_pump(*subcycle_bridge_machine());
        // Blit at presentation rate, not emulation rate: the bridge's scaled
        // blit is a second full-frame format conversion (the machine's video
        // device already rendered into its own fb — that cost is inherent,
        // like the legacy renderer's). An uncapped run emits thousands of
        // frames a second while the render side consumes ~60; converting
        // every one of them was measured at ~2 ms/frame — a 6× cap on the
        // Fast tier under §8.3. Capped (50 Hz) sessions blit every frame as
        // before; consumers (display, screenshots, recorders) see a surface
        // at most one presentation period stale.
        static uint64_t s_last_blit = 0;
        const uint64_t blit_now = SDL_GetPerformanceCounter();
        const bool blit_due =
            limit_now ||
            blit_now - s_last_blit >= SDL_GetPerformanceFrequency() / 60;
        if (blit_due) s_last_blit = blit_now;
        const std::vector<int16_t>& frame_audio = subcycle_bridge_frame(
            rows, blit_due ? back_surface : nullptr, limit_now);
        if (!frame_audio.empty() && CPC.snd_enabled &&
            !g_emu_paused.load(std::memory_order_relaxed)) {
          audio_push_buffer(
              reinterpret_cast<const byte*>(frame_audio.data()),
              static_cast<int>(frame_audio.size() * sizeof(int16_t)));
        }
        if (tape_line_out_active())  // wires -> jack (data + motor carrier)
          tape_line_out_pump(*subcycle_bridge_machine());
        // Signal autotype only when the firmware ACTUALLY read the keyboard
        // this frame (machine-side PSG port-A read detection — the engine=1
        // equivalent of the legacy PPI read handler). Treating every
        // completed frame as a scan made autotype type blindly during BOOT,
        // before the firmware listens — eating the leading autocmd
        // characters (`run"hello` arrived as `un3hello` in the e2e dsk test).
        g_engine1_scanned_rows = subcycle_bridge_scanned_key_rows();
        if (g_engine1_scanned_rows != 0) g_keyboard_scanned = true;
        // Wave-1 debug shim: mirror bench lists into the probe, publish the
        // machine's registers into the legacy view struct, and surface a
        // latched probe hit as the legacy breakpoint flow (pause, DevTools,
        // IPC "wait bp") — all existing debug UX, pin-level truth.
        iExitCondition =
            subcycle_bridge_debug_sync() ? EC_BREAKPOINT : EC_FRAME_COMPLETE;
      } else {
        // The sub-cycle board is the only engine; without it there is no
        // frame to run (start failure is fatal at init).
        SDL_Delay(1);
        iExitCondition = EC_FRAME_COMPLETE;
      }
      z80TimeAccum += SDL_GetPerformanceCounter() - z80Start;
    }

    // Tape wave sample (sub-frame resolution, render thread reads this under
    // condvar)
    if (CPC.tape_motor && CPC.tape_play_button) {
      imgui_state.tape_wave_buf[imgui_state.tape_wave_head] = bTapeLevel;
      imgui_state.tape_wave_head =
          (imgui_state.tape_wave_head + 1) % ImGuiUIState::TAPE_WAVE_SAMPLES;
    }

    // Audio: PSG filled buffer — push to SDL
    if (iExitCondition == EC_SOUND_BUFFER) {
      if (!g_emu_paused.load(std::memory_order_relaxed)) {
        audio_push_buffer(pbSndBuffer.get(),
                          static_cast<int>(CPC.snd_buffersize));
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
        // Mid-frame pause: the render thread may be waiting in
        // try_wait_ready_for() for a frame that will never arrive (we stopped
        // before EC_FRAME_COMPLETE).  Send a skip wake-up so it unblocks, then
        // sees g_emu_paused=true and shows the paused overlay on its next
        // iteration.
        g_frame_signal.signal_ready(true);
        z80.step_in = 0;
        z80.step_out = 0;
        z80.step_out_addresses.clear();
      } else if (z80.step_in >= 2) {
        cpc_pause();
        g_frame_signal.signal_ready(true);  // same: unblock render thread
        z80.step_in = 0;
        z80.step_out = 0;
        z80.step_out_addresses.clear();
      } else {
        z80.break_point = Z80_BREAKPOINT_NONE;
        z80.trace = 1;
        // Release an autotype KONCPC_WAITBREAK that was waiting for this
        // break — or latch the break for a WAITBREAK still queued behind the
        // command that caused it (e.g. `call 0` executing before the queue
        // reaches its WAITBREAK).
        g_waitbreak_latch = true;
        g_autotype_queue.resume();
      }
    } else {
      if (z80.break_point == Z80_BREAKPOINT_NONE) {
        LOG_DEBUG("Rearming EC_BREAKPOINT.");
        z80.break_point = 0;
      }
    }

    if (iExitCondition == EC_FRAME_COMPLETE) {
      dwFrameCountOverall++;
      dwFrameCount++;

      // Engine=1: the sub-cycle PPI/PSG read the keyboard directly and bypass
      // the legacy PPI I/O handler that calls notify_scanned(). Relay the rows
      // the firmware actually scanned this frame so the KeyboardManager's
      // BufferedUntilRead mode can release a held key once its row was read
      // (matches the legacy per-read notify; no-op on engine=0).
      if (subcycle_bridge_active()) {
        // Captured once at frame run (read-and-clear source) — see
        // g_engine1_scanned_rows; a second take here would always read 0.
        uint16_t const scanned = g_engine1_scanned_rows;
        for (int krow = 0; krow < 16; ++krow)
          if (scanned & (1u << krow))
            // Value-aware: confirm only keys actually present in the snapshot
            // the firmware read this frame (keyboard_matrix_live is stable
            // between this frame's publish and here), so a key set mid-frame
            // stays held.
            g_keyboard_manager.notify_scanned(
                krow,
                keyboard_matrix_live[krow].load(std::memory_order_relaxed));
      }
      g_keyboard_manager.update(keyboard_matrix, dwFrameCountOverall);

      // Frame-to-frame timing
      {
        uint64_t const now = SDL_GetPerformanceCounter();
        if (lastFrameStart > 0) {
          uint64_t const elapsed = now - lastFrameStart;
          frameTimeAccum += elapsed;
          frameTimeMin = std::min(elapsed, frameTimeMin);
          frameTimeMax = std::max(elapsed, frameTimeMax);
          frameTimeSamples++;
        }
        lastFrameStart = now;
      }

      // Exit-after checks (--exit-after N frames or N ms)
      if (g_exit_mode == EXIT_FRAMES && dwFrameCountOverall >= g_exit_target) {
        cleanExit(0, false);
      }
      if (g_exit_mode == EXIT_MS &&
          (SDL_GetTicks() - g_exit_start_ticks) >= g_exit_target) {
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
            static_cast<const uint8_t*>(back_surface->pixels), back_surface->w,
            back_surface->h, back_surface->pitch);
      }

      // Session recording: keyboard snapshot per frame
      if (g_session.state() == SessionState::RECORDING) {
        static uint8_t prev_matrix[16] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                          0xFF, 0xFF, 0xFF, 0xFF};
        for (int row = 0; row < 16; row++) {
          byte const cur = keyboard_matrix[row].load(std::memory_order_relaxed);
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
            int const row = (evt.data >> 8) & 0x0F;
            keyboard_matrix[row].store(static_cast<byte>(evt.data & 0xFF),
                                       std::memory_order_relaxed);
          }
        }
        if (!g_session.advance_frame()) { /* recording finished */
        }
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
          g_autotype_queue.tick(
              [](uint16_t cpc_key, bool pressed) {
                // A break only satisfies a WAITBREAK when it fired AFTER the
                // last typed keystroke (i.e. was caused by what autotype just
                // typed) — clear the latch on every key PRESS so a boot-time
                // break can't pre-satisfy a later WAITBREAK. Releases don't
                // clear: `call 0`'s break can land between RETURN's press and
                // its release tick, and must survive to the WAITBREAK.
                if (pressed) g_waitbreak_latch = false;
                CPCScancode const scancode =
                    CPC.InputMapper->CPCscancodeFromCPCkey(
                        static_cast<CPC_KEYS>(cpc_key));
                if (static_cast<byte>(scancode) == 0xff) return;
                if (pressed) {
                  keyboard_matrix[static_cast<byte>(scancode) >> 4].fetch_and(
                      ~bit_values[static_cast<byte>(scancode) & 7],
                      std::memory_order_relaxed);
                  if (scancode & MOD_CPC_SHIFT)
                    keyboard_matrix[0x25 >> 4].fetch_and(
                        ~bit_values[0x25 & 7], std::memory_order_relaxed);
                  else
                    keyboard_matrix[0x25 >> 4].fetch_or(
                        bit_values[0x25 & 7], std::memory_order_relaxed);
                  if (scancode & MOD_CPC_CTRL)
                    keyboard_matrix[0x27 >> 4].fetch_and(
                        ~bit_values[0x27 & 7], std::memory_order_relaxed);
                  else
                    keyboard_matrix[0x27 >> 4].fetch_or(
                        bit_values[0x27 & 7], std::memory_order_relaxed);
                } else {
                  keyboard_matrix[static_cast<byte>(scancode) >> 4].fetch_or(
                      bit_values[static_cast<byte>(scancode) & 7],
                      std::memory_order_relaxed);
                  keyboard_matrix[0x25 >> 4].fetch_or(
                      bit_values[0x25 & 7], std::memory_order_relaxed);
                  keyboard_matrix[0x27 >> 4].fetch_or(
                      bit_values[0x27 & 7], std::memory_order_relaxed);
                }
              },
              [](uint16_t cmd) -> bool {
                koncpc_menu_action(static_cast<int>(cmd));
                // KONCPC_WAITBREAK blocks the queue until the next breakpoint
                // fires (the Z80 thread then calls g_autotype_queue.resume()).
                // A break that already fired while the WAITBREAK was still
                // queued (latched below) satisfies it immediately.
                if (cmd == KONCPC_WAITBREAK && g_waitbreak_latch) {
                  g_waitbreak_latch = false;
                  return false;
                }
                return cmd == KONCPC_WAITBREAK;
              });
        }
      }

      g_telnet.drain_input();

      // IPC frame step
      if (g_ipc->frame_step_active.load()) {
        int const remaining = g_ipc->frame_step_remaining.fetch_sub(1) - 1;
        if (remaining <= 0) {
          cpc_pause();
          g_ipc->notify_frame_step_done();
        }
      }

      // Drive LED state and FPS text — written before signal_ready() so the
      // condvar's happens-before ensures render thread sees them after
      // wait_ready() returns. Under engine=1 the legacy FDC struct is dormant;
      // read the sub-cycle FDC Device instead.
      if (subcycle_bridge_active()) {
        subcycle_bridge_disk_leds(imgui_state.drive_a_led,
                                  imgui_state.drive_b_led);
      } else {
        imgui_state.drive_a_led = FDC.led && (FDC.command[1] & 1) == 0;
        imgui_state.drive_b_led = FDC.led && (FDC.command[1] & 1) == 1;
      }
      if (CPC.scr_fps) {
        char chStr[15];
        snprintf(chStr, sizeof(chStr), "%3dFPS %3d%%", static_cast<int>(dwFPS),
                 static_cast<int>(dwFPS) * 100 /
                     static_cast<int>(1000.0 / FRAME_PERIOD_MS));
        imgui_state.topbar_fps = chStr;
      } else {
        imgui_state.topbar_fps.clear();
      }

      // Finalise the write buffer (ASIC sprites must be drawn before publish)
      // then publish it to the ring and advance back_surface to a free buffer.
      // The Z80 keeps writing the new buffer; the render thread reads the
      // published one.  On a skipped frame nothing is published (the previous
      // frame stays current) and the same write buffer is reused next frame.
      if (!CPC.skip_rendering) {
        back_surface = video_ring_publish();
      }

      // Wake the render thread (it reads the latest published buffer) WITHOUT
      // blocking: the triple-buffer ring gives the Z80 a free buffer to write,
      // so it never waits for render.  render-wait now measures ~0 (the
      // coupling this refactor removes).  The render thread reads
      // g_ring_published and drops intermediate frames if it falls behind.
      uint64_t const render_wait_t0 = SDL_GetPerformanceCounter();
      g_frame_signal.signal_ready(CPC.skip_rendering);
      s_render_wait_accum += SDL_GetPerformanceCounter() - render_wait_t0;
    }
  }
}
}  // namespace

// One iteration of the GUI render path (non-headless).  Pulls the latest
// published frame and presents it (or shows the paused overlay), pumping SDL
// events while waiting so the macOS run loop stays alive.  Extracted so it can
// be driven both from the main loop and (Phase 2) from a CADisplayLink/run-loop
// tick during native-menu tracking.  Returns true if the caller should skip the
// rest of its loop iteration (the quit path, which must not run the deferred
// video-reinit check).
namespace {
bool render_one_frame() {
  if (g_emu_paused.load(std::memory_order_relaxed)) {
    // Paused overlay: render ImGui without waiting for a frame signal
    video_display();
    video_display_b();
    video_take_pending_window_screenshot();
    if (g_m4_http.is_running()) g_m4_http.drain_pending();
    ipc_drain_input();
    std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
    return false;
  }
  // Poll for the Z80 frame signal, pumping SDL events between attempts.
  // On macOS+Metal the Cocoa run loop must stay alive for CADisplayLink
  // and drawable-completion callbacks to fire; a bare condvar wait
  // starves it, causing SDL_RenderPresent / SDL_GL_SwapWindow to hang
  // indefinitely for video styles that spend time in Phase A before the
  // GPU call (CRT Basic/Full with GL shaders, SDL swscale with pixel
  // filters).
  bool skip = false;
  while (!g_frame_signal.try_wait_ready_for(1, skip)) {
    SDL_PumpEvents();  // keep macOS/Metal run loop alive
    // Escape hatch: if the emulator got paused (e.g. focus loss /
    // auto_pause, IPC pause) or a quit was requested while we were
    // waiting, the Z80 thread stops producing frames and this loop
    // would spin forever — SDL_PumpEvents() only enqueues events, so
    // the window-close handler (SDL_PollEvent at the top of the loop)
    // never runs and the app looks frozen.  Bail to the outer loop,
    // which re-reads g_emu_paused and takes the paused-overlay branch
    // that both pumps AND polls events.
    if (g_emu_paused.load(std::memory_order_relaxed) ||
        g_z80_thread_quit.load(std::memory_order_relaxed)) {
      return false;
    }
  }
  // Pace the present pipeline to display rate (F8): an uncapped emulation
  // publishes thousands of frames a second, and running the full present
  // (ring copy + texture upload + ImGui + scaled software blits) for each
  // one burns an entire core — on E-cores it starved the emulation thread
  // itself (§8.3 fast-E read 368 while the machine alone benches 656). The
  // ring already keeps only the newest frame; presenting it at ≤70 Hz shows
  // everything a real display could. A blocking-vsync present self-paces
  // below the threshold (60 Hz ≈ 16.7 ms > 1/70 s) and is unaffected.
  static uint64_t s_last_present = 0;
  const uint64_t present_now = SDL_GetPerformanceCounter();
  if (!skip && s_last_present != 0 &&
      present_now - s_last_present < SDL_GetPerformanceFrequency() / 70) {
    skip = true;
  }
  if (skip) {
    // Skipped frame: nothing to present this time; just service pending
    // screenshot requests.  (No handshake to release — the ring decoupled
    // the Z80 from render.)
    video_take_pending_window_screenshot();
    if (g_take_screenshot) {
      dumpScreen();
      g_take_screenshot = false;
    }
    return false;
  }
  s_last_present = present_now;
  // Copy the latest published frame into the surface the flip reads.
  video_ring_present();
  // OSD text — render thread owns osd_message/osd_timing, no race.
  // Write onto the presented frame (video_render_surface()), not the
  // Z80's live write buffer.
  if (SDL_GetTicks() < osd_timing) {
    print(
        static_cast<byte*>(video_render_surface()->pixels) + CPC.scr_line_offs,
        osd_message.c_str(), true);
  }
  uint64_t const displayStart = SDL_GetPerformanceCounter();
  video_display();  // Phase A: texture upload + ImGui render (~3ms)
  // NOTE: the old "partial audio push before the Phase B stall" lived here. It
  // was safe only because the blocking handshake parked the Z80 during render;
  // post-decouple the Z80 runs free and owns CPC.snd_bufferptr / pbSndBuffer,
  // so touching them here would be a data race. It is also redundant — the Z80
  // feeds the audio queue itself at a steady 50 Hz (EC_SOUND_BUFFER) and never
  // stalls on present. Removed.
  // Main-thread-only housekeeping
  if (g_m4_http.is_running()) g_m4_http.drain_pending();
  ipc_drain_input();
#ifdef __APPLE__
  // Dock preview reads the just-presented frame (render-thread-private
  // after video_ring_present's copy), NOT the Z80's live write buffer.
  dword const frame_count_snap = dwFrameCountOverall;
  SDL_Surface const* preview_surf = video_render_surface();
  if (preview_surf && (frame_count_snap % 50) == 0) {
    koncpc_update_dock_icon_preview(preview_surf->pixels, preview_surf->w,
                                    preview_surf->h, preview_surf->pitch, 0, 0,
                                    preview_surf->w, preview_surf->h);
  }
#endif
  // If quit was requested (e.g. KONCPC_EXIT from the Z80 thread), skip
  // video_display_b() which hangs indefinitely for OpenGL styles (7-19).  The
  // Z80 loop has seen the quit flag and will exit; on the next main-loop
  // iteration SDL_EVENT_QUIT will reach the event handler and doCleanUp() joins
  // the (already-exited) Z80 thread.  Signal the caller to skip its tail.
  if (g_z80_thread_quit.load(std::memory_order_relaxed)) {
    return true;
  }
  video_display_b();  // Phase B: 0-60ms, Z80 runs concurrently!
  uint64_t const displayEnd = SDL_GetPerformanceCounter();
  displayTimeAccum.fetch_add(displayEnd - displayStart,
                             std::memory_order_relaxed);
  if (audio_stream && CPC.snd_ready) {
    int queued = SDL_GetAudioStreamQueued(audio_stream);
    queued = std::max(queued, 0);
    audio_queue_min_bytes = std::min(queued, audio_queue_min_bytes);
    if (queued < static_cast<int>(CPC.snd_buffersize) / 2 &&
        audio_push_count > 0) {
      [[maybe_unused]] double const display_ms =
          static_cast<double>(displayEnd - displayStart) * 1000.0 / perfFreq;
      LOG_DEBUG("Audio low queue after display: "
                << queued << "B, display took " << display_ms << "ms");
    }
  }
  video_take_pending_window_screenshot();
  if (g_take_screenshot) {
    dumpScreen();
    g_take_screenshot = false;
  }
  return false;
}
}  // namespace

// Phase 2: present the latest published frame from a native-menu tracking
// driver (macos_menu.mm CFRunLoopTimer in NSEventTrackingRunLoopMode).  A
// native NSMenu suspends the main loop, but the Z80 keeps producing frames
// (decoupled), so this tick just copies the newest published buffer to the
// screen — NO event pump, NO blocking wait, NO sleep (the tracking run loop
// owns event dispatch). Re-entrancy-guarded: video_display() may spin the run
// loop internally, which could re-fire the timer; the guard turns that into a
// no-op so we never start a second ImGui frame on top of an in-flight one.
void koncpc_render_tracking_tick() {
  if (g_headless) return;
  static bool s_in_tick = false;  // main-thread only
  if (s_in_tick) return;
  s_in_tick = true;
  if (!g_emu_paused.load(std::memory_order_relaxed)) {
    video_ring_present();  // newest published frame → the surface flip reads
  }
  video_display();    // Phase A: upload + ImGui (incl. viewport windows)
  video_display_b();  // Phase B
  s_in_tick = false;
}

int koncpc_main(int argc, char** argv) {
  // Remember the main thread — cleanExit() uses this to route IPC/HTTP/
  // telnet-initiated quits through SDL_EVENT_QUIT instead of letting an
  // auxiliary thread call SDL_Quit() while the main thread is mid-
  // SDL_PumpEvents (which crashes when the video subsystem is torn down
  // out from under it).
  g_main_thread_id = std::this_thread::get_id();

#ifdef _WIN32
  // Set Windows timer resolution to 1ms for accurate SDL_Delay() in the speed
  // limiter. Without this, SDL_Delay(1) actually sleeps ~15.6ms (default 64Hz
  // timer).
  struct Win32TimerGuard {
    Win32TimerGuard() { timeBeginPeriod(1); }
    ~Win32TimerGuard() { timeEndPeriod(1); }
  } win32TimerGuard;
#endif
  int iExitCondition;
  bool bin_loaded = false;
  SDL_Event event;
  std::vector<std::string> slot_list;

  try {
    binPath =
        std::filesystem::absolute(std::filesystem::path(argv[0]).parent_path());
  } catch (const std::filesystem::filesystem_error&) {
    // Fallback in case argv[0] is unresolvable (e.g. found via PATH).
    // binPath is only used for bundles anyway.
    binPath = std::filesystem::absolute(".");
  }
  parseArguments(argc, argv, slot_list, args);
  g_headless = args.headless;
  g_debug = args.debug;
  g_log_fps = args.fps;
  g_exit_on_break = args.exitOnBreak;

  // Parse --exit-after spec: Nf (frames), Ns (seconds), Nms (milliseconds)
  if (!args.exitAfter.empty()) {
    const std::string& spec = args.exitAfter;
    if (spec.size() > 2 && spec.substr(spec.size() - 2) == "ms") {
      g_exit_mode = EXIT_MS;
      g_exit_target = std::stoul(spec.substr(0, spec.size() - 2));
    } else if (spec.back() == 's') {
      g_exit_mode = EXIT_MS;
      g_exit_target = std::stoul(spec.substr(0, spec.size() - 1)) * 1000;
    } else if (spec.back() == 'f') {
      g_exit_mode = EXIT_FRAMES;
      g_exit_target = std::stoul(spec.substr(0, spec.size() - 1));
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
  if (getcwd(chAppPath, sizeof(chAppPath) - 1) == nullptr) {
    fprintf(stderr, "getcwd failed: %s\n", strerror(errno));
    cleanExit(-1);
  }
#else
  snprintf(chAppPath, sizeof(chAppPath), "%s", APP_PATH);
#endif

  loadConfiguration(
      CPC, getConfigurationFilename());  // retrieve the emulator configuration
  if (CPC.printer) {
    if (!printer_start()) {  // start capturing printer output, if enabled
      CPC.printer = 0;
    }
  }

  if (g_headless) {
    // In headless mode, force the headless video plugin (offscreen surface
    // only)
    static video_plugin hp = video_headless_plugin();
    vid_plugin = &hp;
    back_surface = vid_plugin->init(vid_plugin, 2, false);
    if (!back_surface) {
      fprintf(stderr, "headless video_init() failed. Aborting.\n");
      _exit(-1);
    }
    {
      const SDL_PixelFormatDetails* fmt =
          SDL_GetPixelFormatDetails(back_surface->format);
      CPC.scr_bpp = fmt ? fmt->bits_per_pixel : 0;
    }
    video_set_style();
    if (video_set_palette()) {
      fprintf(stderr, "headless video_set_palette() failed. Aborting.\n");
      _exit(-1);
    }
    CPC.scr_bps = back_surface->pitch;
    CPC.scr_line_offs = CPC.scr_bps * dwYScale;
    CPC.scr_pos = CPC.scr_base = static_cast<byte*>(back_surface->pixels);
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
      std::string const icon_path = CPC.resources_path + "/koncepcja-icon.png";
      koncpc_set_dock_icon(icon_path.c_str());
    }
#endif
    topbar_height_px = ui_host().topbar_height();
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

  // Fill the autotype queue with the autocmd if provided.  A single
  // scan-synced path drives all keyboard injection:
  // - ~KEY~ syntax (tilde-delimited) is parsed by enqueue() (~ENTER~, ~PAUSE
  //   50~, etc.).
  // - The legacy \a (CPC keys) / \f (emulator commands like
  //   KONCPC_WAITBREAK/KONCPC_EXIT, from replaceKoncpcKeys()) encoding is
  //   parsed by enqueue_legacy().
  if (!args.autocmd.empty()) {
    // Single scan-synced path for all autocmds: a boot-time delay, then the
    // command — either ~KEY~ syntax or the legacy \a/\f encoding.
    g_autotype_queue.enqueue("~PAUSE " + std::to_string(CPC.boot_time) + "~");
    if (args.autocmd.find('~') != std::string::npos) {
      auto err = g_autotype_queue.enqueue(args.autocmd);
      if (!err.empty()) {
        LOG_ERROR("--autocmd parse error: " << err);
      }
    } else {
      g_autotype_queue.enqueue_legacy(args.autocmd);
    }
  }

  // ----------------------------------------------------------------------------

  update_timings();
  if (!g_headless) audio_resume();

  loadBreakpoints();

  g_exit_start_ticks = SDL_GetTicks();
  iExitCondition = EC_FRAME_COMPLETE;

  // Keyboard matrices start RELEASED. Both are std::atomic arrays with static
  // (zero) init — and 0x00 means ALL KEYS PRESSED (CPC matrix: bit clear =
  // pressed). Nothing sets a row to released until its first SDL key event, but
  // the Z80 thread reads keyboard_matrix_live every frame from power-on, so an
  // uninitialised matrix feeds the firmware "all keys held" through boot. That
  // stalls/derails the 6128+ cartridge boot intermittently (beads-gbey) — the
  // freeze the legacy core never hit — depending on when SDL releases each row.
  // Headless is immune (it feeds 0xFF explicitly), which is why the pin-level
  // PlusCartBoot tests always pass while the GUI raced. Publish released first.
  for (int i = 0; i < 16; ++i) {
    keyboard_matrix[i].store(0xFF, std::memory_order_relaxed);
    keyboard_matrix_live[i].store(0xFF, std::memory_order_relaxed);
  }

  // Spawn Z80 emulation thread for non-headless mode.
  // The render loop (below) becomes render-only; the Z80 thread signals each
  // completed frame via g_frame_signal so Phase A/B can overlap with the next
  // frame.
  // Bring the pin-level board up before the emulation loop starts —
  // threaded AND headless. There is no other engine: failure to load the
  // system ROM is fatal.
  if (!subcycle_bridge_start()) {
    fprintf(stderr,
            "ERROR: cannot start the emulation core (system ROM missing? "
            "check rom_path=%s)\n",
            CPC.rom_path.c_str());
    cleanExit(ERR_CPC_ROM_MISSING, false);
  }
  if (!g_headless) {
    g_z80_thread = std::thread(z80_thread_main);
  }

  dword nextMouseReset = 0;
  // Whether this loop of emulation should release the joystick axis for mouse
  // emulation.
  while (true) {
    // We can only load bin files after the CPC finished the init
    if (!bin_loaded && dwFrameCountOverall > CPC.boot_time) {
      bin_loaded = true;
      if (!args.binFile.empty()) bin_load(args.binFile, args.binOffset);
    }

    // Mouse-as-joystick: release all joystick axes periodically so they don't
    // stick
    if (dwFrameCountOverall >= nextMouseReset &&
        CPC.joystick_emulation == JoystickEmulation::Mouse) {
      // We set release_modifiers = false because otherwise, this somehow breaks
      // some keys, like | on a french keyboard!
      applyKeypress(CPC.InputMapper->CPCscancodeFromCPCkey(CPC_J0_RIGHT),
                    keyboard_matrix, false, false);
      applyKeypress(CPC.InputMapper->CPCscancodeFromCPCkey(CPC_J0_LEFT),
                    keyboard_matrix, false, false);
      applyKeypress(CPC.InputMapper->CPCscancodeFromCPCkey(CPC_J0_DOWN),
                    keyboard_matrix, false, false);
      applyKeypress(CPC.InputMapper->CPCscancodeFromCPCkey(CPC_J0_UP),
                    keyboard_matrix, false, false);
    }
    while (!g_headless && SDL_PollEvent(&event)) {
      // Handle main window close before ImGui consumes the event
      if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        SDL_WindowID const main_id =
            mainSDLWindow ? SDL_GetWindowID(mainSDLWindow) : 0;
        if (event.window.windowID == main_id) {
          cleanExit(0);
        }
      }

      // Feed event to the UI host (Dear ImGui in modern builds, no-op
      // in headless).  Was a direct ImGui_ImplSDL3_ProcessEvent call —
      // routed through IUiHost in P1.5.1 so the main loop doesn't
      // depend on ImGui at the boundary.
      ui_host().process_event(event);

      // ── Drag-and-drop file loading ──
      if (event.type == SDL_EVENT_DROP_FILE) {
        const char* dropped = event.drop.data;
        if (dropped) {
          std::string drop_path(dropped);
          std::string ext =
              std::filesystem::path(drop_path).extension().string();
          std::transform(ext.begin(), ext.end(), ext.begin(),
                         [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                         });
          auto drop_fname =
              std::filesystem::path(drop_path).filename().string();

          // macOS re-delivers the CLI launch-file arguments as DROP_FILE
          // events once the window exists — *after* fillSlots() has already
          // mounted them into the drive slots. Loading every dropped disk
          // into drive A therefore clobbered a launch that filled both
          // drives (the 2nd CLI disk overwrote the 1st). Ignore a drop that
          // merely re-delivers a disk already mounted in a drive; match by
          // filename, since the CLI arg may be relative while the drop path
          // is absolute.
          auto already_mounted = [&](const std::string& slot_file) {
            return !slot_file.empty() &&
                   std::filesystem::path(slot_file).filename() ==
                       std::filesystem::path(drop_path).filename();
          };

          if (ext == ".dsk" || ext == ".ipf" || ext == ".raw") {
            if (already_mounted(CPC.driveA.file) ||
                already_mounted(CPC.driveB.file)) {
              LOG_INFO("Ignoring drop of already-mounted disk: " << drop_fname);
            } else {
              CPC.driveA.file = drop_path;
              if (file_load(CPC.driveA) == 0) {
                ui_host().toast(UiToastLevel::Success,
                                "Drive A: " + drop_fname);
                imgui_mru_push(CPC.mru_disks, drop_path);
              } else {
                ui_host().toast(UiToastLevel::Error,
                                "Failed to load disk: " + drop_fname);
              }
            }
          } else if (ext == ".cdt" || ext == ".voc") {
            CPC.tape.file = drop_path;
            if (file_load(CPC.tape) == 0) {
              ui_host().toast(UiToastLevel::Success,
                              "Tape loaded: " + drop_fname);
              imgui_mru_push(CPC.mru_tapes, drop_path);
              tape_scan_blocks();
            } else {
              ui_host().toast(UiToastLevel::Error,
                              "Failed to load tape: " + drop_fname);
            }
          } else if (ext == ".sna") {
            CPC.snapshot.file = drop_path;
            if (file_load(CPC.snapshot) == 0) {
              ui_host().toast(UiToastLevel::Success,
                              "Snapshot loaded: " + drop_fname);
              imgui_mru_push(CPC.mru_snaps, drop_path);
            } else {
              ui_host().toast(UiToastLevel::Error,
                              "Failed to load snapshot: " + drop_fname);
            }
          } else if (ext == ".cpr") {
            CPC.cartridge.file = drop_path;
            if (file_load(CPC.cartridge) == 0) {
              ui_host().toast(UiToastLevel::Success,
                              "Cartridge loaded: " + drop_fname);
              imgui_mru_push(CPC.mru_carts, drop_path);
              emulator_reset();
            } else {
              ui_host().toast(UiToastLevel::Error,
                              "Failed to load cartridge: " + drop_fname);
            }
          } else if (ext == ".zip") {
            // Try as disk first (most common zip content)
            CPC.driveA.file = drop_path;
            if (file_load(CPC.driveA) == 0) {
              ui_host().toast(UiToastLevel::Success, "Drive A: " + drop_fname);
              imgui_mru_push(CPC.mru_disks, drop_path);
            } else {
              ui_host().toast(UiToastLevel::Error,
                              "Unsupported ZIP content: " + drop_fname);
            }
          } else {
            ui_host().toast(UiToastLevel::Error,
                            "Unknown file type: " + drop_fname);
          }
        }
        continue;
      }

      // Check for command palette shortcut (Cmd+K / Ctrl+K)
      if (event.type == SDL_EVENT_KEY_DOWN) {
        bool const ctrl = (event.key.mod & SDL_KMOD_CTRL) != 0;
        bool const cmd_key = (event.key.mod & SDL_KMOD_GUI) != 0;
        if (g_command_palette.handle_key(event.key.key, ctrl, cmd_key)) {
          continue;
        }
      }

      // If the UI wants input, skip emulator processing.
      // Exception: virtual keyboard events (windowID=0) always reach the
      // emulator.
      //
      // Keyboard policy uses ui_host().any_keyboard_ui_active() rather than
      // ImGui's WantCaptureKeyboard — the latter is set by ImGui::NewFrame()
      // based on internal focus state that can stay stuck after native file
      // dialogs or menu interactions.  any_keyboard_ui_active() checks the
      // actual dialog/menu/devtools state and is the single source of truth.
      // Mouse policy still routes through wants_capture_mouse(), which mirrors
      // ImGui's WantCaptureMouse — that one isn't subject to the same stuck-
      // focus pathology.
      {
        IUiHost const& ui = ui_host();
        bool const is_key_event = (event.type == SDL_EVENT_KEY_DOWN ||
                                   event.type == SDL_EVENT_KEY_UP);
        bool const is_text_event = (event.type == SDL_EVENT_TEXT_INPUT);
        bool const is_mouse_event_imgui =
            (event.type == SDL_EVENT_MOUSE_MOTION ||
             event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
             event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
             event.type == SDL_EVENT_MOUSE_WHEEL);
        bool const is_virtual_key = is_key_event && event.key.windowID == 0;

        bool const ui_wants_kbd = ui.any_keyboard_ui_active();

        if (((is_key_event && !is_virtual_key) && ui_wants_kbd) ||
            (is_text_event && ui_wants_kbd) ||
            (is_mouse_event_imgui && ui.wants_capture_mouse())) {
          continue;
        }
      }

      switch (event.type) {
        case SDL_EVENT_KEY_DOWN: {
          CPCScancode const scancode = CPC.InputMapper->CPCscancodeFromKeysym(
              event.key.key, event.key.mod);
          LOG_VERBOSE(
              "Keyboard: pressed: "
              << SDL_GetKeyName(event.key.key) << " (" << event.key.key
              << ") - scancode: " << SDL_GetScancodeName(event.key.scancode)
              << " (" << event.key.scancode << ") - CPC key: "
              << CPC.InputMapper->CPCkeyToString(
                     CPC.InputMapper->CPCkeyFromKeysym(
                         event.key.key, static_cast<SDL_Keymod>(event.key.mod)))
              << " - CPC scancode: " << scancode);
          if (!(scancode & MOD_EMU_KEY)) {
            applyKeypress(scancode, keyboard_matrix, true);
          } else if (!event.key.repeat) {
            // Emulator commands: fire on initial KEY_DOWN (not repeat, not
            // KEY_UP). One physical press = one command, no debounce needed.
            switch (scancode) {
              case KONCPC_GUI:
                showGui();
                break;
              case KONCPC_VKBD:
                showVKeyboard();
                break;
              case KONCPC_VJOY:
                showVJoystick();
                break;
              case KONCPC_DEVTOOLS: {
                imgui_state.show_devtools = !imgui_state.show_devtools;
                log_verbose = imgui_state.show_devtools;
                if (imgui_state.show_devtools) {
                  set_osd_message("Debug mode: on");
                } else {
                  g_devtools_ui.close_all_windows();
                  set_osd_message("Debug mode: off");
                }
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
                koncpc_menu_action(KONCPC_SCRNSHOT);
                break;
              case KONCPC_SNAPSHOT:
                koncpc_menu_action(KONCPC_SNAPSHOT);
                break;
              case KONCPC_LD_SNAP:
                koncpc_menu_action(KONCPC_LD_SNAP);
                break;
              case KONCPC_TAPEPLAY:
                koncpc_menu_action(KONCPC_TAPEPLAY);
                break;
              case KONCPC_MF2STOP:
                koncpc_menu_action(KONCPC_MF2STOP);
                break;
              case KONCPC_RESET:
                koncpc_menu_action(KONCPC_RESET);
                break;
              case KONCPC_JOY:
                koncpc_menu_action(KONCPC_JOY);
                break;
              case KONCPC_PHAZER:
                koncpc_menu_action(KONCPC_PHAZER);
                break;
              case KONCPC_PASTE:
                koncpc_menu_action(KONCPC_PASTE);
                break;
              case KONCPC_EXIT:
                koncpc_menu_action(KONCPC_EXIT);
                break;
              case KONCPC_SPEED:
                // Delegate to the canonical handler (atomic debounce + OSD),
                // like the surrounding cases — key repeats are already filtered
                // by the outer guard, so no duplicate logic is needed here.
                koncpc_menu_action(KONCPC_SPEED);
                break;
              case KONCPC_FPS:
                CPC.scr_fps = CPC.scr_fps ? 0 : 1;
                set_osd_message(std::string("Performances info: ") +
                                (CPC.scr_fps ? "on" : "off"));
                break;
              case KONCPC_DEBUG:
                log_verbose = !log_verbose;
                set_osd_message(std::string("Debug mode: ") +
                                (log_verbose ? "on" : "off"));
                break;
              case KONCPC_DELAY:
                // Autocmd pacing commands: delegate to the canonical handler
                // (same as every other command above).  These were left as
                // no-op stubs in the KEY_UP->KEY_DOWN refactor, which made
                // `-a KONCPC_WAITBREAK` do nothing in non-headless mode, so a
                // following `-a KONCPC_EXIT` fired before the program ran.
                koncpc_menu_action(KONCPC_DELAY);
                break;
              case KONCPC_WAITBREAK:
                koncpc_menu_action(KONCPC_WAITBREAK);
                break;
              default:
                break;
            }
          }
        } break;

        case SDL_EVENT_KEY_UP: {
          CPCScancode const scancode = CPC.InputMapper->CPCscancodeFromKeysym(
              event.key.key, event.key.mod);
          if (!(scancode & MOD_EMU_KEY)) {
            applyKeypress(scancode, keyboard_matrix, false);
          }
        } break;

        // ── Hotplug ──────────────────────────────────────────────────────
        case SDL_EVENT_GAMEPAD_ADDED:
          controller_open(event.gdevice.which);
          break;
        case SDL_EVENT_GAMEPAD_REMOVED:
          controller_close(event.gdevice.which);
          break;
        case SDL_EVENT_JOYSTICK_ADDED:
          // Gamepad-capable devices are opened via GAMEPAD_ADDED (both events
          // fire for them); only take raw joysticks here.
          if (!SDL_IsGamepad(event.jdevice.which)) {
            controller_open(event.jdevice.which);
          }
          break;
        case SDL_EVENT_JOYSTICK_REMOVED:
          controller_close(event.jdevice.which);  // idempotent if already gone
          break;

        // ── High-level gamepad input (auto-mapped) ───────────────────────
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP: {
          int const slot = controller_slot_of_instance(event.gbutton.which);
          if (slot < 0) break;
          bool const pressed = (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
          switch (event.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_SOUTH:  // A -> Fire 1
              controller_apply_bit(slot, vjoy::VJOY_FIRE1, pressed);
              break;
            case SDL_GAMEPAD_BUTTON_EAST:  // B -> Fire 2
              controller_apply_bit(slot, vjoy::VJOY_FIRE2, pressed);
              break;
            case SDL_GAMEPAD_BUTTON_DPAD_UP:
              controller_apply_bit(slot, vjoy::VJOY_UP, pressed);
              break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
              controller_apply_bit(slot, vjoy::VJOY_DOWN, pressed);
              break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
              controller_apply_bit(slot, vjoy::VJOY_LEFT, pressed);
              break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
              controller_apply_bit(slot, vjoy::VJOY_RIGHT, pressed);
              break;
            default:
              break;
          }
        } break;

        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
          int const slot = controller_slot_of_instance(event.gaxis.which);
          if (slot < 0) break;
          Sint16 const v = event.gaxis.value;
          // Left stick past the threshold -> directions (opposite bit released
          // when centered — idempotent, no per-axis state needed).
          if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
            controller_apply_bit(slot, vjoy::VJOY_LEFT,
                                 v < -JOYSTICK_AXIS_THRESHOLD);
            controller_apply_bit(slot, vjoy::VJOY_RIGHT,
                                 v > JOYSTICK_AXIS_THRESHOLD);
          } else if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            controller_apply_bit(slot, vjoy::VJOY_UP,
                                 v < -JOYSTICK_AXIS_THRESHOLD);
            controller_apply_bit(slot, vjoy::VJOY_DOWN,
                                 v > JOYSTICK_AXIS_THRESHOLD);
          }
        } break;

        // ── Raw joystick fallback (non-gamepad devices only) ─────────────
        case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
        case SDL_EVENT_JOYSTICK_BUTTON_UP: {
          int const slot = controller_slot_of_instance(event.jbutton.which);
          if (slot < 0 || joysticks[slot] == nullptr) break;  // gamepad-managed
          bool const pressed = (event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN);
          if (pressed && event.jbutton.button == CPC.joystick_menu_button) {
            showGui();
          } else if (pressed &&
                     event.jbutton.button == CPC.joystick_vkeyboard_button) {
            showVKeyboard();
          } else if (event.jbutton.button == 0) {
            controller_apply_bit(slot, vjoy::VJOY_FIRE1, pressed);
          } else if (event.jbutton.button == 1) {
            controller_apply_bit(slot, vjoy::VJOY_FIRE2, pressed);
          }
        } break;

        case SDL_EVENT_JOYSTICK_AXIS_MOTION: {
          int const slot = controller_slot_of_instance(event.jaxis.which);
          if (slot < 0 || joysticks[slot] == nullptr) break;  // gamepad-managed
          Sint16 const v = event.jaxis.value;
          if (event.jaxis.axis == 0) {  // X
            controller_apply_bit(slot, vjoy::VJOY_LEFT,
                                 v < -JOYSTICK_AXIS_THRESHOLD);
            controller_apply_bit(slot, vjoy::VJOY_RIGHT,
                                 v > JOYSTICK_AXIS_THRESHOLD);
          } else if (event.jaxis.axis == 1) {  // Y
            controller_apply_bit(slot, vjoy::VJOY_UP,
                                 v < -JOYSTICK_AXIS_THRESHOLD);
            controller_apply_bit(slot, vjoy::VJOY_DOWN,
                                 v > JOYSTICK_AXIS_THRESHOLD);
          }
        } break;

        case SDL_EVENT_MOUSE_MOTION: {
          {
            SDL_WindowID const main_wid =
                mainSDLWindow ? SDL_GetWindowID(mainSDLWindow) : 0;
            bool const on_main = (event.motion.windowID == main_wid);
            bool const over_topbar =
                on_main && event.motion.y < ui_host().topbar_height();
            static bool topbar_cursor_visible = false;
            if (over_topbar && !topbar_cursor_visible) {
              set_cursor_visibility(true);
              topbar_cursor_visible = true;
            } else if (!over_topbar && topbar_cursor_visible &&
                       !CPC.phazer_emulation) {
              set_cursor_visibility(false);
              topbar_cursor_visible = false;
            }
          }
          CPC.phazer_x =
              (event.motion.x - vid_plugin->x_offset) * vid_plugin->x_scale;
          CPC.phazer_y =
              (event.motion.y - vid_plugin->y_offset) * vid_plugin->y_scale;
          if (g_amx_mouse.enabled) {
            amx_mouse_update(event.motion.xrel, event.motion.yrel,
                             SDL_GetMouseState(nullptr, nullptr));
          }
          if (g_symbiface.enabled) {
            symbiface_mouse_update(event.motion.xrel, event.motion.yrel,
                                   SDL_GetMouseState(nullptr, nullptr));
          }
          if (CPC.joystick_emulation == JoystickEmulation::Mouse) {
            int const threshold = 2;
            if (event.motion.yrel > threshold) {
              applyKeypress(CPC.InputMapper->CPCscancodeFromCPCkey(CPC_J0_DOWN),
                            keyboard_matrix, true);
            }
            if (event.motion.yrel < -threshold) {
              applyKeypress(CPC.InputMapper->CPCscancodeFromCPCkey(CPC_J0_UP),
                            keyboard_matrix, true);
            }
            if (event.motion.xrel > threshold) {
              applyKeypress(
                  CPC.InputMapper->CPCscancodeFromCPCkey(CPC_J0_RIGHT),
                  keyboard_matrix, true);
            }
            if (event.motion.xrel < -threshold) {
              applyKeypress(CPC.InputMapper->CPCscancodeFromCPCkey(CPC_J0_LEFT),
                            keyboard_matrix, true);
            }
            nextMouseReset = dwFrameCountOverall + 2;
          }
        } break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
          // Topbar clicks (Menu button, drive LEDs, etc.) are handled by
          // ImGui's imgui_render_topbar(). No legacy showGui() handler needed
          // here.
          if (CPC.phazer_emulation) {
            // Trojan Light Phazer uses Joystick Fire for the trigger button:
            // https://www.cpcwiki.eu/index.php/Trojan_Light_Phazer
            if (CPC.phazer_emulation == PhazerType::TrojanLightPhazer) {
              auto scancode =
                  CPC.InputMapper->CPCscancodeFromCPCkey(CPC_J0_FIRE1);
              applyKeypress(scancode, keyboard_matrix, true);
            }
            CPC.phazer_pressed = true;
          }
          if (g_amx_mouse.enabled) {
            amx_mouse_update(0, 0, SDL_GetMouseState(nullptr, nullptr));
          }
          handle_mouse_joystick_button(event.button, keyboard_matrix, true);
        } break;

        case SDL_EVENT_MOUSE_BUTTON_UP: {
          if (CPC.phazer_emulation) {
            if (CPC.phazer_emulation == PhazerType::TrojanLightPhazer) {
              auto scancode =
                  CPC.InputMapper->CPCscancodeFromCPCkey(CPC_J0_FIRE1);
              applyKeypress(scancode, keyboard_matrix, false);
            }
            CPC.phazer_pressed = false;
          }
          if (g_amx_mouse.enabled) {
            amx_mouse_update(0, 0, SDL_GetMouseState(nullptr, nullptr));
          }
          handle_mouse_joystick_button(event.button, keyboard_matrix, false);
        } break;

        // TODO: What if we were paused because of other reason than losing
        // focus and then only lost focus
        //       the right thing to do here is to restore focus but keep
        //       paused... implementing this require keeping track of pause
        //       source, which will be a pain.
        case SDL_EVENT_WINDOW_MOVED:
          // A move — often an OS window-management nudge rather than the user —
          // may push the main window fully off every display. Rescue it, but
          // with min_visible=1 so an intentional partial drag is never fought
          // (only a truly-lost, zero-overlap window is pulled back). Our own
          // SDL_SetWindowPosition re-fires MOVED once; the next pass sees the
          // window on-screen and no-ops, so this cannot loop.
          if (mainSDLWindow &&
              event.window.windowID == SDL_GetWindowID(mainSDLWindow)) {
            koncpc_rescue_window_onscreen(mainSDLWindow, 1);
          }
          break;
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
    // Render thread waits for frame signal, does Phase A, releases Z80, then
    // Phase B.
    if (!g_headless) {
      // One render iteration (extracted so a Phase-2 menu-tracking driver can
      // call it too).  Returns true on the quit path → skip the rest of the
      // loop body (the deferred video-reinit check below).
      if (render_one_frame()) continue;
    }

    // ---- Headless: original single-threaded emulation (unchanged) ----
    if (g_headless && !CPC.paused) {  // run the emulation
      uint64_t const perfNow = SDL_GetPerformanceCounter();

      if (perfNow >= perfTicksTargetFPS) {  // update FPS counter every second
        dwFPS = dwFrameCount;
        dwFrameCount = 0;
        perfTicksTargetFPS = perfNow + perfFreq;  // next sample in 1 second

        // Publish frame timing stats (use double to avoid integer division
        // precision loss)
        if (frameTimeSamples > 0) {
          double const ticksToUs = 1000000.0 / static_cast<double>(perfFreq);
          imgui_state.frame_time_avg_us =
              static_cast<float>(static_cast<double>(frameTimeAccum) /
                                 frameTimeSamples * ticksToUs);
          imgui_state.frame_time_min_us =
              static_cast<float>(static_cast<double>(frameTimeMin) * ticksToUs);
          imgui_state.frame_time_max_us =
              static_cast<float>(static_cast<double>(frameTimeMax) * ticksToUs);
          imgui_state.display_time_avg_us =
              static_cast<float>(static_cast<double>(displayTimeAccum.load(
                                     std::memory_order_relaxed)) /
                                 frameTimeSamples * ticksToUs);
          imgui_state.sleep_time_avg_us =
              static_cast<float>(static_cast<double>(sleepTimeAccum) /
                                 frameTimeSamples * ticksToUs);
          imgui_state.z80_time_avg_us = static_cast<float>(
              static_cast<double>(z80TimeAccum) / frameTimeSamples * ticksToUs);
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
          double const avg_bytes = audio_queue_sum_bytes / audio_push_count;
          int frame_size = CPC.snd_stereo ? 4 : 2;  // 16-bit stereo=4, mono=2
          if (CPC.snd_bits == 0) frame_size /= 2;   // 8-bit halves it
          int const sample_rate = freq_table[CPC.snd_playback_rate];
          double const bytes_per_ms = sample_rate * frame_size / 1000.0;
          imgui_state.audio_queue_avg_ms =
              static_cast<float>(avg_bytes / bytes_per_ms);
          imgui_state.audio_queue_min_ms =
              static_cast<float>(audio_queue_min_bytes / bytes_per_ms);
          imgui_state.audio_push_interval_max_us =
              static_cast<float>(static_cast<double>(audio_push_interval_max) *
                                 1000000.0 / perfFreq);
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
        // Absolute deadline: sleep until perfTicksTarget, then advance by one
        // frame. Multiple EC_CYCLE_COUNTs may fire per frame (audio-driven
        // cycle boundaries); only the first one sleeps — subsequent ones see
        // the deadline already passed.
        uint64_t const sleepStart = SDL_GetPerformanceCounter();
        if (sleepStart < perfTicksTarget) {
          uint64_t const remaining_ticks = perfTicksTarget - sleepStart;
          uint64_t const remaining_ms = (remaining_ticks * 1000) / perfFreq;
          if (remaining_ms > 2) {
            SDL_Delay(static_cast<Uint32>(remaining_ms - 2));
          }
          while (SDL_GetPerformanceCounter() < perfTicksTarget) {
            SDL_Delay(0);
          }
        }
        sleepTimeAccum += SDL_GetPerformanceCounter() - sleepStart;
        perfTicksTarget += perfTicksOffset;
        // Catch-up: if more than 3 frames behind, reset the deadline.
        uint64_t const now = SDL_GetPerformanceCounter();
        if (!CPC.frameskip && perfTicksTarget + (3 * perfTicksOffset) < now) {
          perfTicksTarget = now + perfTicksOffset;
        }
      } else if (iExitCondition != EC_CYCLE_COUNT) {
        // Speed limiter not active and not a mid-frame audio slice.
        CPC.skip_rendering = false;
        consecutive_skips = 0;
      }

      // Frameskip decision: only on frame boundaries to avoid mid-frame
      // toggles.
      if (iExitCondition == EC_FRAME_COMPLETE && CPC.limit_speed) {
        uint64_t const now = SDL_GetPerformanceCounter();
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

      dword const dwOffset =
          CPC.scr_pos - CPC.scr_base;  // offset in current surface row
      if (VDU.scrln > 0) {
        CPC.scr_base =
            static_cast<byte*>(back_surface->pixels) +
            (VDU.scrln * CPC.scr_line_offs);  // determine current position
      } else {
        CPC.scr_base =
            static_cast<byte*>(back_surface->pixels);  // reset to surface start
      }
      CPC.scr_pos =
          CPC.scr_base + dwOffset;  // update current rendering position

      // Headless runs single-threaded, but the firmware still scans the live
      // matrix — refresh it from pending each frame (uncontended here).
      publish_keyboard_snapshot();

      {
        uint64_t const z80Start = SDL_GetPerformanceCounter();
        if (subcycle_bridge_active()) {
          // Headless flavour of the threaded loop's bridge branch
          // (beads-iymn: this loop used to call z80_execute unconditionally,
          // silently running the LEGACY core under engine=1). Differences
          // from the threaded branch: the bridge's own 50 Hz pacer stays OFF
          // (this loop's pacing keys on EC_FRAME_COMPLETE — one pacer only),
          // and the audio gate reads CPC.paused (single-threaded).
          subcycle_bridge_sync_probe();
          uint8_t rows[16];
          for (int i = 0; i < 16; i++)
            rows[i] = keyboard_matrix_live[i].load(std::memory_order_relaxed);
          if (tape_line_in_active())
            tape_line_in_pump(*subcycle_bridge_machine());
          static uint64_t s_last_blit_hl = 0;
          const uint64_t blit_now = SDL_GetPerformanceCounter();
          const bool blit_due =
              CPC.limit_speed != 0 ||
              blit_now - s_last_blit_hl >= SDL_GetPerformanceFrequency() / 60;
          if (blit_due) s_last_blit_hl = blit_now;
          const std::vector<int16_t>& frame_audio = subcycle_bridge_frame(
              rows, blit_due ? back_surface : nullptr, false);
          if (!frame_audio.empty() && CPC.snd_enabled && !CPC.paused) {
            audio_push_buffer(
                reinterpret_cast<const byte*>(frame_audio.data()),
                static_cast<int>(frame_audio.size() * sizeof(int16_t)));
          }
          if (tape_line_out_active())
            tape_line_out_pump(*subcycle_bridge_machine());
          // Real-scan gate, same as the threaded branch: autotype must wait
          // for the firmware's first matrix read (boot!) or it types into
          // the void — the CI e2e dsk hang.
          g_engine1_scanned_rows = subcycle_bridge_scanned_key_rows();
          if (g_engine1_scanned_rows != 0) g_keyboard_scanned = true;
          iExitCondition =
              subcycle_bridge_debug_sync() ? EC_BREAKPOINT : EC_FRAME_COMPLETE;
        } else {
          SDL_Delay(1);  // no engine without the board (start failure is fatal)
          iExitCondition = EC_FRAME_COMPLETE;
        }
        z80TimeAccum += SDL_GetPerformanceCounter() - z80Start;
      }

      // Sample tape level into waveform ring buffer (sub-frame rate)
      if (CPC.tape_motor && CPC.tape_play_button) {
        imgui_state.tape_wave_buf[imgui_state.tape_wave_head] = bTapeLevel;
        imgui_state.tape_wave_head =
            (imgui_state.tape_wave_head + 1) % ImGuiUIState::TAPE_WAVE_SAMPLES;
      }

      // Audio push: PSG finished filling the back buffer — push it to SDL.
      if (iExitCondition == EC_SOUND_BUFFER) {
        if (!CPC.paused) {
          audio_push_buffer(pbSndBuffer.get(),
                            static_cast<int>(CPC.snd_buffersize));
        }
        CPC.snd_bufferptr = pbSndBuffer.get();  // reset write position
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
          // Step In completed (one instruction) or Step Out completed (RET
          // reached)
          CPC.paused = true;
          z80.step_in = 0;
          z80.step_out = 0;
          z80.step_out_addresses.clear();
        } else {
          // This is an old flavour breakpoint
          // We have to clear breakpoint to let the z80 emulator move on.
          z80.break_point = Z80_BREAKPOINT_NONE;
          z80.trace = 1;  // make sure we'll be here to rearm break point at the
                          // next z80 instruction.

          // Release an autotype KONCPC_WAITBREAK waiting for this break — or
          // latch it for a WAITBREAK still queued (early `call 0`).
          g_waitbreak_latch = true;
          g_autotype_queue.resume();
        }
      } else {
        if (z80.break_point == Z80_BREAKPOINT_NONE) {
          LOG_DEBUG("Rearming EC_BREAKPOINT.");
          z80.break_point = 0;  // set break point for next time
        }
      }

      if (iExitCondition == EC_FRAME_COMPLETE) {  // emulation finished
                                                  // rendering a complete frame?
        dwFrameCountOverall++;
        dwFrameCount++;

        g_keyboard_manager.update(keyboard_matrix, dwFrameCountOverall);

        // Measure frame-to-frame time (only on actual completed frames)
        {
          uint64_t const now = SDL_GetPerformanceCounter();
          if (lastFrameStart > 0) {
            uint64_t const elapsed = now - lastFrameStart;
            frameTimeAccum += elapsed;
            frameTimeMin = std::min(elapsed, frameTimeMin);
            frameTimeMax = std::max(elapsed, frameTimeMax);
            frameTimeSamples++;
          }
          lastFrameStart = now;
        }

        // Check --exit-after condition
        if (g_exit_mode == EXIT_FRAMES &&
            dwFrameCountOverall >= g_exit_target) {
          cleanExit(0, false);
        }
        if (g_exit_mode == EXIT_MS &&
            (SDL_GetTicks() - g_exit_start_ticks) >= g_exit_target) {
          cleanExit(0, false);
        }

        // Check IPC VBL events
        ipc_check_vbl_events();

        // M4 Board activity LED countdown (1 per frame at 50fps)
        if (g_m4board.activity_frames > 0) g_m4board.activity_frames--;

        // M4 HTTP server — drain deferred actions (reset, pause toggle)
        if (g_m4_http.is_running()) g_m4_http.drain_pending();

        // IPC mouse input — flush staged deltas/buttons into the devices
        ipc_drain_input();

#ifdef __APPLE__
        // Update Dock icon with CPC screen preview (~1fps at 50fps emulation)
        // back_surface is already sized to CPC_VISIBLE_SCR_WIDTH/HEIGHT * scale
        if (back_surface && (dwFrameCountOverall % 50) == 0) {
          koncpc_update_dock_icon_preview(
              back_surface->pixels, back_surface->w, back_surface->h,
              back_surface->pitch, 0, 0, back_surface->w, back_surface->h);
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
          static uint8_t prev_matrix[16] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                            0xFF, 0xFF, 0xFF, 0xFF};
          for (int row = 0; row < 16; row++) {
            byte const cur =
                keyboard_matrix[row].load(std::memory_order_relaxed);
            if (cur != prev_matrix[row]) {
              // Encode as row in high byte, value in low byte
              uint16_t const data = static_cast<uint16_t>((row << 8) | cur);
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
              int const row = (evt.data >> 8) & 0x0F;
              keyboard_matrix[row].store(static_cast<byte>(evt.data & 0xFF),
                                         std::memory_order_relaxed);
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
            g_autotype_queue.tick(
                [](uint16_t cpc_key, bool pressed) {
                  // Same latch scoping as the threaded loop: only a break
                  // after the last key PRESS can satisfy a WAITBREAK
                  // (releases don't clear — the break can land mid-keystroke).
                  if (pressed) g_waitbreak_latch = false;
                  CPCScancode const scancode =
                      CPC.InputMapper->CPCscancodeFromCPCkey(
                          static_cast<CPC_KEYS>(cpc_key));
                  // Direct matrix manipulation (same as ipc_apply_keypress)
                  if (static_cast<byte>(scancode) == 0xff) return;
                  if (pressed) {
                    keyboard_matrix[static_cast<byte>(scancode) >> 4].fetch_and(
                        ~bit_values[static_cast<byte>(scancode) & 7],
                        std::memory_order_relaxed);
                    if (scancode & MOD_CPC_SHIFT) {
                      keyboard_matrix[0x25 >> 4].fetch_and(
                          ~bit_values[0x25 & 7], std::memory_order_relaxed);
                    } else {
                      keyboard_matrix[0x25 >> 4].fetch_or(
                          bit_values[0x25 & 7], std::memory_order_relaxed);
                    }
                    if (scancode & MOD_CPC_CTRL) {
                      keyboard_matrix[0x27 >> 4].fetch_and(
                          ~bit_values[0x27 & 7], std::memory_order_relaxed);
                    } else {
                      keyboard_matrix[0x27 >> 4].fetch_or(
                          bit_values[0x27 & 7], std::memory_order_relaxed);
                    }
                  } else {
                    keyboard_matrix[static_cast<byte>(scancode) >> 4].fetch_or(
                        bit_values[static_cast<byte>(scancode) & 7],
                        std::memory_order_relaxed);
                    keyboard_matrix[0x25 >> 4].fetch_or(
                        bit_values[0x25 & 7], std::memory_order_relaxed);
                    keyboard_matrix[0x27 >> 4].fetch_or(
                        bit_values[0x27 & 7], std::memory_order_relaxed);
                  }
                },
                [](uint16_t cmd) -> bool {
                  koncpc_menu_action(static_cast<int>(cmd));
                  // Same early-break latch consumption as the threaded loop.
                  if (cmd == KONCPC_WAITBREAK && g_waitbreak_latch) {
                    g_waitbreak_latch = false;
                    return false;
                  }
                  return cmd == KONCPC_WAITBREAK;
                });
          }
        }

        // Telnet console: drain input into autotype queue
        g_telnet.drain_input();

        // Handle IPC "step frame" — decrement remaining, pause when done
        if (g_ipc->frame_step_active.load()) {
          int const remaining = g_ipc->frame_step_remaining.fetch_sub(1) - 1;
          if (remaining <= 0) {
            cpc_pause();
            g_ipc->notify_frame_step_done();
          }
        }

        if (!g_headless) {
          if (SDL_GetTicks() < osd_timing) {
            print(static_cast<byte*>(back_surface->pixels) + CPC.scr_line_offs,
                  osd_message.c_str(), true);
          }
          std::string fpsText;
          if (CPC.scr_fps) {
            char chStr[15];
            snprintf(chStr, sizeof(chStr), "%3dFPS %3d%%",
                     static_cast<int>(dwFPS),
                     static_cast<int>(dwFPS) * 100 /
                         static_cast<int>(1000.0 / FRAME_PERIOD_MS));
            fpsText = chStr;
          }
          imgui_state.topbar_fps = fpsText;
          if (subcycle_bridge_active()) {
            subcycle_bridge_disk_leds(imgui_state.drive_a_led,
                                      imgui_state.drive_b_led);
          } else {
            imgui_state.drive_a_led = FDC.led && (FDC.command[1] & 1) == 0;
            imgui_state.drive_b_led = FDC.led && (FDC.command[1] & 1) == 1;
          }
        }
        if (!g_headless) {
          if (!CPC.skip_rendering) {
            uint64_t const displayStart = SDL_GetPerformanceCounter();

            video_display();  // phase A: texture upload + ImGui render (~3ms)

            // Push any partial audio buffer accumulated since the last
            // EC_SOUND_BUFFER. This tops up the audio queue before the
            // expensive phase B stall (floating viewports + GL context
            // switches, 0-60ms).
            {
              int const partial =
                  static_cast<int>(CPC.snd_bufferptr - pbSndBuffer.get());
              if (!CPC.paused && partial > 0) {
                audio_push_buffer(pbSndBuffer.get(), partial);
                CPC.snd_bufferptr = pbSndBuffer.get();
              }
            }

            video_display_b();  // phase B: floating viewports + window swap

            uint64_t const displayEnd = SDL_GetPerformanceCounter();
            displayTimeAccum.fetch_add(displayEnd - displayStart,
                                       std::memory_order_relaxed);

            // Check audio queue after display (GL viewport stalls drain it)
            // Sample audio queue depth after display — catches GL stalls.
            // Only updates min (underrun counting is done in audio_push_buffer
            // to avoid double-counting).
            if (audio_stream && CPC.snd_ready) {
              int queued = SDL_GetAudioStreamQueued(audio_stream);
              queued = std::max(queued, 0);
              audio_queue_min_bytes = std::min(queued, audio_queue_min_bytes);
              if (queued < static_cast<int>(CPC.snd_buffersize) / 2 &&
                  audio_push_count > 0) {
                [[maybe_unused]] double const display_ms =
                    static_cast<double>(displayEnd - displayStart) * 1000.0 /
                    perfFreq;
                LOG_DEBUG("Audio low queue after display: "
                          << queued << "B, display took " << display_ms
                          << "ms");
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
    } else if (g_headless) {  // Headless paused: sleep (non-headless handled
                              // above)
      // Drain HTTP deferred actions even while paused (otherwise resume won't
      // work)
      if (g_m4_http.is_running()) g_m4_http.drain_pending();
      ipc_drain_input();
      std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
    }

    // Deferred video plugin switch (triggered by Options combo).
    // Lightweight path swaps CRT resources only; full path tears down
    // window/GL.
    if (imgui_state.video_reinit_pending) {
      imgui_state.video_reinit_pending = false;
      if (!video_try_lightweight_switch()) {
        // Full reinit — preserve window geometry so bars don't shrink the image
        int saved_w = 0, saved_h = 0, saved_x = 0, saved_y = 0;
        if (mainSDLWindow) {
          SDL_GetWindowSize(mainSDLWindow, &saved_w, &saved_h);
          SDL_GetWindowPosition(mainSDLWindow, &saved_x, &saved_y);
        }
        // Quiesce the Z80 thread before tearing down video: video_shutdown()
        // frees the triple-buffer ring, and the Z80 writes into / publishes
        // those buffers (back_surface == a ring buffer). Freeing them while the
        // Z80 runs is a use-after-free → segfault on renderer switch.
        bool const z80_was_paused =
            g_emu_paused.load(std::memory_order_relaxed);
        audio_pause();
        cpc_pause_and_wait();
        // Free cached save-state thumbnail textures while the OLD render device
        // is still alive — video_shutdown() destroys it, leaving stale GPU
        // handles that would be used/freed against a dead device on next use.
        imgui_invalidate_slot_thumbs();
        SDL_Delay(20);
        video_shutdown();
        if (video_init()) {
          fprintf(stderr,
                  "video_init() failed after plugin change. Aborting.\n");
          cleanExit(-1);
        }
        // Only restore window geometry if the output size didn't change
        // (i.e. only the plugin changed, not scale/fullscreen)
        bool const size_changed =
            (CPC.scr_scale != imgui_state.old_cpc_settings.scr_scale) ||
            (CPC.scr_window != imgui_state.old_cpc_settings.scr_window);
        if (saved_w > 0 && mainSDLWindow && !size_changed) {
          SDL_SetWindowSize(mainSDLWindow, saved_w, saved_h);
          SDL_SetWindowPosition(mainSDLWindow, saved_x, saved_y);
        }
#ifdef __APPLE__
        koncpc_setup_macos_menu();
#endif
        audio_resume();
        if (!z80_was_paused) cpc_resume();
      }
    }

    // Handle IPC "repaint" — re-render frame from RAM without Z80 advancement
    // Checked every loop (paused or unpaused)
    if (g_repaint_pending.load()) {
      std::string shot_path;
      {
        std::scoped_lock const lock(g_repaint_mutex);
        shot_path = g_repaint_screenshot_path;
        g_repaint_screenshot_path.clear();
      }

      subcycle_bridge_repaint(back_surface);

      if (!shot_path.empty()) {
        if (SDL_SavePNG(back_surface, shot_path)) {
          std::scoped_lock const lock(g_repaint_mutex);
          g_repaint_error = "SDL_SavePNG failed for " + shot_path;
        } else {
          LOG_INFO("Repaint screenshot saved to " + shot_path);
        }
      }

      video_display();  // Force update UI
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
