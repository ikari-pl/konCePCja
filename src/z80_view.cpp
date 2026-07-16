/* z80_view.cpp — the Z80 debug/view surface (Gate C Wave 1 —
 * replacement-ledger).
 *
 * The t_z80regs view struct the whole app reads (the bridge publishes Device
 * truth into it each frame), the breakpoint/watchpoint/IO-breakpoint editing
 * lists (the probe mirror's editing model), the debug hooks, the global
 * T-state counter, and the tool-facing memory accessors — all backed by the
 * sub-cycle machine's peeks. */

#include "z80_view.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "hw/memory.h"
#include "koncepcja.h"
#include "memory_bus.h"
#include "subcycle/machine.h"
#include "subcycle_bridge.h"

extern t_CRTC CRTC;
extern t_GateArray GateArray;
extern t_PSG PSG;

extern t_z80regs z80;
t_z80regs z80;
std::atomic<bool> z80_stop_requested{false};
// NOLINTNEXTLINE(misc-use-internal-linkage): breakpoints is referenced cross-TU
// (kon_cpc_ja.cpp loadBreakpoints); per-file check can't see the other TU
std::vector<Breakpoint> breakpoints;
namespace {
std::vector<Watchpoint> watchpoints;
}  // namespace
uint64_t g_tstate_counter = 0;

// Consulted by the legacy execution loop (extern there) until S3.
namespace {
BreakpointHitHook g_breakpoint_hit_hook = nullptr;
}  // namespace
namespace {
TxtOutputHook g_txt_output_hook = nullptr;
}  // namespace
namespace {
uint16_t g_txt_output_hook_addr = 0;
}  // namespace
namespace {
TxtOutputHook g_bdos_output_hook = nullptr;
}  // namespace
namespace {
BdosSerialOutHook g_bdos_serial_out_hook;
}  // namespace
namespace {
BdosSerialInHook g_bdos_serial_in_hook;
}  // namespace

// One firing site for the console output mirrors (telnet TXT_OUTPUT and the
// CP/M BDOS C_WRITE capture): the legacy execution loop and the engine=1
// bridge instr hook both call this, so the PC/register semantics live here
// only and cannot drift between the engines.
void z80_service_output_hooks(word pc, byte a, byte c, byte e) {
  if (g_txt_output_hook && pc == g_txt_output_hook_addr) g_txt_output_hook(a);
  if (g_bdos_output_hook && pc == 0x0005 && c == 2) g_bdos_output_hook(e);
}

void z80_set_breakpoint_hit_hook(BreakpointHitHook hook) {
  g_breakpoint_hit_hook = hook;
}

void z80_call_breakpoint_hit_hook(word pc, bool watchpoint) {
  if (g_breakpoint_hit_hook) g_breakpoint_hit_hook(pc, watchpoint);
}

TxtOutputHook z80_get_txt_output_hook(uint16_t* address) {
  if (address != nullptr) *address = g_txt_output_hook_addr;
  return g_txt_output_hook;
}

TxtOutputHook z80_get_bdos_output_hook() { return g_bdos_output_hook; }

void z80_set_txt_output_hook(TxtOutputHook hook, uint16_t address) {
  g_txt_output_hook = hook;
  g_txt_output_hook_addr = address;
}

void z80_set_bdos_output_hook(TxtOutputHook hook) { g_bdos_output_hook = hook; }

void z80_set_bdos_serial_out_hook(BdosSerialOutHook hook) {
  g_bdos_serial_out_hook = hook;
}
void z80_set_bdos_serial_in_hook(BdosSerialInHook hook) {
  g_bdos_serial_in_hook = hook;
}

// --- konCePCja debug helpers ---
void z80_add_breakpoint(word addr) {
  if (!std::any_of(breakpoints.begin(), breakpoints.end(), [&](const auto& b) {
        return b.address == addr && !b.condition;
      })) {
    breakpoints.emplace_back(addr, NORMAL);
  }
}

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other
// translation units/tests; internal linkage would break the link
void z80_add_breakpoint_cond(word addr, std::unique_ptr<ExprNode> condition,
                             const std::string& cond_str, int pass_count) {
  Breakpoint bp(addr, NORMAL);
  bp.condition = std::move(condition);
  bp.condition_str = cond_str;
  bp.pass_count = pass_count;
  breakpoints.push_back(std::move(bp));
}

