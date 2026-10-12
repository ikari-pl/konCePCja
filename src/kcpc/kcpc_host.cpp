#include "kcpc/kcpc_host.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>

#include "hw/memory.h"
#include "hw/z80.h"
#include "iui_host.h"
#include "kcpc/kcpc_gate.h"
#include "kcpc/kcpc_server.h"
#include "koncepcja.h"
#include "log.h"
#include "startup_manifest.h"
#include "subcycle/machine.h"
#include "subcycle_bridge.h"

extern t_CPC CPC;
extern t_CRTC CRTC;
extern byte* memmap_ROM[256];

namespace {

using kcpc::Status;

// How long a read that fell back to a lease waits for other lease holders to
// finish before it gives up. They hold the machine for one operation each.
constexpr int kLeaseAloneWaitMs = 250;

kcpc::GateHost gate_host() {
  kcpc::GateHost host;
  // The atomic the frame runner itself obeys, true for a stop and for a
  // lease alike: either way no frame boundary is coming and the reader has
  // to take the read itself. Not cpc_stopped(): that takes the pause mutex,
  // which the runner holds for the whole of every machine frame, so on an
  // unpaced machine a reader would wait some 15 ms for it on every read
  // (measured).
  host.paused = [] { return g_emu_paused.load(std::memory_order_acquire); };
  host.read_stopped = [](const std::function<void()>& work) {
    return cpc_read_while_stopped(work);
  };
  host.with_lease = [](const std::function<void(bool)>& work) {
    CpcPauseLease lease;
    if (!lease.idle()) {
      lease.restore_run_state();
      return false;
    }
    // Alone: another holder may be changing what this copies (a machine
    // rebuild reallocates the expansion RAM). The bound is the gate's own
    // patience; a reader that cannot have the machine answers BUSY.
    bool const ran =
        lease.read_alone([&] { work(lease.was_paused()); }, kLeaseAloneWaitMs);
    lease.restore_run_state();
    return ran;
  };
  return host;
}

kcpc::Gate g_gate{gate_host()};
kcpc::Server g_kcpc_server;

// Runs change(stamp) with nothing clocking the machine and no other pause
// lease held, then puts the run state back. The stamp says what the run
// state was, and is, around the change.
template <typename Change>
Status changed(Change&& change) {
  CpcPauseLease lease;
  if (!lease.idle()) {
    lease.restore_run_state();
    return Status::kBusy;
  }
  Status status = Status::kInternal;
  // Alone: leases do not exclude one another, and the main thread rebuilds
  // the machine (and reallocates its RAM) under one of its own.
  bool const ran = lease.change_alone(
      [&] {
        kcpc::Stamp stamp;
        stamp.frame = g_gate.frame();
        stamp.paused = lease.was_paused();
        status = change(stamp);
      },
      kLeaseAloneWaitMs);
  lease.restore_run_state();
  return ran ? status : Status::kBusy;
}

void to_wire(const Z80Regs& z, kcpc::Regs& regs) {
  regs.af = z.af;
  regs.bc = z.bc;
  regs.de = z.de;
  regs.hl = z.hl;
  regs.ix = z.ix;
  regs.iy = z.iy;
  regs.sp = z.sp;
  regs.pc = z.pc;
  regs.af_alt = z.af_;
  regs.bc_alt = z.bc_;
  regs.de_alt = z.de_;
  regs.hl_alt = z.hl_;
  regs.i = z.i;
  regs.r = z.r;
  regs.iff1 = z.iff1;
  regs.iff2 = z.iff2;
  regs.im = z.im;
  regs.halt = z.halted;
}

// The REGS_SET mask is handed to the bridge as it came.
static_assert(kcpc::kRegBitPc == kSubcycleRegPC);
static_assert(kcpc::kRegBitIff1 == kSubcycleRegIFF1);
static_assert(kcpc::kRegBitIm == kSubcycleRegIM);
static_assert(kcpc::kRegsSettableMask == kSubcycleRegAll);

// Runs read(stamp) through the gate and returns what it returned, or why it
// did not run. `read` copies out of the machine and nothing else: it runs on
// the frame runner or under a pause lease.
template <typename Read>
Status gated(std::size_t cost, Read&& read) {
  Status status = Status::kInternal;
  switch (g_gate.read(
      cost, [&](const kcpc::Stamp& stamp) { status = read(stamp); })) {
    case kcpc::GateResult::kDone:
      return status;
    case kcpc::GateResult::kBusy:
      return Status::kBusy;
    case kcpc::GateResult::kShuttingDown:
      return Status::kShuttingDown;
  }
  return Status::kInternal;
}

// Bit N = an expansion ROM is fitted in slot N. The on-board BASIC is not
// one: it sets no bit.
std::uint32_t fitted_rom_slots() {
  std::uint32_t slots = 0;
  for (unsigned slot = 0; slot < kcpc::kRomSlotCount; ++slot) {
    if (memmap_ROM[slot] != nullptr) slots |= 1U << slot;
  }
  return slots;
}

}  // namespace

void kcpc_service_frame() { g_gate.service_frame(); }

