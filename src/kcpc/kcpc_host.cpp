#include "kcpc/kcpc_host.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "autotype.h"
#include "emu_frame.h"
#include "errors.h"
#include "host_state.h"
#include "hw/asic.h"
#include "hw/crtc.h"
#include "hw/fdc.h"
#include "hw/gate_array.h"
#include "hw/memory.h"
#include "hw/probe.h"
#include "hw/psg.h"
#include "hw/tape.h"
#include "hw/z80.h"
#include "hw_views.h"
#include "imgui_ui_testable.h"
#include "ipc_mru.h"
#include "iui_host.h"
#include "kcpc/kcpc_gate.h"
#include "kcpc/kcpc_server.h"
#include "keyboard.h"
#include "koncepcja.h"
#include "koncepcja_ipc_server.h"
#include "log.h"
#include "slotshandler.h"
#include "startup_manifest.h"
#include "subcycle/machine.h"
#include "subcycle_bridge.h"
#include "symfile.h"
#include "z80_view.h"
#include "zip_archive.h"

extern t_CPC CPC;
extern t_CRTC CRTC;
extern t_drive driveA;
extern t_drive driveB;
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
      // What was hit, for a breakpoint, a watchpoint or an I/O breakpoint:
      // filled in where the probe's hit is committed.
      event.addr = change.addr;
      event.access = change.access;
      event.value = change.value;
      event.old_value = change.old_value;
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

// The request that is editing the breakpoint lists on this thread, if one
// is: the edit's event names it, so that the server sends the event after
// the reply.
thread_local kcpc::Cause t_bp_cause;

struct BpCauseScope {
  BpCauseScope(std::uint32_t client, std::uint32_t request) {
    t_bp_cause = kcpc::Cause{client, request};
  }
  ~BpCauseScope() { t_bp_cause = kcpc::Cause{}; }
  BpCauseScope(const BpCauseScope&) = delete;
  BpCauseScope& operator=(const BpCauseScope&) = delete;
};

// A breakpoint list changed, whoever edited it: this port, the text IPC, the
// DevTools. Called under the lists' lock on the editing thread, so it only
// posts.
void on_breakpoints_changed(std::uint64_t generation) {
  kcpc::Event event;
  event.opcode = kcpc::Opcode::kEvtBreakpointsChanged;
  event.frame = g_gate.frame();
  event.generation = generation;
  event.cause_client = t_bp_cause.client;
  event.cause_request = t_bp_cause.request;
  g_kcpc_server.post(event);
}

static_assert(static_cast<int>(kcpc::BpKind::kWatch) ==
              static_cast<int>(Z80BpKind::Watch));
static_assert(static_cast<int>(kcpc::BpKind::kIo) ==
              static_cast<int>(Z80BpKind::Io));
static_assert(kcpc::kMaxExecBreakpoints == PROBE_MAX_EXEC);
static_assert(kcpc::kMaxWatchpoints == PROBE_MAX_WATCH);
static_assert(kcpc::kMaxIoBreakpoints == PROBE_MAX_IO);
static_assert(kcpc::kBpAccessRead == READ && kcpc::kBpAccessWrite == WRITE);
static_assert(kcpc::kBpAccessRead == IO_IN && kcpc::kBpAccessWrite == IO_OUT);

// The request that is editing the symbol table on this thread, if one is,
// and the generation its edit ended with: the table's hook runs on the
// editing thread, so both travel in thread-local storage.
thread_local kcpc::Cause t_sym_cause;
thread_local std::uint64_t t_sym_generation = 0;

struct SymCauseScope {
  SymCauseScope(std::uint32_t client, std::uint32_t request) {
    t_sym_cause = kcpc::Cause{client, request};
    t_sym_generation = 0;
  }
  ~SymCauseScope() { t_sym_cause = kcpc::Cause{}; }
  SymCauseScope(const SymCauseScope&) = delete;
  SymCauseScope& operator=(const SymCauseScope&) = delete;
  // The generation after the edit this scope covered. An edit that changed
  // nothing called no hook: the table's own count then.
  std::uint64_t generation() const {
    return t_sym_generation != 0 ? t_sym_generation : g_symfile.generation();
  }
};

// The symbol table changed, whoever edited it: this port, the text IPC, the
// DevTools, the assembler. Called under the table's lock on the editing
// thread, so it only posts.
void on_symbols_changed(std::uint64_t generation) {
  t_sym_generation = generation;
  kcpc::Event event;
  event.opcode = kcpc::Opcode::kEvtSymbolsChanged;
  event.frame = g_gate.frame();
  event.generation = generation;
  event.cause_client = t_sym_cause.client;
  event.cause_request = t_sym_cause.request;
  g_kcpc_server.post(event);
}

static_assert(static_cast<int>(kcpc::MediaSlot::kDriveB) ==
              static_cast<int>(::MediaSlot::DriveB));
static_assert(static_cast<int>(kcpc::MediaSlot::kTape) ==
              static_cast<int>(::MediaSlot::Tape));
static_assert(static_cast<int>(kcpc::MediaSlot::kCartridge) ==
              static_cast<int>(::MediaSlot::Cartridge));
static_assert(kcpc::kMediaSlots == kMediaSlotCount);

// A slot got a different medium, or lost it: a load or an eject on whichever
// thread did it, or the frame runner giving the machine a queued swap. So
// this only posts; what the slot holds is asked for when the event is sent.
//
// A change a request of this port makes happens on that request's thread,
// under its lease, the machine's half included: the request is named in the
// event, so that its client is sent the event after the reply.
thread_local kcpc::Cause t_media_cause;

struct MediaCauseScope {
  MediaCauseScope(std::uint32_t client, std::uint32_t request) {
    t_media_cause = kcpc::Cause{client, request};
  }
  ~MediaCauseScope() { t_media_cause = kcpc::Cause{}; }
  MediaCauseScope(const MediaCauseScope&) = delete;
  MediaCauseScope& operator=(const MediaCauseScope&) = delete;
};

