#include "koncepcja_ipc_server.h"
#include "autotype.h"
#include "imgui_ui.h"

#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#include <cctype>
#include <sstream>
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
#include "keyboard.h"
#include "trace.h"
#include "gif_recorder.h"
#include "wav_recorder.h"
#include "symfile.h"
#include "pokes.h"
#include "config_profile.h"

extern t_z80regs z80;
extern t_CPC CPC;
extern t_CRTC CRTC;
extern t_GateArray GateArray;
extern t_PSG PSG;
extern SDL_Surface *back_surface;
extern byte *pbRAM;
extern byte *membank_read[4], *membank_write[4];
extern byte keyboard_matrix[16];
extern t_drive driveA;
extern t_drive driveB;

// Friendly key names → CPC_KEYS for IPC input commands
static const std::map<std::string, CPC_KEYS> ipc_key_names = {
  {"ESC", CPC_ESC}, {"RETURN", CPC_RETURN}, {"ENTER", CPC_RETURN},
  {"SPACE", CPC_SPACE}, {"TAB", CPC_TAB}, {"DEL", CPC_DEL},
  {"COPY", CPC_COPY}, {"CONTROL", CPC_CONTROL}, {"CTRL", CPC_CONTROL},
  {"SHIFT", CPC_LSHIFT}, {"LSHIFT", CPC_LSHIFT}, {"RSHIFT", CPC_RSHIFT},
  {"UP", CPC_CUR_UP}, {"DOWN", CPC_CUR_DOWN},
  {"LEFT", CPC_CUR_LEFT}, {"RIGHT", CPC_CUR_RIGHT},
  {"CLR", CPC_CLR},
  {"F0", CPC_F0}, {"F1", CPC_F1}, {"F2", CPC_F2}, {"F3", CPC_F3},
  {"F4", CPC_F4}, {"F5", CPC_F5}, {"F6", CPC_F6}, {"F7", CPC_F7},
  {"F8", CPC_F8}, {"F9", CPC_F9},
  // Joystick
  {"J0_UP", CPC_J0_UP}, {"J0_DOWN", CPC_J0_DOWN},
  {"J0_LEFT", CPC_J0_LEFT}, {"J0_RIGHT", CPC_J0_RIGHT},
  {"J0_FIRE1", CPC_J0_FIRE1}, {"J0_FIRE2", CPC_J0_FIRE2},
  {"J1_UP", CPC_J1_UP}, {"J1_DOWN", CPC_J1_DOWN},
  {"J1_LEFT", CPC_J1_LEFT}, {"J1_RIGHT", CPC_J1_RIGHT},
  {"J1_FIRE1", CPC_J1_FIRE1}, {"J1_FIRE2", CPC_J1_FIRE2},
};

// Char → CPC_KEYS for text typing
static const std::map<char, CPC_KEYS> ipc_char_to_key = {
  {'a', CPC_a}, {'b', CPC_b}, {'c', CPC_c}, {'d', CPC_d}, {'e', CPC_e},
  {'f', CPC_f}, {'g', CPC_g}, {'h', CPC_h}, {'i', CPC_i}, {'j', CPC_j},
  {'k', CPC_k}, {'l', CPC_l}, {'m', CPC_m}, {'n', CPC_n}, {'o', CPC_o},
  {'p', CPC_p}, {'q', CPC_q}, {'r', CPC_r}, {'s', CPC_s}, {'t', CPC_t},
  {'u', CPC_u}, {'v', CPC_v}, {'w', CPC_w}, {'x', CPC_x}, {'y', CPC_y},
  {'z', CPC_z},
  {'A', CPC_A}, {'B', CPC_B}, {'C', CPC_C}, {'D', CPC_D}, {'E', CPC_E},
  {'F', CPC_F}, {'G', CPC_G}, {'H', CPC_H}, {'I', CPC_I}, {'J', CPC_J},
  {'K', CPC_K}, {'L', CPC_L}, {'M', CPC_M}, {'N', CPC_N}, {'O', CPC_O},
  {'P', CPC_P}, {'Q', CPC_Q}, {'R', CPC_R}, {'S', CPC_S}, {'T', CPC_T},
  {'U', CPC_U}, {'V', CPC_V}, {'W', CPC_W}, {'X', CPC_X}, {'Y', CPC_Y},
  {'Z', CPC_Z},
  {'0', CPC_0}, {'1', CPC_1}, {'2', CPC_2}, {'3', CPC_3}, {'4', CPC_4},
  {'5', CPC_5}, {'6', CPC_6}, {'7', CPC_7}, {'8', CPC_8}, {'9', CPC_9},
  {' ', CPC_SPACE}, {'\n', CPC_RETURN}, {'\r', CPC_RETURN},
  {'.', CPC_PERIOD}, {',', CPC_COMMA}, {';', CPC_SEMICOLON},
  {':', CPC_COLON}, {'-', CPC_MINUS}, {'+', CPC_PLUS},
  {'/', CPC_SLASH}, {'*', CPC_ASTERISK}, {'=', CPC_EQUAL},
  {'(', CPC_LEFTPAREN}, {')', CPC_RIGHTPAREN},
  {'[', CPC_LBRACKET}, {']', CPC_RBRACKET},
  {'{', CPC_LCBRACE}, {'}', CPC_RCBRACE},
  {'<', CPC_LESS}, {'>', CPC_GREATER},
  {'?', CPC_QUESTION}, {'!', CPC_EXCLAMATN},
  {'@', CPC_AT}, {'#', CPC_HASH}, {'$', CPC_DOLLAR},
  {'%', CPC_PERCENT}, {'^', CPC_POWER}, {'&', CPC_AMPERSAND},
  {'|', CPC_PIPE}, {'\\', CPC_BACKSLASH},
  {'"', CPC_DBLQUOTE}, {'\'', CPC_QUOTE},
  {'`', CPC_BACKQUOTE}, {'_', CPC_UNDERSCORE},
};