Status kcpc_host_machine_info(kcpc::MachineInfo& info) {
  // Host configuration the main thread rewrites when it rebuilds the machine,
  // which it only does with the runner idle: the gate keeps this read out of
  // that.
  return gated(sizeof(info), [&info](const kcpc::Stamp& stamp) {
    subcycle::Machine const* machine = subcycle_bridge_machine();
    if (machine == nullptr) return Status::kInternal;
    constexpr unsigned kModel6128Plus = 3;
    info.model = static_cast<std::uint8_t>(CPC.model);
    info.crtc_type = CRTC.crtc_type;
    info.effective_tier =
        static_cast<std::uint8_t>(subcycle_bridge_effective_tier_index());
    info.flags = 0;
    if (g_headless) info.flags |= kcpc::kMachineFlagHeadless;
    if (CPC.model >= kModel6128Plus) info.flags |= kcpc::kMachineFlagAsic;
    info.ram_bytes = static_cast<std::uint32_t>(machine->ram_size());
    info.rom_slots = fitted_rom_slots();
    info.stamp = stamp;
    return Status::kOk;
  });
}

Status kcpc_host_regs_get(kcpc::Stamp& stamp, kcpc::Regs& regs) {
  return gated(kcpc::kRegsSize, [&](const kcpc::Stamp& taken) {
    if (subcycle_bridge_machine() == nullptr) return Status::kInternal;
    // As the debugger shows them: at a breakpoint the PC is the
    // breakpoint's address, not the machine's own, which is past the opcode.
    to_wire(subcycle_bridge_shown_regs(), regs);
    stamp = taken;
    return Status::kOk;
  });
}

Status kcpc_host_mem_info(kcpc::MemInfo& info) {
  return gated(sizeof(info), [&info](const kcpc::Stamp& stamp) {
    subcycle::Machine const* machine = subcycle_bridge_machine();
    if (machine == nullptr) return Status::kInternal;
    MemRegs banking{};
    mem_peek(machine->mem(), &banking);
    info.ram_bytes = static_cast<std::uint32_t>(machine->ram_size());
    info.ram_config = banking.ram_config;
    info.rom_config = banking.rom_config;
    info.upper_rom = banking.rom_select;
    info.rom_slots = fitted_rom_slots();
    info.stamp = stamp;
    return Status::kOk;
  });
}

Status kcpc_host_mem_read(const kcpc::MemReadRequest& request,
                          kcpc::Stamp& stamp, std::uint8_t* dst) {
  // What does not depend on the machine is refused before it is asked.
  if (request.space > static_cast<std::uint8_t>(kcpc::MemSpace::kRomUpper)) {
    return Status::kBadPayload;  // ASIC_PAGE comes with CAP_CHIPS
  }
  auto const space = static_cast<kcpc::MemSpace>(request.space);
  if (space == kcpc::MemSpace::kRomUpper) {
    if (request.bank >= kcpc::kRomSlotCount) return Status::kOutOfRange;
  } else if (request.bank != 0) {
    return Status::kBadPayload;
  }
  std::uint32_t const addr = request.addr;
  std::uint32_t const len = request.len;
  switch (space) {
    case kcpc::MemSpace::kCpu:
    case kcpc::MemSpace::kCpuRam:
      if (!kcpc::mem_range_ok(addr, len, kcpc::kCpuSpaceSize)) {
        return Status::kOutOfRange;
      }
      break;
    case kcpc::MemSpace::kRomLower:
    case kcpc::MemSpace::kRomUpper:
      if (!kcpc::mem_range_ok(addr, len, kcpc::kRomSpaceSize)) {
        return Status::kOutOfRange;
      }
      break;
    case kcpc::MemSpace::kPhysRam:  // as large as the machine, asked below
    case kcpc::MemSpace::kAsicPage:
      break;
  }

  return gated(len, [&](const kcpc::Stamp& taken) {
    subcycle::Machine const* machine = subcycle_bridge_machine();
    if (machine == nullptr) return Status::kInternal;
    switch (space) {
      case kcpc::MemSpace::kCpu:
        machine->peek_mem_range(static_cast<std::uint16_t>(addr), dst, len);
        break;
      case kcpc::MemSpace::kCpuRam:
        machine->peek_ram_range(static_cast<std::uint16_t>(addr), dst, len);
        break;
      case kcpc::MemSpace::kPhysRam:
        if (!kcpc::mem_range_ok(addr, len, machine->ram_size())) {
          return Status::kOutOfRange;
        }
        machine->ram_copy(addr, dst, len);
        break;
      case kcpc::MemSpace::kRomLower:
        std::memcpy(dst, machine->lower_rom() + addr, len);
        break;
      case kcpc::MemSpace::kRomUpper:
        std::memcpy(
            dst,
            machine->upper_rom(static_cast<std::uint8_t>(request.bank)) + addr,
            len);
        break;
      case kcpc::MemSpace::kAsicPage:
        return Status::kBadPayload;
    }
    stamp = taken;
    return Status::kOk;
  });
}

