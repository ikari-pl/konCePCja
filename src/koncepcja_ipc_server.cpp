#include "koncepcja_ipc_server.h"
#include "log.h"
#include "autotype.h"
#include "gfx_finder.h"
#include "search_engine.h"
#include "imgui_ui.h"

#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <map>
#include <functional>
#include <chrono>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <iomanip>
#include <fstream>
#include <filesystem>
#include <zlib.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

#include "SDL3/SDL.h"
#include "koncepcja.h"
#include "z80.h"
#include "expr_parser.h"
#include "debug_timers.h"
#include "z80_disassembly.h"
#include "slotshandler.h"
#include "disk_format.h"
#include "disk_file_editor.h"
#include "disk_sector_editor.h"
#include "keyboard.h"
#include "cpc_key_tables.h"
#include "trace.h"
#include "gif_recorder.h"
#include "wav_recorder.h"
#include "ym_recorder.h"
#include "avi_recorder.h"
#include "symfile.h"
#include "pokes.h"
#include "config_profile.h"
#include "asic_debug.h"
#include "drive_status.h"
#include "crtc.h"
#include "data_areas.h"
#include "imgui_ui_testable.h"
#include "session_recording.h"
#include "silicon_disc.h"
#include "m4board.h"
#include "m4board_http.h"
#include "serial_interface.h"
#include "plotter.h"
#include "devtools_ui.h"
#include "z80_assembler.h"
#include "video.h"

extern t_z80regs z80;
extern t_CPC CPC;
extern t_CRTC CRTC;
extern t_GateArray GateArray;
extern t_PSG PSG;
extern SDL_Surface *back_surface;
extern byte *pbRAM;
extern byte keyboard_matrix[16];
extern t_drive driveA;
extern t_drive driveB;
extern t_FDC FDC;
extern byte *memmap_ROM[256];
extern byte *pbExpansionROM;
extern byte *pbROMhi;

// Key tables are in cpc_key_tables.h (shared with autotype.cpp)

extern byte bit_values[];

// Parse a number from an IPC argument string.
// Accepts CPC-style hex prefixes ($, &, #), C-style 0x, bare decimal,
// and bare hex as fallback (e.g. "C004" → 0xC004).
// Throws std::invalid_argument with the original string on failure.
template<typename T, T(*Conv)(const std::string&, size_t*, int)>
static T parse_num_impl(const std::string& s) {
  if (s.empty()) throw std::invalid_argument("empty string");
  if (s[0] == '$' || s[0] == '&' || s[0] == '#')
    return Conv(s.substr(1), nullptr, 16);
  // Try base-0 auto-detect first (handles 0x, 0, decimal)
  try {
    size_t pos = 0;
    T v = Conv(s, &pos, 0);
    if (pos == s.size()) return v;
  } catch (const std::logic_error&) {}
  // Fallback: try as bare hex (e.g. "C004", "FF")
  try {
    size_t pos = 0;
    T v = Conv(s, &pos, 16);
    if (pos == s.size()) return v;
  } catch (const std::logic_error&) {}
  throw std::invalid_argument(s);
}

static unsigned long parse_number(const std::string& s) {
  return parse_num_impl<unsigned long, std::stoul>(s);
}

static int parse_int(const std::string& s) {
  return parse_num_impl<int, std::stoi>(s);
}

// Helper to prevent path traversal via IPC.
static bool is_safe_path(const std::string& path_str) {
  if (path_str.empty()) return false;
  std::filesystem::path p(path_str);
  // Normalize the path to collapse "." and ".." components.
  std::filesystem::path normalized = p.lexically_normal();

  // Reject any path that contains a ".." component after normalization.
  for (const auto& comp : normalized) {
    if (comp == "..") {
      return false;
    }
  }

  return true;
}

// Build a compact debug context string for IPC response trailers.
// All fields are O(1) struct reads — safe for every response.
static std::string build_debug_context() {
  char buf[256];
  // ROM config: bit2=lower ROM disable, bit3=upper ROM disable
  bool lo_rom = !(GateArray.ROM_config & 0x04);
  bool hi_rom = !(GateArray.ROM_config & 0x08);
  std::string rom_str;
  if (lo_rom && hi_rom)
    rom_str = "LO," + std::to_string(GateArray.upper_ROM);
  else if (lo_rom)
    rom_str = "LO,--";
  else if (hi_rom)
    rom_str = "--," + std::to_string(GateArray.upper_ROM);
  else
    rom_str = "--,--";

  auto& bps = z80_list_breakpoints_ref();
  auto& wps = z80_list_watchpoints_ref();
  auto& ios = z80_list_io_breakpoints_ref();

  int n = snprintf(buf, sizeof(buf),
    "[PC=%04X SP=%04X|%s|mode=%u|rom:%s|ram:%u/%uK|bp:%zu,wp:%zu,io:%zu",
    z80.PC.w.l, z80.SP.w.l,
    CPC.paused ? "paused" : "running",
    GateArray.scr_mode,
    rom_str.c_str(),
    GateArray.RAM_config & 7, CPC.ram_size,
    bps.size(), wps.size(), ios.size());

  // Clamp to buffer size in case of truncation; n<0 would indicate encoding error
  size_t len = (n < 0) ? 0 : std::min(static_cast<size_t>(n), sizeof(buf) - 1);
  std::string result(buf, len);
  if (z80.HALT) result += "|HALT";
  if (!z80.IFF1) result += "|DI";
  result += ']';
  return result;
}

// Append debug context trailer to an OK response.
static std::string ok_with_context(const std::string& body = "") {
  if (body.empty())
    return "OK " + build_debug_context() + "\n";
  return "OK " + body + " " + build_debug_context() + "\n";
}

// Append debug context trailer to an ERR response.
static std::string err_with_context(int code, const std::string& msg) {
  return "ERR " + std::to_string(code) + " " + msg + " " + build_debug_context() + "\n";
}

// Direct keyboard matrix manipulation that works even when CPC.paused is true.
// applyKeypress() refuses to act when paused, but IPC input commands need to
// set keys before resuming emulation for frame stepping.
static void ipc_apply_keypress(CPCScancode cpc_key, byte keyboard_matrix[], bool pressed) {
    if (static_cast<byte>(cpc_key) == 0xff) return;
    if (pressed) {
        keyboard_matrix[static_cast<byte>(cpc_key) >> 4] &= ~bit_values[static_cast<byte>(cpc_key) & 7];
        if (cpc_key & MOD_CPC_SHIFT) {
            keyboard_matrix[0x25 >> 4] &= ~bit_values[0x25 & 7];
        } else {
            keyboard_matrix[0x25 >> 4] |= bit_values[0x25 & 7];
        }
        if (cpc_key & MOD_CPC_CTRL) {
            keyboard_matrix[0x27 >> 4] &= ~bit_values[0x27 & 7];
        } else {
            keyboard_matrix[0x27 >> 4] |= bit_values[0x27 & 7];
        }
    } else {
        keyboard_matrix[static_cast<byte>(cpc_key) >> 4] |= bit_values[static_cast<byte>(cpc_key) & 7];
        keyboard_matrix[0x25 >> 4] |= bit_values[0x25 & 7];
        keyboard_matrix[0x27 >> 4] |= bit_values[0x27 & 7];
    }
}

static bool has_path_traversal(const std::string& path)
{
  for (const auto& comp : std::filesystem::path(path)) {
    if (comp == "..") return true;
  }
  return false;
}

namespace {
constexpr int kBasePort = 6543;
constexpr int kMaxPortAttempts = 10;  // try 6543..6552
KoncepcjaIpcServer* g_ipc_instance = nullptr;

struct IpcCommand {
  std::string name;
  std::string category;
  std::string usage;
  std::string description;
  std::string man_page;
  std::function<std::string(const std::vector<std::string>&, const std::string&)> handler;
};

std::map<std::string, IpcCommand> g_ipc_commands;
static std::once_flag g_ipc_init_once;

void register_command(const std::string& name, const std::string& category,
                      const std::string& usage, const std::string& description,
                      const std::string& man_page = "",
                      std::function<std::string(const std::vector<std::string>&, const std::string&)> handler = nullptr) {
  g_ipc_commands[name] = {name, category, usage, description, man_page, handler};
}

void init_command_registry() {
  if (!g_ipc_commands.empty()) return;

  register_command("ping", "CORE", "ping", "Check if server is alive",
    "Pings the IPC server. The server will respond with 'OK pong' if it is operational.",
    [](const auto&, const auto&) { return "OK pong\n"; });

  register_command("version", "CORE", "version", "Get emulator and protocol version",
    "Returns the emulator version string and the active IPC port number.",
    [](const auto&, const auto&) {
      int p = g_ipc_instance ? g_ipc_instance->port() : 0;
      return "OK koncepcja-" VERSION_STRING " port=" + std::to_string(p) + "\n";
    });

  register_command("quit", "CORE", "quit [code]", "Exit the emulator with optional exit code",
    "Terminates the emulator process immediately. An optional integer exit code can be provided.");

  register_command("disconnect", "CORE", "disconnect", "Close the current IPC connection",
    "Closes the current persistent IPC connection. The client socket is closed and the server "
    "returns to listening for new connections. Note: 'quit' always terminates the emulator "
    "(with optional exit code); use 'disconnect' to close only the connection.");

  register_command("pause", "CORE", "pause", "Pause emulation",
    "Stops the Z80 CPU and machine timers. The UI remains responsive and can still be used to inspect state.");

  register_command("run", "CORE", "run", "Resume emulation",
    "Resumes the machine from a paused state.");

  register_command("reset", "CORE", "reset [--no-resume]", "Reset the machine and resume (unless --no-resume is used)",
    "Performs a hardware reset of the CPC. By default, emulation resumes immediately after reset. "
    "Use --no-resume to keep the machine paused (useful for setting breakpoints before BIOS startup).");

  register_command("load", "CORE", "load <file>", "Load a .DSK, .SNA, or .CPR file",
    "Loads a media or state file. Supports Disk Images (.DSK), Snapshots (.SNA), and Cartridges (.CPR). "
    "The file type is determined by the extension. For disks, it loads into Drive A.");

  register_command("regs", "DEBUG", "regs", "Get all Z80 and core hardware registers",
    "Returns a comprehensive list of all Z80 registers (AF, BC, HL, etc.), alternate registers, "
    "and core hardware states (Gate Array, CRTC, PSG). Data is returned in space-separated key=val pairs.");

  register_command("mem", "DEBUG", "mem read <addr> <len> | mem write <addr> <hex>", "Access emulated memory",
    "Allows direct manipulation of the 64K/128K RAM space.\n"
    "  read: Returns <len> bytes starting at <addr> as a hex string.\n"
    "  write: Writes the provided <hex> string into memory starting at <addr>.");

  register_command("bp", "DEBUG",
    "bp list | bp add <addr> [if <expr>] [pass <N>] | bp del <addr> | bp clear",
    "Manage execution breakpoints",
    "Controls execution breakpoints.\n"
    "  list:  Shows all active breakpoints with their addresses and conditions.\n"
    "  add:   Adds a new breakpoint at <addr>. Optional condition and pass count.\n"
    "  del:   Removes the breakpoint at the specified address.\n"
    "  clear: Removes all breakpoints.");

  register_command("wp", "DEBUG",
    "wp add <addr> [len] [r|w|rw] [if <expr>] [pass <N>] | wp del <index> | wp list | wp clear",
    "Manage memory watchpoints",
    "Controls memory access watchpoints (break on read/write).\n"
    "  add:   Adds a watchpoint at <addr>. Default length=1, type=rw.\n"
    "  del:   Removes watchpoint by index.\n"
    "  list:  Shows all watchpoints with index, address, range, type and conditions.\n"
    "  clear: Removes all watchpoints.\n"
    "When a watchpoint triggers, 'wait bp' returns: OK PC=XXXX WATCH=1 WP_ADDR=XXXX WP_VAL=XX WP_OLD=XX");

  register_command("iobp", "DEBUG",
    "iobp add <port> [mask] [in|out|both] [if <expr>] | iobp del <index> | iobp list | iobp clear",
    "Manage IO breakpoints",
    "Controls I/O port breakpoints (break on IN/OUT instructions).\n"
    "  add:   Adds an IO breakpoint. Port can use BCXX shorthand (X=wildcard nibble).\n"
    "  del:   Removes IO breakpoint by index.\n"
    "  list:  Shows all IO breakpoints with index, port, mask, direction and conditions.\n"
    "  clear: Removes all IO breakpoints.");

  register_command("wait", "DEBUG",
    "wait pc <addr> [timeout] | wait mem <addr> <val> [mask] [timeout] | wait bp [timeout] | wait vbl <N> [timeout]",
    "Wait for a condition before returning",
    "Blocks until a condition is met or timeout (default 5000ms).\n"
    "  pc:  Resumes and waits until PC equals <addr>.\n"
    "  mem: Resumes and waits until memory at <addr> equals <val> (with optional mask).\n"
    "  bp:  Waits for a breakpoint or watchpoint hit. Watchpoint hits include WP_ADDR/WP_VAL/WP_OLD.\n"
    "  vbl: Waits for N vertical blanks (1/50th second each).");

  register_command("step", "DEBUG", "step in [N] | step over [N] | step out | step frame [N]", "Step the CPU or emulation frame",
    "Executes code and pauses again.\n"
    "  in [N]:    Steps into exactly N instructions (default 1). Traces inside subroutines.\n"
    "  over [N]:  Steps over the current CALL or RST. If not a call, it performs a single step.\n"
    "  out:       Continues execution until the current subroutine returns.\n"
    "  frame [N]: Steps exactly N video frames (1/50th of a second).");

  register_command("input", "INPUT", "input key <scan> | input type <text>", "Simulate user input",
    "Injects keyboard events directly into the emulated hardware.\n"
    "  key: Toggles a specific CPC scancode.\n"
    "  type: Automatically types a string of text. Supports WinAPE syntax (e.g., ~RETURN~).");

  register_command("disk", "HARDWARE", "disk ls <A|B> | disk put <A|B> <path>", "Manage emulated floppy disks",
    "High-level disk management.\n"
    "  ls: Lists files on the disk currently in the specified drive.\n"
    "  put: Copies a file from the host machine onto the emulated disk.");

  register_command("repaint", "DEBUG", "repaint [--screenshot PATH]", "Force screen render from current RAM state (no CPU advancement)",
    "Re-renders the CPC display from the current CRTC registers and video RAM "
    "contents without advancing the Z80 CPU. Useful when the CPU is paused and "
    "the display surface may not reflect the latest writes to screen memory.\n"
    "  --screenshot PATH  Save the repainted frame as a PNG file.");

  register_command("screenshot", "MEDIA", "screenshot window <path>", "Capture emulator window to PNG",
    "Saves a PNG file of the emulator window on the next rendered frame. "
    "Note: Currently captures the emulated CPC display only; ImGui overlays "
    "are omitted for simplicity across all rendering modes.");

  register_command("snapshot", "MEDIA", "snapshot save <path> | snapshot load <path>", "Manage machine snapshots",
    "Saves or loads the entire state of the emulated CPC into a .SNA file.");

  register_command("record", "MEDIA", "record wav|ym|avi <start|stop> [path]", "Record audio or video",
    "Records the emulator output to various file formats.\n"
    "  wav:  Record audio to a WAV file.\n"
    "  ym:   Record PSG registers to a YM file.\n"
    "  avi:  Record video and audio to an AVI file.");

  register_command("frames", "MEDIA", "frames dump <pattern> <count>", "Dump series of frames",
    "Dumps a sequence of frames to an animated GIF (if pattern ends in .gif) "
    "or a series of PNG files. Pattern can include %d or %04d for frame numbering.");

  register_command("devtools", "TOOLS", "devtools <on|off|show|hide> [name]", "Manage developer tools",
    "Toggles the developer tool overlay or individual debug windows.");

  register_command("profile", "TOOLS", "profile list | profile load <name>", "Manage configuration profiles",
    "Lists available profiles or switches the emulator to a different configuration.");

  register_command("config", "TOOLS", "config get <key> | config set <key> <val>", "Access emulator settings",
    "Reads or modifies internal emulator configuration variables.");

  register_command("search", "TOOLS", "search hex <pattern> | search text <string>", "Search memory",
    "Searches the 64KB RAM space for byte sequences or strings.");

  register_command("rom", "HARDWARE", "rom list | rom load <slot> <path>", "Manage expansion ROMs",
    "Lists currently mapped ROMs or loads a new ROM image into a specific slot (0-255).");

  register_command("asm", "TOOLS", "asm text <source> | asm assemble", "Z80 Assembler",
    "Enters Z80 assembly source code and assembles it into emulated memory.");

  register_command("telnet", "TOOLS", "telnet (port 6544)", "Text console for CPC I/O",
    "A telnet interface runs on port IPC+1 (default 6544) that captures all CPC text output "
    "(TXT_OUTPUT calls) and allows text input. Connecting returns a banner line followed by "
    "'---', then all accumulated text output since the last read.\n"
    "  The buffer drains on each read — reconnecting returns only new text since the previous connection.\n"
    "  Input bytes sent to the telnet port are fed into the CPC keyboard buffer (AutoTypeQueue).\n"
    "  ANSI escape sequences are converted to CPC special keys (arrows, DEL, ESC, TAB).\n"
    "  Preferred over screenshots for automated regression testing of text-based programs.\n"
    "  Example:  nc -w 1 localhost 6544 < /dev/null");

  register_command("disasm", "DEBUG",
    "disasm <addr> <count> [--symbols] | disasm follow <addr> | disasm refs <addr> | disasm export <start> <end> [path] [--symbols]",
    "Disassemble Z80 code",
    "Disassemble instructions, follow code flow, find cross-references, or export to .asm source.\n"
    "  <addr> <count>: Disassemble N instructions from address.\n"
    "  follow <addr>:  Recursively follow jumps/calls from entry point.\n"
    "  refs <addr>:    Find all CALL/JP/JR instructions targeting address.\n"
    "  export:         Export address range as Z80 assembly source file.");

  register_command("sym", "DEBUG",
    "sym load <path> | sym add <addr> <name> | sym del <name> | sym list [filter] | sym lookup <addr_or_name>",
    "Manage symbol table",
    "Load .sym files, add/delete symbols, list with optional filter, or look up by address or name.");

  register_command("data", "DEBUG",
    "data mark <start> <end> <bytes|words|text> [label] | data clear <addr|all> | data list",
    "Mark memory regions as data for disassembly",
    "Marks address ranges so the disassembler formats them as data (bytes, words or text) instead of code.");

  register_command("stack", "DEBUG", "stack [depth]",
    "Walk call stack",
    "Heuristic stack walk from SP upward. Shows return addresses with [call] markers and symbol names.\n"
    "  Default depth: 16, max: 128.");

  register_command("trace", "DEBUG",
    "trace on [buffer_size] | trace off | trace dump <path> | trace on_crash <path> | trace status",
    "Z80 instruction trace",
    "Ring-buffer execution trace (default 65536 entries).\n"
    "  on:       Enable tracing.\n"
    "  off:      Disable tracing.\n"
    "  dump:     Write trace buffer to text file.\n"
    "  on_crash: Auto-dump to file on breakpoint/watchpoint hit.\n"
    "  status:   Show active state and entry count.");

  register_command("timer", "DEBUG", "timer list | timer clear",
    "Debug timers",
    "List debug timers with hit count, last/min/max/average elapsed times. Clear all timers.");

  register_command("event", "DEBUG",
    "event on <trigger> <command> | event once <trigger> <command> | event off <id> | event list",
    "Fire IPC commands on triggers",
    "Register commands to auto-execute on PC match (pc=ADDR), memory write (mem=ADDR[:VAL]), or VBL interval (vbl=N).\n"
    "  on:   Register persistent event.\n"
    "  once: Register one-shot event (deleted after firing).\n"
    "  off:  Remove event by ID.\n"
    "  list: Show all registered events.");

  register_command("hash", "DEBUG",
    "hash vram | hash mem <addr> <len> | hash regs",
    "CRC32 hashes for CI assertions",
    "Compute CRC32 of screen surface, memory range, or packed register state.\n"
    "Useful for deterministic regression testing.");

  register_command("autotype", "INPUT", "autotype <text> | autotype status | autotype clear",
    "Queue text for keyboard injection",
    "Types text into the CPC using AutoTypeQueue. Supports WinAPE ~KEY~ syntax (e.g. ~ENTER~, ~CLR~).\n"
    "  status: Show pending queue length.\n"
    "  clear:  Cancel pending input.");

  register_command("status", "CORE", "status | status drives",
    "Emulator status summary",
    "Shows CPU state, clock speed, frame count, and paused status.\n"
    "  drives: Detailed drive info (loaded image, track position, format).");

  register_command("gfx", "DEBUG",
    "gfx view <addr> <w> <h> <mode> [path] | gfx decode <hex> <mode> | gfx paint <addr> <w> <h> <mode> <x> <y> <color> | gfx palette",
    "Graphics finder and pixel tools",
    "Decode CPC pixel data at any address in Mode 0/1/2.\n"
    "  view:    Render pixel block, optionally export to BMP.\n"
    "  decode:  Decode a hex byte into pixel values for a given mode.\n"
    "  paint:   Write a pixel value at (x,y) in a pixel block.\n"
    "  palette: Show current 16-colour palette as RGBA hex.");

  register_command("asic", "HARDWARE",
    "asic sprite <0-15> | asic palette | asic dma [<0-2>]",
    "Read 6128+ ASIC hardware state",
    "Query Plus Range hardware registers.\n"
    "  sprite:  Position, magnification and visibility for sprite 0-15.\n"
    "  palette: 32-colour ASIC RGB palette.\n"
    "  dma:     DMA channel transfer registers (0-2, or all).");

  register_command("sdisc", "HARDWARE",
    "sdisc status | sdisc clear | sdisc save <path> | sdisc load <path>",
    "Silicon Disc (256 KB RAM disc)",
    "Manage the 256 KB battery-backed Silicon Disc in banks 4-7 (6128+ only).\n"
    "  status: Show enabled/disabled and allocation.\n"
    "  clear:  Zero all contents.\n"
    "  save/load: Persist to/from file.");

  register_command("plotter", "HARDWARE",
    "plotter status | plotter export [path] | plotter clear",
    "HP-GL plotter emulation",
    "Query and control the HP 7470A plotter emulation.\n"
    "  status: Show pen position, pen up/down, selected pen, segment count.\n"
    "  export: Export current drawing to SVG (default: plotter_output.svg).\n"
    "  clear:  Clear all drawn segments.");

  register_command("m4", "HARDWARE",
    "m4 status | m4 ls | m4 cd <path> | m4 reset | m4 wifi [0|1] | m4 http [start|stop|status] | m4 ports | m4 port set/del <cpc> [host]",
    "M4 Board virtual filesystem & HTTP server",
    "Manage M4 Board virtual SD card backed by a host directory.\n"
    "  status: Show enabled state, open files, command count.\n"
    "  ls:     List files in current directory.\n"
    "  cd:     Change directory (path traversal protected).\n"
    "  reset:  Reset board state.\n"
    "  wifi:   Toggle network enable.");

  register_command("poke", "TOOLS",
    "poke load <path> | poke list | poke apply <game> <poke|all> | poke unapply <game> <poke> | poke write <addr> <val>",
    "Game cheats (.pok files)",
    "Load, apply and manage game cheats in .pok format.\n"
    "  load:    Load a .pok cheat file.\n"
    "  list:    List games and pokes with application status.\n"
    "  apply:   Apply a single poke or all pokes for a game.\n"
    "  unapply: Revert a previously applied poke.\n"
    "  write:   Direct single-byte write to memory.");

  register_command("session", "TOOLS",
    "session record <path> | session play <path> | session stop | session status",
    "Session recording and replay",
    "Record full emulator sessions (input + state) and play them back.\n"
    "  record: Start recording to file.\n"
    "  play:   Play back a recorded session.\n"
    "  stop:   Stop recording or playback.\n"
    "  status: Show state (idle/recording/playing), frame and event counts.");
}

void breakpoint_hit_hook(word pc, bool watchpoint) {
  if (g_ipc_instance) {
    g_ipc_instance->notify_breakpoint_hit(pc, watchpoint);
  }
}


// Split a line on semicolons for command chaining (e.g. "pause; bp add $4000; run").
// Trims leading/trailing whitespace from each segment. Skips empty segments.
std::vector<std::string> split_semicolons(const std::string& s) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start < s.size()) {
    size_t pos = s.find(';', start);
    if (pos == std::string::npos) pos = s.size();
    // Trim whitespace from the segment
    size_t seg_start = start;
    while (seg_start < pos && (s[seg_start] == ' ' || s[seg_start] == '\t')) seg_start++;
    size_t seg_end = pos;
    while (seg_end > seg_start && (s[seg_end - 1] == ' ' || s[seg_end - 1] == '\t')) seg_end--;
    if (seg_end > seg_start)
      out.push_back(s.substr(seg_start, seg_end - seg_start));
    start = pos + 1;
  }
  return out;
}