extern byte bit_values[];

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

namespace {
constexpr int kPort = 6543;
KoncepcjaIpcServer* g_ipc_instance = nullptr;

void breakpoint_hit_hook(word pc, bool watchpoint) {
  if (g_ipc_instance) {
    g_ipc_instance->notify_breakpoint_hit(pc, watchpoint);
  }
}

std::vector<std::string> split_lines(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == '\n') {
      if (!cur.empty() && cur.back() == '\r') cur.pop_back();
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
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
  if (line.empty()) return "OK\n";
  auto parts = split_ws(line);
  if (parts.empty()) return "OK\n";

  const auto& cmd = parts[0];
  if (cmd == "ping") return "OK pong\n";
  if (cmd == "version") return "OK kaprys-0.1\n";
  if (cmd == "help") return "OK commands: ping version help quit pause run reset load regs reg(set/get) mem(read/write/fill/compare/find) bp(list/add/del/clear) wp(add/del/clear/list) iobp(add/del/clear/list) step(N/over/out/to/frame) wait hash(vram/mem/regs) screenshot snapshot(save/load) disasm(follow/refs) devtools input(keydown/keyup/key/type/joy) trace(on/off/dump/on_crash/status) frames(dump) event(on/once/off/list) timer(list/clear) sym(load/add/del/list/lookup) stack autotype(text/status/clear) disk(formats/format/new/ls/cat/get/put/rm/info) record(wav) poke(load/list/apply/unapply/write) profile(list/current/load/save/delete)\n";
  if (cmd == "quit") {
    int code = 0;
    if (parts.size() >= 2) code = std::stoi(parts[1]);
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
      unsigned int addr = std::stoul(parts[2], nullptr, 0);
      unsigned int len = std::stoul(parts[3], nullptr, 0);
      uLong crc = crc32(0L, nullptr, 0);
      // Read through z80 memory banking for correctness
      std::vector<byte> tmp(len);
      for (unsigned int i = 0; i < len; i++) {
        tmp[i] = z80_read_mem(static_cast<word>(addr + i));
      }
      crc = crc32(crc, tmp.data(), static_cast<uInt>(len));
      snprintf(buf, sizeof(buf), "OK crc32=%08lX\n", crc);
      return std::string(buf);
    }
    if (parts[1] == "regs") {
      // Pack register state and hash it
      struct __attribute__((packed)) {
        word AF, BC, DE, HL, IX, IY, SP, PC;
        word AFx, BCx, DEx, HLx;
        byte I, R, IM, IFF1, IFF2;
      } packed;
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
    return "OK\n";
  }
  if (cmd == "run") {
    cpc_resume();
    return "OK\n";
  }
  if (cmd == "reset") {
    emulator_reset();
    return "OK\n";
  }
  if (cmd == "load") {
    if (parts.size() < 2) return "ERR 400 bad-args\n";
    const std::string& path = parts[1];
    std::string lower = path;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto dot = lower.find_last_of('.');
    if (dot == std::string::npos) return "ERR 415 unsupported\n";
    std::string ext = lower.substr(dot);
    if (ext == ".dsk") {
      CPC.driveA.file = path;
      CPC.driveA.zip_index = 0;
      return file_load(CPC.driveA) == 0 ? "OK\n" : "ERR 500 load-dsk\n";
    }
    if (ext == ".sna") {
      CPC.snapshot.file = path;
      CPC.snapshot.zip_index = 0;
      return file_load(CPC.snapshot) == 0 ? "OK\n" : "ERR 500 load-sna\n";
    }
    if (ext == ".cpr") {
      CPC.cartridge.file = path;
      CPC.cartridge.zip_index = 0;
      return file_load(CPC.cartridge) == 0 ? "OK\n" : "ERR 500 load-cpr\n";
    }
    if (ext == ".bin") {
      bin_load(path, 0x6000);
      return "OK\n";
    }
    return "ERR 415 unsupported\n";
  }
  if ((cmd == "reg" || cmd == "regs") && parts.size() >= 2 && parts[1] == "set") {
    if (parts.size() < 4) return "ERR 400 bad-args\n";
    std::string reg = parts[2];
    for (auto& c : reg) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    unsigned int value = std::stoul(parts[3], nullptr, 0);

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

    return "OK\n";
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
    if (parts.size() >= 2) {
      if (dumpScreenTo(parts[1])) return "OK\n";
      return "ERR 503 no-surface\n";
    }
    dumpScreen();
    return "OK\n";
  }
  if (cmd == "devtools") {
    imgui_state.show_devtools = true;
    return "OK\n";
  }
  if (cmd == "snapshot" && parts.size() >= 2) {
    if (parts[1] == "save") {
      if (parts.size() < 3) return "ERR 400 bad-args\n";
      if (snapshot_save(parts[2]) == 0) return "OK\n";
      return "ERR 500 snapshot-save\n";
    }
    if (parts[1] == "load") {
      if (parts.size() < 3) return "ERR 400 bad-args\n";
      if (snapshot_load(parts[2]) == 0) return "OK\n";
      return "ERR 500 snapshot-load\n";
    }
  }
  if (cmd == "mem" && parts.size() >= 4 && parts[1] == "read") {
    // mem read <addr> <len> [--view=read|write] [--bank=N] [ascii]
    unsigned int addr = std::stoul(parts[2], nullptr, 0);
    unsigned int len = std::stoul(parts[3], nullptr, 0);
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
        raw_bank = std::stoi(parts[pi].substr(7));
      }
    }
    std::string resp = "OK ";
    std::string ascii_str;
    char bytebuf[4];
    for (unsigned int i = 0; i < len; i++) {
      byte v;
      if (raw_bank >= 0) {
        v = z80_read_mem_raw_bank(static_cast<word>(addr + i), raw_bank);
      } else if (view_mode == 1) {
        v = z80_read_mem_via_write_bank(static_cast<word>(addr + i));
      } else {
        v = z80_read_mem(static_cast<word>(addr + i));
      }
      snprintf(bytebuf, sizeof(bytebuf), "%02X", v);
      resp += bytebuf;
      if (with_ascii) {
        char c = (v >= 32 && v <= 126) ? static_cast<char>(v) : '.';
        ascii_str.push_back(c);
        if ((i + 1) % 16 == 0) {
          resp += " |" + ascii_str + "| ";
          ascii_str.clear();
        }
      }
    }
    if (!ascii_str.empty()) {
      resp += " |" + ascii_str + "|";
    }
    resp += "\n";
    return resp;
  }
  if (cmd == "mem" && parts.size() >= 4 && parts[1] == "write") {
    // mem write <addr> <hexbytes...>
    unsigned int addr = std::stoul(parts[2], nullptr, 0);
    std::string hex;
    for (size_t i = 3; i < parts.size(); i++) hex += parts[i];
    if (hex.size() % 2 != 0) return "ERR 400 bad-hex\n";
    for (size_t i = 0; i < hex.size(); i += 2) {
      std::string byte_str = hex.substr(i, 2);
      byte v = static_cast<byte>(std::stoul(byte_str, nullptr, 16));
      z80_write_mem(static_cast<word>(addr + (i/2)), v);
    }
    return "OK\n";
  }
  if (cmd == "mem" && parts.size() >= 5 && parts[1] == "fill") {
    // mem fill <addr> <len> <hex-pattern>
    unsigned int addr = std::stoul(parts[2], nullptr, 0);
    unsigned int len = std::stoul(parts[3], nullptr, 0);
    const std::string& hex = parts[4];
    if (hex.empty() || hex.size() % 2 != 0) return "ERR 400 bad-hex\n";
    std::vector<byte> pattern;
    for (size_t i = 0; i < hex.size(); i += 2) {
      pattern.push_back(static_cast<byte>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    for (unsigned int i = 0; i < len; i++) {
      z80_write_mem(static_cast<word>(addr + i), pattern[i % pattern.size()]);
    }
    return "OK\n";
  }
  if (cmd == "mem" && parts.size() >= 5 && parts[1] == "compare") {
    // mem compare <addr1> <addr2> <len>
    unsigned int addr1 = std::stoul(parts[2], nullptr, 0);
    unsigned int addr2 = std::stoul(parts[3], nullptr, 0);
    unsigned int len = std::stoul(parts[4], nullptr, 0);
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
  if (cmd == "disasm" && parts.size() >= 2) {
    // disasm follow <addr> — recursive disassembly following jumps
    if (parts[1] == "follow" && parts.size() >= 3) {
      unsigned int addr = std::stoul(parts[2], nullptr, 0);
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
      unsigned int target = std::stoul(parts[2], nullptr, 0);
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
    // disasm <addr> <count> [--symbols]
    if (parts.size() >= 3) {
      unsigned int addr = std::stoul(parts[1], nullptr, 0);
      int count = std::stoi(parts[2]);
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
      return resp.str();
    }
  }
  if (cmd == "bp" && parts.size() >= 2) {
    if (parts[1] == "add" && parts.size() >= 3) {
      unsigned int addr = std::stoul(parts[2], nullptr, 0);
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
          pass_count = std::stoi(parts[pi + 1]);
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
      return "OK\n";
    }
    if (parts[1] == "del" && parts.size() >= 3) {
      unsigned int addr = std::stoul(parts[2], nullptr, 0);
      z80_del_breakpoint(static_cast<word>(addr));
      return "OK\n";
    }
    if (parts[1] == "clear") {
      z80_clear_breakpoints();
      return "OK\n";
    }
    if (parts[1] == "list") {
      const auto& bps = z80_list_breakpoints_ref();
      std::string resp = "OK count=" + std::to_string(bps.size());
      char buf[128];
      for (const auto& b : bps) {
        snprintf(buf, sizeof(buf), " %04X", static_cast<unsigned int>(b.address));
        resp += buf;
        if (!b.condition_str.empty()) {
          resp += "[if ";
          resp += b.condition_str;
          resp += "]";
        }
        if (b.pass_count > 0) {
          resp += "[pass ";
          resp += std::to_string(b.pass_count);
          resp += "]";
        }
      }
      resp += "\n";
      return resp;
    }
  }
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
        port_val = static_cast<word>(std::stoul(port_str, nullptr, 0));
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
        else if (!arg.empty() && (argl.rfind("0x", 0) == 0 || (argl[0] >= '0' && argl[0] <= '9'))) {
          mask_val = static_cast<word>(std::stoul(arg, nullptr, 0));
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
      return "OK\n";
    }
    if (parts[1] == "del" && parts.size() >= 3) {
      int idx = std::stoi(parts[2]);
      z80_del_io_breakpoint(idx);
      return "OK\n";
    }
    if (parts[1] == "clear") {
      z80_clear_io_breakpoints();
      return "OK\n";
    }
    if (parts[1] == "list") {
      const auto& bps = z80_list_io_breakpoints_ref();
      std::string resp = "OK count=" + std::to_string(bps.size());
      for (size_t i = 0; i < bps.size(); i++) {
        char buf[64];
        const char* dir_str = "both";
        if (bps[i].dir == IO_IN) dir_str = "in";
        else if (bps[i].dir == IO_OUT) dir_str = "out";
        snprintf(buf, sizeof(buf), " %zu:%04X/%04X/%s",
                 i, static_cast<unsigned>(bps[i].port),
                 static_cast<unsigned>(bps[i].mask), dir_str);
        resp += buf;
        if (!bps[i].condition_str.empty()) {
          resp += "[if ";
          resp += bps[i].condition_str;
          resp += "]";
        }
      }
      resp += "\n";
      return resp;
    }
    return "ERR 400 bad-iobp-cmd (add|del|clear|list)\n";
  }
  if (cmd == "step") {
    // "step frame [N]" — advance N complete frames, then pause
    if (parts.size() >= 2 && parts[1] == "frame") {
      int n = 1;
      if (parts.size() >= 3) n = std::stoi(parts[2]);
      if (n < 1) return "ERR 400 bad-args\n";
      g_ipc_instance->frame_step_remaining.store(n);
      g_ipc_instance->frame_step_active.store(true);
      cpc_resume();
      g_ipc_instance->wait_frame_step_done();
      return "OK\n";
    }
    // "step over [N]" — step over CALL/RST (or single-step if not a call)
    if (parts.size() >= 2 && parts[1] == "over") {
      cpc_pause();
      int count = 1;
      if (parts.size() >= 3) count = std::stoi(parts[2]);
      for (int i = 0; i < count; i++) {
        word pc = z80.PC.w.l;
        if (z80_is_call_or_rst(pc)) {
          int len = z80_instruction_length(pc);
          z80_add_breakpoint_ephemeral(static_cast<word>(pc + len));
          cpc_resume();
          // Wait for breakpoint hit
          auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
          while (true) {
            uint16_t hit_pc = 0;
            bool watch = false;
            if (g_ipc_instance->consume_breakpoint_hit(hit_pc, watch)) break;
            if (std::chrono::steady_clock::now() > deadline) {
              cpc_pause();
              return "ERR 408 timeout\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
          cpc_pause();
        } else {
          z80_step_instruction();
        }
      }
      return "OK\n";
    }
    // "step out" — run until current function returns
    if (parts.size() >= 2 && parts[1] == "out") {
      z80.step_out = 1;
      z80.step_out_addresses.clear();
      cpc_resume();
      // Wait for step_out to complete (step_in transitions to 2 on return)
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (true) {
        uint16_t hit_pc = 0;
        bool watch = false;
        if (g_ipc_instance->consume_breakpoint_hit(hit_pc, watch)) break;
        if (z80.step_in >= 2) break;
        if (std::chrono::steady_clock::now() > deadline) {
          cpc_pause();
          z80.step_out = 0;
          return "ERR 408 timeout\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      cpc_pause();
      return "OK\n";
    }
    // "step to <addr>" — run-to-cursor (ephemeral breakpoint)
    if (parts.size() >= 3 && parts[1] == "to") {
      unsigned int addr = std::stoul(parts[2], nullptr, 0);
      z80_add_breakpoint_ephemeral(static_cast<word>(addr));
      cpc_resume();
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (true) {
        uint16_t hit_pc = 0;
        bool watch = false;
        if (g_ipc_instance->consume_breakpoint_hit(hit_pc, watch)) break;
        if (std::chrono::steady_clock::now() > deadline) {
          cpc_pause();
          return "ERR 408 timeout\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      cpc_pause();
      return "OK\n";
    }
    // "step [N]" — single-step N instructions
    cpc_pause();
    int count = 1;
    if (parts.size() >= 2) count = std::stoi(parts[1]);
    for (int i = 0; i < count; i++) z80_step_instruction();
    return "OK\n";
  }

  // Trace commands: trace on [size], trace off, trace dump <path>, trace on_crash <path>
  if (cmd == "trace" && parts.size() >= 2) {
    if (parts[1] == "on") {
      int size = 65536;
      if (parts.size() >= 3) size = std::stoi(parts[2]);
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

  // Frame dumps: frames dump <path_pattern> <count> [delay_cs]
  // If path ends in .gif → animated GIF; otherwise → PNG series
  if (cmd == "frames" && parts.size() >= 4 && parts[1] == "dump") {
    std::string pattern = parts[2];
    int frame_count = std::stoi(parts[3]);
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
      if (parts.size() >= 5) delay_cs = std::stoi(parts[4]);
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
        // User-provided printf format (e.g. "/tmp/frame_%04d.png") — non-literal by design
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
        snprintf(fname, sizeof(fname), pattern.c_str(), i);
#pragma GCC diagnostic pop
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
      auto it = ipc_key_names.find(upper);
      if (it != ipc_key_names.end()) {
        return {true, CPC.InputMapper->CPCscancodeFromCPCkey(it->second)};
      }
      // Single char shortcut: "A" → CPC_A, "a" → CPC_a, "1" → CPC_1
      if (name.size() == 1) {
        auto charIt = ipc_char_to_key.find(name[0]);
        if (charIt != ipc_char_to_key.end()) {
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
        auto charIt = ipc_char_to_key.find(ch);
        if (charIt == ipc_char_to_key.end()) continue; // skip unmappable chars
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
      int joy_num = std::stoi(parts[2]);
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

  if (cmd == "wait" && parts.size() >= 3) {
    auto timeout_ms = std::chrono::milliseconds(5000);
    auto deadline = std::chrono::steady_clock::now() + timeout_ms;

    if (parts[1] == "pc") {
      unsigned int addr = std::stoul(parts[2], nullptr, 0);
      if (parts.size() >= 4) deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::stoi(parts[3]));
      cpc_resume();
      while (z80.PC.w.l != addr) {
        if (std::chrono::steady_clock::now() > deadline) {
          cpc_pause();
          return "ERR 408 timeout\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      cpc_pause();
      return "OK\n";
    }
    if (parts[1] == "mem" && parts.size() >= 4) {
      unsigned int addr = std::stoul(parts[2], nullptr, 0);
      unsigned int val = std::stoul(parts[3], nullptr, 0);
      unsigned int mask = 0xFF;
      if (parts.size() >= 5) {
        if (parts[4].rfind("mask=", 0) == 0) {
          mask = std::stoul(parts[4].substr(5), nullptr, 0);
          if (parts.size() >= 6) {
            deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::stoi(parts[5]));
          }
        } else if (parts.size() >= 6) {
          mask = std::stoul(parts[4], nullptr, 0);
          deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::stoi(parts[5]));
        } else {
          deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::stoi(parts[4]));
        }
      }
      cpc_resume();
      while (true) {
        byte memv = z80_read_mem(static_cast<word>(addr));
        if ((memv & static_cast<byte>(mask)) == (static_cast<byte>(val) & static_cast<byte>(mask))) break;
        if (std::chrono::steady_clock::now() > deadline) {
          cpc_pause();
          return "ERR 408 timeout\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      cpc_pause();
      return "OK\n";
    }
    if (parts[1] == "bp") {
      if (parts.size() >= 3) deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::stoi(parts[2]));
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
          return "ERR 408 timeout\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    if (parts[1] == "vbl") {
      int count = std::stoi(parts[2]);
      if (parts.size() >= 4) deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::stoi(parts[3]));
      cpc_resume();
      for (int i = 0; i < count; i++) {
        if (std::chrono::steady_clock::now() > deadline) {
          cpc_pause();
          return "ERR 408 timeout\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      cpc_pause();
      return "OK\n";
    }
  }

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
        ev.address = static_cast<uint16_t>(std::stoul(trigger_str.substr(3), nullptr, 0));
      } else if (trigger_str.rfind("mem=", 0) == 0) {
        ev.trigger = EventTrigger::MEM_WRITE;
        std::string addr_val = trigger_str.substr(4);
        auto colon = addr_val.find(':');
        if (colon != std::string::npos) {
          ev.address = static_cast<uint16_t>(std::stoul(addr_val.substr(0, colon), nullptr, 0));
          ev.value = static_cast<uint8_t>(std::stoul(addr_val.substr(colon + 1), nullptr, 0));
          ev.match_value = true;
        } else {
          ev.address = static_cast<uint16_t>(std::stoul(addr_val, nullptr, 0));
          ev.match_value = false;
        }
      } else if (trigger_str.rfind("vbl=", 0) == 0) {
        ev.trigger = EventTrigger::VBL;
        ev.vbl_interval = std::stoi(trigger_str.substr(4));
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
        ev.address = static_cast<uint16_t>(std::stoul(trigger_str.substr(3), nullptr, 0));
      } else if (trigger_str.rfind("mem=", 0) == 0) {
        ev.trigger = EventTrigger::MEM_WRITE;
        std::string addr_val = trigger_str.substr(4);
        auto colon = addr_val.find(':');
        if (colon != std::string::npos) {
          ev.address = static_cast<uint16_t>(std::stoul(addr_val.substr(0, colon), nullptr, 0));
          ev.value = static_cast<uint8_t>(std::stoul(addr_val.substr(colon + 1), nullptr, 0));
          ev.match_value = true;
        } else {
          ev.address = static_cast<uint16_t>(std::stoul(addr_val, nullptr, 0));
          ev.match_value = false;
        }
      } else if (trigger_str.rfind("vbl=", 0) == 0) {
        ev.trigger = EventTrigger::VBL;
        ev.vbl_interval = std::stoi(trigger_str.substr(4));
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
      int id = std::stoi(parts[2]);
      if (g_ipc_instance->remove_event(id)) return "OK\n";
      return "ERR 404 event-not-found\n";
    }
    if (parts[1] == "list") {
      auto evts = g_ipc_instance->list_events();
      std::string resp = "OK count=" + std::to_string(evts.size()) + "\n";
      for (const auto& e : evts) {
        char buf[256];
        const char* trig_name = "?";
        if (e.trigger == EventTrigger::PC) trig_name = "pc";
        else if (e.trigger == EventTrigger::MEM_WRITE) trig_name = "mem";
        else if (e.trigger == EventTrigger::VBL) trig_name = "vbl";
        if (e.trigger == EventTrigger::VBL) {
          snprintf(buf, sizeof(buf), "  id=%d trigger=%s=%d%s cmd=%s\n",
                   e.id, trig_name, e.vbl_interval,
                   e.one_shot ? " once" : "", e.command.c_str());
        } else {
          snprintf(buf, sizeof(buf), "  id=%d trigger=%s=0x%04X%s cmd=%s\n",
                   e.id, trig_name, e.address,
                   e.one_shot ? " once" : "", e.command.c_str());
        }
        resp += buf;
      }
      return resp;
    }
    return "ERR 400 bad-event-cmd (on|once|off|list)\n";
  }

  if (cmd == "timer" && parts.size() >= 2) {
    if (parts[1] == "list") {
      const auto& timers = g_debug_timers.timers();
      std::string resp = "OK count=" + std::to_string(timers.size());
      for (const auto& [id, t] : timers) {
        char buf[128];
        uint32_t avg = t.count > 0 ? static_cast<uint32_t>(t.total_us / t.count) : 0;
        snprintf(buf, sizeof(buf), " id=%d count=%u last=%u min=%u max=%u avg=%u",
                 id, t.count, t.last_us,
                 t.min_us == UINT32_MAX ? 0 : t.min_us,
                 t.max_us, avg);
        resp += buf;
      }
      resp += "\n";
      return resp;
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
      unsigned int addr = std::stoul(parts[2], nullptr, 0);
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
          pass_count = std::stoi(parts[pi + 1]);
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
            unsigned int v = std::stoul(kw, nullptr, 0);
            len = static_cast<word>(v);
          } catch (...) {}
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
      return "OK\n";
    }
    if (parts[1] == "del" && parts.size() >= 3) {
      int idx = std::stoi(parts[2]);
      z80_del_watchpoint(idx);
      return "OK\n";
    }
    if (parts[1] == "clear") {
      z80_clear_watchpoints();
      return "OK\n";
    }
    if (parts[1] == "list") {
      const auto& wps = z80_list_watchpoints_ref();
      std::string resp = "OK count=" + std::to_string(wps.size());
      for (size_t i = 0; i < wps.size(); i++) {
        const auto& w = wps[i];
        char buf[128];
        const char* type_str = "rw";
        if (w.type == READ) type_str = "r";
        else if (w.type == WRITE) type_str = "w";
        snprintf(buf, sizeof(buf), " %zu:%04X+%u/%s",
                 i, static_cast<unsigned>(w.address),
                 static_cast<unsigned>(w.length), type_str);
        resp += buf;
        if (!w.condition_str.empty()) {
          resp += "[if ";
          resp += w.condition_str;
          resp += "]";
        }
        if (w.pass_count > 0) {
          resp += "[pass ";
          resp += std::to_string(w.pass_count);
          resp += "]";
        }
      }
      resp += "\n";
      return resp;
    }
    return "ERR 400 bad-wp-cmd (add|del|clear|list)\n";
  }

  // --- Symbol commands ---
  if (cmd == "sym" && parts.size() >= 2) {
    if (parts[1] == "load" && parts.size() >= 3) {
      Symfile loaded(parts[2]);
      int count = 0;
      for (const auto& [addr, name] : loaded.Symbols()) {
        g_symfile.addSymbol(addr, name);
        count++;
      }
      char buf[32];
      snprintf(buf, sizeof(buf), "OK loaded=%d\n", count);
      return std::string(buf);
    }
    if (parts[1] == "add" && parts.size() >= 4) {
      unsigned int addr = std::stoul(parts[2], nullptr, 0);
      g_symfile.addSymbol(static_cast<word>(addr), parts[3]);
      return "OK\n";
    }
    if (parts[1] == "del" && parts.size() >= 3) {
      g_symfile.delSymbol(parts[2]);
      return "OK\n";
    }
    if (parts[1] == "list") {
      std::string filter;
      if (parts.size() >= 3) filter = parts[2];
      auto syms = g_symfile.listSymbols(filter);
      std::string resp = "OK count=" + std::to_string(syms.size()) + "\n";
      for (const auto& [addr, name] : syms) {
        char buf[64];
        snprintf(buf, sizeof(buf), "  %04X %s\n",
                 static_cast<unsigned>(addr), name.c_str());
        resp += buf;
      }
      return resp;
    }
    if (parts[1] == "lookup" && parts.size() >= 3) {
      // Try as address first
      try {
        unsigned int addr = std::stoul(parts[2], nullptr, 0);
        std::string name = g_symfile.lookupAddr(static_cast<word>(addr));
        if (!name.empty()) {
          return "OK " + name + "\n";
        }
      } catch (...) {}
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

  // --- Memory search ---
  if (cmd == "mem" && parts.size() >= 5 && parts[1] == "find") {
    unsigned int start = std::stoul(parts[3], nullptr, 0);
    unsigned int end = std::stoul(parts[4], nullptr, 0);
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
      std::string resp = "OK";
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
          char buf[8];
          snprintf(buf, sizeof(buf), " %04X", addr);
          resp += buf;
          found++;
        }
      }
      resp += "\n";
      return resp;
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
      std::string resp = "OK";
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
          char buf[8];
          snprintf(buf, sizeof(buf), " %04X", addr);
          resp += buf;
          found++;
        }
      }
      resp += "\n";
      return resp;
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
      std::string resp = "OK";
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
          char buf[8];
          snprintf(buf, sizeof(buf), " %04X", addr);
          resp += buf;
          found++;
        }
        addr += line.Size();
      }
      resp += "\n";
      return resp;
    }
    return "ERR 400 bad-find-type (hex|text|asm)\n";
  }

  // --- Stack command ---
  if (cmd == "stack") {
    int depth = 16;
    if (parts.size() >= 2) depth = std::stoi(parts[1]);
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
    if (parts.size() < 2) return "ERR 400 missing subcommand (formats|format|new|ls|cat|get|put|rm|info)\n";
    if (parts[1] == "formats") {
      auto names = disk_format_names();
      std::string resp = "OK";
      for (const auto& n : names) resp += " " + n;
      resp += "\n";
      return resp;
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
    return "ERR 400 unknown disk subcommand\n";
  }

  // --- WAV audio recording ---
  if (cmd == "record" && parts.size() >= 2) {
    if (parts[1] == "wav") {
      if (parts.size() < 3) return "ERR 400 missing-action (start|stop|status)\n";
      if (parts[2] == "start") {
        if (parts.size() < 4) return "ERR 400 missing-path\n";
        static const unsigned int wav_rates[] = {11025, 22050, 44100, 48000, 96000};
        uint32_t rate = wav_rates[CPC.snd_playback_rate];
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
    return "ERR 400 bad-record-cmd (wav)\n";
  }

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
        game_idx = static_cast<size_t>(std::stoul(parts[2]));
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
          poke_idx = static_cast<size_t>(std::stoul(parts[3]));
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
        game_idx = static_cast<size_t>(std::stoul(parts[2]));
        poke_idx = static_cast<size_t>(std::stoul(parts[3]));
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
        addr = std::stoul(parts[2], nullptr, 16);
        val = std::stoul(parts[3]);
      } catch (const std::invalid_argument&) {
        return "ERR 400 bad-args (poke write <hex_addr> <value>)\n";
      } catch (const std::out_of_range&) {
        return "ERR 400 bad-args (poke write <hex_addr> <value>)\n";
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

  return "ERR 501 not-implemented\n";
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
  addr.sin_port = htons(kPort);

  if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
    closesocket(server_fd);
    WSACleanup();
    return;
  }

  if (listen(server_fd, 1) == SOCKET_ERROR) {
    closesocket(server_fd);
    WSACleanup();
    return;
  }

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

    std::string buffer;
    char buf[1024];
    int n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
      buf[n] = 0;
      buffer.append(buf);
      auto lines = split_lines(buffer);
      for (const auto& line : lines) {
        auto reply = handle_command(line);
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
  addr.sin_port = htons(kPort);

  if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(server_fd);
    return;
  }

  if (listen(server_fd, 1) < 0) {
    ::close(server_fd);
    return;
  }

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

    std::string buffer;
    char buf[1024];
    ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
    if (n > 0) {
      buf[n] = 0;
      buffer.append(buf);
      auto lines = split_lines(buffer);
      for (const auto& line : lines) {
        auto reply = handle_command(line);
        (void)write(client_fd, reply.c_str(), reply.size());
      }
    }
    ::close(client_fd);
  }

  ::close(server_fd);
}

#endif // _WIN32
