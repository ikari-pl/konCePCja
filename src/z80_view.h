/* z80_view.h — the Z80 debug/view surface (Gate C Wave 1 —
 * replacement-ledger).
 *
 * The t_z80regs view struct the whole app reads (the bridge publishes
 * Device truth into it each frame), the breakpoint/watchpoint/
 * IO-breakpoint editing model, the debug hooks, the exit-condition codes
 * the main loop and the bridge share, and the tool-facing memory
 * accessors (machine peeks). All definitions live in z80_view.cpp; the
 * execution core is src/hw/z80 on the sub-cycle board. */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "SDL3/SDL.h"
#include "expr_parser.h"
#include "types.h"

// A pair of register really only needs a word (16 bits).
// So in practice, b.h2, b.h3 and w.h should never be used (there's an
// exception in psg.cpp which uses the same structure for other purposes).
// However, using 32 bits allow to easily implement addition: we can just do the
// addition and handle the overflow after. It also allow for magic values
// outside of the 16 bits range (e.g. deactivating z80.break_point with an
// unreachable address).
typedef union {
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
  struct {
    byte l, h, h2, h3;
  } b;
  struct {
    word l, h;
  } w;
#else
  struct {
    byte h3, h2, h, l;
  } b;
  struct {
    word h, l;
  } w;
#endif
  dword d;
} reg_pair;

#define Sflag 0x80   // sign flag
enum : std::uint8_t {
  Zflag = 0x40,   // zero flag
  Hflag = 0x10,   // halfcarry flag
  Pflag = 0x04,   // parity flag
  Vflag = 0x04,   // overflow flag
  Nflag = 0x02,   // negative flag
  Cflag = 0x01,   // carry flag
  Xflags = 0x28,  // bit 5 & 3 flags
  X1flag = 0x20,  // bit 5 - unused flag
  X2flag = 0x28   // bit 3 - unused flag
};

enum BreakpointType : std::uint8_t {
  NORMAL = 0,
  // Ephemeral breakpoint are removed next time the execution pauses.
  EPHEMERAL = 1,
};

struct Breakpoint {
  Breakpoint(word val, BreakpointType type = NORMAL)
      : address(val), type(type) {};

  dword address;
  BreakpointType type;
  std::unique_ptr<ExprNode> condition;  // nullptr = unconditional
  int pass_count = 0;                   // break only after this many hits
  int hit_count = 0;
  std::string condition_str;  // original condition text for display
};

enum WatchpointType : std::uint8_t {
  READ = 1,
  WRITE = 2,
  READWRITE = 3,
};

struct Watchpoint {
  Watchpoint(word val, WatchpointType t) : address(val), type(t) {};
  dword address;
  word length = 1;  // range: addr..addr+length-1
  WatchpointType type;
  std::unique_ptr<ExprNode> condition;  // nullptr = unconditional
  std::string condition_str;
  int pass_count = 0;
  int hit_count = 0;
};

enum IOBreakpointDir : std::uint8_t { IO_IN = 1, IO_OUT = 2, IO_BOTH = 3 };

struct IOBreakpoint {
  word port;
  word mask;
  IOBreakpointDir dir;
  std::unique_ptr<ExprNode> condition;
  std::string condition_str;
};

class t_z80regs {
 public:
  t_z80regs() {
    AF.d = 0;
    BC.d = 0;
    DE.d = 0;
    HL.d = 0;
    PC.d = 0;
    SP.d = 0;
    AFx.d = 0;
    BCx.d = 0;
    DEx.d = 0;
    HLx.d = 0;
    IX.d = 0;
    IY.d = 0;
    I = 0;
    R = 0;
    Rb7 = 0;
    IFF1 = 0;
    IFF2 = 0;
    IM = 0;
    HALT = 0;
    EI_issued = 0;
    int_pending = 0;
    watchpoint_reached = 0;
    breakpoint_reached = 0;
    watchpoint_addr = 0;
    watchpoint_value = 0;
    watchpoint_old = 0;
    step_in = 0;
    step_out = 0;
    break_point = 0;
    trace = 0;
  };

  reg_pair AF, BC, DE, HL, PC, SP, AFx, BCx, DEx, HLx, IX, IY;
  byte I, R, Rb7, IFF1, IFF2, IM, HALT, EI_issued, int_pending;
  byte watchpoint_reached;
  byte breakpoint_reached;
  word watchpoint_addr;   // address that triggered
  byte watchpoint_value;  // value being read/written
  byte watchpoint_old;    // previous value at address
  byte step_in;
  byte step_out;
  std::vector<word> step_out_addresses;
  dword break_point, trace;
};

inline constexpr dword Z80_BREAKPOINT_NONE = 0xFFFFFFFF;

// IPC signal to break out of the running frame (separate from t_z80regs to
// keep it copyable)
extern std::atomic<bool> z80_stop_requested;

enum : std::uint8_t {
  EC_BREAKPOINT = 10,
  EC_TRACE = 20,
  EC_FRAME_COMPLETE = 30,
  EC_CYCLE_COUNT = 40,
  EC_SOUND_BUFFER = 50,
  EC_STOP_REQUESTED = 60
};

