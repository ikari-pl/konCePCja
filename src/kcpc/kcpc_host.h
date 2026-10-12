#pragma once

// The seam between the KCPC binary server and the emulator: the one global
// server instance, and the code that reads and changes the machine for it.
//
// kcpc_server.cpp knows nothing about the emulator; everything that touches
// CPC state for the binary protocol lives behind this header and follows the
// threading model of docs/protocol/kcpc-binary-v1.md section 8.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "kcpc/kcpc_proto.h"

// Starts the server on the first free port from 6545, unless `enabled` is
// false ([ipc] binary_port_enabled=0): then no socket is created and no
// thread starts. Call once the machine is built, so a client that connects at
// once reads a real machine.
void kcpc_host_start(bool enabled);

// Tells the clients the core is shutting down and joins every server thread.
// Call while the frame runner is still alive: a request in flight may be
// waiting for it to go idle.
void kcpc_host_stop();

// The bound port, or 0 when the server is not running.
int kcpc_host_port();

// The frame runner's half of the machine gate (kcpc_gate.h): counts the frame
// for the stamps and runs the reads clients have queued. emu_run_frame()'s
// epilogue calls it once per completed frame, on the runner, while the machine
// is not being clocked. With no read queued it is an increment and a load; it
// never makes a socket call, waits on a client or allocates.
void kcpc_service_frame();

// The machine reads behind MACHINE_INFO, REGS_GET, MEM_INFO and MEM_READ.
// Each goes through the gate: on a running machine the runner takes it at the
// next frame boundary, on a paused one the calling (socket) thread takes it
// under a pause lease, and either way the run state is left as it was. They
// return the status the request answers with (kcpc_server.h).
kcpc::Status kcpc_host_machine_info(kcpc::MachineInfo& info);
kcpc::Status kcpc_host_regs_get(kcpc::Stamp& stamp, kcpc::Regs& regs);
kcpc::Status kcpc_host_mem_info(kcpc::MemInfo& info);
kcpc::Status kcpc_host_mem_read(const kcpc::MemReadRequest& request,
                                kcpc::Stamp& stamp, std::uint8_t* dst);

// The machine changes behind REGS_SET and MEM_WRITE (seam 1 of section 8).
// Each runs on the calling (socket) thread under a pause lease, while that
// lease is the only one held, and puts the run state back: a running machine
// runs on, a paused one stays paused. kBusy when the runner did not go idle
// or other lease holders kept the machine; nothing was changed then.
//
// `mask` selects fields of `values` (kcpc_proto.h); `after` is the register
// block once the write has been applied.
kcpc::Status kcpc_host_regs_set(std::uint32_t mask, const kcpc::Regs& values,
                                kcpc::Stamp& stamp, kcpc::Regs& after);
// Writes `len` bytes to CPU_RAM or PHYS_RAM. All of them or none: a range
// that does not fit the space is kOutOfRange and writes nothing.
kcpc::Status kcpc_host_mem_write(const kcpc::MemWriteRequest& request,
                                 const std::uint8_t* data, std::size_t len,
                                 kcpc::Stamp& stamp);

// Run control (section 12): RUN_STATE, PAUSE, RUN, RESET, STEP. None waits
// for emulated time. `client` and `request` name the request in the events
// it leads to. What each one settles is announced by the core
// (cpc_set_run_state_observer() in koncepcja.h) and pushed to the clients
// from there, so a pause from the text IPC or the GUI is pushed the same way.
//
// The run state as last announced and whether a step is in progress. Reads
// two atomics: safe to poll beside a running machine.
kcpc::Status kcpc_host_run_state(kcpc::Stamp& stamp, bool& walk_active);
// Stops the machine and ends a step in progress. Returns once the frame in
// flight has ended, so the stamp is that of a stopped machine.
kcpc::Status kcpc_host_pause(std::uint32_t client, std::uint32_t request,
                             kcpc::Stamp& stamp);
// Runs the machine. Never refused: while another actor holds a pause lease
// the machine runs when the last holder lets go, and the stamp still says
// paused.
kcpc::Status kcpc_host_run(std::uint32_t client, std::uint32_t request,
                           kcpc::Stamp& stamp);
// Resets the CPC and leaves it running, or stopped when `stay_paused`.
kcpc::Status kcpc_host_reset(std::uint32_t client, std::uint32_t request,
                             bool stay_paused, kcpc::Stamp& stamp);
// Mode 0 single-steps `count` instructions before it returns; mode 4 arms
// `count` frames and returns. kBusy while a step is in progress.
// Modes 1 to 3 (over, out, to) pause the machine, hand the step to the
// walker thread and return; kLimitReached when the probe has no execution
// comparator left for the one-shot breakpoint such a step runs to.
kcpc::Status kcpc_host_step(std::uint32_t client, std::uint32_t request,
                            const kcpc::StepRequest& step);

// Breakpoints (section 12): the editing lists of z80_view.h, which the text
// IPC and the DevTools edit too. They take the lists' own lock, which the
// frame runner holds for the length of a machine frame: on a running machine
// a call waits for the frame in flight, so none of this is for polling. An
// edit from any source is pushed as EVT_BREAKPOINTS_CHANGED.
kcpc::Status kcpc_host_bp_list(std::uint64_t& generation,
                               std::vector<kcpc::BpEntry>& entries);
// Adds the entry or replaces the one with the same kind and address (and
// mask). kBadPayload for a condition that does not parse, with the parser's
// words in `message`; kLimitReached when the probe would not arm it.
kcpc::Status kcpc_host_bp_add(std::uint32_t client, std::uint32_t request,
                              const kcpc::BpEntry& entry,
                              std::uint64_t& generation, std::string& message);
kcpc::Status kcpc_host_bp_del(std::uint32_t client, std::uint32_t request,
                              const kcpc::BpDelRequest& del,
                              std::uint64_t& generation);
kcpc::Status kcpc_host_bp_clear(std::uint32_t client, std::uint32_t request,
                                std::uint8_t kind, std::uint64_t& generation);
