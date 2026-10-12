#include "kcpc/kcpc_host.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>

#include "emu_frame.h"
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
#include "z80_view.h"

extern t_CPC CPC;
extern t_CRTC CRTC;
extern byte* memmap_ROM[256];

namespace {

using kcpc::Status;

// How long a write, or a read that fell back to a lease, waits for other lease
// holders to finish before it gives up. They hold the machine for one
// operation each.
constexpr int kLeaseAloneWaitMs = 250;

// Calls work(was_paused) with nothing clocking the machine and no pause lease
// held but this thread's own, then puts the run state back. False when the
// runner did not go idle, or other holders kept the machine for longer than
// kLeaseAloneWaitMs; work was not called then.
//
// Leases do not exclude one another, and the main thread rebuilds the machine
// (reallocating its RAM) under one of its own, so nothing here may touch the
// machine beside another holder. A holder that is not alone lets go of its
// lease before it tries again. Waiting with the lease held does not work: two
// threads that both wait to be alone each keep the other from being it, until
// one gives up (a writer and a reader that had fallen back to a lease did
// exactly that, and the write answered BUSY).
//
// `let_go` is how the lease is dropped once work has run: by default the run
// state is put back; a step leaves the machine stopped instead.
void put_back(CpcPauseLease& lease) { lease.restore_run_state(); }

bool alone_under_lease(
    const std::function<void(bool)>& work,
    const std::function<void(CpcPauseLease&)>& let_go = &put_back) {
  auto const deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(kLeaseAloneWaitMs);
  for (;;) {
    CpcPauseLease lease;
    if (!lease.idle()) {
      lease.restore_run_state();
      return false;
    }
    bool const ran =
        lease.change_alone([&] { work(lease.was_paused()); }, /*timeout_ms=*/0);
    if (ran) {
      let_go(lease);
      return true;
    }
    lease.restore_run_state();
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
}

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
    return alone_under_lease(work);
  };
  return host;
}

kcpc::Gate g_gate{gate_host()};
kcpc::Server g_kcpc_server;