// Direct memory access used by IPC and tools:
// - z80_read_mem / z80_write_mem: SmartWatch on read, raw bus on write (no
// watchpoints).
byte z80_read_mem(word addr);
void z80_write_mem(word addr, byte val);
byte z80_read_mem_via_write_bank(word addr);
byte z80_read_mem_raw_bank(word addr, int bank);

// CPU-view accessors (SmartWatch/MF2/ASIC + bus, NO watchpoints).
// Safe for IPC and UI — won't trigger watchpoint hits or IPC re-entrancy.
byte z80_cpu_read_mem(word addr);
void z80_cpu_write_mem(word addr, byte val);

// Multiface II stop button (the hw/mf2 Device, via the bridge).
void z80_mf2stop();

// konCePCja debug helpers
void z80_add_breakpoint(word addr);
void z80_add_breakpoint_cond(word addr, std::unique_ptr<ExprNode> condition,
                             const std::string& cond_str, int pass_count = 0);
void z80_del_breakpoint(word addr);
void z80_clear_breakpoints();
void z80_step_instruction();
const std::vector<Breakpoint>& z80_list_breakpoints_ref();

// Shared fire predicates (condition eval + hit/pass-count bookkeeping):
// the probe post-filters below funnel through these, so conditional and
// pass-count semantics have exactly one definition.
bool z80_bp_should_fire(Breakpoint& b, word pc);
bool z80_wp_should_fire(Watchpoint& w, word addr, byte val, byte old_val,
                        bool is_write);

// engine=1 probe post-filter: apply conditional/pass-count semantics that the
// hardware probe comparators do not model. `old_val` is the pre-access byte,
// peeked by the bridge while the machine is parked on the hit.
bool z80_probe_exec_should_break(uint16_t pc);
bool z80_probe_watch_should_break(uint16_t addr, uint8_t data, bool is_write,
                                  uint8_t old_val);
void z80_remove_ephemeral_breakpoints();

// Watchpoints
void z80_add_watchpoint(word addr, word len, WatchpointType type);
void z80_add_watchpoint_cond(word addr, word len, WatchpointType type,
                             std::unique_ptr<ExprNode> cond,
                             const std::string& cond_str, int pass_count = 0);
void z80_del_watchpoint(int index);
void z80_clear_watchpoints();
const std::vector<Watchpoint>& z80_list_watchpoints_ref();

// Ephemeral breakpoints (removed when execution next pauses)
void z80_add_breakpoint_ephemeral(word addr);

// IO breakpoints
bool z80_check_io_breakpoint(word port, IOBreakpointDir access, byte val = 0);
void z80_add_io_breakpoint(word port, word mask, IOBreakpointDir dir);
void z80_add_io_breakpoint_cond(word port, word mask, IOBreakpointDir dir,
                                std::unique_ptr<ExprNode> condition,
                                const std::string& cond_str);
void z80_del_io_breakpoint(int index);
void z80_clear_io_breakpoints();
const std::vector<IOBreakpoint>& z80_list_io_breakpoints_ref();

// Global T-state counter for debug timers
extern uint64_t g_tstate_counter;

// Breakpoint hit notification hook (konCePCja IPC)
typedef void (*BreakpointHitHook)(word pc, bool watchpoint);
void z80_set_breakpoint_hit_hook(BreakpointHitHook hook);
/* Invoke the installed hook (Wave-1 shim: the sub-cycle bridge reports probe
 * hits through the same channel). */
void z80_call_breakpoint_hit_hook(word pc, bool watchpoint);

// TXT_OUTPUT hook — fires when PC hits the given address, passing the A
// register. Used by the telnet console to mirror CPC text output.
using TxtOutputHook = void (*)(uint8_t ch);
void z80_set_txt_output_hook(TxtOutputHook hook, uint16_t address);

// CP/M BDOS hook — fires when PC == 0x0005 and C == 2 (C_WRITE), passing E
// register. Used by the telnet console to capture CP/M console output.
void z80_set_bdos_output_hook(TxtOutputHook hook);
/* Read back the installed hooks (Wave-1: the sub-cycle bridge re-registers
 * them as machine taps). */
TxtOutputHook z80_get_txt_output_hook(uint16_t* address);
TxtOutputHook z80_get_bdos_output_hook();

/* Fire the TXT_OUTPUT / BDOS C_WRITE console mirrors for one retired
 * instruction — the shared service site for both engines (legacy loop and
 * the engine=1 bridge instr hook). */
void z80_service_output_hooks(word pc, byte a, byte c, byte e);

// CP/M BDOS serial hooks — fired at PC == 0x0005.
//   Out: C == 5 (L_WRITE), called with byte in E.  BDOS call proceeds normally
//   after. In:  C == 3 (A_READ).  Called with no args; return value placed in
//   A, BDOS call
//        is suppressed (RET simulated).  Avoids blocking in BIOS AUXIN on plain
//        CPC.
using BdosSerialOutHook = std::function<void(uint8_t)>;
using BdosSerialInHook = std::function<uint8_t()>;
void z80_set_bdos_serial_out_hook(BdosSerialOutHook hook);
void z80_set_bdos_serial_in_hook(BdosSerialInHook hook);