Status kcpc_host_regs_set(std::uint32_t mask, const kcpc::Regs& values,
                          kcpc::Stamp& stamp, kcpc::Regs& after) {
  kcpc::RegsSetRequest request;
  request.mask = mask;
  request.regs = values;
  if (kcpc::regs_set_refusal(request) != nullptr) return Status::kBadPayload;
  return changed([&](const kcpc::Stamp& taken) {
    if (subcycle_bridge_machine() == nullptr) return Status::kInternal;
    Z80Regs z{};
    z.af = values.af;
    z.bc = values.bc;
    z.de = values.de;
    z.hl = values.hl;
    z.ix = values.ix;
    z.iy = values.iy;
    z.sp = values.sp;
    z.pc = values.pc;
    z.af_ = values.af_alt;
    z.bc_ = values.bc_alt;
    z.de_ = values.de_alt;
    z.hl_ = values.hl_alt;
    z.i = values.i;
    z.r = values.r;
    z.iff1 = values.iff1;
    z.iff2 = values.iff2;
    z.im = values.im;
    subcycle_bridge_write_regs(z, mask);
    to_wire(subcycle_bridge_shown_regs(), after);
    stamp = taken;
    return Status::kOk;
  });
}

Status kcpc_host_mem_write(const kcpc::MemWriteRequest& request,
                           const std::uint8_t* data, std::size_t len,
                           kcpc::Stamp& stamp) {
  if (!kcpc::mem_space_writable(request.space) || request.bank != 0) {
    return Status::kBadPayload;
  }
  auto const space = static_cast<kcpc::MemSpace>(request.space);
  // A request is at most 1 MiB, so its length fits 32 bits.
  auto const len32 = static_cast<std::uint32_t>(len);
  if (space == kcpc::MemSpace::kCpuRam &&
      !kcpc::mem_range_ok(request.addr, len32, kcpc::kCpuSpaceSize)) {
    return Status::kOutOfRange;
  }
  Status const status = changed([&](const kcpc::Stamp& taken) {
    subcycle::Machine* machine = subcycle_bridge_machine();
    if (machine == nullptr) return Status::kInternal;
    if (space == kcpc::MemSpace::kCpuRam) {
      // Where a Z80 write to that address lands: banked RAM, never ROM.
      for (std::size_t i = 0; i < len; ++i) {
        machine->poke_mem(static_cast<std::uint16_t>(request.addr + i),
                          data[i]);
      }
    } else {
      if (!kcpc::mem_range_ok(request.addr, len32, machine->ram_size())) {
        return Status::kOutOfRange;
      }
      for (std::size_t i = 0; i < len; ++i) {
        machine->ram_write(request.addr + i, data[i]);
      }
    }
    stamp = taken;
    return Status::kOk;
  });
  // What the in-process debugger had decoded from those bytes is stale.
  if (status == Status::kOk && len != 0) ui_host().debugger_memory_changed();
  return status;
}

namespace {

// How long a controller may stay silent. A test lowers it; nothing else
// should.
int controller_idle_timeout_ms() {
  constexpr int kDefaultMs = 30000;
  // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, before any thread starts
  const char* const text = std::getenv("KONCPC_KCPC_CONTROLLER_TIMEOUT_MS");
  if (text == nullptr) return kDefaultMs;
  char* end = nullptr;
  long const ms = std::strtol(text, &end, 10);
  return (end != text && *end == '\0' && ms > 0 && ms <= kDefaultMs)
             ? static_cast<int>(ms)
             : kDefaultMs;
}

}  // namespace

void kcpc_host_start(bool enabled) {
  if (!enabled) return;
  kcpc::ServerConfig config;
#ifdef KONCPC_VERSION_STRING
  config.server_version = KONCPC_VERSION_STRING;
#endif
  config.pid = static_cast<std::uint32_t>(startup_manifest_self_pid());
  config.machine_info = &kcpc_host_machine_info;
  config.regs_get = &kcpc_host_regs_get;
  config.mem_info = &kcpc_host_mem_info;
  config.mem_read = &kcpc_host_mem_read;
  config.regs_set = &kcpc_host_regs_set;
  config.mem_write = &kcpc_host_mem_write;
  config.controller_idle_timeout_ms = controller_idle_timeout_ms();
  g_gate.reopen();
  g_kcpc_server.start(config);
}

void kcpc_host_stop() {
  // First the gate, so a reader waiting for a frame boundary answers
  // SHUTTING_DOWN at once and its thread can be joined.
  g_gate.shut_down();
  bool const was_running = g_kcpc_server.running();
  g_kcpc_server.stop();
  if (!was_running) return;
  // Where the reads of this run were taken: the answer to "did a client cost
  // the machine pauses?" without a debugger.
  kcpc::Gate::Counters const c = g_gate.counters();
  LOG_INFO("KCPC: reads served: " << c.at_boundary << " at a frame boundary, "
                                  << c.while_paused << " while paused, "
                                  << c.fallback << " by fallback, " << c.busy
                                  << " busy");
}

int kcpc_host_port() { return g_kcpc_server.port(); }