void on_media_changed(::MediaSlot slot, std::uint64_t generation) {
  kcpc::Event event;
  event.opcode = kcpc::Opcode::kEvtMediaChanged;
  event.frame = g_gate.frame();
  event.slot = static_cast<std::uint8_t>(slot);
  event.generation = generation;
  event.cause_client = t_media_cause.client;
  event.cause_request = t_media_cause.request;
  g_kcpc_server.post(event);
}

// The CONFIG_APPLY in progress, if one is: the rebuild it asked for runs on
// the main thread, so the request cannot travel in thread-local storage.
std::atomic<std::uint32_t> g_apply_client{0};
std::atomic<std::uint32_t> g_apply_request{0};

// The machine as MACHINE_INFO describes it. The caller has made sure nothing
// is clocking or rebuilding it.
bool fill_machine_info(kcpc::MachineInfo& info, const kcpc::Stamp& stamp) {
  subcycle::Machine const* machine = subcycle_bridge_machine();
  if (machine == nullptr) return false;
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
  return true;
}

// The machine was rebuilt (a new model, RAM size, CRTC type). Called by the
// thread that rebuilt it, which still holds its pause lease: the machine is
// read here directly, not through the gate, which would wait for that lease.
void on_machine_rebuilt(bool was_paused) {
  kcpc::Event event;
  event.opcode = kcpc::Opcode::kEvtMachineChanged;
  event.frame = g_gate.frame();
  kcpc::Stamp stamp;
  stamp.frame = event.frame;
  stamp.paused = was_paused;
  if (!fill_machine_info(event.machine, stamp)) return;
  event.cause_client = g_apply_client.load(std::memory_order_acquire);
  event.cause_request = g_apply_request.load(std::memory_order_acquire);
  g_kcpc_server.post(event);
}

// The walker: the one thread that runs the steps that walk the machine
// (over, out, to). A request arms a step and returns; the walk happens here
// and ends as an event. One step at a time.
struct Walker {
  std::mutex mutex;
  std::condition_variable wake;
  // A step is armed and the thread has not taken it yet. From then until the
  // walk closes, cpc_step_active() says a step is in progress.
  bool pending = false;
  bool quit = false;
  kcpc::StepRequest job;
  std::thread thread;
};
Walker g_walker;

// Runs one step. Each walk tells the core how it ended (CpcWalk), and the
// core announces that as the step's EVT_STOPPED when the walk closes.
void walk(const kcpc::StepRequest& step) {
  auto const deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(step.timeout_ms);
  bool stalled = false;
  switch (static_cast<kcpc::StepMode>(step.mode)) {
    case kcpc::StepMode::kOver:
      stalled =
          z80_step_over(step.count, deadline) == Z80StepOutResult::Stalled;
      break;
    case kcpc::StepMode::kOut:
      stalled = z80_step_out_until(deadline) == Z80StepOutResult::Stalled;
      break;
    case kcpc::StepMode::kTo:
      stalled = z80_run_until_ephemeral(step.addr, deadline) ==
                Z80RunUntilResult::Stalled;
      break;
    case kcpc::StepMode::kIn:
    case kcpc::StepMode::kFrames:
      break;  // not walks: kcpc_host_step() runs them itself
  }
  // No machine: the walk did not even open, so it gave no verdict.
  if (stalled) cpc_walk_verdict(CpcStopReason::StepStalled);
}

void walker_main() {
  for (;;) {
    kcpc::StepRequest step;
    {
      std::unique_lock lock(g_walker.mutex);
      g_walker.wake.wait(lock,
                         [] { return g_walker.pending || g_walker.quit; });
      if (!g_walker.pending) return;  // quitting, and nothing was armed
      step = g_walker.job;
      g_walker.pending = false;
    }
    try {
      walk(step);
    } catch (const std::exception& e) {
      // A walker that died would leave every later STEP answering BUSY.
      LOG_ERROR("KCPC: step walk failed: " << e.what());
    }
    // The request opened the walk (kcpc_host_step); this closes it, and the
    // step's one EVT_STOPPED is announced here. Nothing of the walker's is
    // left to tidy after it: a client may answer that event with its next
    // STEP at once, and must not find this one still "busy".
    cpc_walk_end();
  }
}

void walker_start() {
  std::scoped_lock const lock(g_walker.mutex);
  g_walker.quit = false;
  if (!g_walker.thread.joinable()) g_walker.thread = std::thread(&walker_main);
}

void walker_stop() {
  {
    std::scoped_lock const lock(g_walker.mutex);
    g_walker.quit = true;
  }
  // Ends the walk in progress as a pause would: the thread is about to be
  // joined, and a walk only watches its own deadline otherwise.
  cpc_walk_interrupt();
  g_walker.wake.notify_all();
  if (g_walker.thread.joinable()) g_walker.thread.join();
}

kcpc::BpEntry to_wire(const Z80BpSpec& spec) {
  kcpc::BpEntry entry;
  entry.kind = static_cast<std::uint8_t>(spec.kind);
  entry.access = spec.access;
  entry.addr = spec.addr;
  entry.len_or_mask = spec.len_or_mask;
  entry.pass_count =
      spec.pass_count > 0 ? static_cast<std::uint32_t>(spec.pass_count) : 0;
  entry.hit_count =
      spec.hit_count > 0 ? static_cast<std::uint32_t>(spec.hit_count) : 0;
  entry.condition = spec.condition;
  return entry;
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

  auto const mode = static_cast<kcpc::StepMode>(step.mode);
  if (!kcpc::step_mode_walks(mode)) return Status::kBadPayload;
  // The machine is paused first if it was running, and stays paused until
  // the walker runs it: the lease waits for the frame in flight, and it is
  // let go as part of the step, which announces nothing. Whatever the walk
  // does to the run state after that is the walk's to announce.
  CpcPauseLease lease;
  if (!lease.idle() || subcycle_bridge_machine() == nullptr) {
    bool const idle = lease.idle();
    lease.restore_run_state();
    return idle ? Status::kInternal : Status::kBusy;
  }
  // A step that runs to an address plants a one-shot breakpoint, and the
  // probe has to have a comparator for it. Without one the step would run
  // to its deadline and call that a timeout.
  if (!z80_exec_comparator_free()) {
    lease.restore_run_state();
    return Status::kLimitReached;
  }
  {
    std::scoped_lock const lock(g_walker.mutex);
    if (g_walker.quit || !g_walker.thread.joinable()) {
      lease.restore_run_state();
      return Status::kShuttingDown;
    }
    if (g_walker.pending || cpc_step_active()) {
      lease.restore_run_state();
      return Status::kBusy;
    }
    // The step is in progress from here: a second STEP answers BUSY, and
    // RUN_STATE says a walk is active.
    cpc_walk_begin(cause);
    g_walker.job = step;
    g_walker.pending = true;
  }
  lease.release_stopped(cause);
  g_walker.wake.notify_one();
  return Status::kOk;
}