// Runs change(stamp) with nothing clocking the machine and no other pause
// lease held, then puts the run state back. The stamp says what the run
// state was, and is, around the change.
// Every write pauses the machine between two frames, and reads of a running
// machine are taken at a frame boundary. A controller that sends its next
// write as soon as the last is answered would leave the runner no frame at
// all, and every read beside it would wait out the gate's fallback and take a
// pause lease (measured on CI: 15 reads in 1.5 s from three clients). So a
// write that finds reads waiting on a running machine lets one frame boundary
// pass first. With nobody reading, and on a paused machine, it waits for
// nothing.
void let_waiting_reads_in() {
  // A paced frame is 20 ms; this is that and room for a late wake-up. A
  // runner that turns out no frame in that time does not hold the write up
  // any longer.
  constexpr int kBoundaryWaitMs = 50;
  if (g_gate.waiting() == 0) return;
  std::uint64_t const frame = g_gate.frame();
  auto const deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(kBoundaryWaitMs);
  while (g_gate.waiting() != 0 && g_gate.frame() == frame &&
         !g_emu_paused.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
}

template <typename Change>
Status changed(Change&& change) {
  let_waiting_reads_in();
  Status status = Status::kInternal;
  bool const ran = alone_under_lease([&](bool was_paused) {
    kcpc::Stamp stamp;
    stamp.frame = g_gate.frame();
    stamp.paused = was_paused;
    status = change(stamp);
  });
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

// The frame step this port armed, if one is running. It shares the text
// IPC's frame counter (arm_frame_step()), which nobody waits on here: the
// end of the step is an event.
std::atomic<bool> g_frame_step_armed{false};

static_assert(static_cast<int>(CpcStopReason::Paused) ==
              static_cast<int>(kcpc::StopReason::kPaused));
static_assert(static_cast<int>(CpcStopReason::StepStalled) ==
              static_cast<int>(kcpc::StopReason::kStepStalled));

// The core announces a change of run state: push it to the clients. Called
// under the pause mutex, on whichever thread settled the state, the frame
// runner included, so this only fills in an event and posts it.
void on_run_event(const CpcRunEvent& change) {
  kcpc::Event event;
  event.frame = g_gate.frame();
  event.cause_client = change.cause_client;
  event.cause_request = change.cause_request;
  switch (change.kind) {
    case CpcRunEventKind::Stopped:
      event.opcode = kcpc::Opcode::kEvtStopped;
      event.reason = static_cast<std::uint8_t>(change.reason);
      event.pc = change.pc;
      // The details of a watchpoint or I/O hit come with the probe hook of
      // slice 5; a breakpoint's address is where the machine stopped.
      if (change.reason == CpcStopReason::Breakpoint) event.addr = change.pc;
      event.step_client = change.step_client;
      // Whatever stopped the machine, a frame step this port armed is over:
      // left armed, it would stop the machine again some frames after the
      // next Run.
      if (g_frame_step_armed.exchange(false, std::memory_order_acq_rel)) {
        emu_cancel_frame_step();
      }
      break;
    case CpcRunEventKind::Resumed:
      event.opcode = kcpc::Opcode::kEvtResumed;
      break;
    case CpcRunEventKind::Reset:
      event.opcode = kcpc::Opcode::kEvtReset;
      break;
  }
  g_kcpc_server.post(event);
}

// The stamp of a run-control reply: the run state as announced, now.
kcpc::Stamp announced_stamp() {
  kcpc::Stamp stamp;
  stamp.paused = cpc_announced_stopped();
  stamp.frame = g_gate.frame();
  return stamp;
}

CpcRunCause cause_of(std::uint32_t client, std::uint32_t request) {
  CpcRunCause cause;
  cause.client = client;
  cause.request = request;
  return cause;
}

}  // namespace

void kcpc_service_frame() {
  g_gate.service_frame();
  // Nobody subscribed to frames: one relaxed load, and that is all.
  std::uint64_t const frame = g_gate.frame();
  if (g_kcpc_server.frame_due(frame)) {
    kcpc::Event event;
    event.opcode = kcpc::Opcode::kEvtFrame;
    event.frame = frame;
    g_kcpc_server.post(event);
  }
}

Status kcpc_host_run_state(kcpc::Stamp& stamp, bool& walk_active) {
  stamp = announced_stamp();
  walk_active = cpc_step_active();
  return Status::kOk;
}

Status kcpc_host_pause(std::uint32_t client, std::uint32_t request,
                       kcpc::Stamp& stamp) {
  {
    // A lease, and not cpc_pause() alone, for what it waits for: the frame
    // in flight. When this returns the stop has happened, it has been
    // announced, and the reply's stamp is that of the stopped machine. A
    // runner that will not go idle is paused all the same; its stop is
    // announced when it parks.
    CpcPauseLease lease;
    lease.release_stopped(cause_of(client, request));
  }
  stamp = announced_stamp();
  return Status::kOk;
}

Status kcpc_host_run(std::uint32_t client, std::uint32_t request,
                     kcpc::Stamp& stamp) {
  // Not cpc_resume(): that is dropped while another actor holds a pause
  // lease (a read that fell back to one, the text IPC, the GUI), and a Run
  // must not vanish for that.
  cpc_run(cause_of(client, request));
  stamp = announced_stamp();
  return Status::kOk;
}

Status kcpc_host_reset(std::uint32_t client, std::uint32_t request,
                       bool stay_paused, kcpc::Stamp& stamp) {
  CpcPauseLease lease;
  if (!lease.idle() || subcycle_bridge_machine() == nullptr) {
    bool const idle = lease.idle();
    lease.restore_run_state();
    return idle ? Status::kInternal : Status::kBusy;
  }
  CpcRunCause const cause = cause_of(client, request);
  emulator_reset(cause);
  // RESET promises a running machine, or a stopped one, whichever it was.
  if (stay_paused) {
    lease.release_stopped(cause);
  } else {
    lease.release_running(cause);
  }
  stamp = announced_stamp();
  return Status::kOk;
}

Status kcpc_host_step(std::uint32_t client, std::uint32_t request,
                      const kcpc::StepRequest& step) {
  if (cpc_step_active() || emu_frame_step_active()) {
    return Status::kBusy;
  }
  CpcRunCause cause = cause_of(client, request);
  cause.step = true;
  std::uint32_t const count = step.count == 0 ? 1 : step.count;

  if (step.mode == static_cast<std::uint8_t>(kcpc::StepMode::kIn)) {
    if (count > kcpc::kMaxStepInCount) return Status::kOutOfRange;
    cause.reason = CpcStopReason::StepDone;
    Status status = Status::kInternal;
    bool const ran = alone_under_lease(
        [&](bool /*was_paused*/) {
          if (subcycle_bridge_machine() == nullptr) return;
          for (std::uint32_t i = 0; i < count; ++i) z80_step_instruction();
          status = Status::kOk;
        },
        [&](CpcPauseLease& lease) {
          // A step is a stop somebody asked for, and it is announced even
          // when the machine was stopped before it.
          if (status == Status::kOk) {
            lease.release_stopped(cause);
          } else {
            lease.restore_run_state();
          }
        });
    return ran ? status : Status::kBusy;
  }

  if (step.mode == static_cast<std::uint8_t>(kcpc::StepMode::kFrames)) {
    if (count > kcpc::kMaxStepFrames) return Status::kOutOfRange;
    CpcPauseLease lease;
    if (!lease.idle()) {
      lease.restore_run_state();
      return Status::kBusy;
    }
    g_frame_step_armed.store(true, std::memory_order_release);
    emu_arm_frame_step(static_cast<int>(count));
    // release_running(), not a resume by hand: with another lease held the
    // frames then run when its holder lets go, instead of never.
    lease.release_running(cause);
    return Status::kOk;
  }
  return Status::kBadPayload;  // over, out, to: slice 5
}

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

// A number from the environment, or `fallback` when it is unset, not a
// number or outside [min, max]. For the two limits a test lowers.
int env_int(const char* name, int fallback, int min, int max) {
  // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, before any thread starts
  const char* const text = std::getenv(name);
  if (text == nullptr) return fallback;
  char* end = nullptr;
  long const value = std::strtol(text, &end, 10);
  return (end != text && *end == '\0' && value >= min && value <= max)
             ? static_cast<int>(value)
             : fallback;
}

// How long a controller may stay silent. A test lowers it; nothing else
// should.
int controller_idle_timeout_ms() {
  constexpr int kDefaultMs = 30000;
  return env_int("KONCPC_KCPC_CONTROLLER_TIMEOUT_MS", kDefaultMs, 1,
                 kDefaultMs);
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
  config.run_state = &kcpc_host_run_state;
  config.pause = [](const kcpc::Cause& cause, kcpc::Stamp& stamp) {
    return kcpc_host_pause(cause.client, cause.request, stamp);
  };
  config.run = [](const kcpc::Cause& cause, kcpc::Stamp& stamp) {
    return kcpc_host_run(cause.client, cause.request, stamp);
  };
  config.reset = [](const kcpc::Cause& cause, bool stay_paused,
                    kcpc::Stamp& stamp) {
    return kcpc_host_reset(cause.client, cause.request, stay_paused, stamp);
  };
  config.step = [](const kcpc::Cause& cause, const kcpc::StepRequest& step) {
    return kcpc_host_step(cause.client, cause.request, step);
  };
  config.controller_idle_timeout_ms = controller_idle_timeout_ms();
  config.max_event_queue_bytes = static_cast<std::size_t>(env_int(
      "KONCPC_KCPC_EVENT_QUEUE_BYTES", 4 << 20, /*min=*/1, /*max=*/32 << 20));
  cpc_set_run_state_observer(&on_run_event);
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
