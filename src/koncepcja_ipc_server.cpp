#include "koncepcja_ipc_server.h"
#include "imgui_ui.h"

#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#include <cctype>
#include <sstream>
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
#include "z80_disassembly.h"
#include "slotshandler.h"
#include "keyboard.h"
#include "trace.h"
#include "gif_recorder.h"

extern t_z80regs z80;
extern t_CPC CPC;
extern SDL_Surface *back_surface;
extern byte *pbRAM;
extern byte keyboard_matrix[16];

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
  if (cmd == "help") return "OK commands: ping version help quit pause run reset load regs reg(set/get) mem(read/write) bp(list/add/del/clear) step wait hash(vram/mem/regs) screenshot snapshot(save/load) disasm devtools input(keydown/keyup/key/type/joy) trace(on/off/dump/on_crash/status) frames(dump) event(on/once/off/list)\n";
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
      uLong crc = crc32(0L, Z_NULL, 0);
      crc = crc32(crc, static_cast<const Bytef*>(back_surface->pixels),
                  static_cast<uInt>(back_surface->h * back_surface->pitch));
      snprintf(buf, sizeof(buf), "OK crc32=%08lX\n", crc);
      return std::string(buf);
    }
    if (parts[1] == "mem" && parts.size() >= 4) {
      unsigned int addr = std::stoul(parts[2], nullptr, 0);
      unsigned int len = std::stoul(parts[3], nullptr, 0);
      uLong crc = crc32(0L, Z_NULL, 0);
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
      uLong crc = crc32(0L, Z_NULL, 0);
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
    // mem read <addr> <len> [ascii]
    unsigned int addr = std::stoul(parts[2], nullptr, 0);
    unsigned int len = std::stoul(parts[3], nullptr, 0);
    bool with_ascii = (parts.size() >= 5 && parts[4] == "ascii");
    std::string resp = "OK ";
    std::string ascii;
    char bytebuf[4];
    for (unsigned int i = 0; i < len; i++) {
      byte v = z80_read_mem(static_cast<word>(addr + i));
      snprintf(bytebuf, sizeof(bytebuf), "%02X", v);
      resp += bytebuf;
      if (with_ascii) {
        char c = (v >= 32 && v <= 126) ? static_cast<char>(v) : '.';
        ascii.push_back(c);
        if ((i + 1) % 16 == 0) {
          resp += " |" + ascii + "| ";
          ascii.clear();
        }
      }
    }
    if (!ascii.empty()) {
      resp += " |" + ascii + "|";
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
  if (cmd == "disasm" && parts.size() >= 3) {
    unsigned int addr = std::stoul(parts[1], nullptr, 0);
    int count = std::stoi(parts[2]);
    if (count < 0) return "ERR 400 bad-args\n";
    std::ostringstream resp;
    resp << "OK\n";
    DisassembledCode code;
    std::vector<dword> entry_points;
    word pos = static_cast<word>(addr);
    for (int i = 0; i < count; i++) {
      auto line = disassemble_one(pos, code, entry_points);
      code.lines.insert(line);
      resp << line << "\n";
      pos = static_cast<word>(pos + line.Size());
    }
    return resp.str();
  }
  if (cmd == "bp" && parts.size() >= 2) {
    if (parts[1] == "add" && parts.size() >= 3) {
      unsigned int addr = std::stoul(parts[2], nullptr, 0);
      z80_add_breakpoint(static_cast<word>(addr));
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
      auto bps = z80_list_breakpoints();
      std::string resp = "OK count=" + std::to_string(bps.size());
      char buf[8];
      for (const auto& b : bps) {
        snprintf(buf, sizeof(buf), " %04X", static_cast<unsigned int>(b.address));
        resp += buf;
      }
      resp += "\n";
      return resp;
    }
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
      // Block until frame stepping completes
      while (g_ipc_instance->frame_step_active.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
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
        snprintf(fname, sizeof(fname), pattern.c_str(), i);
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
            char resp[64];
            snprintf(resp, sizeof(resp), "OK PC=%04X WATCH=%u\n", pc, watch ? 1 : 0);
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
  // Execute the IPC command internally (reuse handle_command from anonymous namespace)
  handle_command(cmd);
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