Status kcpc_host_bp_list(std::uint64_t& generation,
                         std::vector<kcpc::BpEntry>& entries) {
  std::vector<Z80BpSpec> const specs = z80_bp_list(generation);
  entries.clear();
  entries.reserve(specs.size());
  for (const Z80BpSpec& spec : specs) entries.push_back(to_wire(spec));
  return Status::kOk;
}

Status kcpc_host_bp_add(std::uint32_t client, std::uint32_t request,
                        const kcpc::BpEntry& entry, std::uint64_t& generation,
                        std::string& message) {
  if (kcpc::bp_entry_refusal(entry) != nullptr) return Status::kBadPayload;
  Z80BpSpec spec;
  spec.kind = static_cast<Z80BpKind>(entry.kind);
  spec.access = entry.access;
  spec.addr = entry.addr;
  spec.len_or_mask = entry.len_or_mask;
  spec.pass_count = static_cast<int>(entry.pass_count);
  spec.condition = entry.condition;
  BpCauseScope const editing(client, request);
  std::string error;
  switch (z80_bp_put(spec, error, generation)) {
    case Z80BpEdit::Ok:
      return Status::kOk;
    case Z80BpEdit::BadCondition:
      message = "the condition does not parse: " + error;
      return Status::kBadPayload;
    case Z80BpEdit::LimitReached:
      return Status::kLimitReached;
    case Z80BpEdit::NotFound:
      break;
  }
  return Status::kInternal;
}

Status kcpc_host_bp_del(std::uint32_t client, std::uint32_t request,
                        const kcpc::BpDelRequest& del,
                        std::uint64_t& generation) {
  if (del.kind > static_cast<std::uint8_t>(kcpc::BpKind::kIo)) {
    return Status::kBadPayload;
  }
  BpCauseScope const editing(client, request);
  return z80_bp_remove(static_cast<Z80BpKind>(del.kind), del.addr,
                       del.len_or_mask, generation) == Z80BpEdit::Ok
             ? Status::kOk
             : Status::kNotFound;
}

Status kcpc_host_bp_clear(std::uint32_t client, std::uint32_t request,
                          std::uint8_t kind, std::uint64_t& generation) {
  bool const all = kind == kcpc::kBpKindAll;
  if (!all && kind > static_cast<std::uint8_t>(kcpc::BpKind::kIo)) {
    return Status::kBadPayload;
  }
  BpCauseScope const editing(client, request);
  z80_bp_clear(all || kind == static_cast<std::uint8_t>(kcpc::BpKind::kExec),
               all || kind == static_cast<std::uint8_t>(kcpc::BpKind::kWatch),
               all || kind == static_cast<std::uint8_t>(kcpc::BpKind::kIo),
               generation);
  return Status::kOk;
}

Status kcpc_host_machine_info(kcpc::MachineInfo& info) {
  // Host configuration the main thread rewrites when it rebuilds the machine,
  // which it only does with the runner idle: the gate keeps this read out of
  // that.
  return gated(sizeof(info), [&info](const kcpc::Stamp& stamp) {
    return fill_machine_info(info, stamp) ? Status::kOk : Status::kInternal;
  });
}

namespace {

void read_crtc(const subcycle::Machine& machine, kcpc::CrtcState& out) {
  CrtcRegs regs{};
  crtc_peek(machine.crtc(), &regs);
  static_assert(sizeof(regs.reg) == kcpc::kCrtcRegisters);
  out.type = regs.type;
  out.reg_select = regs.reg_select;
  std::memcpy(out.reg.data(), regs.reg, sizeof(regs.reg));
  // The counter is 8 bits wide on the chip; the Device keeps it in 16.
  out.hcc = static_cast<std::uint8_t>(regs.hcc);
  out.vcc = regs.vcc;
  out.ra = regs.ra;
  out.hsw = regs.hsw;
  out.vsw = regs.vsw;
  out.vta = regs.vta;
  out.ma = regs.ma;
  out.scanline = regs.scanline;
}

void read_gate_array(const subcycle::Machine& machine,
                     kcpc::GateArrayState& out) {
  GateArrayRegs regs{};
  ga_peek(machine.gate_array(), &regs);
  MemRegs banking{};
  mem_peek(machine.mem(), &banking);
  static_assert(sizeof(regs.ink) == kcpc::kGateArrayInks);
  out.mode = regs.mode;
  out.requested_mode = regs.req_mode;
  out.pen = regs.pen;
  out.rom_config = regs.rom_config;
  out.ram_config = regs.ram_config;
  out.upper_rom = banking.rom_select;  // the latch is the memory Device's
  out.sl_count = regs.sl_count;
  out.hs_count = regs.hs_count;
  std::memcpy(out.ink.data(), regs.ink, sizeof(regs.ink));
}

void read_psg(const subcycle::Machine& machine, kcpc::PsgState& out) {
  PsgRegs regs{};
  psg_peek(machine.psg(), &regs);
  static_assert(sizeof(regs.reg) == kcpc::kPsgRegisters);
  std::memcpy(out.reg.data(), regs.reg, sizeof(regs.reg));
  out.reg_select = regs.sel;
}

void read_fdc(const subcycle::Machine& machine, kcpc::FdcState& out) {
  FdcRegs regs{};
  fdc_peek(machine.fdc(), &regs);
  out.motor = regs.motor;
  for (std::uint8_t unit = 0; unit < kcpc::kFdcDrives; ++unit) {
    out.track[unit] = regs.track[unit];
    out.present[unit] =
        static_cast<std::uint8_t>(fdc_media_present_unit(machine.fdc(), unit));
    out.generation[unit] = dsk_media_generation(unit);
  }
}

// False when there is no ASIC on this board.
bool read_asic(const subcycle::Machine& machine, kcpc::AsicState& out) {
  const Device* asic = machine.asic();
  AsicRegs regs{};
  asic_peek(asic, &regs);
  if (regs.plugged == 0) return false;
  out.unlocked = regs.locked == 0 ? 1 : 0;
  out.pri_line = regs.pri_line;
  out.lock_pos = static_cast<std::uint8_t>(asic_lock_pos(asic));
  out.page_on = regs.page_on;
  out.hscroll = regs.hscroll;
  out.vscroll = regs.vscroll;
  out.extend_border = regs.extend_border;
  out.int_vector = regs.int_vector;
  out.split_line = regs.split_line;
  out.split_addr = regs.split_addr;
  for (std::size_t ch = 0; ch < kcpc::kAsicDmaChannels; ++ch) {
    kcpc::AsicDmaState& dma = out.dma[ch];
    asic_dma_regs(asic, static_cast<int>(ch), &dma.source, &dma.prescaler,
                  &dma.enabled);
    asic_dma_debug(asic, static_cast<int>(ch), &dma.loop_addr, &dma.loops,
                   &dma.pause_ticks, &dma.tick_cycles, &dma.irq);
  }
  for (std::size_t id = 0; id < kcpc::kAsicSprites; ++id) {
    kcpc::AsicSpriteState& sprite = out.sprite[id];
    asic_vid_sprite_attr(asic, static_cast<int>(id), &sprite.x, &sprite.y,
                         &sprite.mag_x, &sprite.mag_y);
  }
  return true;
}

}  // namespace

