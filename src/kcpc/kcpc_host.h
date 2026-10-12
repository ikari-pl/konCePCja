#pragma once

// The seam between the KCPC binary server and the emulator: the one global
// server instance, and the code that reads and changes the machine for it.
//
// kcpc_server.cpp knows nothing about the emulator; everything that touches
// CPC state for the binary protocol lives behind this header and follows the
// threading model of docs/protocol/kcpc-binary-v1.md section 8.

#include <cstddef>
#include <cstdint>

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
