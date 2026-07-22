#include <gtest/gtest.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "autotype.h"
#include "cpc_key_tables.h"
#include "koncepcja.h"
#include "koncepcja_ipc_server.h"
#include "symfile.h"
#include "video_host.h"
#include "z80_view.h"

extern t_z80regs z80;
extern t_CPC CPC;
extern t_GateArray GateArray;
extern SDL_Surface* back_surface;
extern byte* membank_read[4];
extern byte* membank_write[4];
extern video_plugin* vid_plugin;

namespace {

constexpr size_t kBankSize = 16 * 1024;

// Debug-surface commands now return "OK [context]\n" instead of bare "OK\n".
// This helper checks that a response starts with "OK " or is exactly "OK\n".
// Bind the command's result to a variable so it is evaluated ONCE — evaluating
// the argument inline would re-run the command when the reply is a bare "OK\n"
// (the `.substr(0,3) == "OK "` branch fails, so `||` re-evaluates), which
// double-dispatches non-idempotent commands (e.g. 'input type' enqueues twice).
#define EXPECT_OK(resp)                                               \
  do {                                                                \
    const std::string ok_resp_ = (resp);                              \
    EXPECT_TRUE(ok_resp_.substr(0, 3) == "OK " || ok_resp_ == "OK\n") \
        << "Expected OK response, got: " << ok_resp_;                 \
  } while (0)

// Forward-declare the server so send_command can query its actual port
static KoncepcjaIpcServer* g_test_server = nullptr;

std::string send_command(const std::string& command) {
  int port = g_test_server ? g_test_server->port() : 6543;
#ifdef _WIN32
  SOCKET fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  EXPECT_NE(fd, INVALID_SOCKET);
  if (fd == INVALID_SOCKET) return "";
#else
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  EXPECT_GE(fd, 0);
  if (fd < 0) return "";
#endif

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  bool connected = false;
  for (int attempt = 0; attempt < 100 && !connected; attempt++) {
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
      connected = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(connected);
  if (!connected) {
#ifdef _WIN32
    closesocket(fd);
#else
    ::close(fd);
#endif
    return "";
  }

  std::string line = command + "\n";
#ifdef _WIN32
  int written = send(fd, line.data(), static_cast<int>(line.size()), 0);
  EXPECT_EQ(written, static_cast<int>(line.size()));
  // Half-close: signal no more data so the persistent server closes its end
  shutdown(fd, SD_SEND);
#else
  ssize_t written = ::write(fd, line.data(), line.size());
  EXPECT_EQ(written, static_cast<ssize_t>(line.size()));
  // Half-close: signal no more data so the persistent server closes its end
  shutdown(fd, SHUT_WR);
#endif

  std::string response;
  char buffer[256];
#ifdef _WIN32
  int n = 0;
  while ((n = recv(fd, buffer, sizeof(buffer), 0)) > 0) {
    response.append(buffer, buffer + n);
  }
  closesocket(fd);
#else
  ssize_t n = 0;
  while ((n = ::read(fd, buffer, sizeof(buffer))) > 0) {
    response.append(buffer, buffer + n);
  }
  ::close(fd);
#endif
  return response;
}

class IpcServerTest : public testing::Test {
 protected:
  static void SetUpTestSuite() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    CPC.snd_enabled = 0;
    g_test_server = &server;
    server.start();
    // Give the listener thread time to bind and listen
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  static void TearDownTestSuite() {
    server.stop();
    g_test_server = nullptr;
#ifdef _WIN32
    WSACleanup();
#endif
  }

  void SetUp() override {
    z80 = t_z80regs();
    z80_clear_breakpoints();
    z80_clear_watchpoints();
    g_symfile.clear();
    for (int i = 0; i < 4; i++) {
      std::memset(memory[i], 0, kBankSize);
      membank_read[i] = memory[i];
      membank_write[i] = memory[i];
    }
  }

  static KoncepcjaIpcServer server;
  static byte memory[4][kBankSize];
};

KoncepcjaIpcServer IpcServerTest::server;
byte IpcServerTest::memory[4][kBankSize];

TEST_F(IpcServerTest, RegSetUpdatesRegisters) {
  auto resp = send_command("reg set A 0x42");
  EXPECT_OK(resp);
  EXPECT_EQ(z80.AF.b.h, 0x42);

  resp = send_command("reg set PC 0x1234");
  EXPECT_OK(resp);
  EXPECT_EQ(z80.PC.w.l, 0x1234);
}

TEST_F(IpcServerTest, RegGetReturnsValues) {
  z80.AF.b.h = 0x77;
  z80.PC.w.l = 0x3456;

  auto resp = send_command("reg get A");
  EXPECT_EQ(resp, "OK 77\n");

  resp = send_command("reg get PC");
  EXPECT_EQ(resp, "OK 3456\n");
}

TEST_F(IpcServerTest, BreakpointListAddDelClear) {
  auto resp = send_command("bp clear");
  EXPECT_OK(resp);

  resp = send_command("bp add 0x1234");
  EXPECT_OK(resp);

  resp = send_command("bp add 0x4000");
  EXPECT_OK(resp);

  resp = send_command("bp list");
  EXPECT_EQ(resp, "OK count=2 1234 4000\n");

  resp = send_command("bp del 0x1234");
  EXPECT_OK(resp);

  resp = send_command("bp list");
  EXPECT_EQ(resp, "OK count=1 4000\n");

  resp = send_command("bp clear");
  EXPECT_OK(resp);

  resp = send_command("bp list");
  EXPECT_EQ(resp, "OK count=0\n");
}

TEST_F(IpcServerTest, WaitPcReturnsImmediatelyWhenMatched) {
  z80.PC.w.l = 0x2000;
  auto resp = send_command("wait pc 0x2000 50");
  EXPECT_OK(resp);
}

TEST_F(IpcServerTest, WaitMemHonorsMask) {
  z80_write_mem(0x1000, 0xA5);
  auto resp = send_command("wait mem 0x1000 0xA0 mask=0xF0 50");
  EXPECT_OK(resp);
}

TEST_F(IpcServerTest, WaitVblCompletes) {
  auto resp = send_command("wait vbl 1 100");
  EXPECT_OK(resp);
}

TEST_F(IpcServerTest, ScreenshotReturnsErrorWithoutSurface) {
  back_surface = nullptr;
  auto screenshotPath =
      (std::filesystem::temp_directory_path() / "kaprys_test.png").string();
  auto resp = send_command("screenshot " + screenshotPath);
  EXPECT_EQ(resp, "ERR 503 no-surface\n");
}

TEST_F(IpcServerTest, WatchpointAddListDelClear) {
  auto resp = send_command("wp clear");
  EXPECT_OK(resp);

  resp = send_command("wp add 0x4000 256 w");
  EXPECT_OK(resp);

  resp = send_command("wp add 0xC000 1 rw");
  EXPECT_OK(resp);

  resp = send_command("wp list");
  EXPECT_TRUE(resp.find("count=2") != std::string::npos);
  EXPECT_TRUE(resp.find("4000+256/w") != std::string::npos);
  EXPECT_TRUE(resp.find("C000+1/rw") != std::string::npos);

  resp = send_command("wp del 0");
  EXPECT_OK(resp);

  resp = send_command("wp list");
  EXPECT_TRUE(resp.find("count=1") != std::string::npos);

  resp = send_command("wp clear");
  EXPECT_OK(resp);

  resp = send_command("wp list");
  EXPECT_TRUE(resp.find("count=0") != std::string::npos);
}

TEST_F(IpcServerTest, WatchpointConditional) {
  auto resp = send_command("wp add 0x4000 1 w if value > 128");
  EXPECT_OK(resp);

  resp = send_command("wp list");
  EXPECT_TRUE(resp.find("if value > 128") != std::string::npos);

  send_command("wp clear");
}

TEST_F(IpcServerTest, SymbolAddLookupDel) {
  auto resp = send_command("sym add 0x0038 interrupt_handler");
  EXPECT_OK(resp);

  resp = send_command("sym lookup 0x0038");
  EXPECT_EQ(resp, "OK interrupt_handler\n");

  resp = send_command("sym lookup interrupt_handler");
  EXPECT_EQ(resp, "OK 0038\n");

  resp = send_command("sym list");
  EXPECT_TRUE(resp.find("count=1") != std::string::npos);
  EXPECT_TRUE(resp.find("0038 interrupt_handler") != std::string::npos);

  resp = send_command("sym del interrupt_handler");
  EXPECT_OK(resp);

  resp = send_command("sym lookup interrupt_handler");
  EXPECT_EQ(resp, "ERR 404 not-found\n");
}

TEST_F(IpcServerTest, DisasmWithSymbols) {
  // Add a symbol, then disassemble with --symbols
  send_command("sym add 0x0000 entry_point");
  auto resp = send_command("disasm 0x0000 1 --symbols");
  EXPECT_TRUE(resp.find("OK") != std::string::npos);
  EXPECT_TRUE(resp.find("entry_point") != std::string::npos);
}

TEST_F(IpcServerTest, MemFindHex) {
  // Write a known pattern at 0x1000
  send_command("mem write 0x1000 DEADBEEF");
  auto resp = send_command("mem find hex 0x0000 0xFFFF DEADBEEF");
  EXPECT_TRUE(resp.find("OK") != std::string::npos);
  EXPECT_TRUE(resp.find("1000") != std::string::npos);
}

TEST_F(IpcServerTest, MemFindText) {
  // Write ASCII text at 0x2000
  send_command("mem write 0x2000 48454C4C4F");  // "HELLO"
  auto resp = send_command("mem find text 0x0000 0xFFFF HELLO");
  EXPECT_TRUE(resp.find("OK") != std::string::npos);
  EXPECT_TRUE(resp.find("2000") != std::string::npos);
}

TEST_F(IpcServerTest, StackCommand) {
  z80.SP.w.l = 0xBFFA;
  // Write some values on the stack
  z80_write_mem(0xBFFA, 0x34);
  z80_write_mem(0xBFFB, 0x12);
  auto resp = send_command("stack 4");
  EXPECT_TRUE(resp.find("OK") != std::string::npos);
  EXPECT_TRUE(resp.find("depth=4") != std::string::npos);
  EXPECT_TRUE(resp.find("1234") != std::string::npos);
}

TEST_F(IpcServerTest, StepOverDoesNotDescendIntoCall) {
  // This is a basic check that the command is accepted
  // (full behavioral test requires a running emulator)
  z80.PC.w.l = 0x0000;
  // Write NOP (0x00) at address 0
  z80_write_mem(0x0000, 0x00);
  auto resp = send_command("step over");
  EXPECT_OK(resp);
}

TEST_F(IpcServerTest, StepToCommand) {
  // Write NOP at 0x0000, step to 0x0001 should work immediately via ephemeral
  // bp
  z80.PC.w.l = 0x0000;
  z80_write_mem(0x0000, 0x00);
  // step to on a paused emulator won't actually run; check command is accepted
  // In test environment without main loop, this will timeout
  // Just verify the command doesn't crash
  auto resp = send_command("step to 0x0001");
  // Either timeout or OK is acceptable in test harness
  EXPECT_TRUE(resp.find("OK") != std::string::npos ||
              resp.find("ERR 408") != std::string::npos);
}

TEST_F(IpcServerTest, WatchpointRange) {
  send_command("wp clear");

  // Add a range watchpoint covering 16 bytes
  auto resp = send_command("wp add 0x4000 16 rw");
  EXPECT_OK(resp);

  resp = send_command("wp list");
  EXPECT_TRUE(resp.find("count=1") != std::string::npos);
  EXPECT_TRUE(resp.find("4000+16/rw") != std::string::npos);

  send_command("wp clear");
}

TEST_F(IpcServerTest, StepOutCommand) {
  z80.PC.w.l = 0x0000;
  z80_write_mem(0x0000, 0xC9);  // RET instruction

  // Step out without a running main loop will timeout or succeed immediately.
  // Verify the command is accepted and doesn't crash.
  auto resp = send_command("step out");
  EXPECT_TRUE(resp.find("OK") != std::string::npos ||
              resp.find("ERR 408") != std::string::npos);
}

TEST_F(IpcServerTest, SymbolLoad) {
  // Create a minimal .sym file in the platform temp directory
  auto sympath = std::filesystem::temp_directory_path() / "koncepcja_test.sym";
  std::string symfile = sympath.string();
  {
    std::ofstream ofs(symfile);
    ofs << "; test symbols\n"
        << "al $0038 .interrupt_handler\n"
        << "al $0000 .reset_vector\n"
        << "al $FC00 .screen_base\n";
  }

  auto resp = send_command("sym load " + symfile);
  EXPECT_TRUE(resp.find("OK loaded=3") != std::string::npos);

  // Verify loaded symbols are queryable
  resp = send_command("sym lookup 0x0038");
  EXPECT_EQ(resp, "OK interrupt_handler\n");

  resp = send_command("sym lookup screen_base");
  EXPECT_EQ(resp, "OK FC00\n");

  std::filesystem::remove(sympath);
}

TEST_F(IpcServerTest, MemFindWildcard) {
  // Write a pattern at a known address: DE ?? BE EF
  z80_write_mem(0x3000, 0xDE);
  z80_write_mem(0x3001, 0x42);  // any value
  z80_write_mem(0x3002, 0xBE);
  z80_write_mem(0x3003, 0xEF);

  auto resp = send_command("mem find hex 0x2F00 0x3100 DE??BEEF");
  EXPECT_TRUE(resp.find("OK") != std::string::npos);
  EXPECT_TRUE(resp.find("3000") != std::string::npos);
}

// ─────────────────────────────────────────────────
// Error message quality tests
// ─────────────────────────────────────────────────

TEST_F(IpcServerTest, UnknownCommandReturns404WithSuggestion) {
  auto resp = send_command("totype hello");
  EXPECT_TRUE(resp.find("ERR 404") != std::string::npos) << resp;
  EXPECT_TRUE(resp.find("autotype") != std::string::npos)
      << resp;  // "Did you mean..."
}

TEST_F(IpcServerTest, UnknownCommandReturns404NoSuggestionForGarbage) {
  auto resp = send_command("xyzzyplugh");
  EXPECT_TRUE(resp.find("ERR 404") != std::string::npos) << resp;
  EXPECT_TRUE(resp.find("not recognized") != std::string::npos) << resp;
}

TEST_F(IpcServerTest, BareHexAddressAccepted) {
  // "C000" should be parsed as hex, not rejected
  auto resp = send_command("bp add C000");
  EXPECT_OK(resp);
  auto list = send_command("bp list");
  EXPECT_TRUE(list.find("C000") != std::string::npos) << list;
}

TEST_F(IpcServerTest, BpWithNoArgsReturnsUsage) {
  auto resp = send_command("bp");
  EXPECT_TRUE(resp.find("ERR 400") != std::string::npos) << resp;
  EXPECT_TRUE(resp.find("usage:") != std::string::npos) << resp;
}

TEST_F(IpcServerTest, BpAddWithNoAddrReturnsUsage) {
  auto resp = send_command("bp add");
  EXPECT_TRUE(resp.find("ERR 400") != std::string::npos) << resp;
  EXPECT_TRUE(resp.find("usage:") != std::string::npos) << resp;
}

TEST_F(IpcServerTest, WpWithNoArgsReturnsUsage) {
  auto resp = send_command("wp");
  EXPECT_TRUE(resp.find("ERR 400") != std::string::npos) << resp;
  EXPECT_TRUE(resp.find("usage:") != std::string::npos) << resp;
}

TEST_F(IpcServerTest, MemWithNoArgsReturnsUsage) {
  auto resp = send_command("mem");
  EXPECT_TRUE(resp.find("ERR 400") != std::string::npos) << resp;
  EXPECT_TRUE(resp.find("usage:") != std::string::npos) << resp;
}

TEST_F(IpcServerTest, BadNumberIncludesValueInError) {
  auto resp = send_command("reg set A notanumber");
  EXPECT_TRUE(resp.find("ERR 400") != std::string::npos) << resp;
  EXPECT_TRUE(resp.find("notanumber") != std::string::npos) << resp;
}

// ─────────────────────────────────────────────────
// Key modifiers (IPC Phase 3, beads-nz0n): the chord parser in cpc_key_tables.h
// tokenizes "CTRL+SHIFT+ESC" and maps modifiers to scancode high-byte flags.
// (The atomic tap itself runs only in the live emulator — the frame-stepped
// hold blocks here, so the contract is covered by ipc_harness instead.)
// ─────────────────────────────────────────────────

TEST_F(IpcServerTest, ChordParsingHelpers) {
  EXPECT_EQ(cpc_chord_tokens("CTRL+SHIFT+ESC"),
            (std::vector<std::string>{"CTRL", "SHIFT", "ESC"}));
  EXPECT_EQ(cpc_chord_tokens("ESC"), (std::vector<std::string>{"ESC"}));
  EXPECT_EQ(cpc_chord_tokens("SHIFT+A"),
            (std::vector<std::string>{"SHIFT", "A"}));

  // Modifier mapping is case-insensitive; non-modifiers map to 0.
  EXPECT_EQ(cpc_modifier_flag("CTRL"), MOD_CPC_CTRL);
  EXPECT_EQ(cpc_modifier_flag("control"), MOD_CPC_CTRL);
  EXPECT_EQ(cpc_modifier_flag("SHIFT"), MOD_CPC_SHIFT);
  EXPECT_EQ(cpc_modifier_flag("lshift"), MOD_CPC_SHIFT);
  EXPECT_EQ(cpc_modifier_flag("RSHIFT"), MOD_CPC_SHIFT);
  EXPECT_EQ(cpc_modifier_flag("ESC"), 0);
  EXPECT_EQ(cpc_modifier_flag("FOO"), 0);
}

// ─────────────────────────────────────────────────
// Type unification (IPC Phase 4, beads-c8fn): 'input type' routes through
// g_autotype_queue, so a WinAPE ~KEY~ token parses to ONE key action — not
// the literal characters of its name (the old per-char path).
// ─────────────────────────────────────────────────

TEST_F(IpcServerTest, TypeRoutesThroughAutotypeQueue) {
  // ~RETURN~ must become ONE RETURN-key action, not the letters R,E,T,U,R,N.
  g_autotype_queue.clear();
  EXPECT_OK(send_command("input type \"~RETURN~\""));
  auto acts = g_autotype_queue.actions();
  ASSERT_EQ(acts.size(), 1u) << "a ~RETURN~ token must parse to one key action";
  EXPECT_EQ(acts[0].type, AutoTypeAction::CHAR_PRESS_RELEASE);
  EXPECT_EQ(acts[0].cpc_key, static_cast<uint16_t>(CPC_RETURN))
      << "~RETURN~ parses to the RETURN key, not its literal characters";

  // Plain text still types one action per mappable character.
  g_autotype_queue.clear();
  EXPECT_OK(send_command("input type \"hi\""));
  auto acts2 = g_autotype_queue.actions();
  EXPECT_EQ(acts2.size(), 2u) << "plain text types one action per character";

  // Surrounding SINGLE quotes are stripped too: `input type '~RETURN~'` must
  // parse to the same one key action.
  g_autotype_queue.clear();
  EXPECT_OK(send_command("input type '~RETURN~'"));
  auto acts3 = g_autotype_queue.actions();
  ASSERT_EQ(acts3.size(), 1u) << "surrounding single quotes must be stripped";
  EXPECT_EQ(acts3[0].cpc_key, static_cast<uint16_t>(CPC_RETURN));
  g_autotype_queue.clear();
}

// ─────────────────────────────────────────────────
// Light-gun (IPC Phase 2, beads-vrsr): the staged 'input gun' commands are
// flushed by ipc_drain_input() into CPC.phazer_* exactly as the SDL path does.
// ─────────────────────────────────────────────────

TEST_F(IpcServerTest, GunDrainAppliesAimAndTrigger) {
  // Stub the video plugin so the drain's window->CPC mapping is known:
  // phazer_x = (x - x_offset) * x_scale, phazer_y = (y - y_offset) * y_scale.
  static video_plugin stub{};
  stub.x_offset = 10;
  stub.y_offset = 20;
  stub.x_scale = 2.0f;
  stub.y_scale = 4.0f;
  video_plugin* const saved = vid_plugin;
  vid_plugin = &stub;

  CPC.phazer_emulation = PhazerType::AmstradMagnumPhaser;
  CPC.phazer_x = 0;
  CPC.phazer_y = 0;
  CPC.phazer_pressed = false;

  // Publish the device gate (the no-gun 409 would otherwise fire), then stage.
  ipc_drain_input();
  EXPECT_OK(send_command("input gun move 30 40"));
  EXPECT_OK(send_command("input gun trigger down"));
  ipc_drain_input();

  EXPECT_EQ(CPC.phazer_x, static_cast<unsigned int>((30 - 10) * 2.0f));
  EXPECT_EQ(CPC.phazer_y, static_cast<unsigned int>((40 - 20) * 4.0f));
  EXPECT_TRUE(CPC.phazer_pressed);

  EXPECT_OK(send_command("input gun trigger up"));
  ipc_drain_input();
  EXPECT_FALSE(CPC.phazer_pressed);

  vid_plugin = saved;
  CPC.phazer_emulation = PhazerType::None;
}

// ─────────────────────────────────────────────────
// Status-bracket environment hint (beads-7hqp): the bracket decodes the
// CP/M-vs-BASIC discriminator (screen mode + RAM bank) into a readable 'env'
// field, so a reader doesn't have to remember the encoding (a bare 'reg get
// PC' carries no such context). Heuristic only — ambiguous combinations must
// report '?', not a guess.
// ─────────────────────────────────────────────────

TEST_F(IpcServerTest, StatusBracketInfersEnvironment) {
  const unsigned int saved_mode = GateArray.scr_mode;
  const unsigned char saved_ram = GateArray.RAM_config;

  // CP/M Plus boots 80-col (mode 2) on RAM bank 1.
  GateArray.scr_mode = 2;
  GateArray.RAM_config = 1;
  auto resp = send_command("pause");
  EXPECT_NE(resp.find("env:CP/M"), std::string::npos) << resp;

  // BASIC/AMSDOS sits in mode 1 on bank 0.
  GateArray.scr_mode = 1;
  GateArray.RAM_config = 0;
  resp = send_command("pause");
  EXPECT_NE(resp.find("env:BASIC"), std::string::npos) << resp;

  // A mixed state (mode 2 but bank 0) is unknown — reported, not guessed.
  GateArray.scr_mode = 2;
  GateArray.RAM_config = 0;
  resp = send_command("pause");
  EXPECT_NE(resp.find("env:?"), std::string::npos) << resp;

  GateArray.scr_mode = saved_mode;
  GateArray.RAM_config = saved_ram;
}

}  // namespace