// Every field comes off the Devices, through the gate: the structs the
// in-process views read (CRTC, GateArray, PSG, asic) are rewritten by the
// frame runner once a frame and by whoever steps the machine, and are not
// for a socket thread to read.
Status kcpc_host_chip_get(kcpc::ChipState& state) {
  if (!kcpc::chip_known(state.chip)) return Status::kBadPayload;
  return gated(sizeof(state), [&state](const kcpc::Stamp& taken) {
    subcycle::Machine const* machine = subcycle_bridge_machine();
    if (machine == nullptr) return Status::kInternal;
    switch (static_cast<kcpc::Chip>(state.chip)) {
      case kcpc::Chip::kCrtc:
        read_crtc(*machine, state.crtc);
        break;
      case kcpc::Chip::kGateArray:
        read_gate_array(*machine, state.gate_array);
        break;
      case kcpc::Chip::kPsg:
        read_psg(*machine, state.psg);
        break;
      case kcpc::Chip::kFdc:
        read_fdc(*machine, state.fdc);
        break;
      case kcpc::Chip::kAsic:
        if (!read_asic(*machine, state.asic)) return Status::kNotPresent;
        break;
    }
    state.stamp = taken;
    return Status::kOk;
  });
}

Status kcpc_host_sym_list(std::uint64_t& generation,
                          std::vector<kcpc::Symbol>& symbols) {
  auto const listed = g_symfile.listSymbols(generation);
  symbols.clear();
  symbols.reserve(listed.size());
  for (const auto& [addr, name] : listed) {
    symbols.push_back(kcpc::Symbol{addr, name});
  }
  return Status::kOk;
}

Status kcpc_host_sym_add(std::uint32_t client, std::uint32_t request,
                         const kcpc::Symbol& symbol,
                         std::uint64_t& generation) {
  if (kcpc::symbol_name_refusal(symbol.name) != nullptr) {
    return Status::kBadPayload;
  }
  {
    SymCauseScope const editing(client, request);
    g_symfile.addSymbol(symbol.addr, symbol.name);
    generation = editing.generation();
  }
  // What the in-process debugger has listed is stale.
  ui_host().debugger_symbols_changed();
  return Status::kOk;
}

Status kcpc_host_sym_del(std::uint32_t client, std::uint32_t request,
                         const std::string& name, std::uint64_t& generation) {
  {
    SymCauseScope const editing(client, request);
    if (!g_symfile.delSymbol(name)) return Status::kNotFound;
    generation = editing.generation();
  }
  ui_host().debugger_symbols_changed();
  return Status::kOk;
}

Status kcpc_host_sym_load(std::uint32_t client, std::uint32_t request,
                          const std::string& path, std::uint64_t& generation,
                          std::uint32_t& loaded, std::string& message) {
  if (!ipc_is_safe_path(path)) {
    message = "a path must not be empty or lead out through '..'";
    return Status::kBadPayload;
  }
  Symfile const file(path);
  if (!file.readable()) return Status::kNotFound;
  {
    SymCauseScope const editing(client, request);
    loaded = static_cast<std::uint32_t>(g_symfile.addSymbols(file));
    generation = editing.generation();
  }
  if (loaded != 0) ui_host().debugger_symbols_changed();
  return Status::kOk;
}