void z80_del_breakpoint(word addr) {
  breakpoints.erase(
      std::remove_if(breakpoints.begin(), breakpoints.end(),
                     [&](const auto& b) { return b.address == addr; }),
      breakpoints.end());
}

void z80_clear_breakpoints() { breakpoints.clear(); }

void z80_step_instruction() {
  if (subcycle::Machine* m = subcycle_bridge_machine()) {
    m->step_instruction();  // probe-blind: never re-trips the halt
    subcycle_bridge_sync_regs_view();
  }
}

// ---- Tool-facing memory accessors (IPC, DevTools, expr parser) ----------
// All of these read/write the machine's CPU-visible view via peeks — no
// watchpoint hits, no IPC re-entrancy. When the machine is not up (unit
// tests, early startup) they fall back to the host bank tables so tools
// that plant bytes in membank_read/pbRAM keep working.

extern byte* pbRAM;

byte z80_read_mem(word addr) {
  if (subcycle::Machine const* m = subcycle_bridge_machine())
    return m->peek_mem(addr);
  return g_memory_bus.read_raw(addr);
}

void z80_write_mem(word addr, byte val) {
  if (subcycle::Machine* m = subcycle_bridge_machine()) {
    m->poke_mem(addr, val);
    return;
  }
  g_memory_bus.write_raw(addr, val);
}

byte z80_cpu_read_mem(word addr) { return z80_read_mem(addr); }

void z80_cpu_write_mem(word addr, byte val) { z80_write_mem(addr, val); }

byte z80_read_mem_via_write_bank(word addr) {
  // The banked RAM byte at addr, ignoring ROM overlays — what a CPU write
  // would land on (the "write bank" view of the memory hex window).
  if (subcycle::Machine const* m = subcycle_bridge_machine())
    return mem_read_ram(m->mem(), addr);
  return g_memory_bus.read_raw_via_write_bank(addr);
}

byte z80_read_mem_raw_bank(word addr, int bank) {
  // One physical 16K page (base 64K then expansion), independent of banking.
  if (subcycle::Machine const* m = subcycle_bridge_machine()) {
    const size_t phys = (static_cast<size_t>(bank) * 16384) + (addr % 16384);
    if (phys < m->ram_size()) return m->ram_read(phys);
  }
  return pbRAM != nullptr ? pbRAM[(bank * 16384) + (addr % 16384)] : 0xFF;
}

// The Multiface II STOP button (the hw/mf2 Device rides the bridge).
void z80_mf2stop() { subcycle_bridge_mf2_stop(); }

namespace {

bool eval_bp_condition(const Breakpoint& b, uint16_t addr) {
  if (!b.condition) return true;
  ExprContext ctx;
  ctx.z80 = &z80;
  ctx.crtc = &CRTC;
  ctx.ga = &GateArray;
  ctx.psg = &PSG;
  ctx.address = addr;
  return expr_eval(b.condition.get(), ctx) != 0;
}

bool eval_wp_condition(const Watchpoint& w, uint16_t addr, uint8_t val,
                       uint8_t old_val, bool is_write) {
  if (!w.condition) return true;
  ExprContext ctx;
  ctx.z80 = &z80;
  ctx.address = addr;
  ctx.value = val;
  ctx.previous = old_val;
  ctx.mode = is_write ? 2 : 1;
  return expr_eval(w.condition.get(), ctx) != 0;
}

}  // namespace

// The one pair of fire predicates: condition eval + hit-count/pass-count
// bookkeeping. The legacy core's exec/read/write loops and the engine=1
// probe post-filters all funnel through these two, so conditional and
// pass-count semantics cannot drift between the engines.
bool z80_bp_should_fire(Breakpoint& b, word pc) {
  if (!eval_bp_condition(b, pc)) return false;
  b.hit_count++;
  return b.pass_count <= 0 || b.hit_count >= b.pass_count;
}

bool z80_wp_should_fire(Watchpoint& w, word addr, byte val, byte old_val,
                        bool is_write) {
  if (is_write && !(w.type & WRITE)) return false;
  if (!is_write && !(w.type & READ)) return false;
  if (addr < w.address || addr >= w.address + w.length) return false;
  if (!eval_wp_condition(w, addr, val, old_val, is_write)) return false;
  w.hit_count++;
  return w.pass_count <= 0 || w.hit_count >= w.pass_count;
}