std::vector<std::string> split_ws(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ' ' || c == '\t') {
      if (!cur.empty()) { out.push_back(cur); cur.clear(); }
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::string handle_command(const std::string& line) {
  std::call_once(g_ipc_init_once, init_command_registry);
  if (line.empty()) return "OK\n";
  auto parts = split_ws(line);
  if (parts.empty()) return "OK\n";

  try {

  const auto& cmd = parts[0];

  // Dispatch to registered handler if available
  auto cmd_it = g_ipc_commands.find(cmd);
  if (cmd_it != g_ipc_commands.end() && cmd_it->second.handler) {
    return cmd_it->second.handler(parts, line);
  }

  if (cmd == "help") {
    if (parts.size() > 1) {
      auto it = g_ipc_commands.find(parts[1]);
      if (it != g_ipc_commands.end()) {
        std::ostringstream oss;
        oss << "OK usage: " << it->second.usage << "\n";
        oss << "DESCRIPTION: " << it->second.description << "\n";
        if (!it->second.man_page.empty()) {
          oss << "\n" << it->second.man_page << "\n";
        }
        return oss.str();
      }
      return "ERR 404 no specific help for '" + parts[1] + "'. Try 'help' for the list.\n";
    }

    std::map<std::string, std::vector<std::string>> categories;
    for (const auto& [name, info] : g_ipc_commands) {
      categories[info.category].push_back(info.usage);
    }

    std::ostringstream oss;
    oss << "OK available commands (usage: help <command>):\n"
        << "  Protocol: persistent connections, ';' chains commands, 'disconnect' closes.\n"
        << "  Numbers: 0x, $, &, # hex prefixes accepted (e.g. $C000, &4000, #BB5A).\n";
    static const std::vector<std::string> order = {"CORE", "DEBUG", "HARDWARE", "INPUT", "MEDIA", "TOOLS"};
    for (const auto& cat : order) {
      if (categories.find(cat) == categories.end()) continue;
      oss << "  " << std::setw(10) << std::left << (cat + ":") << " ";
      const auto& cmds = categories[cat];
      for (size_t i = 0; i < cmds.size(); i++) {
        oss << cmds[i] << (i == cmds.size() - 1 ? "" : ", ");
      }
      oss << "\n";
    }
    return oss.str();
  }

  if (cmd == "commands") {
    std::string resp = "OK\n";
    for (const auto& [name, info] : g_ipc_commands) {
      resp += name + "\n";
    }
    return resp;
  }

  if (cmd == "quit") {
    int code = 0;
    if (parts.size() >= 2) {
      try { code = parse_int(parts[1]); }
      catch (const std::exception&) { return "ERR 400 bad-exit-code\n"; }
    }
    cleanExit(code, false);
    return "OK\n"; // unreachable, but satisfies return type
  }
  if (cmd == "hash" && parts.size() >= 2) {
    char buf[64];
    if (parts[1] == "vram") {
      // Hash the visible video memory (back_surface pixels)
      if (!back_surface) return "ERR 503 no-surface\n";
      uLong crc = crc32(0L, nullptr, 0);
      crc = crc32(crc, static_cast<const Bytef*>(back_surface->pixels),
                  static_cast<uInt>(back_surface->h * back_surface->pitch));
      snprintf(buf, sizeof(buf), "OK crc32=%08lX\n", crc);
      return std::string(buf);
    }
    if (parts[1] == "mem" && parts.size() >= 4) {
      unsigned int addr, len;
      try {
        addr = parse_number(parts[2]);
        len = parse_number(parts[3]);
      } catch (const std::exception&) { return "ERR 400 bad-number\n"; }
      if (len > 65536) len = 65536; // Clamp to full address space
      uLong crc = crc32(0L, nullptr, 0);
      // Read through direct Z80 memory (SmartWatch only, no watchpoints)
      const unsigned int CHUNK_SIZE = 4096;
      std::vector<byte> tmp(CHUNK_SIZE);
      unsigned int remaining = len;
      unsigned int current_addr = addr;

      while (remaining > 0) {
        unsigned int chunk = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : remaining;
        for (unsigned int i = 0; i < chunk; i++) {
          tmp[i] = z80_read_mem(static_cast<word>(current_addr + i));
        }
        crc = crc32(crc, tmp.data(), static_cast<uInt>(chunk));
        remaining -= chunk;
        current_addr += chunk;
      }

      snprintf(buf, sizeof(buf), "OK crc32=%08lX\n", crc);
      return std::string(buf);
    }
    if (parts[1] == "regs") {
      // Pack register state and hash it
      PACK_BEGIN struct
#ifndef _MSC_VER
      __attribute__((packed))
#endif
      {
        word AF, BC, DE, HL, IX, IY, SP, PC;
        word AFx, BCx, DEx, HLx;
        byte I, R, IM, IFF1, IFF2;
      } PACK_END packed;
      packed.AF = z80.AF.w.l; packed.BC = z80.BC.w.l;
      packed.DE = z80.DE.w.l; packed.HL = z80.HL.w.l;
      packed.IX = z80.IX.w.l; packed.IY = z80.IY.w.l;
      packed.SP = z80.SP.w.l; packed.PC = z80.PC.w.l;
      packed.AFx = z80.AFx.w.l; packed.BCx = z80.BCx.w.l;
      packed.DEx = z80.DEx.w.l; packed.HLx = z80.HLx.w.l;
      packed.I = z80.I; packed.R = z80.R;
      packed.IM = z80.IM; packed.IFF1 = z80.IFF1; packed.IFF2 = z80.IFF2;
      uLong crc = crc32(0L, nullptr, 0);
      crc = crc32(crc, reinterpret_cast<const Bytef*>(&packed), sizeof(packed));
      snprintf(buf, sizeof(buf), "OK crc32=%08lX\n", crc);
      return std::string(buf);
    }
    return "ERR 400 bad-args (hash vram|mem|regs)\n";
  }

  if (cmd == "pause") {
    cpc_pause();
    return ok_with_context();
  }
  if (cmd == "run") {
    cpc_resume();
    return ok_with_context();
  }
  if (cmd == "reset") {
    bool was_paused = CPC.paused;
    if (!was_paused) cpc_pause();
    emulator_reset();
    bool no_resume = false;
    for (size_t i = 1; i < parts.size(); i++) {
      if (parts[i] == "--no-resume") no_resume = true;
    }
    if (!no_resume) {
      cpc_resume();
    } else if (was_paused) {
      // Was already paused and user wants no-resume, keep paused
    }
    return ok_with_context();
  }
  if (cmd == "repaint") {
    std::string shot_path;
    for (size_t i = 1; i < parts.size(); i++) {
      if (parts[i] == "--screenshot" && i + 1 < parts.size()) {
        shot_path = parts[i+1];
        if (!is_safe_path(shot_path)) return "ERR 403 path-traversal-blocked\n";
        break;
      }
    }
    
    {
      std::lock_guard<std::mutex> lock(g_repaint_mutex);
      g_repaint_screenshot_path = shot_path;
      g_repaint_error.clear();
      g_repaint_done.store(false);
      g_repaint_pending.store(true);
    }
    
    // Wait for completion (with 5s timeout)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!g_repaint_done.load()) {
      if (std::chrono::steady_clock::now() > deadline) {
        return "ERR 408 timeout\n";
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    {
      std::lock_guard<std::mutex> lock(g_repaint_mutex);
      if (!g_repaint_error.empty()) {
        return "ERR 500 " + g_repaint_error + "\n";
      }
    }
    
    return "OK\n";
  }

  if (cmd == "load") {
    if (parts.size() < 2) return "ERR 400 bad-args\n";
    const std::string& path = parts[1];
    if (!is_safe_path(path)) return "ERR 403 path-traversal-blocked\n";
    std::string lower = path;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto dot = lower.find_last_of('.');
    if (dot == std::string::npos) return "ERR 415 unsupported\n";
    std::string ext = lower.substr(dot);
    if (ext == ".dsk") {
      CPC.driveA.file = path;
      CPC.driveA.zip_index = 0;
      return file_load(CPC.driveA) == 0 ? ok_with_context() : "ERR 500 load-dsk\n";
    }
    if (ext == ".sna") {
      bool was_paused = CPC.paused;
      if (!was_paused) cpc_pause();
      CPC.snapshot.file = path;
      CPC.snapshot.zip_index = 0;
      int rc = file_load(CPC.snapshot);
      if (!was_paused) cpc_resume();
      return rc == 0 ? ok_with_context() : "ERR 500 load-sna\n";
    }
    if (ext == ".cpr") {
      CPC.cartridge.file = path;
      CPC.cartridge.zip_index = 0;
      return file_load(CPC.cartridge) == 0 ? ok_with_context() : "ERR 500 load-cpr\n";
    }
    if (ext == ".bin") {
      bin_load(path, 0x6000);
      return ok_with_context();
    }
    return "ERR 415 unsupported\n";
  }
  if ((cmd == "reg" || cmd == "regs") && parts.size() >= 2 && parts[1] == "set") {
    if (parts.size() < 4) return "ERR 400 bad-args\n";
    std::string reg = parts[2];
    for (auto& c : reg) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    unsigned int value = parse_number(parts[3]);

    auto set8 = [&](byte& target) { target = static_cast<byte>(value); };
    auto set16 = [&](word& target) { target = static_cast<word>(value); };

    if (reg == "A") set8(z80.AF.b.h);
    else if (reg == "F") set8(z80.AF.b.l);
    else if (reg == "B") set8(z80.BC.b.h);
    else if (reg == "C") set8(z80.BC.b.l);
    else if (reg == "D") set8(z80.DE.b.h);
    else if (reg == "E") set8(z80.DE.b.l);
    else if (reg == "H") set8(z80.HL.b.h);
    else if (reg == "L") set8(z80.HL.b.l);
    else if (reg == "I") set8(z80.I);
    else if (reg == "R") set8(z80.R);
    else if (reg == "IM") set8(z80.IM);
    else if (reg == "HALT") set8(z80.HALT);
    else if (reg == "IFF1") set8(z80.IFF1);
    else if (reg == "IFF2") set8(z80.IFF2);
    else if (reg == "AF") set16(z80.AF.w.l);
    else if (reg == "BC") set16(z80.BC.w.l);
    else if (reg == "DE") set16(z80.DE.w.l);
    else if (reg == "HL") set16(z80.HL.w.l);
    else if (reg == "IX") set16(z80.IX.w.l);
    else if (reg == "IY") set16(z80.IY.w.l);
    else if (reg == "SP") set16(z80.SP.w.l);
    else if (reg == "PC") set16(z80.PC.w.l);
    else if (reg == "AF'" || reg == "AFX") set16(z80.AFx.w.l);
    else if (reg == "BC'" || reg == "BCX") set16(z80.BCx.w.l);
    else if (reg == "DE'" || reg == "DEX") set16(z80.DEx.w.l);
    else if (reg == "HL'" || reg == "HLX") set16(z80.HLx.w.l);
    else return "ERR 400 bad-reg\n";

    return ok_with_context();
  }
  if ((cmd == "reg" || cmd == "regs") && parts.size() >= 2 && parts[1] == "get") {
    if (parts.size() < 3) return "ERR 400 bad-args\n";
    std::string reg = parts[2];
    for (auto& c : reg) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    auto out8 = [&](byte v) {
      char buf[8];
      snprintf(buf, sizeof(buf), "OK %02X\n", v);
      return std::string(buf);
    };
    auto out16 = [&](word v) {
      char buf[16];
      snprintf(buf, sizeof(buf), "OK %04X\n", v);
      return std::string(buf);
    };

    if (reg == "A") return out8(z80.AF.b.h);
    if (reg == "F") return out8(z80.AF.b.l);
    if (reg == "B") return out8(z80.BC.b.h);
    if (reg == "C") return out8(z80.BC.b.l);
    if (reg == "D") return out8(z80.DE.b.h);
    if (reg == "E") return out8(z80.DE.b.l);
    if (reg == "H") return out8(z80.HL.b.h);
    if (reg == "L") return out8(z80.HL.b.l);
    if (reg == "I") return out8(z80.I);
    if (reg == "R") return out8(z80.R);
    if (reg == "IM") return out8(z80.IM);
    if (reg == "HALT") return out8(z80.HALT);
    if (reg == "IFF1") return out8(z80.IFF1);
    if (reg == "IFF2") return out8(z80.IFF2);
    if (reg == "AF") return out16(z80.AF.w.l);
    if (reg == "BC") return out16(z80.BC.w.l);
    if (reg == "DE") return out16(z80.DE.w.l);
    if (reg == "HL") return out16(z80.HL.w.l);
    if (reg == "IX") return out16(z80.IX.w.l);
    if (reg == "IY") return out16(z80.IY.w.l);
    if (reg == "SP") return out16(z80.SP.w.l);
    if (reg == "PC") return out16(z80.PC.w.l);
    if (reg == "AF'" || reg == "AFX") return out16(z80.AFx.w.l);
    if (reg == "BC'" || reg == "BCX") return out16(z80.BCx.w.l);
    if (reg == "DE'" || reg == "DEX") return out16(z80.DEx.w.l);
    if (reg == "HL'" || reg == "HLX") return out16(z80.HLx.w.l);

    return "ERR 400 bad-reg\n";
  }
  if ((cmd == "reg" || cmd == "regs") && parts.size() >= 2 && parts[1] == "crtc") {
    // regs crtc → CRTC 6845 registers + internal counters
    std::ostringstream resp;
    resp << "OK";
    for (int i = 0; i < 18; i++) {
      char buf[16];
      snprintf(buf, sizeof(buf), " R%d=%02X", i, CRTC.registers[i]);
      resp << buf;
    }
    char buf[128];
    snprintf(buf, sizeof(buf),
      " VCC=%02X VLC=%02X HCC=%02X HSC=%02X VSC=%02X VMA=%04X R52=%02X SL=%02X",
      CRTC.line_count, CRTC.raster_count, CRTC.char_count,
      CRTC.hsw_count, CRTC.vsw_count, CRTC.addr,
      CRTC.reg5, CRTC.sl_count);
    resp << buf << "\n";
    return resp.str();
  }
  if ((cmd == "reg" || cmd == "regs") && parts.size() >= 2 && parts[1] == "ga") {
    // regs ga → Gate Array state
    std::ostringstream resp;
    resp << "OK";
    char buf[64];
    snprintf(buf, sizeof(buf), " MODE=%u PEN=%02X", GateArray.scr_mode, GateArray.pen);
    resp << buf;
    for (int i = 0; i < 17; i++) {
      snprintf(buf, sizeof(buf), " INK%d=%02X", i, GateArray.ink_values[i]);
      resp << buf;
    }
    snprintf(buf, sizeof(buf), " ROM_CFG=%02X RAM_CFG=%02X SL=%02X INT_DELAY=%02X",
             GateArray.ROM_config, GateArray.RAM_config,
             GateArray.sl_count, GateArray.int_delay);
    resp << buf << "\n";
    return resp.str();
  }
  if ((cmd == "reg" || cmd == "regs") && parts.size() >= 2 && parts[1] == "psg") {
    // regs psg → AY-3-8912 registers
    std::ostringstream resp;
    resp << "OK";
    char buf[32];
    for (int i = 0; i < 16; i++) {
      snprintf(buf, sizeof(buf), " R%d=%02X", i, PSG.RegisterAY.Index[i]);
      resp << buf;
    }
    snprintf(buf, sizeof(buf), " SELECT=%02X CONTROL=%02X", PSG.reg_select, PSG.control);
    resp << buf << "\n";
    return resp.str();
  }
  if ((cmd == "reg" || cmd == "regs") && parts.size() >= 2 && parts[1] == "asic") {
    if (parts.size() >= 3 && parts[2] == "dma") {
      return "OK\n" + asic_dump_dma() + "\n";
    }
    if (parts.size() >= 3 && parts[2] == "sprites") {
      return "OK\n" + asic_dump_sprites() + "\n";
    }
    if (parts.size() >= 3 && parts[2] == "interrupts") {
      return "OK\n" + asic_dump_interrupts() + "\n";
    }
    if (parts.size() >= 3 && parts[2] == "palette") {
      return "OK\n" + asic_dump_palette() + "\n";
    }
    // regs asic → full ASIC state dump
    return "OK\n" + asic_dump_all() + "\n";
  }
  if (cmd == "reg") return "ERR 400 usage: reg (get|set|crtc|ga|psg|asic) ...\n";
  // Top-level "asic" commands for detailed views
  if (cmd == "asic" && parts.size() >= 2) {
    if (parts[1] == "sprite") {
      if (parts.size() < 3) return "ERR 400 bad-args (asic sprite <0-15>)\n";
      int idx = 0;
      try { idx = parse_int(parts[2]); } catch (const std::exception&) { return "ERR 400 bad-args (asic sprite <0-15>)\n"; }
      if (idx < 0 || idx > 15) return "ERR 400 sprite index out of range (0-15)\n";
      return "OK\n" + asic_dump_sprite(idx) + "\n";
    }
    if (parts[1] == "palette") {
      return "OK\n" + asic_dump_palette() + "\n";
    }
    if (parts[1] == "dma") {
      if (parts.size() >= 3) {
        int ch = 0;
        try { ch = parse_int(parts[2]); } catch (const std::exception&) { return "ERR 400 bad-args (asic dma <0-2>)\n"; }
        if (ch < 0 || ch > 2) return "ERR 400 DMA channel out of range (0-2)\n";
        return "OK\n" + asic_dump_dma_channel(ch) + "\n";
      }
      return "OK\n" + asic_dump_dma() + "\n";
    }
    return "ERR 400 bad-args (asic sprite|palette|dma)\n";
  }
  if (cmd == "regs") {
    char out[256];
    snprintf(out, sizeof(out),
      "OK A=%02X F=%02X B=%02X C=%02X D=%02X E=%02X H=%02X L=%02X "
      "IX=%04X IY=%04X SP=%04X PC=%04X IM=%u HALT=%u\n",
      z80.AF.b.h, z80.AF.b.l,
      z80.BC.b.h, z80.BC.b.l,
      z80.DE.b.h, z80.DE.b.l,
      z80.HL.b.h, z80.HL.b.l,
      z80.IX.w.l, z80.IY.w.l,
      z80.SP.w.l, z80.PC.w.l,
      z80.IM, z80.HALT);
    return std::string(out);
  }
  if (cmd == "screenshot") {
    if (parts.size() >= 3 && parts[1] == "window") {
      std::string path = parts[2];
      // Force z80_execute to return so the main loop can render a frame
      bool was_paused = CPC.paused;
      z80_stop_requested.store(true, std::memory_order_relaxed);
      if (!was_paused) cpc_pause();
      video_request_window_screenshot(path);
      // Wait for the main loop to render a frame and capture (up to 5 seconds)
      for (int i = 0; i < 500; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (access(path.c_str(), F_OK) == 0) {
          if (!was_paused) cpc_resume();
          return "OK\n";
        }
      }
      if (!was_paused) cpc_resume();
      return "ERR 504 timeout\n";
    }
    if (parts.size() >= 2) {
      if (dumpScreenTo(parts[1])) return "OK\n";
      return "ERR 503 no-surface\n";
    }
    dumpScreen();
    return "OK\n";
  }
  if (cmd == "devtools") {
    if (parts.size() >= 3 && parts[1] == "show") {
      bool* ptr = g_devtools_ui.window_ptr(parts[2]);
      if (!ptr) return "ERR 404 unknown window\n";
      *ptr = true;
      return "OK\n";
    }
    if (parts.size() >= 3 && parts[1] == "hide") {
      bool* ptr = g_devtools_ui.window_ptr(parts[2]);
      if (!ptr) return "ERR 404 unknown window\n";
      *ptr = false;
      return "OK\n";
    }
    imgui_state.show_devtools = true;
    return "OK\n";
  }
  if (cmd == "snapshot" && parts.size() >= 2) {
    if (parts[1] == "save") {
      if (parts.size() < 3) return "ERR 400 bad-args\n";
      if (!is_safe_path(parts[2])) return "ERR 403 path-traversal-blocked\n";
      if (snapshot_save(parts[2]) == 0) return ok_with_context();
      return "ERR 500 snapshot-save\n";
    }
    if (parts[1] == "load") {
      if (parts.size() < 3) return "ERR 400 bad-args\n";
      if (!is_safe_path(parts[2])) return "ERR 403 path-traversal-blocked\n";
      bool was_paused = CPC.paused;
      if (!was_paused) cpc_pause();
      int rc = snapshot_load(parts[2]);
      if (!was_paused) cpc_resume();
      return rc == 0 ? ok_with_context() : "ERR 500 snapshot-load\n";
    }
  }
  if (cmd == "snapshot") return "ERR 400 usage: snapshot (save|load) <path>\n";
  if (cmd == "mem" && parts.size() >= 4 && parts[1] == "read") {
    // mem read <addr> <len> [--view=read|write] [--bank=N] [ascii]
    unsigned int addr = parse_number(parts[2]);
    unsigned int len = parse_number(parts[3]);
    bool with_ascii = false;
    int view_mode = 0; // 0=read(default), 1=write
    int raw_bank = -1; // -1=not set
    for (size_t pi = 4; pi < parts.size(); pi++) {
      if (parts[pi] == "ascii") with_ascii = true;
      else if (parts[pi].rfind("--view=", 0) == 0) {
        std::string v = parts[pi].substr(7);
        if (v == "write") view_mode = 1;
      }
      else if (parts[pi].rfind("--bank=", 0) == 0) {
        raw_bank = parse_int(parts[pi].substr(7));
      }
    }
    std::ostringstream resp;
    resp << "OK ";
    std::string ascii_str;
    for (unsigned int i = 0; i < len; i++) {
      byte v;
      if (raw_bank >= 0) {
        v = z80_read_mem_raw_bank(static_cast<word>(addr + i), raw_bank);
      } else if (view_mode == 1) {
        v = z80_read_mem_via_write_bank(static_cast<word>(addr + i));
      } else {
        v = z80_read_mem(static_cast<word>(addr + i));
      }
      resp << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << static_cast<int>(v);
      if (with_ascii) {
        char c = (v >= 32 && v <= 126) ? static_cast<char>(v) : '.';
        ascii_str.push_back(c);
        if ((i + 1) % 16 == 0) {
          resp << " |" << ascii_str << "| ";
          ascii_str.clear();
        }
      }
    }
    if (!ascii_str.empty()) {
      resp << " |" << ascii_str << "|";
    }
    resp << "\n";
    return resp.str();
  }
  if (cmd == "mem" && parts.size() >= 4 && parts[1] == "write") {
    // mem write <addr> <hexbytes...>
    unsigned int addr = parse_number(parts[2]);
    std::string hex;
    for (size_t i = 3; i < parts.size(); i++) hex += parts[i];
    if (hex.size() % 2 != 0) return "ERR 400 bad-hex\n";
    for (size_t i = 0; i < hex.size(); i += 2) {
      std::string byte_str = hex.substr(i, 2);
      byte v = static_cast<byte>(std::stoul(byte_str, nullptr, 16));
      z80_write_mem(static_cast<word>(addr + (i/2)), v);
    }
    g_devtools_ui.disasm_cache_invalidate();
    return ok_with_context();
  }
  if (cmd == "mem" && parts.size() >= 5 && parts[1] == "fill") {
    // mem fill <addr> <len> <hex-pattern>
    unsigned int addr = parse_number(parts[2]);
    unsigned int len = parse_number(parts[3]);
    const std::string& hex = parts[4];
    if (hex.empty() || hex.size() % 2 != 0) return "ERR 400 bad-hex\n";
    std::vector<byte> pattern;
    for (size_t i = 0; i < hex.size(); i += 2) {
      pattern.push_back(static_cast<byte>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    for (unsigned int i = 0; i < len; i++) {
      z80_write_mem(static_cast<word>(addr + i), pattern[i % pattern.size()]);
    }
    g_devtools_ui.disasm_cache_invalidate();
    return ok_with_context();
  }
  if (cmd == "mem" && parts.size() >= 5 && parts[1] == "compare") {
    // mem compare <addr1> <addr2> <len>
    unsigned int addr1 = parse_number(parts[2]);
    unsigned int addr2 = parse_number(parts[3]);
    unsigned int len = parse_number(parts[4]);
    int diff_count = 0;
    std::string diffs;
    for (unsigned int i = 0; i < len; i++) {
      byte v1 = z80_read_mem(static_cast<word>(addr1 + i));
      byte v2 = z80_read_mem(static_cast<word>(addr2 + i));
      if (v1 != v2) {
        diff_count++;
        if (diff_count <= 64) {
          char buf[32];
          snprintf(buf, sizeof(buf), " %04X:%02X:%02X",
                   static_cast<unsigned int>(addr1 + i), v1, v2);
          diffs += buf;
        }
      }
    }
    return "OK diffs=" + std::to_string(diff_count) + diffs + "\n";
  }
  if (cmd == "mem" && parts.size() >= 4 && parts[1] == "cpu-read") {
    // mem cpu-read <addr> <len> — reads through CPU memory map (SmartWatch/ROM banking)
    // but does NOT trigger watchpoints or IPC events.
    try {
      unsigned int addr = parse_number(parts[2]);
      unsigned int len = parse_number(parts[3]);
      if (len > 0x10000) return "ERR 400 len exceeds 64K\n";
      std::ostringstream resp;
      resp << "OK ";
      for (unsigned int i = 0; i < len; i++) {
        byte v = z80_cpu_read_mem(static_cast<word>(addr + i));
        resp << std::hex << std::uppercase << std::setfill('0') << std::setw(2)
             << static_cast<int>(v);
      }
      resp << "\n";
      return resp.str();
    } catch (const std::exception&) {
      return "ERR 400 bad-number\n";
    }
  }
  if (cmd == "mem" && parts.size() >= 4 && parts[1] == "cpu-write") {
    // mem cpu-write <addr> <hexbytes...> — writes through CPU memory map
    // but does NOT trigger watchpoints or IPC mem-write events.
    try {
      unsigned int addr = parse_number(parts[2]);
      std::string hex;
      for (size_t i = 3; i < parts.size(); i++) hex += parts[i];
      if (hex.size() % 2 != 0) return "ERR 400 bad-hex\n";
      if (hex.size() / 2 > 0x10000) return "ERR 400 len exceeds 64K\n";
      for (size_t i = 0; i < hex.size(); i += 2) {
        std::string byte_str = hex.substr(i, 2);
        byte v = static_cast<byte>(std::stoul(byte_str, nullptr, 16));
        z80_cpu_write_mem(static_cast<word>(addr + (i/2)), v);
      }
      return "OK\n";
    } catch (const std::exception&) {
      return "ERR 400 bad-number\n";
    }
  }
  if (cmd == "disasm" && parts.size() >= 2) {
    // disasm follow <addr> — recursive disassembly following jumps
    if (parts[1] == "follow" && parts.size() >= 3) {
      unsigned int addr = parse_number(parts[2]);
      std::vector<word> eps = { static_cast<word>(addr) };
      DisassembledCode code = disassemble(eps);
      std::ostringstream resp;
      resp << "OK count=" << code.lines.size() << "\n";
      for (const auto& line : code.lines) {
        std::string sym = g_symfile.lookupAddr(line.address_);
        if (!sym.empty()) resp << sym << ":\n";
        resp << line << "\n";
      }
      return resp.str();
    }
    // disasm refs <addr> — cross-reference search
    if (parts[1] == "refs" && parts.size() >= 3) {
      unsigned int target = parse_number(parts[2]);
      std::ostringstream resp;
      resp << "OK";
      int found = 0;
      DisassembledCode dummy;
      std::vector<dword> dummy_eps;
      for (unsigned int addr = 0; addr <= 0xFFFF && found < 100; ) {
        auto line = disassemble_one(addr, dummy, dummy_eps);
        if (line.ref_address_ == static_cast<word>(target) &&
            !line.ref_address_string_.empty()) {
          char buf[8];
          snprintf(buf, sizeof(buf), " %04X", addr);
          resp << buf;
          found++;
        }
        addr += line.Size();
      }
      resp << "\n";
      return resp.str();
    }
    // disasm export <start> <end> [path] [--symbols]
    // Exports memory range as assembler source (.asm)
    if (parts[1] == "export" && parts.size() >= 4) {
      unsigned int start_addr, end_addr;
      try {
        start_addr = parse_number(parts[2]);
        end_addr = parse_number(parts[3]);
      } catch (const std::exception&) {
        return "ERR 400 bad-address\n";
      }
      if (start_addr > 0xFFFF || end_addr > 0xFFFF || start_addr > end_addr)
        return "ERR 400 bad-range\n";

      std::string path;
      bool with_symbols = false;
      for (size_t pi = 4; pi < parts.size(); pi++) {
        if (parts[pi] == "--symbols") with_symbols = true;
        else if (path.empty()) path = parts[pi];
      }

      std::ostringstream oss;
      char buf[32];
      snprintf(buf, sizeof(buf), "$%04X", start_addr);
      oss << "; Disassembly export from konCePCja\n";
      oss << "org " << buf << "\n\n";

      DisassembledCode code;
      std::vector<dword> entry_points;
      word pos = static_cast<word>(start_addr);
      word end_pos = static_cast<word>(end_addr);

      while (pos <= end_pos) {
        // Emit symbol label if present
        if (with_symbols) {
          std::string sym = g_symfile.lookupAddr(pos);
          if (!sym.empty()) oss << sym << ":\n";
        }

        // Check data areas first
        const DataArea* da = g_data_areas.find(pos);
        if (da) {
          int remaining = static_cast<int>(da->end) - static_cast<int>(pos) + 1;
          int max_bytes = (da->type == DataType::TEXT) ? 64 : 8;
          int buf_len = std::min(remaining, max_bytes);
          // Don't exceed end of export range
          if (pos + buf_len - 1 > end_pos) buf_len = end_pos - pos + 1;
          std::vector<uint8_t> membuf(buf_len);
          for (int mi = 0; mi < buf_len; mi++) {
            membuf[mi] = z80_read_mem(static_cast<word>(pos + mi));
          }
          int line_bytes = 0;
          std::string formatted = g_data_areas.format_at(pos, membuf.data(), membuf.size(), &line_bytes);
          oss << "  " << formatted;
          // Add hex comment
          oss << "  ; ";
          snprintf(buf, sizeof(buf), "%04X:", static_cast<unsigned>(pos));
          oss << buf;
          for (int mi = 0; mi < line_bytes && mi < 8; mi++) {
            snprintf(buf, sizeof(buf), " %02X", membuf[mi]);
            oss << buf;
          }
          oss << "\n";
          if (line_bytes == 0) line_bytes = 1;
          unsigned int next = static_cast<unsigned int>(pos) + line_bytes;
          if (next > 0xFFFF || next > end_addr + 1u) break;
          pos = static_cast<word>(next);
        } else {
          auto line = disassemble_one(pos, code, entry_points);
          code.lines.insert(line);
          std::string instr = line.instruction_;
          // Replace hex refs with symbol names if requested
          if (with_symbols && !line.ref_address_string_.empty()) {
            std::string sym = g_symfile.lookupAddr(line.ref_address_);
            if (!sym.empty()) {
              auto ref_pos = instr.find(line.ref_address_string_);
              if (ref_pos != std::string::npos) {
                instr.replace(ref_pos, line.ref_address_string_.size(), sym);
              }
            }
          }
          oss << "  " << instr;
          // Add hex byte comment
          oss << "  ; ";
          snprintf(buf, sizeof(buf), "%04X:", static_cast<unsigned>(pos));
          oss << buf;
          int sz = line.Size();
          for (int bi = 0; bi < sz; bi++) {
            snprintf(buf, sizeof(buf), " %02X", z80_read_mem(static_cast<word>(pos + bi)));
            oss << buf;
          }
          oss << "\n";
          unsigned int next = static_cast<unsigned int>(pos) + line.Size();
          if (next > 0xFFFF || next > end_addr + 1u) break;
          pos = static_cast<word>(next);
        }
      }

      std::string result = oss.str();
      if (!path.empty()) {
        // Reject path traversal
        for (const auto& comp : std::filesystem::path(path)) {
          if (comp == "..") return "ERR 403 path-traversal\n";
        }
        std::ofstream f(path);
        if (!f) return "ERR 500 cannot-write " + path + "\n";
        f << result;
        f.close();
        snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(result.size()));
        return std::string("OK written ") + buf + " bytes to " + path + "\n";
      }
      return "OK\n" + result;
    }
    // disasm <addr> <count> [--symbols]
    if (parts.size() >= 3) {
      unsigned int addr;
      int count;
      try {
        addr = parse_number(parts[1]);
        count = parse_int(parts[2]);
      } catch (const std::exception&) {
        return "ERR 400 bad-address\n";
      }
      if (count < 0) return "ERR 400 bad-args\n";
      bool with_symbols = false;
      for (size_t pi = 3; pi < parts.size(); pi++) {
        if (parts[pi] == "--symbols") with_symbols = true;
      }
      std::ostringstream resp;
      resp << "OK\n";
      DisassembledCode code;
      std::vector<dword> entry_points;
      word pos = static_cast<word>(addr);
      for (int i = 0; i < count; i++) {
        if (with_symbols) {
          std::string sym = g_symfile.lookupAddr(pos);
          if (!sym.empty()) resp << sym << ":\n";
        }
        // Check if this address is in a data area
        const DataArea* da = g_data_areas.find(pos);
        if (da) {
          // Read only the bytes needed for this line from emulated memory
          int remaining = static_cast<int>(da->end) - static_cast<int>(pos) + 1;
          int max_bytes = (da->type == DataType::TEXT) ? 64 : 8;
          int buf_len = std::min(remaining, max_bytes);
          std::vector<uint8_t> membuf(buf_len);
          for (int mi = 0; mi < buf_len; mi++) {
            membuf[mi] = z80_read_mem(static_cast<word>(pos + mi));
          }
          int line_bytes = 0;
          std::string formatted = g_data_areas.format_at(pos, membuf.data(), membuf.size(), &line_bytes);
          resp << std::setfill('0') << std::setw(4) << std::hex << pos << ":          " << formatted << "\n";
          if (line_bytes == 0) line_bytes = 1;
          pos = static_cast<word>(pos + line_bytes);
        } else {
          auto line = disassemble_one(pos, code, entry_points);
          code.lines.insert(line);
          if (with_symbols && !line.ref_address_string_.empty()) {
            // Try to replace hex reference with symbol name
            std::string sym = g_symfile.lookupAddr(line.ref_address_);
            if (!sym.empty()) {
              std::string instr = line.instruction_;
              auto ref_pos = instr.find(line.ref_address_string_);
              if (ref_pos != std::string::npos) {
                instr.replace(ref_pos, line.ref_address_string_.size(), sym);
              }
              resp << std::setfill('0') << std::setw(4) << std::hex << line.address_ << ": ";
              resp << std::setfill(' ') << std::setw(8) << line.opcode_ << " " << instr << "\n";
            } else {
              resp << line << "\n";
            }
          } else {
            resp << line << "\n";
          }
          pos = static_cast<word>(pos + line.Size());
        }
      }
      return resp.str();
    }
  }
  if (cmd == "bp" && parts.size() >= 2) {
    if (parts[1] == "add" && parts.size() >= 3) {
      unsigned int addr = parse_number(parts[2]);
      // Parse optional "if <expr>" and "pass <N>" in a single pass.
      // Tokens after "if" up to "pass" (or end) form the expression.
      std::string cond_str;
      int pass_count = 0;
      bool in_expr = false;
      for (size_t pi = 3; pi < parts.size(); pi++) {
        std::string kw = parts[pi];
        std::string kwl = kw;
        for (auto& c : kwl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (kwl == "if") {
          in_expr = true;
          continue;
        }
        if (kwl == "pass" && pi + 1 < parts.size()) {
          in_expr = false;
          pass_count = parse_int(parts[pi + 1]);
          pi++; // skip the value
          continue;
        }
        if (in_expr) {
          if (!cond_str.empty()) cond_str += " ";
          cond_str += kw;
        }
      }
      if (!cond_str.empty()) {
        std::string err;
        auto ast = expr_parse(cond_str, err);
        if (!ast) return "ERR 400 bad-expr: " + err + "\n";
        z80_add_breakpoint_cond(static_cast<word>(addr), std::move(ast), cond_str, pass_count);
      } else {
        z80_add_breakpoint(static_cast<word>(addr));
      }
      return ok_with_context();
    }
    if (parts[1] == "del" && parts.size() >= 3) {
      unsigned int addr = parse_number(parts[2]);
      z80_del_breakpoint(static_cast<word>(addr));
      return ok_with_context();
    }
    if (parts[1] == "clear") {
      z80_clear_breakpoints();
      return ok_with_context();
    }
    if (parts[1] == "list") {
      const auto& bps = z80_list_breakpoints_ref();
      std::ostringstream resp;
      resp << "OK count=" << bps.size();
      for (const auto& b : bps) {
        resp << " " << std::hex << std::uppercase << std::setfill('0') << std::setw(4)
             << static_cast<unsigned int>(b.address);
        if (!b.condition_str.empty()) {
          resp << "[if " << b.condition_str << "]";
        }
        if (b.pass_count > 0) {
          resp << "[pass " << std::dec << b.pass_count << "]";
        }
      }
      resp << "\n";
      return resp.str();
    }
    return "ERR 400 usage: bp (add <addr> [if <expr>] [pass <N>] | del <addr> | list | clear)\n";
  }
  if (cmd == "bp") return "ERR 400 usage: bp (add <addr> [if <expr>] [pass <N>] | del <addr> | list | clear)\n";
  if (cmd == "iobp" && parts.size() >= 2) {
    if (parts[1] == "add" && parts.size() >= 3) {
      // iobp add <port> [mask] [in|out|both]
      // Port can be shorthand like "BCXX" where X=wildcard nibble
      std::string port_str = parts[2];
      word port_val = 0, mask_val = 0xFFFF;
      if (port_str.size() == 4 && (port_str.find('X') != std::string::npos || port_str.find('x') != std::string::npos)) {
        // Shorthand: BCXX → port=0xBC00, mask=0xFF00
        port_val = 0;
        mask_val = 0;
        for (int ni = 0; ni < 4; ni++) {
          char ch = port_str[ni];
          int shift = (3 - ni) * 4;
          if (ch == 'X' || ch == 'x') {
            // wildcard nibble: port bits = 0, mask bits = 0
          } else {
            unsigned int nibble = 0;
            if (ch >= '0' && ch <= '9') nibble = ch - '0';
            else if (ch >= 'A' && ch <= 'F') nibble = ch - 'A' + 10;
            else if (ch >= 'a' && ch <= 'f') nibble = ch - 'a' + 10;
            port_val |= static_cast<word>(nibble << shift);
            mask_val |= static_cast<word>(0xF << shift);
          }
        }
      } else {
        port_val = static_cast<word>(parse_number(port_str));
      }
      // Parse optional mask, direction, and condition
      IOBreakpointDir dir = IO_BOTH;
      std::string cond_str;
      for (size_t pi = 3; pi < parts.size(); pi++) {
        std::string arg = parts[pi];
        std::string argl = arg;
        for (auto& c : argl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (argl == "in") dir = IO_IN;
        else if (argl == "out") dir = IO_OUT;
        else if (argl == "both") dir = IO_BOTH;
        else if (argl == "if") {
          // Everything after "if" is the expression
          std::string expr;
          for (size_t ei = pi + 1; ei < parts.size(); ei++) {
            if (!expr.empty()) expr += " ";
            expr += parts[ei];
          }
          cond_str = expr;
          break;
        }
        else if (!arg.empty() && (argl.rfind("0x", 0) == 0 || (argl[0] >= '0' && argl[0] <= '9')
                 || arg[0] == '$' || arg[0] == '&' || arg[0] == '#')) {
          mask_val = static_cast<word>(parse_number(arg));
        }
      }
      if (!cond_str.empty()) {
        std::string err;
        auto ast = expr_parse(cond_str, err);
        if (!ast) return "ERR 400 bad-expr: " + err + "\n";
        z80_add_io_breakpoint_cond(port_val, mask_val, dir, std::move(ast), cond_str);
      } else {
        z80_add_io_breakpoint(port_val, mask_val, dir);
      }
      return ok_with_context();
    }
    if (parts[1] == "del" && parts.size() >= 3) {
      int idx = parse_int(parts[2]);
      z80_del_io_breakpoint(idx);
      return ok_with_context();
    }
    if (parts[1] == "clear") {
      z80_clear_io_breakpoints();
      return ok_with_context();
    }
    if (parts[1] == "list") {
      const auto& bps = z80_list_io_breakpoints_ref();
      std::ostringstream resp;
      resp << "OK count=" << bps.size();
      for (size_t i = 0; i < bps.size(); i++) {
        const char* dir_str = "both";
        if (bps[i].dir == IO_IN) dir_str = "in";
        else if (bps[i].dir == IO_OUT) dir_str = "out";
        resp << " " << i << ":" << std::hex << std::uppercase << std::setfill('0')
             << std::setw(4) << static_cast<unsigned>(bps[i].port) << "/"
             << std::setw(4) << static_cast<unsigned>(bps[i].mask) << "/"
             << dir_str;
        if (!bps[i].condition_str.empty()) {
          resp << "[if " << bps[i].condition_str << "]";
        }
      }
      resp << "\n";
      return resp.str();
    }
    return "ERR 400 bad-iobp-cmd (add|del|clear|list)\n";
  }
  if (cmd == "iobp") return "ERR 400 usage: iobp (add|del|clear|list)\n";
  if (cmd == "step") {
    cpc_pause();
    // "step in [N]" or "step [N]" — single-step instructions
    if (parts.size() == 1 || (parts.size() >= 2 && (parts[1] == "in" || std::isdigit(static_cast<unsigned char>(parts[1][0]))))) {
      int count = 1;
      if (parts.size() >= 2) {
        if (parts[1] == "in") {
          if (parts.size() >= 3) count = parse_int(parts[2]);
        } else {
          try { count = parse_int(parts[1]); } catch (...) { count = 1; }
        }
      }
      for (int i = 0; i < count; i++) {
        z80_step_instruction();
      }
      return ok_with_context();
    }
    // "step frame [N]" — advance N complete frames, then pause
    if (parts.size() >= 2 && parts[1] == "frame") {
      int n = 1;
      if (parts.size() >= 3) n = parse_int(parts[2]);
      if (n < 1) return "ERR 400 bad-args\n";
      g_ipc_instance->frame_step_remaining.store(n);
      g_ipc_instance->frame_step_active.store(true);
      cpc_resume();
      g_ipc_instance->wait_frame_step_done();
      return ok_with_context();
    }
    // "step over [N]" — step over CALL/RST (or single-step if not a call)
    if (parts.size() >= 2 && parts[1] == "over") {
      int count = 1;
      if (parts.size() >= 3) count = parse_int(parts[2]);
      for (int i = 0; i < count; i++) {
        word pc = z80.PC.w.l;
        if (z80_is_call_or_rst(pc)) {
          int len = z80_instruction_length(pc);
          word next_pc = static_cast<word>(pc + len);
          z80_add_breakpoint_ephemeral(next_pc);
          // Clear stale hits before resume to avoid race conditions
          uint16_t dummy_pc; bool dummy_watch;
          g_ipc_instance->consume_breakpoint_hit(dummy_pc, dummy_watch);
          cpc_resume();
          // Wait for breakpoint hit
          auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
          while (true) {
            uint16_t hit_pc = 0;
            bool watch = false;
            if (g_ipc_instance->consume_breakpoint_hit(hit_pc, watch)) {
              if (hit_pc == next_pc) break;
              // If we hit a different breakpoint, stop stepping
              return "OK breakpoint-hit\n";
            }
            if (std::chrono::steady_clock::now() > deadline) {
              cpc_pause();
              return err_with_context(408, "timeout");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
          cpc_pause();
        } else {
          z80_step_instruction();
        }
      }
      return ok_with_context();
    }
    // "step out" — run until current function returns
    if (parts.size() >= 2 && parts[1] == "out") {
      z80.step_out = 1;
      z80.step_out_addresses.clear();
      // Clear stale hits before resume to avoid race conditions
      uint16_t dummy_pc; bool dummy_watch;
      g_ipc_instance->consume_breakpoint_hit(dummy_pc, dummy_watch);
      cpc_resume();
      // Wait for step_out to complete (main loop pauses when step_in >= 2)
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (true) {
        uint16_t hit_pc = 0;
        bool watch = false;
        if (g_ipc_instance->consume_breakpoint_hit(hit_pc, watch)) break;
        if (CPC.paused) break;  // main loop paused after step completion
        if (std::chrono::steady_clock::now() > deadline) {
          cpc_pause();
          z80.step_out = 0;
          return err_with_context(408, "timeout");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      if (!CPC.paused) cpc_pause();
      return ok_with_context();
    }
    // "step to <addr>" — run-to-cursor (ephemeral breakpoint)
    if (parts.size() >= 3 && parts[1] == "to") {
      unsigned int addr = parse_number(parts[2]);
      z80_add_breakpoint_ephemeral(static_cast<word>(addr));
      // Clear stale hits before resume to avoid race conditions
      uint16_t dummy_pc; bool dummy_watch;
      g_ipc_instance->consume_breakpoint_hit(dummy_pc, dummy_watch);
      cpc_resume();
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (true) {
        uint16_t hit_pc = 0;
        bool watch = false;
        if (g_ipc_instance->consume_breakpoint_hit(hit_pc, watch)) break;
        if (std::chrono::steady_clock::now() > deadline) {
          cpc_pause();
          return err_with_context(408, "timeout");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      cpc_pause();
      return ok_with_context();
    }
    // "step [N]" — single-step N instructions
    cpc_pause();
    int count = 1;
    if (parts.size() >= 2) count = parse_int(parts[1]);
    for (int i = 0; i < count; i++) z80_step_instruction();
    return ok_with_context();
  }

  // Trace commands: trace on [size], trace off, trace dump <path>, trace on_crash <path>
  if (cmd == "trace" && parts.size() >= 2) {
    if (parts[1] == "on") {
      int size = 65536;
      if (parts.size() >= 3) size = parse_int(parts[2]);
      g_trace.enable(size);
      return "OK\n";
    }
    if (parts[1] == "off") {
      g_trace.disable();
      return "OK\n";
    }
    if (parts[1] == "dump" && parts.size() >= 3) {
      if (g_trace.dump(parts[2])) {
        char buf[64];
        snprintf(buf, sizeof(buf), "OK entries=%d\n", g_trace.entry_count());
        return std::string(buf);
      }
      return "ERR 500 trace-dump-failed\n";
    }
    if (parts[1] == "on_crash" && parts.size() >= 3) {
      g_trace.set_crash_path(parts[2]);
      if (!g_trace.is_active()) g_trace.enable();
      return "OK\n";
    }
    if (parts[1] == "status") {
      char buf[64];
      snprintf(buf, sizeof(buf), "OK active=%d entries=%d\n",
               g_trace.is_active() ? 1 : 0, g_trace.entry_count());
      return std::string(buf);
    }
    return "ERR 400 bad-trace-cmd (on|off|dump|on_crash|status)\n";
  }
  if (cmd == "trace") return "ERR 400 usage: trace (on|off|dump|on_crash|status)\n";

  // Frame dumps: frames dump <path_pattern> <count> [delay_cs]
  // If path ends in .gif → animated GIF; otherwise → PNG series
  if (cmd == "frames" && parts.size() >= 4 && parts[1] == "dump") {
    std::string pattern = parts[2];
    if (!is_safe_path(pattern)) return "ERR 403 path-traversal-blocked\n";
    int frame_count = parse_int(parts[3]);
    if (frame_count < 1 || frame_count > 10000) return "ERR 400 bad-count\n";

    // Check if output is GIF
    std::string lower_pattern = pattern;
    for (auto& c : lower_pattern) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    bool is_gif = (lower_pattern.size() >= 4 &&
                   lower_pattern.substr(lower_pattern.size() - 4) == ".gif");

    if (is_gif) {
      // Animated GIF output
      if (!back_surface) return "ERR 503 no-surface\n";
      int delay_cs = 2; // default: 50fps (matches CPC VBL rate)
      if (parts.size() >= 5) delay_cs = parse_int(parts[4]);
      GifRecorder gif;
      if (!gif.begin(back_surface->w, back_surface->h, delay_cs)) {
        return "ERR 500 gif-begin-failed\n";
      }
      for (int i = 0; i < frame_count; i++) {
        // Advance one frame
        if (g_ipc_instance) {
          g_ipc_instance->frame_step_remaining.store(1);
          g_ipc_instance->frame_step_active.store(true);
          cpc_resume();
          while (g_ipc_instance->frame_step_active.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        }
        gif.add_frame(static_cast<const uint8_t*>(back_surface->pixels),
                       back_surface->pitch);
      }
      if (gif.end(pattern)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "OK frames=%d\n", frame_count);
        return std::string(buf);
      }
      return "ERR 500 gif-write-failed\n";
    }

    // PNG series output
    int saved = 0;
    for (int i = 0; i < frame_count; i++) {
      if (g_ipc_instance) {
        g_ipc_instance->frame_step_remaining.store(1);
        g_ipc_instance->frame_step_active.store(true);
        cpc_resume();
        while (g_ipc_instance->frame_step_active.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
      char fname[512];
      if (pattern.find('%') != std::string::npos) {
        // Safe replacement for common patterns
        std::string fname_str = pattern;
        size_t p;
        if ((p = fname_str.find("%04d")) != std::string::npos) {
           char buf[16]; snprintf(buf, sizeof(buf), "%04d", i);
           fname_str.replace(p, 4, buf);
        } else if ((p = fname_str.find("%d")) != std::string::npos) {
           fname_str.replace(p, 2, std::to_string(i));
        } else {
           return "ERR 400 bad-format (only %d or %04d supported)\n";
        }
        snprintf(fname, sizeof(fname), "%s", fname_str.c_str());
      } else {
        snprintf(fname, sizeof(fname), "%s_%04d.png", pattern.c_str(), i);
      }
      if (dumpScreenTo(fname)) saved++;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "OK saved=%d\n", saved);
    return std::string(buf);
  }

  // Input replay commands
  if (cmd == "input" && parts.size() >= 2) {
    // Helper: resolve a key name to CPC scancode
    auto resolve_key = [](const std::string& name) -> std::pair<bool, CPCScancode> {
      // Try friendly short names first (case-insensitive)
      std::string upper = name;
      for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      auto it = cpc_key_names().find(upper);
      if (it != cpc_key_names().end()) {
        return {true, CPC.InputMapper->CPCscancodeFromCPCkey(it->second)};
      }
      // Single char shortcut: "A" → CPC_A, "a" → CPC_a, "1" → CPC_1
      if (name.size() == 1) {
        auto charIt = cpc_char_to_key().find(name[0]);
        if (charIt != cpc_char_to_key().end()) {
          return {true, CPC.InputMapper->CPCscancodeFromCPCkey(charIt->second)};
        }
      }
      return {false, 0};
    };

    if (parts[1] == "keydown" && parts.size() >= 3) {
      auto [found, scancode] = resolve_key(parts[2]);
      if (!found) return "ERR 400 unknown-key\n";
      ipc_apply_keypress(scancode, keyboard_matrix, true);
      return "OK\n";
    }
    if (parts[1] == "keyup" && parts.size() >= 3) {
      auto [found, scancode] = resolve_key(parts[2]);
      if (!found) return "ERR 400 unknown-key\n";
      ipc_apply_keypress(scancode, keyboard_matrix, false);
      return "OK\n";
    }
    if (parts[1] == "key" && parts.size() >= 3) {
      // Tap: press key, advance frames while held, then release
      auto [found, scancode] = resolve_key(parts[2]);
      if (!found) return "ERR 400 unknown-key\n";
      // Set key in matrix before resuming (bypasses CPC.paused guard)
      ipc_apply_keypress(scancode, keyboard_matrix, true);
      // Hold for 2 frames to ensure the CPC firmware scans it
      if (g_ipc_instance) {
        g_ipc_instance->frame_step_remaining.store(2);
        g_ipc_instance->frame_step_active.store(true);
        cpc_resume();
        while (g_ipc_instance->frame_step_active.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
      ipc_apply_keypress(scancode, keyboard_matrix, false);
      return "OK\n";
    }
    if (parts[1] == "type") {
      // Collect the rest of the line as text (may include spaces)
      // Find the start of the text after "input type "
      size_t pos = line.find("type ");
      if (pos == std::string::npos) return "ERR 400 bad-args\n";
      std::string text = line.substr(pos + 5);
      // Strip surrounding quotes if present
      if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text = text.substr(1, text.size() - 2);
      }
      for (char ch : text) {
        auto charIt = cpc_char_to_key().find(ch);
        if (charIt == cpc_char_to_key().end()) continue; // skip unmappable chars
        CPCScancode scancode = CPC.InputMapper->CPCscancodeFromCPCkey(charIt->second);
        // Set key in matrix before resuming (bypasses CPC.paused guard)
        ipc_apply_keypress(scancode, keyboard_matrix, true);
        // Hold for 2 frames
        if (g_ipc_instance) {
          g_ipc_instance->frame_step_remaining.store(2);
          g_ipc_instance->frame_step_active.store(true);
          cpc_resume();
          while (g_ipc_instance->frame_step_active.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        }
        ipc_apply_keypress(scancode, keyboard_matrix, false);
        // Wait 1 frame between chars for debouncer
        if (g_ipc_instance) {
          g_ipc_instance->frame_step_remaining.store(1);
          g_ipc_instance->frame_step_active.store(true);
          cpc_resume();
          while (g_ipc_instance->frame_step_active.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        }
      }
      return "OK\n";
    }
    if (parts[1] == "joy" && parts.size() >= 4) {
      int joy_num = parse_int(parts[2]);
      std::string dir = parts[3];
      for (auto& c : dir) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      bool release = (dir[0] == '-');
      if (release) dir = dir.substr(1);
      CPC_KEYS key;
      if (dir == "U" || dir == "UP") key = (joy_num == 0) ? CPC_J0_UP : CPC_J1_UP;
      else if (dir == "D" || dir == "DOWN") key = (joy_num == 0) ? CPC_J0_DOWN : CPC_J1_DOWN;
      else if (dir == "L" || dir == "LEFT") key = (joy_num == 0) ? CPC_J0_LEFT : CPC_J1_LEFT;
      else if (dir == "R" || dir == "RIGHT") key = (joy_num == 0) ? CPC_J0_RIGHT : CPC_J1_RIGHT;
      else if (dir == "F" || dir == "F1" || dir == "FIRE1") key = (joy_num == 0) ? CPC_J0_FIRE1 : CPC_J1_FIRE1;
      else if (dir == "F2" || dir == "FIRE2") key = (joy_num == 0) ? CPC_J0_FIRE2 : CPC_J1_FIRE2;
      else if (dir == "0") {
        // Release all directions
        for (auto k : {CPC_J0_UP, CPC_J0_DOWN, CPC_J0_LEFT, CPC_J0_RIGHT, CPC_J0_FIRE1, CPC_J0_FIRE2}) {
          CPC_KEYS jk = (joy_num == 0) ? k : static_cast<CPC_KEYS>(k + (CPC_J1_UP - CPC_J0_UP));
          ipc_apply_keypress(CPC.InputMapper->CPCscancodeFromCPCkey(jk), keyboard_matrix, false);
        }
        return "OK\n";
      }
      else return "ERR 400 bad-dir\n";
      CPCScancode scancode = CPC.InputMapper->CPCscancodeFromCPCkey(key);
      ipc_apply_keypress(scancode, keyboard_matrix, !release);
      return "OK\n";
    }
    return "ERR 400 bad-input-cmd (keydown|keyup|key|type|joy)\n";
  }
  if (cmd == "input") return "ERR 400 usage: input (keydown|keyup|key|type|joy)\n";

  if (cmd == "wait" && parts.size() >= 2) {
    auto timeout_ms = std::chrono::milliseconds(5000);
    auto deadline = std::chrono::steady_clock::now() + timeout_ms;

    if (parts[1] == "pc") {
      unsigned int addr = parse_number(parts[2]);
      if (parts.size() >= 4) deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(parse_int(parts[3]));
      cpc_resume();
      while (z80.PC.w.l != addr) {
        if (std::chrono::steady_clock::now() > deadline) {
          cpc_pause();
          return err_with_context(408, "timeout");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      cpc_pause();
      return ok_with_context();
    }
    if (parts[1] == "mem" && parts.size() >= 4) {
      unsigned int addr = parse_number(parts[2]);
      unsigned int val = parse_number(parts[3]);
      unsigned int mask = 0xFF;
      if (parts.size() >= 5) {
        if (parts[4].rfind("mask=", 0) == 0) {
          mask = parse_number(parts[4].substr(5));
          if (parts.size() >= 6) {
            deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(parse_int(parts[5]));
          }
        } else if (parts.size() >= 6) {
          mask = parse_number(parts[4]);
          deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(parse_int(parts[5]));
        } else {
          deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(parse_int(parts[4]));
        }
      }
      cpc_resume();
      while (true) {
        byte memv = z80_read_mem(static_cast<word>(addr));
        if ((memv & static_cast<byte>(mask)) == (static_cast<byte>(val) & static_cast<byte>(mask))) break;
        if (std::chrono::steady_clock::now() > deadline) {
          cpc_pause();
          return err_with_context(408, "timeout");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      cpc_pause();
      return ok_with_context();
    }
    if (parts[1] == "bp") {
      if (parts.size() >= 3) deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(parse_int(parts[2]));
      while (true) {
        if (g_ipc_instance) {
          uint16_t pc = 0;
          bool watch = false;
          if (g_ipc_instance->consume_breakpoint_hit(pc, watch)) {
            char resp[128];
            if (watch) {
              snprintf(resp, sizeof(resp), "OK PC=%04X WATCH=1 WP_ADDR=%04X WP_VAL=%02X WP_OLD=%02X\n",
                       pc, z80.watchpoint_addr, z80.watchpoint_value, z80.watchpoint_old);
            } else {
              snprintf(resp, sizeof(resp), "OK PC=%04X WATCH=0\n", pc);
            }
            return std::string(resp);
          }
        }
        if (std::chrono::steady_clock::now() > deadline) {
          return err_with_context(408, "timeout");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    if (parts[1] == "vbl") {
      int count = parse_int(parts[2]);
      if (parts.size() >= 4) deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(parse_int(parts[3]));
      cpc_resume();
      for (int i = 0; i < count; i++) {
        if (std::chrono::steady_clock::now() > deadline) {
          cpc_pause();
          return err_with_context(408, "timeout");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      cpc_pause();
      return ok_with_context();
    }
  }
  if (cmd == "wait") return "ERR 400 usage: wait (pc|mem|bp|vbl) ...\n";

  // Event system: event on <trigger> <command>, event off <id>, event list
  if (cmd == "event" && parts.size() >= 2) {
    if (parts[1] == "on" && parts.size() >= 4) {
      // Parse trigger: pc=0xADDR, mem=0xADDR[:VAL], vbl=N
      std::string trigger_str = parts[2];
      // Command is everything after the trigger
      size_t cmd_start = line.find(parts[2]) + parts[2].size();
      while (cmd_start < line.size() && line[cmd_start] == ' ') cmd_start++;
      std::string event_cmd = line.substr(cmd_start);

      IpcEvent ev{};
      ev.one_shot = false;

      if (trigger_str.rfind("pc=", 0) == 0) {
        ev.trigger = EventTrigger::PC;
        ev.address = static_cast<uint16_t>(parse_number(trigger_str.substr(3)));
      } else if (trigger_str.rfind("mem=", 0) == 0) {
        ev.trigger = EventTrigger::MEM_WRITE;
        std::string addr_val = trigger_str.substr(4);
        auto colon = addr_val.find(':');
        if (colon != std::string::npos) {
          ev.address = static_cast<uint16_t>(parse_number(addr_val.substr(0, colon)));
          ev.value = static_cast<uint8_t>(parse_number(addr_val.substr(colon + 1)));
          ev.match_value = true;
        } else {
          ev.address = static_cast<uint16_t>(parse_number(addr_val));
          ev.match_value = false;
        }
      } else if (trigger_str.rfind("vbl=", 0) == 0) {
        ev.trigger = EventTrigger::VBL;
        ev.vbl_interval = parse_int(trigger_str.substr(4));
        ev.vbl_counter = ev.vbl_interval;
      } else {
        return "ERR 400 bad-trigger (pc=ADDR|mem=ADDR[:VAL]|vbl=N)\n";
      }

      ev.command = event_cmd;
      int id = g_ipc_instance->add_event(ev);
      char buf[32];
      snprintf(buf, sizeof(buf), "OK id=%d\n", id);
      return std::string(buf);
    }
    if (parts[1] == "once" && parts.size() >= 4) {
      // Same as "event on" but one-shot (removed after first fire)
      std::string trigger_str = parts[2];
      size_t cmd_start = line.find(parts[2]) + parts[2].size();
      while (cmd_start < line.size() && line[cmd_start] == ' ') cmd_start++;
      std::string event_cmd = line.substr(cmd_start);

      IpcEvent ev{};
      ev.one_shot = true;

      if (trigger_str.rfind("pc=", 0) == 0) {
        ev.trigger = EventTrigger::PC;
        ev.address = static_cast<uint16_t>(parse_number(trigger_str.substr(3)));
      } else if (trigger_str.rfind("mem=", 0) == 0) {
        ev.trigger = EventTrigger::MEM_WRITE;
        std::string addr_val = trigger_str.substr(4);
        auto colon = addr_val.find(':');
        if (colon != std::string::npos) {
          ev.address = static_cast<uint16_t>(parse_number(addr_val.substr(0, colon)));
          ev.value = static_cast<uint8_t>(parse_number(addr_val.substr(colon + 1)));
          ev.match_value = true;
        } else {
          ev.address = static_cast<uint16_t>(parse_number(addr_val));
          ev.match_value = false;
        }
      } else if (trigger_str.rfind("vbl=", 0) == 0) {
        ev.trigger = EventTrigger::VBL;
        ev.vbl_interval = parse_int(trigger_str.substr(4));
        ev.vbl_counter = ev.vbl_interval;
      } else {
        return "ERR 400 bad-trigger (pc=ADDR|mem=ADDR[:VAL]|vbl=N)\n";
      }

      ev.command = event_cmd;
      int id = g_ipc_instance->add_event(ev);
      char buf[32];
      snprintf(buf, sizeof(buf), "OK id=%d\n", id);
      return std::string(buf);
    }
    if (parts[1] == "off" && parts.size() >= 3) {
      int id = parse_int(parts[2]);
      if (g_ipc_instance->remove_event(id)) return "OK\n";
      return "ERR 404 event-not-found\n";
    }
    if (parts[1] == "list") {
      auto evts = g_ipc_instance->list_events();
      std::ostringstream resp;
      resp << "OK count=" << evts.size() << "\n";
      for (const auto& e : evts) {
        const char* trig_name = "?";
        if (e.trigger == EventTrigger::PC) trig_name = "pc";
        else if (e.trigger == EventTrigger::MEM_WRITE) trig_name = "mem";
        else if (e.trigger == EventTrigger::VBL) trig_name = "vbl";
        resp << "  id=" << e.id << " trigger=" << trig_name << "=";
        if (e.trigger == EventTrigger::VBL) {
          resp << std::dec << e.vbl_interval;
        } else {
          resp << "0x" << std::hex << std::uppercase << std::setfill('0')
               << std::setw(4) << e.address;
        }
        if (e.one_shot) resp << " once";
        resp << " cmd=" << e.command << "\n";
      }
      return resp.str();
    }
    return "ERR 400 bad-event-cmd (on|once|off|list)\n";
  }

  if (cmd == "timer" && parts.size() >= 2) {
    if (parts[1] == "list") {
      const auto& timers = g_debug_timers.timers();
      std::ostringstream resp;
      resp << "OK count=" << timers.size();
      for (const auto& [id, t] : timers) {
        uint32_t avg = t.count > 0 ? static_cast<uint32_t>(t.total_us / t.count) : 0;
        resp << " id=" << id << " count=" << t.count
             << " last=" << t.last_us
             << " min=" << (t.min_us == UINT32_MAX ? 0 : t.min_us)
             << " max=" << t.max_us << " avg=" << avg;
      }
      resp << "\n";
      return resp.str();
    }
    if (parts[1] == "clear") {
      g_debug_timers.clear();
      return "OK\n";
    }
    return "ERR 400 bad-timer-cmd (list|clear)\n";
  }

  // --- Watchpoint commands ---
  if (cmd == "wp" && parts.size() >= 2) {
    if (parts[1] == "add" && parts.size() >= 3) {
      unsigned int addr = parse_number(parts[2]);
      word len = 1;
      WatchpointType wtype = READWRITE;
      std::string cond_str;
      int pass_count = 0;
      bool in_expr = false;
      for (size_t pi = 3; pi < parts.size(); pi++) {
        std::string kw = parts[pi];
        std::string kwl = kw;
        for (auto& c : kwl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (kwl == "if") {
          in_expr = true;
          continue;
        }
        if (kwl == "pass" && pi + 1 < parts.size()) {
          in_expr = false;
          pass_count = parse_int(parts[pi + 1]);
          pi++;
          continue;
        }
        if (in_expr) {
          if (!cond_str.empty()) cond_str += " ";
          cond_str += kw;
          continue;
        }
        if (kwl == "r") wtype = READ;
        else if (kwl == "w") wtype = WRITE;
        else if (kwl == "rw") wtype = READWRITE;
        else {
          // Try as length
          try {
            unsigned int v = parse_number(kw);
            len = static_cast<word>(v);
          } catch (const std::exception&) {}
        }
      }
      if (!cond_str.empty()) {
        std::string err;
        auto ast = expr_parse(cond_str, err);
        if (!ast) return "ERR 400 bad-expr: " + err + "\n";
        z80_add_watchpoint_cond(static_cast<word>(addr), len, wtype,
                                std::move(ast), cond_str, pass_count);
      } else {
        z80_add_watchpoint(static_cast<word>(addr), len, wtype);
      }
      return ok_with_context();
    }
    if (parts[1] == "del" && parts.size() >= 3) {
      int idx = parse_int(parts[2]);
      z80_del_watchpoint(idx);
      return ok_with_context();
    }
    if (parts[1] == "clear") {
      z80_clear_watchpoints();
      return ok_with_context();
    }
    if (parts[1] == "list") {
      const auto& wps = z80_list_watchpoints_ref();
      std::ostringstream resp;
      resp << "OK count=" << wps.size();
      for (size_t i = 0; i < wps.size(); i++) {
        const auto& w = wps[i];
        const char* type_str = "rw";
        if (w.type == READ) type_str = "r";
        else if (w.type == WRITE) type_str = "w";
        resp << " " << i << ":" << std::hex << std::uppercase << std::setfill('0')
             << std::setw(4) << static_cast<unsigned>(w.address)
             << "+" << std::dec << static_cast<unsigned>(w.length)
             << "/" << type_str;
        if (!w.condition_str.empty()) {
          resp << "[if " << w.condition_str << "]";
        }
        if (w.pass_count > 0) {
          resp << "[pass " << std::dec << w.pass_count << "]";
        }
      }
      resp << "\n";
      return resp.str();
    }
    return "ERR 400 bad-wp-cmd (add|del|clear|list)\n";
  }
  if (cmd == "wp") return "ERR 400 usage: wp (add|del|clear|list)\n";

  // --- Symbol commands ---
  if (cmd == "sym" && parts.size() >= 2) {
    if (parts[1] == "load" && parts.size() >= 3) {
      if (!is_safe_path(parts[2])) return "ERR 403 path-traversal-blocked\n";
      Symfile loaded(parts[2]);
      int count = 0;
      for (const auto& [addr, name] : loaded.Symbols()) {
        g_symfile.addSymbol(addr, name);
        count++;
      }
      g_devtools_ui.symtable_mark_dirty();
      char buf[32];
      snprintf(buf, sizeof(buf), "OK loaded=%d\n", count);
      return std::string(buf);
    }
    if (parts[1] == "add" && parts.size() >= 4) {
      unsigned int addr = parse_number(parts[2]);
      g_symfile.addSymbol(static_cast<word>(addr), parts[3]);
      g_devtools_ui.symtable_mark_dirty();
      return "OK\n";
    }
    if (parts[1] == "del" && parts.size() >= 3) {
      g_symfile.delSymbol(parts[2]);
      g_devtools_ui.symtable_mark_dirty();
      return "OK\n";
    }
    if (parts[1] == "list") {
      std::string filter;
      if (parts.size() >= 3) filter = parts[2];
      auto syms = g_symfile.listSymbols(filter);
      std::ostringstream resp;
      resp << "OK count=" << syms.size() << "\n";
      for (const auto& [addr, name] : syms) {
        resp << "  " << std::hex << std::uppercase << std::setfill('0')
             << std::setw(4) << static_cast<unsigned>(addr)
             << " " << name << "\n";
      }
      return resp.str();
    }
    if (parts[1] == "lookup" && parts.size() >= 3) {
      // Try as address first
      try {
        unsigned int addr = parse_number(parts[2]);
        std::string name = g_symfile.lookupAddr(static_cast<word>(addr));
        if (!name.empty()) {
          return "OK " + name + "\n";
        }
      } catch (const std::exception&) {}
      // Try as name
      word addr = 0;
      if (g_symfile.lookupName(parts[2], addr) == 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "OK %04X\n", static_cast<unsigned>(addr));
        return std::string(buf);
      }
      return "ERR 404 not-found\n";
    }
    return "ERR 400 bad-sym-cmd (load|add|del|list|lookup)\n";
  }
  if (cmd == "sym") return "ERR 400 usage: sym (load|add|del|list|lookup)\n";

  // --- Memory search ---
  if (cmd == "mem" && parts.size() >= 5 && parts[1] == "find") {
    unsigned int start = parse_number(parts[3]);
    unsigned int end = parse_number(parts[4]);
    if (end > 0xFFFF) end = 0xFFFF;

    if (parts[2] == "hex" && parts.size() >= 6) {
      // Parse hex pattern with ?? wildcards
      const std::string& hex = parts[5];
      std::vector<int> pattern; // -1 = wildcard, else byte value
      for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        if (hex[i] == '?' && hex[i + 1] == '?') {
          pattern.push_back(-1);
        } else {
          pattern.push_back(static_cast<int>(std::stoul(hex.substr(i, 2), nullptr, 16)));
        }
      }
      if (pattern.empty()) return "ERR 400 empty-pattern\n";
      std::ostringstream resp;
      resp << "OK";
      int found = 0;
      for (unsigned int addr = start; addr + pattern.size() - 1 <= end && found < 32; addr++) {
        bool match = true;
        for (size_t j = 0; j < pattern.size(); j++) {
          if (pattern[j] < 0) continue;
          if (z80_read_mem(static_cast<word>(addr + j)) != static_cast<byte>(pattern[j])) {
            match = false;
            break;
          }
        }
        if (match) {
          resp << " " << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << addr;
          found++;
        }
      }
      resp << "\n";
      return resp.str();
    }
    if (parts[2] == "text" && parts.size() >= 6) {
      // Collect text from remaining parts (may have spaces)
      std::string text;
      for (size_t pi = 5; pi < parts.size(); pi++) {
        if (!text.empty()) text += " ";
        text += parts[pi];
      }
      // Strip surrounding quotes
      if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text = text.substr(1, text.size() - 2);
      }
      if (text.empty()) return "ERR 400 empty-pattern\n";
      std::ostringstream resp;
      resp << "OK";
      int found = 0;
      for (unsigned int addr = start; addr + text.size() - 1 <= end && found < 32; addr++) {
        bool match = true;
        for (size_t j = 0; j < text.size(); j++) {
          if (z80_read_mem(static_cast<word>(addr + j)) != static_cast<byte>(text[j])) {
            match = false;
            break;
          }
        }
        if (match) {
          resp << " " << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << addr;
          found++;
        }
      }
      resp << "\n";
      return resp.str();
    }
    if (parts[2] == "asm" && parts.size() >= 6) {
      // Collect asm pattern from remaining parts
      std::string pattern;
      for (size_t pi = 5; pi < parts.size(); pi++) {
        if (!pattern.empty()) pattern += " ";
        pattern += parts[pi];
      }
      // Lowercase pattern for case-insensitive matching
      std::string lower_pattern = pattern;
      for (auto& c : lower_pattern) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      std::ostringstream resp;
      resp << "OK";
      int found = 0;
      DisassembledCode dummy;
      std::vector<dword> dummy_eps;
      for (unsigned int addr = start; addr <= end && found < 32; ) {
        auto line = disassemble_one(addr, dummy, dummy_eps);
        std::string lower_instr = line.instruction_;
        for (auto& c : lower_instr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        // Match: * = any substring in operand position
        bool match = false;
        if (lower_pattern.find('*') != std::string::npos) {
          // Split pattern at *, check prefix and suffix
          auto star = lower_pattern.find('*');
          std::string prefix = lower_pattern.substr(0, star);
          std::string suffix = lower_pattern.substr(star + 1);
          match = (lower_instr.rfind(prefix, 0) == 0);
          if (match && !suffix.empty()) {
            match = (lower_instr.size() >= suffix.size() &&
                     lower_instr.substr(lower_instr.size() - suffix.size()) == suffix);
          }
        } else {
          match = (lower_instr.find(lower_pattern) != std::string::npos);
        }
        if (match) {
          resp << " " << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << addr;
          found++;
        }
        addr += line.Size();
      }
      resp << "\n";
      return resp.str();
    }
    return "ERR 400 bad-find-type (hex|text|asm)\n";
  }
  if (cmd == "mem") return "ERR 400 usage: mem (read|write|fill|compare|cpu-read|cpu-write|find) ...\n";

  // --- Stack command ---
  if (cmd == "stack") {
    int depth = 16;
    if (parts.size() >= 2) depth = parse_int(parts[1]);
    if (depth < 1) depth = 1;
    if (depth > 128) depth = 128;
    word sp = z80.SP.w.l;
    std::ostringstream resp;
    resp << "OK depth=" << depth << "\n";
    DisassembledCode dummy;
    std::vector<dword> dummy_eps;
    for (int i = 0; i < depth; i++) {
      word addr = static_cast<word>(sp + i * 2);
      byte lo = z80_read_mem(addr);
      byte hi = z80_read_mem(static_cast<word>(addr + 1));
      word val = static_cast<word>((hi << 8) | lo);
      char buf[64];
      snprintf(buf, sizeof(buf), "  SP+%d: %04X", i * 2, static_cast<unsigned>(val));
      resp << buf;
      // Heuristic: check if instruction before val is a CALL or RST
      if (val >= 1) {
        // Check 3, 2, 1 bytes back for CALL/RST
        bool is_ret_addr = false;
        for (int back = 3; back >= 1; back--) {
          word check_addr = static_cast<word>(val - back);
          auto dline = disassemble_one(check_addr, dummy, dummy_eps);
          if (dline.Size() == back &&
              (dline.instruction_.rfind("call", 0) == 0 ||
               dline.instruction_.rfind("rst", 0) == 0)) {
            is_ret_addr = true;
            break;
          }
        }
        if (is_ret_addr) resp << " [call]";
      }
      // Include symbol name if known
      std::string sym = g_symfile.lookupAddr(val);
      if (!sym.empty()) resp << " " << sym;
      resp << "\n";
    }
    return resp.str();
  }


  // Auto-type command: queue text/key sequences for injection
  if (cmd == "autotype") {
    if (parts.size() >= 2 && parts[1] == "status") {
      if (g_autotype_queue.is_active()) {
        return "OK active: " + std::to_string(g_autotype_queue.remaining()) + " actions remaining\n";
      }
      return "OK idle\n";
    }
    if (parts.size() >= 2 && parts[1] == "clear") {
      g_autotype_queue.clear();
      return "OK\n";
    }
    // Everything after "autotype " is the text to type
    size_t pos = line.find(' ');
    if (pos == std::string::npos || pos + 1 >= line.size()) {
      return "ERR 400 bad-args (autotype TEXT|status|clear)\n";
    }
    std::string text = line.substr(pos + 1);
    auto err = g_autotype_queue.enqueue(text);
    if (!err.empty()) {
      return "ERR 400 " + err + "\n";
    }
    return "OK\n";
  }

  // --- Disk management commands ---
  if (cmd == "disk") {
    if (parts.size() < 2) return "ERR 400 missing subcommand (formats|format|new|ls|cat|get|put|rm|info|sector)\n";
    if (parts[1] == "formats") {
      auto names = disk_format_names();
      std::ostringstream resp;
      resp << "OK";
      for (const auto& n : names) resp << " " << n;
      resp << "\n";
      return resp.str();
    }
    if (parts[1] == "format") {
      if (parts.size() < 4)
        return "ERR 400 usage: disk format <A|B> <format_name>\n";
      char drive = parts[2][0];
      std::string err = disk_format_drive(drive, parts[3]);
      if (!err.empty()) return "ERR " + err + "\n";
      return "OK\n";
    }
    if (parts[1] == "new") {
      if (parts.size() < 3)
        return "ERR 400 usage: disk new <path> [format]\n";
      std::string path = parts[2];
      std::string fmt = (parts.size() >= 4) ? parts[3] : "data";
      std::string err = disk_create_new(path, fmt);
      if (!err.empty()) return "ERR " + err + "\n";
      return "OK\n";
    }
    // Helper lambda: resolve drive letter to t_drive*
    auto resolve_drive = [&](const std::string& letter) -> t_drive* {
      if (letter.empty()) return nullptr;
      char c = static_cast<char>(std::toupper(static_cast<unsigned char>(letter[0])));
      if (c == 'A') return &driveA;
      if (c == 'B') return &driveB;
      return nullptr;
    };

    if (parts[1] == "ls") {
      if (parts.size() < 3)
        return "ERR 400 usage: disk ls <A|B>\n";
      t_drive* drv = resolve_drive(parts[2]);
      if (!drv) return "ERR 400 invalid drive letter\n";
      std::string err;
      auto files = disk_list_files(drv, err);
      if (!err.empty()) return "ERR " + err + "\n";
      std::ostringstream resp;
      resp << "OK\n";
      for (const auto& f : files) {
        resp << f.display_name << " " << f.size_bytes;
        if (f.read_only) resp << " R/O";
        if (f.system) resp << " SYS";
        resp << "\n";
      }
      return resp.str();
    }
    if (parts[1] == "cat") {
      if (parts.size() < 4)
        return "ERR 400 usage: disk cat <A|B> <filename>\n";
      t_drive* drv = resolve_drive(parts[2]);
      if (!drv) return "ERR 400 invalid drive letter\n";
      std::string err;
      auto raw = disk_read_file(drv, parts[3], err);
      if (!err.empty()) return "ERR " + err + "\n";
      // Check for AMSDOS header -- if present, skip it and report actual length
      auto hdr_info = disk_parse_amsdos_header(raw);
      size_t offset = 0;
      size_t reported_size = raw.size();
      if (hdr_info.valid && raw.size() >= 128) {
        offset = 128;
        reported_size = hdr_info.file_length;
      }
      std::ostringstream resp;
      resp << "OK size=" << reported_size << "\n";
      resp << std::hex << std::uppercase << std::setfill('0');
      for (size_t i = offset; i < raw.size() && (i - offset) < reported_size; i++) {
        if (i > offset) resp << ' ';
        resp << std::setw(2) << static_cast<unsigned>(raw[i]);
      }
      resp << "\n";
      return resp.str();
    }
    if (parts[1] == "get") {
      if (parts.size() < 5)
        return "ERR 400 usage: disk get <A|B> <filename> <local_path>\n";
      t_drive* drv = resolve_drive(parts[2]);
      if (!drv) return "ERR 400 invalid drive letter\n";
      std::string err;
      auto raw = disk_read_file(drv, parts[3], err);
      if (!err.empty()) return "ERR " + err + "\n";
      // Strip AMSDOS header if present
      auto hdr_info = disk_parse_amsdos_header(raw);
      size_t offset = 0;
      size_t length = raw.size();
      if (hdr_info.valid && raw.size() >= 128) {
        offset = 128;
        length = hdr_info.file_length;
      }
      if (offset + length > raw.size()) length = raw.size() - offset;
      std::ofstream out(parts[4], std::ios::binary);
      if (!out) return "ERR failed to open " + parts[4] + "\n";
      out.write(reinterpret_cast<const char*>(raw.data() + offset),
                static_cast<std::streamsize>(length));
      out.close();
      return "OK bytes=" + std::to_string(length) + "\n";
    }
    if (parts[1] == "put") {
      if (parts.size() < 4)
        return "ERR 400 usage: disk put <A|B> <local_path> [cpc_filename]\n";
      t_drive* drv = resolve_drive(parts[2]);
      if (!drv) return "ERR 400 invalid drive letter\n";
      std::string local_path = parts[3];
      std::string cpc_name;
      if (parts.size() >= 5) {
        cpc_name = parts[4];
        // Uppercase it
        for (auto& c : cpc_name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      } else {
        cpc_name = disk_to_cpc_filename(local_path);
        if (cpc_name.empty()) return "ERR cannot derive CPC filename from path\n";
      }
      std::ifstream in(local_path, std::ios::binary);
      if (!in) return "ERR cannot open " + local_path + "\n";
      std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
      in.close();
      std::string err = disk_write_file(drv, cpc_name, data, true);
      if (!err.empty()) return "ERR " + err + "\n";
      return "OK\n";
    }
    if (parts[1] == "rm") {
      if (parts.size() < 4)
        return "ERR 400 usage: disk rm <A|B> <filename>\n";
      t_drive* drv = resolve_drive(parts[2]);
      if (!drv) return "ERR 400 invalid drive letter\n";
      std::string err = disk_delete_file(drv, parts[3]);
      if (!err.empty()) return "ERR " + err + "\n";
      return "OK\n";
    }
    if (parts[1] == "info") {
      if (parts.size() < 4)
        return "ERR 400 usage: disk info <A|B> <filename>\n";
      t_drive* drv = resolve_drive(parts[2]);
      if (!drv) return "ERR 400 invalid drive letter\n";
      std::string err;
      auto raw = disk_read_file(drv, parts[3], err);
      if (!err.empty()) return "ERR " + err + "\n";
      auto info = disk_parse_amsdos_header(raw);
      if (!info.valid) return "ERR no valid AMSDOS header\n";
      char buf[256];
      const char* type_str = "unknown";
      switch (info.type) {
        case AmsdosFileType::BASIC: type_str = "basic"; break;
        case AmsdosFileType::PROTECTED: type_str = "protected"; break;
        case AmsdosFileType::BINARY: type_str = "binary"; break;
        default: break;
      }
      std::snprintf(buf, sizeof(buf), "OK type=%s load=%04X exec=%04X size=%u\n",
                    type_str, info.load_addr, info.exec_addr, info.file_length);
      return std::string(buf);
    }
    if (parts[1] == "sector") {
      if (parts.size() < 3)
        return "ERR 400 usage: disk sector (read|write|info) ...\n";

      // Helper lambda: resolve drive letter to t_drive* (reuse from above scope)
      auto sec_resolve_drive = [&](const std::string& letter) -> t_drive* {
        if (letter.empty()) return nullptr;
        char c = static_cast<char>(std::toupper(static_cast<unsigned char>(letter[0])));
        if (c == 'A') return &driveA;
        if (c == 'B') return &driveB;
        return nullptr;
      };

      if (parts[2] == "read") {
        // disk sector read <drive> <track> <side> <sector_id>
        if (parts.size() < 7)
          return "ERR 400 usage: disk sector read <drive> <track> <side> <sector_id>\n";
        t_drive* drv = sec_resolve_drive(parts[3]);
        if (!drv) return "ERR 400 invalid drive letter\n";
        unsigned int trk = static_cast<unsigned int>(parse_number(parts[4]));
        unsigned int side = static_cast<unsigned int>(parse_number(parts[5]));
        uint8_t sector_id = static_cast<uint8_t>(std::stoul(parts[6], nullptr, 16));
        std::string err;
        auto data = disk_sector_read(drv, trk, side, sector_id, err);
        if (!err.empty()) return "ERR " + err + "\n";
        std::ostringstream resp;
        resp << "OK size=" << data.size() << "\n";
        resp << std::hex << std::uppercase << std::setfill('0');
        for (size_t i = 0; i < data.size(); i++) {
          if (i > 0) resp << ' ';
          resp << std::setw(2) << static_cast<unsigned>(data[i]);
        }
        resp << "\n";
        return resp.str();
      }
      if (parts[2] == "write") {
        // disk sector write <drive> <track> <side> <sector_id> <hex_data>
        if (parts.size() < 8)
          return "ERR 400 usage: disk sector write <drive> <track> <side> <sector_id> <hex_data>\n";
        t_drive* drv = sec_resolve_drive(parts[3]);
        if (!drv) return "ERR 400 invalid drive letter\n";
        unsigned int trk = static_cast<unsigned int>(parse_number(parts[4]));
        unsigned int side = static_cast<unsigned int>(parse_number(parts[5]));
        uint8_t sector_id = static_cast<uint8_t>(std::stoul(parts[6], nullptr, 16));
        // Parse hex data: remaining parts are space-separated hex bytes
        std::vector<uint8_t> data;
        for (size_t i = 7; i < parts.size(); i++) {
          // Each part may be a single hex byte like "FF" or multiple bytes
          // Handle both "FF" and "FFAA" formats
          const std::string& hex_str = parts[i];
          for (size_t j = 0; j + 1 < hex_str.size(); j += 2) {
            std::string byte_str = hex_str.substr(j, 2);
            data.push_back(static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16)));
          }
        }
        std::string err = disk_sector_write(drv, trk, side, sector_id, data);
        if (!err.empty()) return "ERR " + err + "\n";
        return "OK\n";
      }
      if (parts[2] == "info") {
        // disk sector info <drive> <track> <side>
        if (parts.size() < 6)
          return "ERR 400 usage: disk sector info <drive> <track> <side>\n";
        t_drive* drv = sec_resolve_drive(parts[3]);
        if (!drv) return "ERR 400 invalid drive letter\n";
        unsigned int trk = static_cast<unsigned int>(parse_number(parts[4]));
        unsigned int side = static_cast<unsigned int>(parse_number(parts[5]));
        std::string err;
        auto sectors = disk_sector_info(drv, trk, side, err);
        if (!err.empty()) return "ERR " + err + "\n";
        std::ostringstream resp;
        resp << "OK sectors=" << sectors.size() << "\n";
        resp << std::hex << std::uppercase << std::setfill('0');
        for (const auto& s : sectors) {
          resp << "C=" << std::setw(2) << static_cast<unsigned>(s.C)
               << " H=" << std::setw(2) << static_cast<unsigned>(s.H)
               << " R=" << std::setw(2) << static_cast<unsigned>(s.R)
               << " N=" << std::setw(2) << static_cast<unsigned>(s.N)
               << " size=" << std::dec << s.size << "\n";
          resp << std::hex; // reset for next iteration
        }
        return resp.str();
      }
      return "ERR 400 unknown sector subcommand (read|write|info)\n";
    }
    return "ERR 400 unknown disk subcommand\n";
  }

  // --- WAV audio recording ---
  if (cmd == "record" && parts.size() >= 2) {
    if (parts[1] == "wav") {
      if (parts.size() < 3) return "ERR 400 missing-action (start|stop|status)\n";
      if (parts[2] == "start") {
        if (parts.size() < 4) return "ERR 400 missing-path\n";
        uint32_t rate = SAMPLE_RATES[CPC.snd_playback_rate];
        uint16_t bits = CPC.snd_bits ? 16 : 8;
        uint16_t channels = CPC.snd_stereo ? 2 : 1;
        auto err = g_wav_recorder.start(parts[3], rate, bits, channels);
        if (err.empty()) return "OK\n";
        return "ERR " + err + "\n";
      }
      if (parts[2] == "stop") {
        if (!g_wav_recorder.is_recording()) return "ERR not-recording\n";
        std::string path = g_wav_recorder.current_path();
        uint32_t bytes = g_wav_recorder.stop();
        return "OK " + path + " " + std::to_string(bytes) + "\n";
      }
      if (parts[2] == "status") {
        if (g_wav_recorder.is_recording()) {
          return "OK recording " + g_wav_recorder.current_path() + " " +
                 std::to_string(g_wav_recorder.bytes_written()) + "\n";
        }
        return "OK idle\n";
      }
      return "ERR 400 bad-wav-cmd (start|stop|status)\n";
    }
    if (parts[1] == "ym") {
      if (parts.size() < 3) return "ERR 400 missing-action (start|stop|status)\n";
      if (parts[2] == "start") {
        if (parts.size() < 4) return "ERR 400 missing-path\n";
        auto err = g_ym_recorder.start(parts[3]);
        if (err.empty()) return "OK\n";
        return "ERR " + err + "\n";
      }
      if (parts[2] == "stop") {
        if (!g_ym_recorder.is_recording()) return "ERR not-recording\n";
        std::string path = g_ym_recorder.current_path();
        uint32_t frames = g_ym_recorder.stop();
        return "OK " + path + " " + std::to_string(frames) + "\n";
      }
      if (parts[2] == "status") {
        if (g_ym_recorder.is_recording()) {
          return "OK recording " + g_ym_recorder.current_path() + " " +
                 std::to_string(g_ym_recorder.frame_count()) + "\n";
        }
        return "OK idle\n";
      }
      return "ERR 400 bad-ym-cmd (start|stop|status)\n";
    }
    if (parts[1] == "avi") {
      if (parts.size() < 3) return "ERR 400 missing-action (start|stop|status)\n";
      if (parts[2] == "start") {
        if (parts.size() < 4) return "ERR 400 missing-path\n";
        int quality = 85;
        if (parts.size() >= 5) {
          try { quality = parse_int(parts[4]); } catch (const std::exception&) {}
        }
        uint32_t rate = SAMPLE_RATES[CPC.snd_playback_rate];
        uint16_t bits = CPC.snd_bits ? 16 : 8;
        uint16_t channels = CPC.snd_stereo ? 2 : 1;
        auto err = g_avi_recorder.start(parts[3], quality, rate, channels, bits);
        if (err.empty()) return "OK\n";
        return "ERR " + err + "\n";
      }
      if (parts[2] == "stop") {
        if (!g_avi_recorder.is_recording()) return "ERR not-recording\n";
        std::string path = g_avi_recorder.current_path();
        uint32_t frames = g_avi_recorder.stop();
        return "OK " + path + " " + std::to_string(frames) + "\n";
      }
      if (parts[2] == "status") {
        if (g_avi_recorder.is_recording()) {
          return "OK recording " + g_avi_recorder.current_path() + " frames=" +
                 std::to_string(g_avi_recorder.frame_count()) + " bytes=" +
                 std::to_string(g_avi_recorder.bytes_written()) + "\n";
        }
        return "OK idle\n";
      }
      return "ERR 400 bad-avi-cmd (start|stop|status)\n";
    }
    return "ERR 400 bad-record-cmd (wav|ym|avi)\n";
  }
  if (cmd == "record") return "ERR 400 usage: record (wav|ym|avi)\n";

  // --- Poke commands ---
  if (cmd == "poke" && parts.size() >= 2) {
    if (parts[1] == "load" && parts.size() >= 3) {
      // Reconstruct path (may contain spaces if quoted, but split_ws breaks on space)
      // For simplicity, take everything after "poke load "
      size_t pos = line.find("load ");
      if (pos == std::string::npos) return "ERR 400 bad-args\n";
      std::string path = line.substr(pos + 5);
      // Strip surrounding quotes if present
      if (path.size() >= 2 && path.front() == '"' && path.back() == '"') {
        path = path.substr(1, path.size() - 2);
      }
      auto err = g_poke_manager.load(path);
      if (err.empty()) {
        return "OK loaded " + std::to_string(g_poke_manager.games().size()) + " games\n";
      }
      return "ERR " + err + "\n";
    }
    if (parts[1] == "list") {
      const auto& games = g_poke_manager.games();
      if (games.empty()) return "OK (no games loaded)\n";
      std::ostringstream resp;
      resp << "OK\n";
      for (size_t gi = 0; gi < games.size(); gi++) {
        resp << games[gi].title << "\n";
        for (size_t pi = 0; pi < games[gi].pokes.size(); pi++) {
          resp << "  Poke: " << games[gi].pokes[pi].description;
          resp << " [" << games[gi].pokes[pi].values.size() << " value";
          if (games[gi].pokes[pi].values.size() != 1) resp << "s";
          resp << "]";
          if (games[gi].pokes[pi].applied) resp << " [applied]";
          resp << "\n";
        }
      }
      return resp.str();
    }
    if (parts[1] == "apply" && parts.size() >= 3) {
      size_t game_idx;
      try {
        game_idx = static_cast<size_t>(parse_number(parts[2]));
      } catch (const std::invalid_argument&) {
        return "ERR 400 invalid game index\n";
      } catch (const std::out_of_range&) {
        return "ERR 400 game index out of range\n";
      }
      if (parts.size() >= 4 && parts[3] == "all") {
        int total_vals = 0;
        int n = g_poke_manager.apply_all(game_idx,
          [](uint16_t a, uint8_t v){ z80_write_mem(static_cast<word>(a), v); },
          [](uint16_t a) -> uint8_t { return z80_read_mem(static_cast<word>(a)); },
          &total_vals);
        if (n < 0) return "ERR invalid game index\n";
        return "OK applied " + std::to_string(n) + " pokes (" + std::to_string(total_vals) + " values total)\n";
      }
      if (parts.size() >= 4) {
        size_t poke_idx;
        try {
          poke_idx = static_cast<size_t>(parse_number(parts[3]));
        } catch (const std::invalid_argument&) {
          return "ERR 400 invalid poke index\n";
        } catch (const std::out_of_range&) {
          return "ERR 400 poke index out of range\n";
        }
        int n = g_poke_manager.apply(game_idx, poke_idx,
          [](uint16_t a, uint8_t v){ z80_write_mem(static_cast<word>(a), v); },
          [](uint16_t a) -> uint8_t { return z80_read_mem(static_cast<word>(a)); });
        if (n < 0) return "ERR invalid index\n";
        return "OK applied " + std::to_string(n) + " values\n";
      }
      return "ERR 400 bad-args (poke apply <game> <poke|all>)\n";
    }
    if (parts[1] == "unapply" && parts.size() >= 4) {
      size_t game_idx, poke_idx;
      try {
        game_idx = static_cast<size_t>(parse_number(parts[2]));
        poke_idx = static_cast<size_t>(parse_number(parts[3]));
      } catch (const std::invalid_argument&) {
        return "ERR 400 invalid index\n";
      } catch (const std::out_of_range&) {
        return "ERR 400 index out of range\n";
      }
      int n = g_poke_manager.unapply(game_idx, poke_idx,
        [](uint16_t a, uint8_t v){ z80_write_mem(static_cast<word>(a), v); });
      if (n < 0) return "ERR unapply failed (not applied or invalid index)\n";
      return "OK restored " + std::to_string(n) + " values\n";
    }
    if (parts[1] == "write" && parts.size() >= 4) {
      unsigned int addr;
      unsigned int val;
      try {
        addr = parse_number(parts[2]);
        val = parse_number(parts[3]);
      } catch (const std::invalid_argument&) {
        return "ERR 400 bad-args (poke write <addr> <value>)\n";
      } catch (const std::out_of_range&) {
        return "ERR 400 bad-args (poke write <addr> <value>)\n";
      }
      if (val > 255) return "ERR 400 value must be 0-255\n";
      z80_write_mem(static_cast<word>(addr), static_cast<byte>(val));
      return "OK\n";
    }
    return "ERR 400 bad-poke-cmd (load|list|apply|unapply|write)\n";
  }

  // --- Profile commands ---
  if (cmd == "profile") {
    if (parts.size() < 2) return "ERR 400 missing subcommand (list|current|load|save|delete)\n";
    if (parts[1] == "list") {
      auto names = g_profile_manager.list();
      std::string cur = g_profile_manager.current();
      std::ostringstream resp;
      resp << "OK\n";
      for (const auto& n : names) {
        if (n == cur) resp << "* ";
        else resp << "  ";
        resp << n << "\n";
      }
      return resp.str();
    }
    if (parts[1] == "current") {
      std::string cur = g_profile_manager.current();
      if (cur.empty()) return "OK (default)\n";
      return "OK " + cur + "\n";
    }
    if (parts[1] == "load") {
      if (parts.size() < 3) return "ERR 400 missing profile name\n";
      auto err = g_profile_manager.load(parts[2]);
      if (!err.empty()) return "ERR " + err + "\n";
      return "OK\n";
    }
    if (parts[1] == "save") {
      if (parts.size() < 3) return "ERR 400 missing profile name\n";
      auto err = g_profile_manager.save(parts[2]);
      if (!err.empty()) return "ERR " + err + "\n";
      return "OK\n";
    }
    if (parts[1] == "delete") {
      if (parts.size() < 3) return "ERR 400 missing profile name\n";
      auto err = g_profile_manager.remove(parts[2]);
      if (!err.empty()) return "ERR " + err + "\n";
      return "OK\n";
    }
    return "ERR 400 unknown profile subcommand (list|current|load|save|delete)\n";
  }

  // --- Status command ---
  if (cmd == "status") {
    if (parts.size() >= 2 && parts[1] == "drives") {
      return "OK " + drive_status_detailed() + "\n";
    }
    return "OK " + emulator_status_summary() + "\n" + drive_status_summary() + "\n";
  }

  // --- Config commands ---
  if (cmd == "config" && parts.size() >= 2) {
    if (parts[1] == "get" && parts.size() >= 3) {
      if (parts[2] == "crtc_type") {
        return "OK " + std::to_string(CRTC.crtc_type) + "\n";
      }
      if (parts[2] == "crtc_info") {
        char buf[128];
        snprintf(buf, sizeof(buf), "OK type=%d chip=%s manufacturer=%s",
                 CRTC.crtc_type,
                 crtc_type_chip_name(CRTC.crtc_type),
                 crtc_type_manufacturer(CRTC.crtc_type));
        return std::string(buf) + "\n";
      }
      if (parts[2] == "ram_size") {
        return "OK " + std::to_string(CPC.ram_size) + "\n";
      }
      if (parts[2] == "silicon_disc") {
        return std::string("OK ") + (g_silicon_disc.enabled ? "1" : "0") + "\n";
      }
      if (parts[2] == "m4board") {
        return std::string("OK ") + (g_m4board.enabled ? "1" : "0") + "\n";
      }
      if (parts[2] == "m4_sd_path") {
        return "OK " + g_m4board.sd_root_path + "\n";
      }
      if (parts[2] == "m4_rom_slot") {
        return "OK " + std::to_string(g_m4board.rom_slot) + "\n";
      }
      return "ERR 400 unknown-config-key\n";
    }
    if (parts[1] == "set" && parts.size() >= 4) {
      if (parts[2] == "crtc_type") {
        int t = parse_int(parts[3]);
        if (t < 0 || t > 3) return "ERR 400 crtc_type must be 0-3\n";
        CRTC.crtc_type = static_cast<unsigned char>(t);
        return "OK\n";
      }
      if (parts[2] == "ram_size") {
        int sz = parse_int(parts[3]);
        if (!is_valid_ram_size(static_cast<unsigned int>(sz))) {
          return "ERR 400 invalid ram_size\n";
        }
        CPC.ram_size = static_cast<unsigned int>(sz);
        return "OK (reset required)\n";
      }
      if (parts[2] == "silicon_disc") {
        int v;
        try { v = parse_int(parts[3]); } catch (const std::exception&) {
          return "ERR 400 bad-value\n";
        }
        g_silicon_disc.enabled = (v != 0);
        if (g_silicon_disc.enabled && !g_silicon_disc.data) {
          silicon_disc_init(g_silicon_disc);
        }
        ga_memory_manager();
        return "OK\n";
      }
      if (parts[2] == "m4board") {
        int v;
        try { v = parse_int(parts[3]); } catch (const std::exception&) {
          return "ERR 400 bad-value\n";
        }
        g_m4board.enabled = (v != 0);
        return "OK (reset required)\n";
      }
      if (parts[2] == "m4_sd_path") {
        // Collect rest of line after "config set m4_sd_path "
        size_t pos = line.find("m4_sd_path ");
        if (pos == std::string::npos) return "ERR 400 bad-args\n";
        g_m4board.sd_root_path = line.substr(pos + 11);
        return "OK\n";
      }
      return "ERR 400 unknown-config-key\n";
    }
    return "ERR 400 bad-config-cmd (get|set)\n";
  }

  // --- Silicon Disc commands ---
  if (cmd == "sdisc" && parts.size() >= 2) {
    if (parts[1] == "status") {
      std::string status = g_silicon_disc.enabled ? "enabled" : "disabled";
      std::string allocated = g_silicon_disc.data ? "allocated" : "not-allocated";
      return "OK " + status + " " + allocated + " size=256K banks=4-7\n";
    }
    if (parts[1] == "clear") {
      if (!g_silicon_disc.enabled) return "ERR 400 silicon-disc-not-enabled\n";
      silicon_disc_clear(g_silicon_disc);
      return "OK cleared 256K\n";
    }
    if (parts[1] == "save" && parts.size() >= 3) {
      if (!g_silicon_disc.enabled || !g_silicon_disc.data)
        return "ERR 400 silicon-disc-not-enabled\n";
      // Reject path traversal
      for (const auto& comp : std::filesystem::path(parts[2])) {
        if (comp == "..") return "ERR 403 path-traversal\n";
      }
      if (silicon_disc_save(g_silicon_disc, parts[2]))
        return "OK saved to " + parts[2] + "\n";
      return "ERR 500 save-failed\n";
    }
    if (parts[1] == "load" && parts.size() >= 3) {
      // Reject path traversal
      for (const auto& comp : std::filesystem::path(parts[2])) {
        if (comp == "..") return "ERR 403 path-traversal\n";
      }
      if (!g_silicon_disc.enabled) {
        g_silicon_disc.enabled = true;
        silicon_disc_init(g_silicon_disc);
      }
      if (silicon_disc_load(g_silicon_disc, parts[2])) {
        ga_memory_manager();
        return "OK loaded from " + parts[2] + "\n";
      }
      return "ERR 500 load-failed\n";
    }
    return "ERR 400 usage: sdisc (status|clear|save|load) [path]\n";
  }

  // --- Serial Interface commands ---
  if (cmd == "serial") {
    if (parts.size() < 2) {
      return "ERR 400 usage: serial (status|send|send_string|config)\n";
    }
    if (parts[1] == "status") {
      auto cfg = g_serial_interface.get_config();
      std::stringstream ss;
      ss << "OK enabled=" << (cfg.enabled ? 1 : 0)
         << " backend=" << static_cast<int>(cfg.backend_type)
         << " tx_empty=" << (g_serial_interface.dart.tx_empty() ? 1 : 0)
         << " rx_available=" << (g_serial_interface.dart.rx_available() ? 1 : 0)
         << " baud=" << cfg.baud_rate;
      if (g_serial_interface.backend) {
        ss << " backend_name=" << g_serial_interface.backend->name();
        ss << " backend_status=" << g_serial_interface.backend->status();
      }
      ss << "\n";
      return ss.str();
    }
    if (parts[1] == "send" && parts.size() >= 3) {
      try {
        int byte = parse_int(parts[2]);
        if (byte < 0 || byte > 255) {
          return "ERR 400 byte must be 0-255\n";
        }
        g_serial_interface.dart.enqueue_rx(static_cast<uint8_t>(byte));
        return "OK sent byte " + parts[2] + "\n";
      } catch (const std::exception&) {
        return "ERR 400 bad-byte\n";
      }
    }
    if (parts[1] == "send_string" && parts.size() >= 3) {
      size_t pos = line.find("send_string ");
      if (pos == std::string::npos) return "ERR 400 bad-args\n";
      std::string str = line.substr(pos + 12);
      for (char c : str) {
        g_serial_interface.dart.enqueue_rx(static_cast<uint8_t>(c));
      }
      return "OK sent " + std::to_string(str.size()) + " bytes\n";
    }
    if (parts[1] == "config") {
      if (parts.size() == 3 && parts[2] == "get") {
        auto cfg = g_serial_interface.get_config();
        std::stringstream ss;
        ss << "OK enabled=" << (cfg.enabled ? 1 : 0)
           << " backend=" << static_cast<int>(cfg.backend_type)
           << " device=" << cfg.device_path
           << " tcp_host=" << cfg.tcp_host
           << " tcp_port=" << cfg.tcp_port
           << " baud=" << cfg.baud_rate << "\n";
        return ss.str();
      }
      if (parts.size() >= 5 && parts[2] == "set") {
        if (parts[3] == "enabled") {
          auto cfg = g_serial_interface.get_config();
          cfg.enabled = (parse_int(parts[4]) != 0);
          g_serial_interface.set_config(cfg);
          g_serial_interface.apply_config();
          return "OK\n";
        }
        if (parts[3] == "backend") {
          auto cfg = g_serial_interface.get_config();
          cfg.backend_type = static_cast<SerialBackendType>(parse_int(parts[4]));
          g_serial_interface.set_config(cfg);
          g_serial_interface.apply_config();
          return "OK (reset to apply backend change)\n";
        }
        if (parts[3] == "baud") {
          auto cfg = g_serial_interface.get_config();
          cfg.baud_rate = parse_int(parts[4]);
          g_serial_interface.set_config(cfg);
          g_serial_interface.apply_config();
          return "OK\n";
        }
        return "ERR 400 unknown serial config key\n";
      }
      return "ERR 400 usage: serial config (get|set key value)\n";
    }
    return "ERR 400 usage: serial (status|send|send_string|config)\n";
  }

  // --- Plotter commands ---
  if (cmd == "plotter") {
    if (parts.size() < 2) {
      return "ERR 400 usage: plotter (status|export [path]|clear)\n";
    }
    if (parts[1] == "status") {
      std::stringstream ss;
      ss << "OK pen=" << g_plotter.selected_pen()
         << " pen_down=" << (g_plotter.pen_down() ? 1 : 0)
         << " x=" << g_plotter.pen_x()
         << " y=" << g_plotter.pen_y()
         << " segments=" << g_plotter.segments().size()
         << "\n";
      return ss.str();
    }
    if (parts[1] == "export") {
      std::string path = (parts.size() >= 3) ? parts[2] : "plotter_output.svg";
      if (g_plotter.export_svg(path)) {
        return "OK exported to " + path + "\n";
      }
      return "ERR 500 export-failed\n";
    }
    if (parts[1] == "clear") {
      g_plotter.clear();
      return "OK\n";
    }
    return "ERR 400 usage: plotter (status|export [path]|clear)\n";
  }

  // --- Enhanced search command (with wildcard support) ---
  if (cmd == "search" && parts.size() >= 3) {
    std::string submode = parts[1];
    // Collect pattern from remaining parts
    std::string pattern;
    for (size_t pi = 2; pi < parts.size(); pi++) {
      if (!pattern.empty()) pattern += " ";
      pattern += parts[pi];
    }
    // Strip surrounding quotes
    if (pattern.size() >= 2 && pattern.front() == '"' && pattern.back() == '"') {
      pattern = pattern.substr(1, pattern.size() - 2);
    }
    if (pattern.empty()) return "ERR 400 empty-pattern\n";

    SearchMode mode;
    if (submode == "hex") mode = SearchMode::HEX;
    else if (submode == "text") mode = SearchMode::TEXT;
    else if (submode == "asm") mode = SearchMode::ASM;
    else return "ERR 400 bad-search-mode (hex|text|asm)\n";

    if (mode == SearchMode::ASM) {
      // ASM mode uses disassembly infrastructure directly
      std::string lower_pattern = pattern;
      for (auto& c : lower_pattern) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      std::ostringstream resp;
      resp << "OK";
      int found = 0;
      DisassembledCode dummy;
      std::vector<dword> dummy_eps;
      for (unsigned int addr = 0; addr <= 0xFFFF && found < 256; ) {
        auto line = disassemble_one(addr, dummy, dummy_eps);
        std::string lower_instr = line.instruction_;
        for (auto& c : lower_instr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        bool match = false;
        // Glob match with ? for single char and * for any sequence
        size_t gpi = 0, gti = 0;
        size_t plen = lower_pattern.size(), tlen = lower_instr.size();
        size_t star_p = std::string::npos, star_t = 0;
        while (gti < tlen) {
          if (gpi < plen && (lower_pattern[gpi] == lower_instr[gti] || lower_pattern[gpi] == '?')) {
            gpi++; gti++;
          } else if (gpi < plen && lower_pattern[gpi] == '*') {
            star_p = gpi++;
            star_t = gti;
          } else if (star_p != std::string::npos) {
            gpi = star_p + 1;
            gti = ++star_t;
          } else {
            break;
          }
        }
        while (gpi < plen && lower_pattern[gpi] == '*') gpi++;
        match = (gpi == plen && gti == tlen);
        if (match) {
          resp << " " << std::hex << std::uppercase << std::setfill('0')
               << std::setw(4) << addr << " " << line.instruction_;
          found++;
        }
        int sz = line.Size();
        if (sz < 1) sz = 1;
        addr += static_cast<unsigned int>(sz);
      }
      resp << "\n";
      return resp.str();
    }

    // HEX / TEXT mode: read full 64K into buffer
    std::vector<uint8_t> membuf(65536);
    for (size_t i = 0; i < 65536; i++) {
      membuf[i] = z80_read_mem(static_cast<word>(i));
    }
    auto results = search_memory(membuf.data(), membuf.size(), pattern, mode, 256);
    std::ostringstream resp;
    resp << "OK";
    for (const auto& r : results) {
      resp << " " << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << r.address;
    }
    resp << "\n";
    return resp.str();
  }


  // --- ROM slot management ---
  if (cmd == "rom" && parts.size() >= 2) {
    if (parts[1] == "list") {
      std::ostringstream oss;
      oss << "OK";
      for (int i = 0; i < MAX_ROM_SLOTS; i++) {
        oss << " " << i << "=";
        if (CPC.rom_file[i].empty()) {
          oss << "(empty)";
        } else {
          oss << CPC.rom_file[i];
        }
      }
      oss << "\n";
      return oss.str();
    }
    if (parts[1] == "load" && parts.size() >= 4) {
      int slot = -1;
      try { slot = parse_int(parts[2]); } catch (const std::exception&) {}
      if (slot < 0 || slot >= MAX_ROM_SLOTS) return "ERR 400 slot must be 0-31\n";
      std::string path;
      for (size_t pi = 3; pi < parts.size(); pi++) {
        if (!path.empty()) path += " ";
        path += parts[pi];
      }
      // Strip surrounding quotes
      if (path.size() >= 2 && path.front() == '"' && path.back() == '"') {
        path = path.substr(1, path.size() - 2);
      }
      if (path.empty()) return "ERR 400 missing-path\n";
      if (!is_safe_path(path)) return "ERR 403 path-traversal-blocked\n";
      // Resolve relative paths against rom_path
      if (path[0] != '/' && path[0] != '\\' && !(path.size() >= 2 && path[1] == ':')) {
        path = CPC.rom_path + "/" + path;
      }
      // Validate file exists and is a valid ROM
      FILE *f = fopen(path.c_str(), "rb");
      if (!f) return "ERR 404 file-not-found\n";
      byte header[128];
      if (fread(header, 128, 1, f) != 1) {
        fclose(f);
        return "ERR 400 file-too-small\n";
      }
      // Check for AMSDOS header and skip if present
      word checksum = 0;
      for (int n = 0; n < 0x43; n++) checksum += header[n];
      bool has_amsdos = (checksum == ((header[0x43] << 8) + header[0x44]));
      if (has_amsdos) {
        if (fread(header, 128, 1, f) != 1) {
          fclose(f);
          return "ERR 400 file-too-small-after-header\n";
        }
      }
      // Validate ROM type byte (0=foreground, 1=background, 2=extension) or Graduate 'G'
      bool valid_rom = !(header[0] & 0xfc);
      bool graduate_rom = (header[0] == 0x47);
      if (!valid_rom && !graduate_rom) {
        fclose(f);
        return "ERR 400 not-a-valid-rom\n";
      }
      // Allocate and load
      byte *rom_data = new byte[16384];
      memcpy(rom_data, header, 128);
      size_t remaining = 16384 - 128;
      if (fread(rom_data + 128, remaining, 1, f) != 1) {
        fclose(f);
        delete[] rom_data;
        return "ERR 400 rom-read-error\n";
      }
      fclose(f);
      // Free old ROM data if present (but not for slots 0/1 which are system ROMs)
      if (slot >= 2 && memmap_ROM[slot] != nullptr) {
        delete[] memmap_ROM[slot];
      }
      memmap_ROM[slot] = rom_data;
      CPC.rom_file[slot] = path;
      // If the currently selected upper ROM is this slot, update pointer
      if (GateArray.upper_ROM == static_cast<unsigned char>(slot)) {
        pbExpansionROM = memmap_ROM[slot];
        if (!(GateArray.ROM_config & 0x08)) {
          memory_set_read_bank(3, pbExpansionROM);
        }
      }
      return "OK\n";
    }
    if (parts[1] == "unload" && parts.size() >= 3) {
      int slot = -1;
      try { slot = parse_int(parts[2]); } catch (const std::exception&) {}
      if (slot < 0 || slot >= MAX_ROM_SLOTS) return "ERR 400 slot must be 0-31\n";
      if (slot < 2) return "ERR 400 cannot-unload-system-rom\n";
      if (memmap_ROM[slot] != nullptr) {
        delete[] memmap_ROM[slot];
        memmap_ROM[slot] = nullptr;
      }
      CPC.rom_file[slot] = "";
      // If this was the active upper ROM, revert to BASIC
      if (GateArray.upper_ROM == static_cast<unsigned char>(slot)) {
        pbExpansionROM = pbROMhi;
        if (!(GateArray.ROM_config & 0x08)) {
          memory_set_read_bank(3, pbExpansionROM);
        }
      }
      return "OK\n";
    }
    if (parts[1] == "info" && parts.size() >= 3) {
      int slot = -1;
      try { slot = parse_int(parts[2]); } catch (const std::exception&) {}
      if (slot < 0 || slot >= MAX_ROM_SLOTS) return "ERR 400 slot must be 0-31\n";
      if (memmap_ROM[slot] == nullptr) {
        return "OK slot=" + std::to_string(slot) + " loaded=false\n";
      }
      // Compute CRC32 of ROM data
      uLong crc = crc32(0L, nullptr, 0);
      crc = crc32(crc, memmap_ROM[slot], 16384);
      char buf[256];
      snprintf(buf, sizeof(buf), "OK slot=%d loaded=true size=16384 crc=%08lX path=%s\n",
               slot, static_cast<unsigned long>(crc), CPC.rom_file[slot].c_str());
      return std::string(buf);
    }
    return "ERR 400 bad-rom-cmd (list|load|unload|info)\n";
  }

  if (cmd == "data" && parts.size() >= 2) {
    if (parts[1] == "mark" && parts.size() >= 5) {
      unsigned int start = parse_number(parts[2]);
      unsigned int end = parse_number(parts[3]);
      if (start > 0xFFFF || end > 0xFFFF || start > end)
        return "ERR 400 bad-range\n";
      DataType dtype;
      if (parts[4] == "bytes") dtype = DataType::BYTES;
      else if (parts[4] == "words") dtype = DataType::WORDS;
      else if (parts[4] == "text") dtype = DataType::TEXT;
      else return "ERR 400 bad-type (bytes|words|text)\n";
      std::string label;
      if (parts.size() >= 6) {
        for (size_t i = 5; i < parts.size(); i++) {
          if (!label.empty()) label += " ";
          label += parts[i];
        }
      }
      g_data_areas.mark(static_cast<uint16_t>(start), static_cast<uint16_t>(end), dtype, label);
      return "OK\n";
    }
    if (parts[1] == "clear") {
      if (parts.size() < 3) return "ERR 400 usage: data clear <addr|all>\n";
      if (parts[2] == "all") {
        g_data_areas.clear_all();
      } else {
        unsigned int start = parse_number(parts[2]);
        g_data_areas.clear(static_cast<uint16_t>(start));
      }
      return "OK\n";
    }
    if (parts[1] == "list") {
      auto areas = g_data_areas.list();
      std::ostringstream resp;
      resp << "OK count=" << areas.size() << "\n";
      for (const auto& a : areas) {
        char buf[64];
        const char* type_str = "bytes";
        if (a.type == DataType::WORDS) type_str = "words";
        else if (a.type == DataType::TEXT) type_str = "text";
        snprintf(buf, sizeof(buf), "%04X %04X %s", a.start, a.end, type_str);
        resp << buf;
        if (!a.label.empty()) resp << " " << a.label;
        resp << "\n";
      }
      return resp.str();
    }
    return "ERR 400 bad-data-cmd (mark|clear|list)\n";
  }


  // --- Graphics Finder commands ---
  if (cmd == "gfx" && parts.size() >= 2) {
    if (parts[1] == "view" && parts.size() >= 6) {
      // gfx view <addr> <width_bytes> <height> <mode> [path]
      try {
        unsigned int addr = parse_number(parts[2]);
        int w = parse_int(parts[3]);
        int h = parse_int(parts[4]);
        int mode = parse_int(parts[5]);
        if (mode < 0 || mode > 2) return "ERR 400 mode must be 0-2\n";
        if (w <= 0 || w > 256) return "ERR 400 width must be 1-256 bytes\n";
        if (h <= 0 || h > 256) return "ERR 400 height must be 1-256 rows\n";

        GfxViewParams params;
        params.address = static_cast<uint16_t>(addr & 0xFFFF);
        params.width = w;
        params.height = h;
        params.mode = mode;

        uint32_t palette[16];
        gfx_get_palette_rgba(palette, 16);

        std::vector<uint32_t> pixels;
        int pixel_width = gfx_decode(pbRAM, CPC.ram_size * 1024, params, palette, pixels);
        if (pixel_width == 0) return "ERR 500 decode failed\n";

        if (parts.size() >= 7) {
          // Export to BMP
          if (!gfx_export_bmp(parts[6], pixels.data(), pixel_width, h)) {
            return "ERR 500 export failed\n";
          }
          char buf[128];
          snprintf(buf, sizeof(buf), "OK exported %dx%d to %s\n",
                   pixel_width, h, parts[6].c_str());
          return std::string(buf);
        }

        // Return hex dump of first row as preview
        std::ostringstream resp;
        resp << "OK " << pixel_width << "x" << h << " mode=" << mode << "\n";
        return resp.str();
      } catch (const std::exception& e) {
        return std::string("ERR 400 ") + e.what() + "\n";
      }
    }
    if (parts[1] == "decode" && parts.size() >= 4) {
      // gfx decode <byte_hex> <mode> — decode a single byte
      try {
        unsigned int byte_val = std::stoul(parts[2], nullptr, 16);
        int mode = parse_int(parts[3]);
        uint8_t indices[8];
        int count = gfx_decode_byte(static_cast<uint8_t>(byte_val), mode, indices);
        if (count == 0) return "ERR 400 invalid mode\n";
        std::ostringstream resp;
        resp << "OK";
        for (int i = 0; i < count; i++) {
          resp << " " << static_cast<int>(indices[i]);
        }
        resp << "\n";
        return resp.str();
      } catch (const std::exception& e) {
        return std::string("ERR 400 ") + e.what() + "\n";
      }
    }
    if (parts[1] == "paint" && parts.size() >= 8) {
      // gfx paint <addr> <width_bytes> <height> <mode> <x> <y> <color>
      // Note: parts indices are paint=1, addr=2, w=3, h=4, mode=5, x=6, y=7, color=8
      // We need 9 parts total for the paint command
      if (parts.size() < 9) return "ERR 400 usage: gfx paint <addr> <w> <h> <mode> <x> <y> <color>\n";
      try {
        unsigned int addr = parse_number(parts[2]);
        int w = parse_int(parts[3]);
        int h = parse_int(parts[4]);
        int mode = parse_int(parts[5]);
        int x = parse_int(parts[6]);
        int y = parse_int(parts[7]);
        int color = parse_int(parts[8]);

        GfxViewParams params;
        params.address = static_cast<uint16_t>(addr & 0xFFFF);
        params.width = w;
        params.height = h;
        params.mode = mode;

        if (!gfx_paint(pbRAM, CPC.ram_size * 1024, params, x, y,
                       static_cast<uint8_t>(color))) {
          return "ERR 400 paint failed (out of bounds?)\n";
        }
        return "OK\n";
      } catch (const std::exception& e) {
        return std::string("ERR 400 ") + e.what() + "\n";
      }
    }
    if (parts[1] == "palette") {
      // gfx palette — show current palette as RGBA hex
      uint32_t palette[16];
      gfx_get_palette_rgba(palette, 16);
      std::ostringstream resp;
      resp << "OK\n";
      for (int i = 0; i < 16; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%2d: #%08X\n", i, palette[i]);
        resp << buf;
      }
      return resp.str();
    }
    return "ERR 400 bad-gfx-cmd (view|decode|paint|palette)\n";
  }

  // --- Session recording/playback ---
  if (cmd == "session" && parts.size() >= 2) {
    if (parts[1] == "record" && parts.size() >= 3) {
      if (g_session.state() != SessionState::IDLE)
        return "ERR 400 session-already-active\n";
      // Save a snapshot first, then start recording
      std::string snap_path = parts[2] + ".sna";
      if (snapshot_save(snap_path) != 0)
        return "ERR 500 snapshot-save-failed\n";
      if (!g_session.start_recording(parts[2], snap_path))
        return "ERR 500 record-start-failed\n";
      return "OK recording to " + parts[2] + "\n";
    }
    if (parts[1] == "play" && parts.size() >= 3) {
      if (g_session.state() != SessionState::IDLE)
        return "ERR 400 session-already-active\n";
      std::string snap_path;
      if (!g_session.start_playback(parts[2], snap_path))
        return "ERR 500 playback-start-failed\n";
      // Load the embedded snapshot to restore state
      {
        bool was_paused = CPC.paused;
        if (!was_paused) cpc_pause();
        int rc = snapshot_load(snap_path);
        if (!was_paused) cpc_resume();
        if (rc != 0) {
          g_session.stop_playback();
          return "ERR 500 snapshot-load-failed\n";
        }
      }
      return "OK playing from " + parts[2] +
             " frames=" + std::to_string(g_session.total_frames()) + "\n";
    }
    if (parts[1] == "stop") {
      if (g_session.state() == SessionState::RECORDING) {
        g_session.stop_recording();
        return "OK stopped recording (frames=" +
               std::to_string(g_session.frame_count()) +
               " events=" + std::to_string(g_session.event_count()) + ")\n";
      }
      if (g_session.state() == SessionState::PLAYING) {
        g_session.stop_playback();
        return "OK stopped playback\n";
      }
      return "ERR 400 no-active-session\n";
    }
    if (parts[1] == "status") {
      const char* state_str = "idle";
      if (g_session.state() == SessionState::RECORDING) state_str = "recording";
      else if (g_session.state() == SessionState::PLAYING) state_str = "playing";
      char buf[256];
      snprintf(buf, sizeof(buf), "OK state=%s frames=%u events=%u path=%s",
               state_str, g_session.frame_count(), g_session.event_count(),
               g_session.path().empty() ? "(none)" : g_session.path().c_str());
      return std::string(buf) + "\n";
    }
    return "ERR 400 usage: session (record|play|stop|status) [path]\n";
  }

  // ── asm ──
  if (cmd == "asm" && parts.size() >= 2) {
    if (parts[1] == "text" && parts.size() >= 3) {
      // "asm text <source>" — set assembler source (rest of line after "asm text ")
      size_t offset = line.find("text ");
      if (offset == std::string::npos) return "ERR 400 bad-args\n";
      std::string source = line.substr(offset + 5);
      size_t max_len = g_devtools_ui.asm_source_buf_size() - 1;
      if (source.size() > max_len) source.resize(max_len);
      g_devtools_ui.asm_set_source(source.c_str());
      return "OK\n";
    }
    if (parts[1] == "load" && parts.size() >= 3) {
      std::string path = parts[2];
      if (has_path_traversal(path)) return "ERR 403 path-traversal\n";
      std::ifstream f(path);
      if (!f.good()) return "ERR 404 file-not-found\n";
      std::string content((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
      size_t max_len = g_devtools_ui.asm_source_buf_size() - 1;
      if (content.size() > max_len) content.resize(max_len);
      g_devtools_ui.asm_set_source(content.c_str());
      return "OK\n";
    }
    if (parts[1] == "assemble") {
      AsmResult r = g_assembler.assemble(g_devtools_ui.asm_source_buf());
      if (r.success) {
        char buf[128];
        snprintf(buf, sizeof(buf), "OK bytes=%d start=%04X end=%04X\n",
                 r.bytes_written, r.start_addr, r.end_addr);
        // Export symbols
        for (auto& [name, addr] : r.symbols)
          g_symfile.addSymbol(addr, name);
        return std::string(buf);
      }
      char buf[64];
      snprintf(buf, sizeof(buf), "ERR asm %d errors\n", static_cast<int>(r.errors.size()));
      return std::string(buf);
    }
    if (parts[1] == "errors") {
      // Run check without writing and report errors
      AsmResult r = g_assembler.check(g_devtools_ui.asm_source_buf());
      std::string resp = "OK";
      for (auto& e : r.errors) {
        resp += " line=" + std::to_string(e.line) + " " + e.message + "\n";
      }
      if (r.errors.empty()) resp += " no-errors";
      return resp + "\n";
    }
    if (parts[1] == "symbols") {
      AsmResult r = g_assembler.check(g_devtools_ui.asm_source_buf());
      std::string resp = "OK";
      for (auto& [name, addr] : r.symbols) {
        char buf[64];
        snprintf(buf, sizeof(buf), " %s=%04X", name.c_str(), addr);
        resp += buf;
      }
      return resp + "\n";
    }
    if (parts[1] == "source") {
      return "OK " + std::string(g_devtools_ui.asm_source_buf()) + "\n";
    }
    return "ERR 400 usage: asm (text|load|assemble|errors|symbols|source)\n";
  }

  // ── m4 ──
  if (cmd == "m4" && parts.size() >= 2) {
    if (parts[1] == "status") {
      int open_files = 0;
      for (int i = 0; i < 4; i++) {
        if (g_m4board.open_files[i]) open_files++;
      }
      std::ostringstream oss;
      oss << "OK enabled=" << (g_m4board.enabled ? 1 : 0)
          << " sd_path=" << (g_m4board.sd_root_path.empty() ? "(none)" : g_m4board.sd_root_path)
          << " dir=" << g_m4board.current_dir
          << " files=" << open_files << "/4"
          << " cmds=" << g_m4board.cmd_count << "\n";
      return oss.str();
    }
    if (parts[1] == "ls") {
      if (g_m4board.sd_root_path.empty()) return "ERR 400 no-sd-path\n";
      // Inside a DSK container — list files from the disk image, not the host FS
      if (g_m4board.container_type != M4Board::ContainerType::NONE
          && g_m4board.container_drive) {
        std::string err;
        auto files = disk_list_files(g_m4board.container_drive, err);
        if (!err.empty()) return "ERR 500 " + err + "\n";
        std::string resp = "OK\n";
        for (const auto& f : files)
          resp += f.display_name + "\n";
        return resp;
      }
      try {
        auto root = std::filesystem::weakly_canonical(g_m4board.sd_root_path);
        std::filesystem::path dir_path = root / g_m4board.current_dir.substr(1);
        auto dir = std::filesystem::weakly_canonical(dir_path);
        // Verify dir is inside root (component-wise, not string prefix)
        auto rel = dir.lexically_normal().lexically_relative(root.lexically_normal());
        if (rel.empty() || (!rel.empty() && *rel.begin() == ".."))
          return "ERR 403 path-traversal\n";
        std::string resp = "OK\n";
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
          std::string name = entry.path().filename().string();
          if (name.empty() || name[0] == '.') continue;
          if (entry.is_directory()) resp += ">" + name + "\n";
          else resp += name + "\n";
        }
        return resp;
      } catch (const std::filesystem::filesystem_error& e) {
        return "ERR 500 " + std::string(e.what()) + "\n";
      }
    }
    if (parts[1] == "cd" && parts.size() >= 3) {
      std::string path = parts[2];
      if (path == "/") {
        // Exit container if inside one, then go to root
        if (g_m4board.container_type != M4Board::ContainerType::NONE) {
          // container_exit is static in m4board.cpp; replicate its effect
          if (g_m4board.container_drive) {
            dsk_eject(g_m4board.container_drive);
            delete g_m4board.container_drive;
            g_m4board.container_drive = nullptr;
          }
          g_m4board.container_parent_dir.clear();
          g_m4board.container_host_path.clear();
          g_m4board.container_type = M4Board::ContainerType::NONE;
        }
        g_m4board.current_dir = "/";
        return "OK /\n";
      }
      // "cd .." from inside a container exits the container
      if ((path == ".." || path == "../")
          && g_m4board.container_type != M4Board::ContainerType::NONE) {
        std::string parent = g_m4board.container_parent_dir;
        if (g_m4board.container_drive) {
          dsk_eject(g_m4board.container_drive);
          delete g_m4board.container_drive;
          g_m4board.container_drive = nullptr;
        }
        g_m4board.container_parent_dir.clear();
        g_m4board.container_host_path.clear();
        g_m4board.container_type = M4Board::ContainerType::NONE;
        g_m4board.current_dir = parent.empty() ? "/" : parent;
        return "OK " + g_m4board.current_dir + "\n";
      }
      // Cannot navigate further inside a container (no subdirs in DSK)
      if (g_m4board.container_type != M4Board::ContainerType::NONE)
        return "ERR 400 cannot-cd-inside-container\n";
      if (g_m4board.sd_root_path.empty()) return "ERR 400 no-sd-path\n";
      try {
        auto root = std::filesystem::weakly_canonical(g_m4board.sd_root_path);
        std::filesystem::path full_path = root;
        if (path.front() == '/') {
          full_path /= path.substr(1);
        } else {
          full_path /= g_m4board.current_dir.substr(1);
          full_path /= path;
        }
        auto canonical = std::filesystem::weakly_canonical(full_path);
        // Component-aware path traversal check
        auto rel = canonical.lexically_normal().lexically_relative(root.lexically_normal());
        if (rel.empty() || (!rel.empty() && *rel.begin() == ".."))
          return "ERR 403 path-traversal\n";
        if (!std::filesystem::is_directory(canonical)) return "ERR 404 not-a-directory\n";
        std::string rel_str;
        if (rel == ".") {
          rel_str = "/";
        } else {
          rel_str = "/" + rel.generic_string();
          if (rel_str.back() != '/') rel_str += '/';
        }
        g_m4board.current_dir = rel_str;
        return "OK " + rel_str + "\n";
      } catch (const std::filesystem::filesystem_error& e) {
        return "ERR 500 " + std::string(e.what()) + "\n";
      }
    }
    if (parts[1] == "reset") {
      if (!CPC.paused) return "ERR 400 pause-first\n";
      m4board_cleanup();
      m4board_reset();
      return "OK\n";
    }
    if (parts[1] == "wifi") {
      if (parts.size() < 3) {
        return std::string("OK ") + (g_m4board.network_enabled ? "1" : "0") + "\n";
      }
      g_m4board.network_enabled = (parts[2] != "0");
      return "OK\n";
    }
    if (parts[1] == "http") {
      if (parts.size() < 3 || parts[2] == "status") {
        std::ostringstream oss;
        oss << "OK running=" << (g_m4_http.is_running() ? 1 : 0)
            << " port=" << g_m4_http.port()
            << " bind=" << g_m4_http.bind_ip() << "\n";
        return oss.str();
      }
      if (parts[2] == "start") {
        if (g_m4_http.is_running()) return "OK already-running\n";
        g_m4_http.start(CPC.m4_http_port, CPC.m4_bind_ip);
        return "OK started\n";
      }
      if (parts[2] == "stop") {
        g_m4_http.stop();
        return "OK stopped\n";
      }
      return "ERR 400 usage: m4 http [start|stop|status]\n";
    }
    if (parts[1] == "ports") {
      auto mappings = g_m4_http.get_port_mappings_snapshot();
      if (mappings.empty()) return "OK count=0\n";
      std::ostringstream oss;
      oss << "OK count=" << mappings.size();
      for (const auto& m : mappings) {
        oss << " " << m.cpc_port << ":" << m.host_port
            << (m.user_override ? ":user" : ":auto")
            << (m.active ? ":active" : ":idle");
      }
      oss << "\n";
      return oss.str();
    }
    if (parts[1] == "port" && parts.size() >= 3) {
      if (parts[2] == "set" && parts.size() >= 5) {
        try {
          unsigned long cpc_val = std::stoul(parts[3]);
          unsigned long host_val = std::stoul(parts[4]);
          if (cpc_val < 1 || cpc_val > 65535)
            return "ERR 400 cpc port out of range (1-65535)\n";
          if (host_val < 1 || host_val > 65535)
            return "ERR 400 host port out of range (1-65535)\n";
          g_m4_http.set_port_mapping(
            static_cast<uint16_t>(cpc_val),
            static_cast<uint16_t>(host_val), true);
          return "OK\n";
        } catch (const std::exception& e) {
          return std::string("ERR 400 ") + e.what() + "\n";
        }
      }
      if (parts[2] == "del" && parts.size() >= 4) {
        try {
          unsigned long cpc_val = std::stoul(parts[3]);
          if (cpc_val < 1 || cpc_val > 65535)
            return "ERR 400 port out of range (1-65535)\n";
          g_m4_http.remove_port_mapping(static_cast<uint16_t>(cpc_val));
          return "OK\n";
        } catch (const std::exception& e) {
          return std::string("ERR 400 ") + e.what() + "\n";
        }
      }
      return "ERR 400 usage: m4 port set <cpc> <host> | m4 port del <cpc>\n";
    }
    return "ERR 400 usage: m4 (status|ls|cd|reset|wifi|http|ports|port)\n";
  }

  // Unknown command — find closest match for "did you mean?" suggestion
  {
    std::string suggestion;
    size_t best_dist = SIZE_MAX;
    // Stack-based Levenshtein — command names are short (<20 chars)
    static constexpr size_t MAX_CMD_LEN = 32;
    size_t prev[MAX_CMD_LEN + 1], curr[MAX_CMD_LEN + 1];
    for (const auto& kv : g_ipc_commands) {
      const std::string& name = kv.first;
      size_t n = cmd.size(), m = name.size();
      if (n > MAX_CMD_LEN || m > MAX_CMD_LEN) continue;
      for (size_t j = 0; j <= m; j++) prev[j] = j;
      for (size_t i = 1; i <= n; i++) {
        curr[0] = i;
        for (size_t j = 1; j <= m; j++) {
          size_t cost = (cmd[i - 1] == name[j - 1]) ? 0 : 1;
          size_t del = prev[j] + 1, ins = curr[j - 1] + 1, sub = prev[j - 1] + cost;
          curr[j] = del < ins ? (del < sub ? del : sub) : (ins < sub ? ins : sub);
        }
        for (size_t j = 0; j <= m; j++) prev[j] = curr[j];
      }
      size_t dist = prev[m];
      if (dist < best_dist && dist <= 3) {
        best_dist = dist;
        suggestion = name;
      }
    }
    std::string msg = "unknown-command: '" + cmd + "' is not recognized";
    if (!suggestion.empty())
      msg += ". Did you mean '" + suggestion + "'?";
    msg += " (Type 'help' for a list of commands)";
    return "ERR 404 " + msg + "\n";
  }

  } catch (const std::invalid_argument& e) {
    std::string val = e.what();
    std::string msg = "bad-number";
    if (!val.empty() && val != "stoi" && val != "stoul")
      msg += ": '" + val + "' is not a valid number";
    msg += " (accepted: 0x, $, &, # prefixes, decimal, or bare hex)";
    return err_with_context(400, msg);
  } catch (const std::out_of_range&) {
    return err_with_context(400, "number-out-of-range (value too large for target type)");
  }
}
}

KoncepcjaIpcServer::~KoncepcjaIpcServer() {
  stop();
}

void KoncepcjaIpcServer::start() {
  if (running.load()) return;
  running.store(true);
  g_ipc_instance = this;
  z80_set_breakpoint_hit_hook(&breakpoint_hit_hook);
  server_thread = std::thread(&KoncepcjaIpcServer::run, this);
}

void KoncepcjaIpcServer::stop() {
  running.store(false);
  if (server_thread.joinable()) server_thread.join();
  if (g_ipc_instance == this) {
    g_ipc_instance = nullptr;
    z80_set_breakpoint_hit_hook(nullptr);
  }
}

void KoncepcjaIpcServer::notify_breakpoint_hit(uint16_t pc, bool watchpoint) {
  breakpoint_pc.store(pc);
  breakpoint_watchpoint.store(watchpoint);
  breakpoint_hit.store(true);
}

bool KoncepcjaIpcServer::consume_breakpoint_hit(uint16_t& pc, bool& watchpoint) {
  if (!breakpoint_hit.load()) return false;
  pc = breakpoint_pc.load();
  watchpoint = breakpoint_watchpoint.load();
  breakpoint_hit.store(false);
  return true;
}

// --- Frame step synchronization ---

void KoncepcjaIpcServer::notify_frame_step_done() {
  frame_step_active.store(false);
  frame_step_cv.notify_all();
}

void KoncepcjaIpcServer::wait_frame_step_done() {
  std::unique_lock<std::mutex> lock(frame_step_mutex);
  frame_step_cv.wait(lock, [this]{ return !frame_step_active.load(); });
}

// --- Event system ---

void KoncepcjaIpcServer::update_event_flags() {
  bool pc = false, mem = false, vbl = false;
  for (const auto& e : events) {
    if (e.trigger == EventTrigger::PC) pc = true;
    else if (e.trigger == EventTrigger::MEM_WRITE) mem = true;
    else if (e.trigger == EventTrigger::VBL) vbl = true;
  }
  has_pc_events.store(pc);
  has_mem_events.store(mem);
  has_vbl_events.store(vbl);
}

int KoncepcjaIpcServer::add_event(const IpcEvent& ev) {
  std::lock_guard<std::mutex> lock(events_mutex);
  IpcEvent e = ev;
  e.id = next_event_id++;
  events.push_back(e);
  update_event_flags();
  return e.id;
}

bool KoncepcjaIpcServer::remove_event(int id) {
  std::lock_guard<std::mutex> lock(events_mutex);
  for (auto it = events.begin(); it != events.end(); ++it) {
    if (it->id == id) {
      events.erase(it);
      update_event_flags();
      return true;
    }
  }
  return false;
}

std::vector<IpcEvent> KoncepcjaIpcServer::list_events() const {
  std::lock_guard<std::mutex> lock(events_mutex);
  return events;
}

void KoncepcjaIpcServer::execute_event_command(const std::string& cmd) {
  // Guard against infinite recursion (e.g. event triggers command that
  // re-triggers the same event: "event on mem=0xC000 mem write 0xC000 1")
  static thread_local int recursion_depth = 0;
  if (recursion_depth >= 4) {
    fprintf(stderr, "IPC event recursion limit reached, dropping: %s\n", cmd.c_str());
    return;
  }
  recursion_depth++;
  handle_command(cmd);
  recursion_depth--;
}

void KoncepcjaIpcServer::check_pc_events(uint16_t pc) {
  if (!has_pc_events.load(std::memory_order_relaxed)) return;
  std::lock_guard<std::mutex> lock(events_mutex);
  bool removed = false;
  for (auto it = events.begin(); it != events.end(); ) {
    if (it->trigger == EventTrigger::PC && it->address == pc) {
      execute_event_command(it->command);
      if (it->one_shot) {
        it = events.erase(it);
        removed = true;
        continue;
      }
    }
    ++it;
  }
  if (removed) update_event_flags();
}

void KoncepcjaIpcServer::check_mem_write_events(uint16_t addr, uint8_t val) {
  if (!has_mem_events.load(std::memory_order_relaxed)) return;
  std::lock_guard<std::mutex> lock(events_mutex);
  bool removed = false;
  for (auto it = events.begin(); it != events.end(); ) {
    if (it->trigger == EventTrigger::MEM_WRITE && it->address == addr) {
      if (!it->match_value || it->value == val) {
        execute_event_command(it->command);
        if (it->one_shot) {
          it = events.erase(it);
          removed = true;
          continue;
        }
      }
    }
    ++it;
  }
  if (removed) update_event_flags();
}

void KoncepcjaIpcServer::check_vbl_events() {
  if (!has_vbl_events.load(std::memory_order_relaxed)) return;
  std::lock_guard<std::mutex> lock(events_mutex);
  bool removed = false;
  for (auto it = events.begin(); it != events.end(); ) {
    if (it->trigger == EventTrigger::VBL) {
      it->vbl_counter--;
      if (it->vbl_counter <= 0) {
        execute_event_command(it->command);
        if (it->one_shot) {
          it = events.erase(it);
          removed = true;
          continue;
        }
        it->vbl_counter = it->vbl_interval; // reset for next fire
      }
    }
    ++it;
  }
  if (removed) update_event_flags();
}

// Free functions for z80.cpp / main loop
void ipc_check_pc_events(uint16_t pc) {
  if (g_ipc_instance) g_ipc_instance->check_pc_events(pc);
}
void ipc_check_mem_write_events(uint16_t addr, uint8_t val) {
  if (g_ipc_instance) g_ipc_instance->check_mem_write_events(addr, val);
}
void ipc_check_vbl_events() {
  if (g_ipc_instance) g_ipc_instance->check_vbl_events();
}

#ifdef _WIN32

void KoncepcjaIpcServer::run() {
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;

  SOCKET server_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server_fd == INVALID_SOCKET) { WSACleanup(); return; }

  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  int bound_port = 0;
  for (int p = kBasePort; p < kBasePort + kMaxPortAttempts; p++) {
    addr.sin_port = htons(static_cast<uint16_t>(p));
    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != SOCKET_ERROR) {
      bound_port = p;
      break;
    }
  }
  if (bound_port == 0) {
    LOG_ERROR("IPC: could not bind to any port in range " << kBasePort << "-" << (kBasePort + kMaxPortAttempts - 1));
    closesocket(server_fd);
    WSACleanup();
    return;
  }

  if (listen(server_fd, 1) == SOCKET_ERROR) {
    closesocket(server_fd);
    WSACleanup();
    return;
  }

  actual_port.store(bound_port);
  LOG_INFO("IPC: listening on port " << bound_port);

  while (running.load()) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(server_fd, &readfds);
    timeval tv{0, 200000}; // 200ms

    int ready = select(0, &readfds, nullptr, nullptr, &tv);
    if (ready <= 0) continue;

    sockaddr_in client{};
    int len = sizeof(client);
    SOCKET client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&client), &len);
    if (client_fd == INVALID_SOCKET) continue;

    // Persistent connection: read lines until client disconnects, sends "disconnect", or idle timeout.
    std::string buffer;
    char buf[1024];
    bool client_quit = false;
    while (running.load() && !client_quit) {
      fd_set cfds;
      FD_ZERO(&cfds);
      FD_SET(client_fd, &cfds);
      timeval ctv{60, 0}; // 60s idle timeout

      int cready = select(0, &cfds, nullptr, nullptr, &ctv);
      if (cready <= 0) break; // timeout or error

      int n = recv(client_fd, buf, sizeof(buf) - 1, 0);
      if (n <= 0) break; // client disconnected or error
      buf[n] = 0;
      buffer.append(buf);

      // Extract and dispatch complete lines
      size_t pos;
      while ((pos = buffer.find('\n')) != std::string::npos) {
        std::string raw_line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);
        if (!raw_line.empty() && raw_line.back() == '\r') raw_line.pop_back();

        // Split on semicolons for command chaining
        auto cmds = split_semicolons(raw_line);
        for (const auto& cmd : cmds) {
          if (cmd == "disconnect") { client_quit = true; break; }
          auto reply = handle_command(cmd);
          send(client_fd, reply.c_str(), static_cast<int>(reply.size()), 0);
        }
        if (client_quit) break;
      }
    }
    // Dispatch any trailing data without newline (for single-shot clients like echo|nc)
    if (!client_quit && !buffer.empty()) {
      if (!buffer.empty() && buffer.back() == '\r') buffer.pop_back();
      auto cmds = split_semicolons(buffer);
      for (const auto& cmd : cmds) {
        if (cmd == "disconnect") break;
        auto reply = handle_command(cmd);
        send(client_fd, reply.c_str(), static_cast<int>(reply.size()), 0);
      }
    }
    closesocket(client_fd);
  }

  closesocket(server_fd);
  WSACleanup();
}

#else // POSIX

void KoncepcjaIpcServer::run() {
  int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) return;

  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  int bound_port = 0;
  for (int p = kBasePort; p < kBasePort + kMaxPortAttempts; p++) {
    addr.sin_port = htons(static_cast<uint16_t>(p));
    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
      bound_port = p;
      break;
    }
  }
  if (bound_port == 0) {
    LOG_ERROR("IPC: could not bind to any port in range " << kBasePort << "-" << (kBasePort + kMaxPortAttempts - 1));
    ::close(server_fd);
    return;
  }

  if (listen(server_fd, 1) < 0) {
    ::close(server_fd);
    return;
  }

  actual_port.store(bound_port);
  LOG_INFO("IPC: listening on port " << bound_port);

  while (running.load()) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(server_fd, &readfds);
    timeval tv{0, 200000}; // 200ms

    int ready = select(server_fd + 1, &readfds, nullptr, nullptr, &tv);
    if (ready <= 0) continue;

    sockaddr_in client{};
    socklen_t len = sizeof(client);
    int client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&client), &len);
    if (client_fd < 0) continue;

    // Persistent connection: read lines until client disconnects, sends "disconnect", or idle timeout.
    std::string buffer;
    char buf[1024];
    bool client_quit = false;
    while (running.load() && !client_quit) {
      fd_set cfds;
      FD_ZERO(&cfds);
      FD_SET(client_fd, &cfds);
      timeval ctv{60, 0}; // 60s idle timeout

      int cready = select(client_fd + 1, &cfds, nullptr, nullptr, &ctv);
      if (cready <= 0) break; // timeout or error

      ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
      if (n <= 0) break; // client disconnected or error
      buf[n] = 0;
      buffer.append(buf);

      // Extract and dispatch complete lines
      size_t pos;
      while ((pos = buffer.find('\n')) != std::string::npos) {
        std::string raw_line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);
        if (!raw_line.empty() && raw_line.back() == '\r') raw_line.pop_back();

        // Split on semicolons for command chaining
        auto cmds = split_semicolons(raw_line);
        for (const auto& cmd : cmds) {
          if (cmd == "disconnect") { client_quit = true; break; }
          auto reply = handle_command(cmd);
          (void)write(client_fd, reply.c_str(), reply.size());
        }
        if (client_quit) break;
      }
    }
    // Dispatch any trailing data without newline (for single-shot clients like echo|nc)
    if (!client_quit && !buffer.empty()) {
      if (!buffer.empty() && buffer.back() == '\r') buffer.pop_back();
      auto cmds = split_semicolons(buffer);
      for (const auto& cmd : cmds) {
        if (cmd == "disconnect") break;
        auto reply = handle_command(cmd);
        (void)write(client_fd, reply.c_str(), reply.size());
      }
    }
    ::close(client_fd);
  }

  ::close(server_fd);
}

#endif // _WIN32