Status kcpc_host_fb_info(kcpc::FbInfo& info) {
  constexpr int kBytesPerPixel = 3;  // RGB24
  info.width = static_cast<std::uint16_t>(subcycle::kFbWidth);
  info.height = static_cast<std::uint16_t>(subcycle::kFbHeight);
  info.format = kcpc::kFbFormatRgb24;
  info.transport = kcpc::kFbTransportNone;
  info.stride = static_cast<std::uint32_t>(subcycle::kFbWidth * kBytesPerPixel);
  info.fb_seq = 0;
  info.shm_name.clear();
  return Status::kOk;
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
  if (request.space > static_cast<std::uint8_t>(kcpc::MemSpace::kAsicPage)) {
    return Status::kBadPayload;
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
    case kcpc::MemSpace::kAsicPage:
      if (!kcpc::mem_range_ok(addr, len, kcpc::kAsicPageSize)) {
        return Status::kOutOfRange;
      }
      break;
    case kcpc::MemSpace::kPhysRam:  // as large as the machine, asked below
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
      case kcpc::MemSpace::kAsicPage: {
        // The page itself, whether or not the ASIC is unlocked and has it
        // paged in at &4000.
        AsicRegs regs{};
        asic_peek(machine->asic(), &regs);
        if (regs.plugged == 0) return Status::kNotPresent;
        asic_page_copy(machine->asic(), static_cast<std::uint16_t>(addr), dst,
                       len);
        break;
      }
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

static_assert(static_cast<int>(kcpc::ConfigEffect::kNextFrame) ==
              static_cast<int>(IpcConfigEffect::NextFrame));
static_assert(static_cast<int>(kcpc::ConfigEffect::kStaged) ==
              static_cast<int>(IpcConfigEffect::Staged));

// The extension of `path`, lower case and with its dot, or nothing.
std::string lower_extension(const std::string& path) {
  auto const dot = path.find_last_of('.');
  auto const slash = path.find_last_of("/\\");
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
    return {};
  }
  std::string ext = path.substr(dot);
  for (char& c : ext) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return ext;
}

DRIVE drive_of(kcpc::MediaSlot slot) {
  switch (slot) {
    case kcpc::MediaSlot::kDriveA:
      return DRIVE::DSK_A;
    case kcpc::MediaSlot::kDriveB:
      return DRIVE::DSK_B;
    case kcpc::MediaSlot::kTape:
      return DRIVE::TAPE;
    case kcpc::MediaSlot::kCartridge:
      break;
  }
  return DRIVE::CARTRIDGE;
}

// Whether the file at `path` is of a kind `slot` takes, by its name: for an
// archive, by the name of a member. `why` says what is wrong otherwise.
bool fits_slot(kcpc::MediaSlot slot, const std::string& path,
               std::string& why) {
  std::string const accepted = drive_extensions(drive_of(slot));
  std::string const ext = lower_extension(path);
  if (ext == ".zip") {
    zip::t_zip_info archive;
    archive.filename = path;
    archive.extensions = accepted;
    if (zip::dir(&archive) != 0 || archive.filesOffsets.empty()) {
      why = "the archive holds nothing this slot takes (" + accepted + ")";
      return false;
    }
    return true;
  }
  if (extension_in_dotted_list(accepted, ext)) return true;
  why = slot == kcpc::MediaSlot::kDriveB &&
                extension_in_dotted_list(drive_extensions(DRIVE::DSK_A), ext)
            ? "a flux image goes in drive A only"
            : "this slot takes " + accepted;
  return false;
}

Status load_status(int rc) {
  if (rc == 0) return Status::kOk;
  return rc == ERR_FILE_NOT_FOUND ? Status::kNotFound : Status::kBadPayload;
}

// A disc into drive `unit`. Under a lease, alone.
Status insert_disc(std::uint8_t unit, const std::string& path) {
  // A file that is refused changes nothing, the slot's file included.
  int const rc = disc_insert(unit == 0 ? CPC.driveA : CPC.driveB, path);
  // The loader queued the machine's half for the next frame boundary. Nothing
  // is clocking the machine: give it the disc now, so that the reply is sent
  // with the disc in the drive. The disc that leaves is written back to its
  // own file there: the bridge remembers which file each drive's disc is.
  subcycle_bridge_apply_pending_media();
  return load_status(rc);
}

Status insert_tape(const std::string& path) {
  // A file that is refused changes nothing, the slot's file included.
  int const rc = tape_deck_insert(CPC.tape, path);
  subcycle_bridge_apply_pending_media();
  tape_scan_blocks();  // the block table TAPE_CONTROL seeks by
  return load_status(rc);
}

Status insert_cartridge(const std::string& path) {
  constexpr unsigned kModel6128Plus = 3;
  if (CPC.model < kModel6128Plus) return Status::kNotPresent;
  // The load and the re-attach; a file that is refused changes nothing.
  return load_status(cpc_cartridge_swap(path));
}

// The deck, as MEDIA_INFO and TAPE_CONTROL report it. Nothing is clocking
// the machine.
void read_tape(const subcycle::Machine& machine, kcpc::TapeState& out) {
  TapeRegs regs{};
  tape_peek(machine.tape(), &regs);
  out.playing = CPC.tape_play_button != 0 ? 1 : 0;
  out.motor = regs.motor != 0 ? 1 : 0;
  out.blocks = static_cast<std::uint32_t>(g_host_tape.block_offsets.size());
  out.block = regs.attached != 0 ? regs.block : 0;
}

}  // namespace

Status kcpc_host_media_insert(std::uint32_t client, std::uint32_t request,
                              const kcpc::MediaInsertRequest& insert,
                              std::uint64_t& generation, std::string& message) {
  if (!kcpc::media_slot_known(insert.slot) || insert.flags != 0) {
    return Status::kBadPayload;
  }
  if (!ipc_is_safe_path(insert.path)) {
    message = "a path must not be empty or lead out through '..'";
    return Status::kBadPayload;
  }
  auto const slot = static_cast<kcpc::MediaSlot>(insert.slot);
  // Absolute, as it goes onto the Recent list and into the events.
  std::string const path = ipc_mru_canonical_path(insert.path);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) return Status::kNotFound;
  if (!fits_slot(slot, path, message)) return Status::kBadPayload;

  MediaCauseScope const named(client, request);
  CpcRunCause const cause = cause_of(client, request);
  Status status = Status::kInternal;
  bool const ran = alone_under_lease(
      [&](bool /*was_paused*/) {
        subcycle::Machine const* machine = subcycle_bridge_machine();
        if (machine == nullptr) return;
        switch (slot) {
          case kcpc::MediaSlot::kDriveA:
          case kcpc::MediaSlot::kDriveB:
            status = insert_disc(insert.slot, path);
            break;
          case kcpc::MediaSlot::kTape:
            status = insert_tape(path);
            break;
          case kcpc::MediaSlot::kCartridge:
            status = insert_cartridge(path);
            break;
        }
      },
      [&](CpcPauseLease& lease) {
        // A new cartridge starts from a reset, as it does from the menu. Not
        // inside the change: a reset takes a lease of its own.
        if (status == Status::kOk && slot == kcpc::MediaSlot::kCartridge) {
          emulator_reset(cause);
        }
        lease.restore_run_state();
      });
  if (!ran) return Status::kBusy;
  generation = media_generation(static_cast<::MediaSlot>(insert.slot));
  if (status != Status::kOk) return status;
  switch (slot) {
    case kcpc::MediaSlot::kDriveA:
    case kcpc::MediaSlot::kDriveB:
      ipc_mru_stage(&t_CPC::mru_disks, path);
      break;
    case kcpc::MediaSlot::kTape:
      ipc_mru_stage(&t_CPC::mru_tapes, path);
      break;
    case kcpc::MediaSlot::kCartridge:
      ipc_mru_stage(&t_CPC::mru_carts, path);
      // What the in-process debugger had decoded is another program's.
      ui_host().debugger_memory_changed();
      break;
  }
  return Status::kOk;
}

