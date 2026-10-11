#pragma once

// The seam between the KCPC binary server and the emulator: the one global
// server instance, and the code that reads the machine for it.
//
// kcpc_server.cpp knows nothing about the emulator; everything that touches
// CPC state for the binary protocol lives behind this header and follows the
// threading model of docs/protocol/kcpc-binary-v1.md section 8.

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

// Reads the MACHINE_INFO fields under a pause lease, on the calling (socket)
// thread, and restores the run state the lease found. False when the frame
// runner did not go idle in time: nothing was read and the request must
// answer BUSY.
bool kcpc_host_machine_info(kcpc::MachineInfo& info);
