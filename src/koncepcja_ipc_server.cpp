#include "koncepcja_ipc_server.h"
#include "imgui_ui.h"

#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#include <cctype>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

#include "cap32.h"
#include "z80.h"
#include "z80_disassembly.h"
#include "slotshandler.h"

extern t_z80regs z80;
extern t_CPC CPC;

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
  if (cmd == "help") return "OK commands: ping version help pause run reset load regs reg set/get mem bp(list/add/del/clear) step wait screenshot snapshot(save/load) disasm devtools\n";

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
    cpc_pause();
    int count = 1;
    if (parts.size() >= 2) count = std::stoi(parts[1]);
    for (int i = 0; i < count; i++) z80_step_instruction();
    return "OK\n";
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