Status kcpc_host_media_eject(std::uint32_t client, std::uint32_t request,
                             std::uint8_t slot_number,
                             std::uint64_t& generation, std::string& message) {
  if (!kcpc::media_slot_known(slot_number)) return Status::kBadPayload;
  auto const slot = static_cast<kcpc::MediaSlot>(slot_number);
  if (slot == kcpc::MediaSlot::kCartridge) {
    message =
        "a cartridge is replaced, not ejected: a 6128+ does not run "
        "without one";
    return Status::kBadPayload;
  }
  MediaCauseScope const named(client, request);
  Status const status = changed([&](const kcpc::Stamp& /*taken*/) {
    if (subcycle_bridge_machine() == nullptr) return Status::kInternal;
    if (slot == kcpc::MediaSlot::kTape) {
      tape_eject();
      subcycle_bridge_apply_pending_media();
      CPC.tape.file.clear();
      tape_scan_blocks();  // no tape, no blocks
      return Status::kOk;
    }
    bool const drive_a = slot == kcpc::MediaSlot::kDriveA;
    dsk_eject(drive_a ? &driveA : &driveB);
    // The machine's half now, before the slot forgets its file: that is
    // where a disc with unsaved writes is written back to.
    subcycle_bridge_apply_pending_media();
    (drive_a ? CPC.driveA : CPC.driveB).file.clear();
    return Status::kOk;
  });
  generation = media_generation(static_cast<::MediaSlot>(slot_number));
  return status;
}

Status kcpc_host_media_info(kcpc::MediaInfo& info) {
  if (!kcpc::media_slot_known(info.slot)) return Status::kBadPayload;
  auto const slot = static_cast<kcpc::MediaSlot>(info.slot);
  std::uint8_t flags = 0;
  kcpc::TapeState tape;
  Status const status = gated(sizeof(info), [&](const kcpc::Stamp& /*taken*/) {
    subcycle::Machine const* machine = subcycle_bridge_machine();
    if (machine == nullptr) return Status::kInternal;
    if (slot == kcpc::MediaSlot::kTape) {
      read_tape(*machine, tape);
      return Status::kOk;
    }
    if (slot == kcpc::MediaSlot::kCartridge) return Status::kOk;
    const Device* fdc = machine->fdc();
    std::uint8_t const unit = info.slot;
    if (fdc_media_present_unit(fdc, unit) == 0) return Status::kOk;
    // What ST3's WP line says: no image the FDC can write to (a flux
    // dump without an overlay).
    std::size_t len = 0;
    if (fdc_media_image_unit(fdc, unit, len) == nullptr) {
      flags |= kcpc::kMediaFlagWriteProtected;
    }
    if (fdc_media_dirty_unit(fdc, unit) != 0) {
      flags |= kcpc::kMediaFlagUnsaved;
    }
    if (unit == 0 && fdc_media_flux_scp(fdc, len) != nullptr) {
      flags |= kcpc::kMediaFlagFlux;
    }
    return Status::kOk;
  });
  if (status != Status::kOk) return status;
  // What the host last put in the slot, as the events say it: a lock, so not
  // on the frame runner.
  MediaState state = media_state(static_cast<::MediaSlot>(info.slot));
  info.present = state.present ? 1 : 0;
  info.flags = flags;
  info.generation = state.generation;
  info.path = std::move(state.path);
  info.tape = tape;
  return Status::kOk;
}

Status kcpc_host_tape_control(const kcpc::TapeControlRequest& control,
                              kcpc::TapeState& tape) {
  if (!kcpc::tape_action_known(control.action)) return Status::kBadPayload;
  return changed([&](const kcpc::Stamp& /*taken*/) {
    subcycle::Machine* machine = subcycle_bridge_machine();
    if (machine == nullptr) return Status::kInternal;
    // A tape somebody loaded a moment ago may still be waiting for a frame
    // boundary.
    subcycle_bridge_apply_pending_media();
    TapeRegs regs{};
    tape_peek(machine->tape(), &regs);
    if (regs.attached == 0) return Status::kNotPresent;
    constexpr byte kPlayPressed = 0x10;
    switch (static_cast<kcpc::TapeAction>(control.action)) {
      case kcpc::TapeAction::kStop:
        CPC.tape_play_button = 0;
        machine->tape_play_button(false);
        break;
      case kcpc::TapeAction::kPlay:
        // The button the F4 key and the tape window press; the deck follows
        // it at the start of every frame, and here at once.
        CPC.tape_play_button = kPlayPressed;
        machine->tape_play_button(true);
        break;
      case kcpc::TapeAction::kRewind:
        machine->tape_rewind_deck();
        break;
      case kcpc::TapeAction::kSeek:
        if (control.block >= g_host_tape.block_offsets.size()) {
          return Status::kOutOfRange;
        }
        // The deck is live wiring and must not be moved under a running
        // frame; the tape window defers its seeks to a frame boundary for
        // that. Nothing is clocking the machine here.
        machine->tape_seek(control.block);
        break;
    }
    read_tape(*machine, tape);
    return Status::kOk;
  });
}