bool z80_probe_exec_should_break(uint16_t pc) {
  if (breakpoints.empty()) return true;
  // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated
  // (out-param/compound-assign/loop/reference)
  bool any = false;
  for (auto& b : breakpoints) {
    if (b.address != pc) continue;
    any = true;
    if (z80_bp_should_fire(b, static_cast<word>(pc))) return true;
  }
  return !any;
}

// Parameters retained for API/callback signature stability.
bool z80_probe_watch_should_break([[maybe_unused]] uint16_t addr,
                                  [[maybe_unused]] uint8_t data,
                                  [[maybe_unused]] bool is_write,
                                  [[maybe_unused]] uint8_t old_val) {
  return watchpoints.empty();
}

void z80_remove_ephemeral_breakpoints() {
  breakpoints.erase(
      std::remove_if(breakpoints.begin(), breakpoints.end(),
                     [](const Breakpoint& b) { return b.type == EPHEMERAL; }),
      breakpoints.end());
}

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other
// translation units/tests; internal linkage would break the link
const std::vector<Breakpoint>& z80_list_breakpoints_ref() {
  return breakpoints;
}

// --- IO breakpoints ---
namespace {
std::vector<IOBreakpoint> io_breakpoints;
}  // namespace

bool z80_check_io_breakpoint(word port, IOBreakpointDir access, byte val) {
  if (io_breakpoints.empty()) return false;
  for (auto& bp : io_breakpoints) {
    if ((bp.dir & access) && ((port & bp.mask) == (bp.port & bp.mask))) {
      if (bp.condition) {
        ExprContext ctx;
        ctx.z80 = &z80;
        ctx.crtc = &CRTC;
        ctx.ga = &GateArray;
        ctx.psg = &PSG;
        ctx.address = port;
        ctx.value = val;
        if (expr_eval(bp.condition.get(), ctx) == 0) continue;
      }
      return true;
    }
  }
  return false;
}

void z80_add_io_breakpoint(word port, word mask, IOBreakpointDir dir) {
  IOBreakpoint bp;
  bp.port = port;
  bp.mask = mask;
  bp.dir = dir;
  io_breakpoints.push_back(std::move(bp));
}

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other
// translation units/tests; internal linkage would break the link
void z80_add_io_breakpoint_cond(word port, word mask, IOBreakpointDir dir,
                                std::unique_ptr<ExprNode> condition,
                                const std::string& cond_str) {
  IOBreakpoint bp;
  bp.port = port;
  bp.mask = mask;
  bp.dir = dir;
  bp.condition = std::move(condition);
  bp.condition_str = cond_str;
  io_breakpoints.push_back(std::move(bp));
}

void z80_del_io_breakpoint(int index) {
  if (index >= 0 && index < static_cast<int>(io_breakpoints.size())) {
    io_breakpoints.erase(io_breakpoints.begin() + index);
  }
}

void z80_clear_io_breakpoints() { io_breakpoints.clear(); }

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other
// translation units/tests; internal linkage would break the link
const std::vector<IOBreakpoint>& z80_list_io_breakpoints_ref() {
  return io_breakpoints;
}

// --- Watchpoints ---

void z80_add_watchpoint(word addr, word len, WatchpointType type) {
  Watchpoint wp(addr, type);
  wp.length = len;
  watchpoints.push_back(std::move(wp));
}

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other
// translation units/tests; internal linkage would break the link
void z80_add_watchpoint_cond(word addr, word len, WatchpointType type,
                             std::unique_ptr<ExprNode> cond,
                             const std::string& cond_str, int pass_count) {
  Watchpoint wp(addr, type);
  wp.length = len;
  wp.condition = std::move(cond);
  wp.condition_str = cond_str;
  wp.pass_count = pass_count;
  watchpoints.push_back(std::move(wp));
}

void z80_del_watchpoint(int index) {
  if (index >= 0 && index < static_cast<int>(watchpoints.size())) {
    watchpoints.erase(watchpoints.begin() + index);
  }
}

void z80_clear_watchpoints() { watchpoints.clear(); }

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other
// translation units/tests; internal linkage would break the link
const std::vector<Watchpoint>& z80_list_watchpoints_ref() {
  return watchpoints;
}

// --- Ephemeral breakpoints ---

void z80_add_breakpoint_ephemeral(word addr) {
  breakpoints.emplace_back(addr, EPHEMERAL);
}