Status kcpc_host_snapshot_save(const std::string& path, kcpc::Stamp& stamp,
                               std::string& message) {
  if (!ipc_is_safe_path(path)) {
    message = "a path must not be empty or lead out through '..'";
    return Status::kBadPayload;
  }
  std::error_code ec;
  std::filesystem::path const target = ipc_mru_canonical_path(path);
  if (std::filesystem::is_directory(target, ec)) {
    message = "the path is a directory";
    return Status::kBadPayload;
  }
  if (target.has_parent_path() &&
      !std::filesystem::is_directory(target.parent_path(), ec)) {
    message = "no such directory";
    return Status::kNotFound;
  }
  return changed([&](const kcpc::Stamp& taken) {
    if (subcycle_bridge_machine() == nullptr) return Status::kInternal;
    if (snapshot_save(target.string()) != 0) {
      message = "the snapshot could not be written";
      return Status::kInternal;
    }
    stamp = taken;
    return Status::kOk;
  });
}

Status kcpc_host_snapshot_load(std::uint32_t client, std::uint32_t request,
                               const kcpc::SnapshotLoadRequest& load,
                               kcpc::Stamp& stamp, std::string& message) {
  if (!ipc_is_safe_path(load.path)) {
    message = "a path must not be empty or lead out through '..'";
    return Status::kBadPayload;
  }
  std::string const path = ipc_mru_canonical_path(load.path);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) return Status::kNotFound;
  bool const stay_paused = (load.flags & kcpc::kSnapshotFlagStayPaused) != 0;
  CpcRunCause const cause = cause_of(client, request);
  Status status = Status::kInternal;
  bool const ran = alone_under_lease(
      [&](bool /*was_paused*/) {
        if (subcycle_bridge_machine() == nullptr) return;
        int const rc = snapshot_load(path);
        if (rc == ERR_SNA_SIZE) {
          message = "the snapshot holds more RAM than this machine has";
        }
        status = load_status(rc);
      },
      [&](CpcPauseLease& lease) {
        if (status != Status::kOk) {
          lease.restore_run_state();
          return;
        }
        // The machine is another program's now. Announced here and not in
        // the change: the announcement takes the pause mutex, which the
        // change holds.
        cpc_announce_reset(cause);
        if (stay_paused) {
          lease.release_stopped(cause);
        } else {
          lease.restore_run_state();
        }
      });
  if (!ran) return Status::kBusy;
  if (status != Status::kOk) return status;
  stamp = announced_stamp();
  ipc_mru_stage(&t_CPC::mru_snaps, path);
  ui_host().debugger_memory_changed();
  return Status::kOk;
}

Status kcpc_host_config_get(const std::string& key, kcpc::ConfigValue& value) {
  // The settings a rebuild and a CONFIG_SET rewrite, in one copy taken while
  // neither can run. The rest of the table is behind locks of its own and is
  // read on this thread.
  IpcConfigMachine machine;
  Status const status =
      gated(sizeof(machine), [&machine](const kcpc::Stamp& /*taken*/) {
        machine = ipc_config_machine();
        return Status::kOk;
      });
  if (status != Status::kOk) return status;
  IpcConfigValue got;
  switch (ipc_config_get(key, machine, got)) {
    case IpcConfigStatus::Ok:
      break;
    case IpcConfigStatus::NotReady:
    case IpcConfigStatus::NoWindow:
      return Status::kNotPresent;
    case IpcConfigStatus::UnknownKey:
    case IpcConfigStatus::BadValue:
      return Status::kNotFound;
  }
  value.value = std::move(got.value);
  value.has_pending = got.has_pending;
  value.pending = std::move(got.pending);
  return Status::kOk;
}

Status kcpc_host_config_set(const std::string& key, const std::string& value,
                            kcpc::ConfigEffect& effect, std::string& message) {
  // Under a lease whatever the key: some of them change the machine (the
  // CRTC type, the Silicon Disc's banking), and all of them are read by
  // CONFIG_GET through the gate.
  return changed([&](const kcpc::Stamp& /*taken*/) {
    IpcConfigEffect how = IpcConfigEffect::Now;
    std::string note;
    switch (ipc_config_set(key, value, how, note)) {
      case IpcConfigStatus::Ok:
        effect = static_cast<kcpc::ConfigEffect>(how);
        return Status::kOk;
      case IpcConfigStatus::NotReady:
      case IpcConfigStatus::NoWindow:
        return Status::kNotPresent;
      case IpcConfigStatus::BadValue:
        message = std::move(note);
        return Status::kBadPayload;
      case IpcConfigStatus::UnknownKey:
        break;
    }
    return Status::kNotFound;
  });
}

Status kcpc_host_config_apply(std::uint32_t client, std::uint32_t request,
                              kcpc::Stamp& stamp, std::string& message) {
  // No lease here: the main thread takes its own for the rebuild, and a
  // rebuild is refused while it is not the only one.
  g_apply_client.store(client, std::memory_order_release);
  g_apply_request.store(request, std::memory_order_release);
  int code = 0;
  IpcRebuildResult const result = ipc_config_apply(code);
  g_apply_request.store(0, std::memory_order_release);
  g_apply_client.store(0, std::memory_order_release);
  switch (result) {
    case IpcRebuildResult::Ok:
      stamp = announced_stamp();
      return Status::kOk;
    case IpcRebuildResult::NotDrained:
      message = "the main loop did not take the rebuild";
      return Status::kBusy;
    case IpcRebuildResult::StillRunning:
      message = "the rebuild is still running";
      return Status::kBusy;
    case IpcRebuildResult::NotIdle:
      return Status::kBusy;
    case IpcRebuildResult::Failed:
      message = "the machine could not be rebuilt (code " +
                std::to_string(code) + "); it is left paused";
      break;
  }
  return Status::kInternal;
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

kcpc::Status kcpc_host_key(std::uint8_t row, std::uint8_t bit, bool down) {
  kbd_matrix_set_line(keyboard_matrix, row, bit, down);
  return Status::kOk;
}

kcpc::Status kcpc_host_keys_release() {
  kbd_matrix_release_all(keyboard_matrix);
  return Status::kOk;
}

kcpc::Status kcpc_host_text(const std::string& text, std::uint32_t& pending,
                            std::string& message) {
  std::string error = g_autotype_queue.enqueue(text);
  if (!error.empty()) {
    message = std::move(error);
    return Status::kBadPayload;
  }
  pending = static_cast<std::uint32_t>(g_autotype_queue.remaining());
  return Status::kOk;
}

kcpc::Status kcpc_host_mouse(int dx, int dy, bool set_buttons,
                             std::uint8_t buttons) {
  // The protocol's button bits are SDL's, which the staging takes.
  std::optional<uint32_t> mask;
  if (set_buttons) mask = buttons;
  return ipc_stage_mouse(dx, dy, mask) ? Status::kOk : Status::kNotPresent;
}

kcpc::Status kcpc_host_input_state(kcpc::KeyMatrix& rows) {
  static_assert(kcpc::kKeyRows == kKbdMatrixRows,
                "INPUT_STATE is the matrix, row for row");
  kbd_matrix_read(keyboard_matrix, rows.data());
  return Status::kOk;
}

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
  config.bp_list = &kcpc_host_bp_list;
  config.bp_add = [](const kcpc::Cause& cause, const kcpc::BpEntry& entry,
                     std::uint64_t& generation, std::string& message) {
    return kcpc_host_bp_add(cause.client, cause.request, entry, generation,
                            message);
  };
  config.bp_del = [](const kcpc::Cause& cause, const kcpc::BpDelRequest& del,
                     std::uint64_t& generation) {
    return kcpc_host_bp_del(cause.client, cause.request, del, generation);
  };
  config.bp_clear = [](const kcpc::Cause& cause, std::uint8_t kind,
                       std::uint64_t& generation) {
    return kcpc_host_bp_clear(cause.client, cause.request, kind, generation);
  };
  config.key = &kcpc_host_key;
  config.keys_release = &kcpc_host_keys_release;
  config.text = &kcpc_host_text;
  config.mouse = &kcpc_host_mouse;
  config.input_state = &kcpc_host_input_state;
  config.chip_get = &kcpc_host_chip_get;
  config.sym_list = &kcpc_host_sym_list;
  config.sym_add = [](const kcpc::Cause& cause, const kcpc::Symbol& symbol,
                      std::uint64_t& generation) {
    return kcpc_host_sym_add(cause.client, cause.request, symbol, generation);
  };
  config.sym_del = [](const kcpc::Cause& cause, const std::string& name,
                      std::uint64_t& generation) {
    return kcpc_host_sym_del(cause.client, cause.request, name, generation);
  };
  config.sym_load = [](const kcpc::Cause& cause, const std::string& path,
                       std::uint64_t& generation, std::uint32_t& loaded,
                       std::string& message) {
    return kcpc_host_sym_load(cause.client, cause.request, path, generation,
                              loaded, message);
  };
  config.fb_info = &kcpc_host_fb_info;
  config.media_insert = [](const kcpc::Cause& cause,
                           const kcpc::MediaInsertRequest& insert,
                           std::uint64_t& generation, std::string& message) {
    return kcpc_host_media_insert(cause.client, cause.request, insert,
                                  generation, message);
  };
  config.media_eject = [](const kcpc::Cause& cause, std::uint8_t slot,
                          std::uint64_t& generation, std::string& message) {
    return kcpc_host_media_eject(cause.client, cause.request, slot, generation,
                                 message);
  };
  config.media_info = &kcpc_host_media_info;
  config.tape_control = &kcpc_host_tape_control;
  config.snapshot_save = &kcpc_host_snapshot_save;
  config.snapshot_load = [](const kcpc::Cause& cause,
                            const kcpc::SnapshotLoadRequest& load,
                            kcpc::Stamp& stamp, std::string& message) {
    return kcpc_host_snapshot_load(cause.client, cause.request, load, stamp,
                                   message);
  };
  config.config_get = &kcpc_host_config_get;
  config.config_set = &kcpc_host_config_set;
  config.config_apply = [](const kcpc::Cause& cause, kcpc::Stamp& stamp,
                           std::string& message) {
    return kcpc_host_config_apply(cause.client, cause.request, stamp, message);
  };
  config.media_state = [](std::uint8_t slot, kcpc::MediaNote& note) {
    if (slot >= kcpc::kMediaSlots) return;
    MediaState state = media_state(static_cast<::MediaSlot>(slot));
    note.present = state.present;
    note.path = std::move(state.path);
  };
  config.controller_idle_timeout_ms = controller_idle_timeout_ms();
  config.max_event_queue_bytes = static_cast<std::size_t>(env_int(
      "KONCPC_KCPC_EVENT_QUEUE_BYTES", 4 << 20, /*min=*/1, /*max=*/32 << 20));
  cpc_set_run_state_observer(&on_run_event);
  z80_set_breakpoints_changed_hook(&on_breakpoints_changed);
  g_symfile.set_changed_hook(&on_symbols_changed);
  media_set_changed_hook(&on_media_changed);
  cpc_set_machine_rebuilt_hook(&on_machine_rebuilt);
  g_gate.reopen();
  walker_start();
  g_kcpc_server.start(config);
}

void kcpc_host_stop() {
  // First the gate, so a reader waiting for a frame boundary answers
  // SHUTTING_DOWN at once and its thread can be joined.
  g_gate.shut_down();
  bool const was_running = g_kcpc_server.running();
  g_kcpc_server.stop();
  // After the server: no reader is left to arm a step. The walk in progress
  // is ended, not waited for.
  walker_stop();
  z80_set_breakpoints_changed_hook(nullptr);
  g_symfile.set_changed_hook(nullptr);
  media_set_changed_hook(nullptr);
  cpc_set_machine_rebuilt_hook(nullptr);
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
