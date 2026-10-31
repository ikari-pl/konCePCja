# KCPC binary frontend protocol, version 1

**Status: implemented; phase 3 is complete.** This is the contract for
phase 3 of the headless-core epic (beads-cv2, bead beads-3o1), and every
opcode, event and capability bit below is in the server. The slices it was
built in are listed in [section 16](#16-implementation-slices), and the test
that closes the phase is its
[exit criterion](#exit-criterion-of-phase-3). A server tells a client what
it implements through the `caps` mask of `HELLO`. The server announces
protocol version 0, and the layouts below may still change, until phase 5
(the remote GUI) has shipped; see [section 6](#6-versioning-and-hello).
Client and server must come from the same checkout until then.

The text IPC on port 6543 ([ipc-protocol.md](../ipc-protocol.md)) stays as it
is. This is a second, separate server.

## 1. What this protocol is for

A *frontend* is a separate process that shows and drives the emulated CPC: a
remote copy of the ImGui GUI (`--connect`, phase 5), a test script, a tool.
The text IPC cannot serve one, for three reasons:

- it talks to one client at a time;
- it never sends anything unasked, so a client has to poll (`wait bp`) to
  learn that a breakpoint fired;
- every byte of memory travels as two ASCII hex digits.

KCPC fixes those three things and nothing else:

| Need | Answer |
|------|--------|
| Several clients at once | Up to 8 connections, one *controller* and any number of *observers* ([section 7](#7-clients-roles-and-control)) |
| Push events | The server sends *event* frames the client subscribed to ([section 9](#9-events)) |
| Binary bulk data | Length-prefixed binary frames; 64 KiB of memory is one reply ([section 11](#11-memory)) |

The first real client is the DevTools of a remote GUI, so the protocol carries
what those windows show: Z80 registers, memory through every banking view,
chip state (CRTC, Gate Array, PSG, FDC, ASIC), breakpoints and watchpoints,
stepping, symbols, and input.

### Out of scope for version 1

- **Video frames.** They travel through shared memory in phase 4 (beads-0ir):
  every core publishes each frame to the ring [fb-ring-v1.md](fb-ring-v1.md)
  describes (beads-0ir.2). This protocol only describes the frame and names
  the ring (`FB_INFO`) and announces each new frame with its number in the
  ring (`EVT_FRAME`, beads-0ir.3).
- **Audio.** Decision D4: none for remote frontends at first.
- **Recording, disc tools (files and sectors inside a disc image), pokes,
  sessions, the M4 board.** They stay on the text IPC, which a frontend may
  use alongside this port. Loading media, snapshots and configuration *are*
  here ([section 12](#media)), because a remote GUI needs them on its one
  connection.
- **`Machine::save_devices()`.** It is an unversioned format for disposable
  measurement runs and is never exposed here. Snapshots are SNA files.
- **Disassembly.** The client disassembles the bytes it reads. One Z80
  disassembler per frontend is cheaper than a wire format for listings.
- **Network exposure, authentication, encryption.** See
  [section 15](#15-security).

## 2. Terms

- **Core**: the emulator process. It owns the machine.
- **Machine**: the emulated CPC (`subcycle::Machine` and the host state around
  it).
- **Runner**: the one thread that runs emulated frames. In the GUI build it is
  the Z80 thread; under `-H` it is the main thread.
- **Frame**: one 50 Hz picture of emulated time, about 20 ms.
- **Frame boundary**: the point between two frames where the runner is in
  `emu_run_frame()`'s epilogue and the machine is not being clocked.
- **Pause lease**: `CpcPauseLease`, the existing RAII object that pauses the
  machine and waits until the runner is idle. While one is held, nothing
  clocks the machine.
- **Probe**: the bus probe Device (`src/hw/probe.h`), which watches the bus
  and latches breakpoint, watchpoint and I/O hits.
- **Client**: one TCP connection that has completed `HELLO`.

## 3. Transport and discovery

- TCP, bound to the loopback address `127.0.0.1` only.
- Default port **6545**. If it is taken the server tries the next one, up to
  6554, exactly like the text IPC does from 6543. The text IPC and the telnet
  console probe through the same numbers, so on a machine with a stale
  instance 6545 may belong to another server. **Never assume the port.**
- The startup manifest gains a line under `ports:` and `manifest_version`
  becomes 3:

  ```yaml
  ports:
    ipc: 6543
    telnet: 6544
    binary: 6545          # null = the binary server is not running
    m4_http: null
  ```

  In `StartupManifest` the field is `binary_port`. The server binds its
  port before `start()` returns, so the manifest reads it directly and does
  not have to wait for a server thread as it does for the other ports.
- The server starts after the configuration is read and the machine is
  built, just before the manifest is printed. A client that connects the
  moment it sees the manifest gets a `HELLO` reply that describes the real
  machine. (The text IPC starts earlier and has a window in which the
  machine is not there yet.)
- The server is **on by default**. One configuration key turns it off:

  ```ini
  [ipc]
  binary_port_enabled=1   # 1 (default) = start the binary server on 6545;
                          # 0 = do not open the port at all
  ```

  With `0` no socket is created, no server thread starts, and the manifest
  prints `binary: null`. The key is read once at startup, so changing it
  needs a restart; `-O ipc.binary_port_enabled=0` works for a single run. It
  does not affect the text IPC or the telnet console.
- The server runs in the GUI build, under `-H`, and in the UI-free build
  (`KONCPC_MODERN_UI=0`). It uses `std::thread` and BSD sockets (Winsock on
  Windows) and nothing from SDL (decision D5).

## 4. Framing

All integers are **little-endian**. Fields are packed with no padding. Both
ends read and write field by field; nobody casts a buffer to a struct, so
compiler padding and host byte order never matter.

Every frame is a 16-byte header followed by `length` bytes of payload:

| Offset | Size | Field | Meaning |
|-------:|-----:|-------|---------|
| 0 | 4 | `magic` | The bytes `4B 43 50 43`, ASCII `KCPC` |
| 4 | 1 | `version` | Protocol major version (section 6) |
| 5 | 1 | `kind` | 1 = request, 2 = response, 3 = event |
| 6 | 2 | `opcode` | Section 13 |
| 8 | 4 | `id` | Request id, or event sequence number |
| 12 | 4 | `length` | Payload bytes that follow; may be 0 |

Rules:

- **Requests** go from client to server. The client picks `id`; any value
  except 0, and it should not reuse one that is still unanswered. Id 0
  belongs to `ERROR` (section 5): a request with `id` 0 is a bad frame.
- Requests are the only kind a client may send. A response or an event sent
  by a client is a bad frame.
- **Responses** go from server to client and carry the `opcode` and `id` of
  the request they answer. Every request gets exactly one response.
- **Events** go from server to client. Their `id` is a per-connection sequence
  number that starts at 1 and grows by 1 for every event sent on that
  connection.
- Events may arrive between a request and its response. Match responses by
  `id`, never by position in the stream.
- An event that a client's own request caused is sent to that client after
  the response to the request, never before it: the reply to `PAUSE` comes
  first and its `EVT_STOPPED` second. Other clients may receive the event
  while the request is still being served.
- The server answers one connection's requests in the order they arrived, one
  at a time. A client may send several requests without waiting (pipelining).
- Maximum payload a client may send: **1 MiB**. Maximum payload the server
  sends: the smaller of 16 MiB and the `max_frame` the client gave in `HELLO`.
- There is no resynchronisation. TCP already delivers bytes in order; the
  magic exists to catch a client that connected to the wrong port. On a bad
  magic, a wrong `version`, an unknown `kind` or an oversized `length`, the
  server sends one `ERROR` frame (section 5) and closes the connection. The
  header is checked in wire order and the first fault is the one reported.
  An oversized `length` is refused from the header alone: the server never
  waits for, or allocates, the payload it announces.

### Common field types

| Type | Layout |
|------|--------|
| `u8`, `u16`, `u32`, `u64` | Unsigned, little-endian |
| `i16`, `i32` | Two's complement, little-endian |
| `str` | `u16` byte count, then that many UTF-8 bytes, no terminator |
| `bytes` | The rest of the payload |
| `stamp` | 12 bytes: `u64 frame`, `u8 run_state` (0 running, 1 paused), 3 zero bytes |

A `stamp` says when a read was taken: `frame` is the count of completed
frames since the core started. Two replies with the same `frame` taken while
`run_state` is 1 describe the same instant.

### Growing a payload

A later minor version may append fields to any payload. A receiver must
ignore payload bytes beyond the fields it knows. A sender must not shorten a
payload or reorder fields; that needs a new major version.

## 5. Responses and errors

Every response payload starts with:

| Size | Field | Meaning |
|-----:|-------|---------|
| 2 | `status` | 0 = success |
| 2 | reserved | 0 |

On success the opcode's reply fields follow. On failure a `str message`
follows: English, for logs and humans. Clients act on `status`, never on the
text.

| Status | Name | Meaning | Connection |
|-------:|------|---------|------------|
| 0 | `OK` | | stays open |
| 1 | `BAD_FRAME` | Bad magic, kind or length | closed |
| 2 | `UNSUPPORTED_VERSION` | Major version mismatch | closed |
| 3 | `HELLO_REQUIRED` | First frame was not `HELLO`, or a second `HELLO` | closed |
| 4 | `TOO_MANY_CLIENTS` | 8 clients are already connected | closed |
| 5 | `UNKNOWN_OPCODE` | Not implemented by this server | stays open |
| 6 | `BAD_PAYLOAD` | Payload too short or a field has an invalid value | stays open |
| 7 | `NOT_CONTROLLER` | The request changes the machine and the client is an observer | stays open |
| 8 | `BUSY` | The runner did not go idle in time, another pause lease kept the machine, or a step is already in progress (a client's, the text IPC's or the DevTools') | stays open |
| 9 | `OUT_OF_RANGE` | Address, length, bank or index outside what exists | stays open |
| 10 | `TOO_LARGE` | The reply would exceed the frame limit | stays open |
| 11 | `NOT_PRESENT` | The hardware is not fitted (ASIC on a 6128, a mouse that is off) | stays open |
| 12 | `NOT_FOUND` | No such breakpoint, symbol or file | stays open |
| 13 | `LIMIT_REACHED` | The probe has no comparator left for it (it holds 32 exec, 16 watch, 8 I/O): a breakpoint that would be listed and never fire, or a step that could not plant its one-shot breakpoint | stays open |
| 14 | `SHUTTING_DOWN` | The core is quitting | closed |
| 15 | `INTERNAL` | A bug; the message says what | stays open |
| 16 | `IDLE_TIMEOUT` | The controller sent no frame for 30 s ([section 7](#7-clients-roles-and-control)) | closed |

`BUSY` maps to the text IPC's `ERR 409 z80-not-idle`: the request changed
nothing and the machine runs or stays paused as before.

**`ERROR` (opcode `0x0000`)** is a response the server sends on its own,
with `id` 0, right before it closes a connection it cannot serve: a bad
frame, a ninth client, a controller that went silent, shutdown. Its payload
is the same `status` and `message`.

The server makes sure the `ERROR` arrives. Closing a TCP connection that
still has unread input makes the operating system send a reset, and a reset
can destroy data the other side has not read yet; a client that had already
sent more requests would then never see why it was dropped. So after
`ERROR` the server closes only its sending direction, keeps reading and
discarding for up to half a second, and closes fully when the client has
closed or the time is up. A client that receives `ERROR` should close its
socket; it will read end-of-stream right after the frame.

A core that is quitting tells a client twice, in this order: `EVT_SHUTDOWN`
(an event, so a client that is waiting for events sees it), then `ERROR`
with status `SHUTTING_DOWN` (so a client that is waiting for a response
sees it), then end of stream. A connection that has not completed `HELLO`
is promised no events and gets the `ERROR` alone.

## 6. Versioning and `HELLO`

The first frame on a connection must be a `HELLO` request. The server sends
nothing until then.

Request:

| Size | Field | Meaning |
|-----:|-------|---------|
| 2 | `client_minor` | Highest minor version the client understands |
| 1 | `role` | 0 = observer, 1 = controller |
| 1 | reserved | 0 |
| 4 | `max_frame` | Largest payload the client accepts; 0 means 16 MiB |
| 8 | `want_caps` | Capability bits the client intends to use; informational |
| `str` | `client_name` | For logs, e.g. `koncepcja-gui 6.5.0` |

Reply:

| Size | Field | Meaning |
|-----:|-------|---------|
| 2 | `server_minor` | |
| 1 | `role` | The role granted; observer if a controller already exists |
| 1 | reserved | 0 |
| 4 | `max_frame` | The limit the server will respect |
| 8 | `caps` | Capability bits this server implements |
| 4 | `client_id` | Unique for the life of the core; never 0 |
| 4 | `pid` | The core's process id |
| `str` | `server_version` | e.g. `v6.5.0` |
| ... | machine info | The `MACHINE_INFO` reply fields (section 10) |

- A `HELLO` the server refuses with a status that keeps the connection open
  (`BAD_PAYLOAD`, `BUSY`) has not happened: the connection is still waiting
  for its first `HELLO`, and the client may send it again. `BUSY` here means
  the machine did not go idle in time for the server to read the machine
  information.
- `role` must be 0 or 1 (`BAD_PAYLOAD` otherwise). A client that asks for
  control gets it only if nobody has it; the reply says which role it was
  given ([section 7](#7-clients-roles-and-control)). The control role comes
  with `CAP_MACHINE_WRITE`: a server that does not announce that bit has no
  controller and admits everyone as an observer.
- `max_frame` must be 0 or at least **128 KiB** (131072; `BAD_PAYLOAD`
  otherwise), so that a 64 KiB memory read always fits one reply
  (section 11). The reply carries the smaller of the client's value and
  16 MiB.
- `client_name` is written to the core's log, cut to 64 characters, with
  anything that is not printable ASCII replaced.
- The **major** version is the `version` byte of every frame. A server that
  does not speak the client's major answers `UNSUPPORTED_VERSION` and closes.
- **Major 0 is the development version, and it is the only one for now.**
  The server speaks major 0 and any payload may change between commits;
  client and server must come from the same checkout. Nothing outside this
  repository should be built against it.
- **Major 0 lasts until phase 5 has shipped** (beads-3sp, the ImGui GUI with
  `--connect`). Phase 3 ends with the exit test and finished documentation,
  not with a freeze: the remote GUI is the first client that uses the whole
  protocol, and the layouts are frozen only after it has. The switch to
  major 1, minor 0 is a step of its own after phase 5.
- From major 1 on, a **minor** bump only adds: new opcodes, new events, new
  capability bits, fields appended to payloads. Old clients keep working.
  While the major is 0 the `minor` fields are sent as 0 and ignored.

### Capability bits

A bit is set when the server implements the group. An opcode from a group
whose bit is clear answers `UNKNOWN_OPCODE`. This is what lets the slices
ship one at a time.

| Bit | Name | Group |
|----:|------|-------|
| 0 | `CAP_MACHINE_READ` | `REGS_GET`, `MEM_INFO`, `MEM_READ` |
| 1 | `CAP_MACHINE_WRITE` | `REGS_SET`, `MEM_WRITE`, `CONTROL` |
| 2 | `CAP_RUN_CONTROL` | `PAUSE`, `RUN`, `RESET`, `STEP`, `RUN_STATE` |
| 3 | `CAP_EVENTS` | `SUBSCRIBE` and every event |
| 4 | `CAP_BREAKPOINTS` | `BP_*` |
| 5 | `CAP_INPUT` | `KEY`, `KEYS_RELEASE`, `TEXT`, `MOUSE`, `INPUT_STATE` |
| 6 | `CAP_CHIPS` | `CHIP_GET`, memory space `ASIC_PAGE` |
| 7 | `CAP_SYMBOLS` | `SYM_*` |
| 8 | `CAP_FB_INFO` | `FB_INFO` |
| 9 | `CAP_FB_SHM` | `FB_INFO` names the shared-memory frame ring; announced only while the core publishes it |
| 10 | `CAP_MEM_ROM` | Memory spaces `ROM_LOWER` and `ROM_UPPER` |
| 11 | `CAP_MEDIA` | `MEDIA_*`, `TAPE_CONTROL` |
| 12 | `CAP_SNAPSHOT` | `SNAPSHOT_SAVE`, `SNAPSHOT_LOAD` |
| 13 | `CAP_CONFIG` | `CONFIG_GET`, `CONFIG_SET`, `CONFIG_APPLY` |
| 14 | `CAP_GUN` | `GUN` |

`PING` (empty request, empty reply) needs no capability.

## 7. Clients, roles and control

Up to **8** clients. A ninth connection receives `ERROR` with
`TOO_MANY_CLIENTS` and is closed. It is refused as soon as it connects; it
does not have to send `HELLO` to find out.

A connection takes one of the 8 places when it is accepted, not when its
`HELLO` completes: otherwise connections that never speak would cost the
core threads without limit. In return a connection that has not completed
`HELLO` within **5 s** is closed with `ERROR` and `HELLO_REQUIRED`, so it
cannot keep a real client out for long. Once `HELLO` has completed there is
no such deadline. A place is free again as soon as the server has noticed
that its connection closed.

Each client has a role:

- An **observer** may read (registers, memory, chips, breakpoint and symbol
  lists, run state), subscribe to events and `PING`.
- The **controller** may also change things: write registers and memory, run,
  pause, step, reset, edit breakpoints and symbols, send input, load and
  eject media, load snapshots, change configuration.

At most one client is the controller.

- `HELLO` with `role = 1` gets control if nobody has it. Otherwise the client
  is admitted as an observer and the reply says so. `HELLO` never takes
  control away from anyone.
- `CONTROL` with `action = 1` (acquire) asks for control later. Without the
  force flag it succeeds only when nobody has it. `action = 0` gives it up.
- **Takeover: the newest request wins.** `CONTROL` acquire with `force = 1`
  always succeeds. The previous controller is not disconnected: it becomes an
  observer on the spot, any request of its that has not started yet answers
  `NOT_CONTROLLER`, and the keys and mouse buttons it was holding down are
  released ([section 12](#input)). It may force control back the same way.
  No client is special: the GUI, a test script and a tool all follow this
  one rule.
- Control is also released when the controller disconnects, sends
  `CONTROL release`, or goes quiet: a controller that has sent no frame for
  **30 s** is disconnected, with `ERROR` and `IDLE_TIMEOUT`, and control is
  free at that moment. A controller with nothing to say sends `PING` every
  10 s. The 30 s count from the reply to its last request, so the time the
  core took to serve a request is not held against the client, and they
  count only while it is the controller: an observer is never timed out,
  and one that takes control after an hour of silence starts with a full
  30 s.
- Every change of controller is announced to **all** clients, the demoted
  one included, with `EVT_CONTROL_CHANGED` carrying the new controller's
  `client_id` (0 when control became free). The event ignores subscription
  masks, so a client always learns that it lost control.
- A step that the previous controller started keeps running across a
  takeover. Its `EVT_STOPPED` still goes to the client that asked for it, and
  the new controller may end it with `PAUSE`.

A controller that loses control in any of these ways (takeover, release,
disconnection, silence) has the keys and mouse buttons it holds released, and
they are up before `EVT_CONTROL_CHANGED` is sent: a client that reads
`INPUT_STATE` after the event sees them released.

A client that wants to know who has control without waiting for an event
asks:
`RUN_STATE` names the controller, and so does the reply to a `CONTROL`
acquire without force, which changes nothing while another client has
control.

A request that needs control is refused with `NOT_CONTROLLER` before its
payload is looked at: an observer learns nothing from a malformed write but
that it may not write.

A polite client asks without force first and uses force only when its user
asked for it; two clients that both force in a loop will take control from
each other for ever, and the protocol does not try to stop them.

Control only orders the binary clients among themselves. It is not a lock on
the machine: the text IPC, the telnet console, and in the GUI build the user
at the keyboard and the in-process DevTools can all pause, step and edit as
they do today. Clients learn about those changes from events, whoever caused
them.

## 8. Threading model

The rule that everything else follows from: **only the runner clocks the
machine, and nobody reads or writes machine state while a frame is running.**
The server adds no new way to touch the machine. It uses the three seams the
core already has.

### Threads the server owns

| Thread | Count | Job |
|--------|------:|-----|
| Acceptor | 1 | `accept()`, enforces the client limit, joins the threads of connections that have ended |
| Reader | 1 per client | Reads frames, runs requests, queues responses; starts and joins its writer and closes the socket |
| Writer | 1 per client | Sends that client's queued responses and events |
| Fan-out | 1 | Takes events from the ring and copies them to subscribed clients |
| Walker | 1 | Runs steps that walk the machine (step over, out, to) |

At most 19 threads with 8 clients. They wait in `poll()` (`select()` on
Windows) for at most 50 ms at a time or on a condition variable; none of
them spins. (The walker sleeps on a condition variable until a step is
armed. While it walks it looks at the machine every millisecond, as the text
IPC's walks do.) The sockets are non-blocking, so no thread is ever inside a
`recv()` or `send()` that only the other side can end, and each one sees
its stop flag within 50 ms.

A connection refused for being over the limit gets the same two threads for
the second it takes to deliver its `ERROR`. At most 8 such connections are
handled at once; beyond that a refused connection is simply closed.

### Seam 1: the pause lease, for every change

Every request that changes the machine runs on the client's reader thread
under a `CpcPauseLease`, the same way the text IPC's `reg set` and `reset`
do (the one exception is a write to a running machine that clients read or
that runs unpaced, which the runner takes at a frame boundary: see *Writes
beside reads* and *Writes made together* below):

1. Take the lease. If `idle()` is false, call `restore_run_state()` and
   answer `BUSY`.
2. Make the change through the existing host functions:
   `subcycle_bridge_regs_to_machine()` for registers, `z80_write_mem()` and
   `Machine::ram_write()` for memory, `emulator_reset()`,
   `z80_step_instruction()`.
3. Let go of the lease with the one call that says what the request means:
   - `restore_run_state()`: the request borrowed the machine, and what was
     there before is put back (a register or memory write).
   - `release_stopped()`: the request is a stop somebody asked for (a step).
   - `release_running()`: the request promises a running machine (a reset
     that resumes, the start of a frame step).

Leases overlap as soon as there is more than one client: two connections
here, a connection and the text IPC, the IPC and the GUI. Three rules keep
that safe, and the first slice had to add all three to `CpcPauseLease`,
because until then only one holder at a time had been expected:

- Every holder of overlapping leases is told the run state from before the
  first of them (`was_paused()`), and the holder that lets go last puts it
  back. A holder never takes "another lease has paused the machine" for
  "the machine was paused".
- A real stop that arrives while a lease is held (a breakpoint in the frame
  the lease was waiting out, the end of a frame step, somebody's pause)
  stands. `restore_run_state()` does not run the machine again after it.
- A lease is not a stop. Code that waits for emulated time to pass asks
  `cpc_stopped()` (paused, and no lease held), not `CPC.paused`.

Each of the three calls lets go and settles the run state in one step, under
the pause mutex. Doing it by hand (drop the lease, then call `cpc_resume()`
or nothing) is wrong as soon as there is a second actor, in three ways:

- It resumes over a real stop that arrived while the lease was held.
- Between the two halves the machine is paused with no lease held, which is
  what a real stop looks like to `cpc_stopped()`; a frame step waiting at
  that instant gives up.
- `cpc_resume()` does nothing while another lease is held, so a request that
  promised a running machine does not deliver one, and a request that meant
  to leave the machine stopped has it run again by the other holder.

So `release()` is private to the lease, and every holder in the core (the
text IPC, DevTools, the reset, rebuild and video paths) ends with one of the
three. When several leases overlap, the last request stands: a stop after a
`release_running()` leaves the machine stopped, a `release_running()` after a
stop runs it, and either takes effect when the last holder lets go.

The two writes of this port, `REGS_SET` and `MEM_WRITE`, go one step
further than step 1 asks. They make the change only while their lease is the
only one held, holding the pause mutex meanwhile
(`CpcPauseLease::change_alone()`). A write that finds another lease lets go
of its own, waits 200 µs and takes a new one, for up to 250 ms, and answers
`BUSY` if it never was alone. It must not wait with its lease held: two
threads that both wait to be alone each keep the other from being it. (The
first version of this slice did, and a write beside a read that had fallen
back to a lease answered `BUSY` on a slow host.) A read that falls back to a
lease ([seam 2](#seam-2-the-frame-boundary-for-reads-while-running)) is
taken the same way. The reason is the paragraph below: a machine rebuild runs
under a lease of its own and reallocates the expansion RAM, and a
`MEM_WRITE` that ran beside it would write into freed memory. Both changes
are short (a 1 MiB write is a copy; a register write lets at most one
instruction finish), so nothing waits for that mutex for long.

**Writes beside reads.** A write under a lease pauses the machine between
two frames, and reads of a running machine are taken at a frame boundary. A
controller that sends its next write as soon as the last one is answered
would leave the runner no frame at all: measured on the CI hosts, three
clients reading beside such a controller got 15 reads through in 1.5 s, each
one by the gate's 250 ms fallback and so under a pause lease. So while
clients read a running machine (a read is waiting at the gate, or the runner
served one at one of the last four boundaries) a `REGS_SET` or `MEM_WRITE`
takes no lease: it is queued at the gate behind the reads
(`Gate::at_boundary()`) and the runner makes it at the same boundary, right
after them. That is the moment a lease would have had too, between two
frames, so a register write still lets the instruction in flight finish;
and while the runner is at a boundary no lease, and so no machine rebuild,
can hold the machine. A write costs one frame then, and the machine is never
paused for it (beads-df1x; three readers of 64 KiB and a writer, paced: 19
writes a second before, 41 to 48 after; unpaced: 146 to 174 writes and 637
to 791 frames a second before, 2728 to 2803 of both after). With nobody
reading, on a paced machine, and on a paused one, the write takes its lease
at once, which waits for no boundary at all (thousands of writes a second).
A write the runner has not taken within 50 ms (the machine paused under it,
or no frame came) takes its lease after all. A client with a lot to write
puts it in one `MEM_WRITE`.

**Writes made together.** On an unpaced machine a lease per write stops a
runner that has no sleep to give: a controller writing back to back with
nobody reading cut it from ~3000 to ~150 frames a second (beads-dws9). So an
unpaced running machine takes its writes at the frame boundary too, read or
not (`subcycle_bridge_paced()` says which), and the reader thread makes the
writes that have arrived together as one change: when a `REGS_SET` or
`MEM_WRITE` of the controller is read, the ones that follow it in what the
socket has already delivered (up to 256 requests or 1 MiB of payload) join
it, are checked one by one as on their own, made in order at one boundary
or under one lease, and then answered one by one, in request order; a
request behind them is served after their replies. Nothing is held back
waiting for a write that has not arrived. Measured under `-H`, unpaced, a
write-only controller: 22000 writes and 143 frames a second before, ~2930
of both after when it waits for each reply, and 32 writes sent together
are one change (~54000 writes and ~2900 frames a second, against 90000 and
131). Paced, a lease per run, unchanged (~2000-4500 writes a second, the
same spread before and after). The other changes (media, the tape deck, snapshots,
configuration) do file I/O or more, and stay off the runner: one that finds
reads waiting on a running machine lets one frame boundary pass first, at
most 50 ms, and then takes its lease.

**Leases do not exclude one another.** Two holders both find the runner idle
and both go ahead. That is deliberate: the text IPC's `profile load` holds a
lease while the main thread rebuilds the machine under its own, so making
holders wait for one another would deadlock that pair. It is safe between
operations that each restore what they found, and it is not safe for a
holder that copies out of the machine while another rebuilds it. A holder
that only reads therefore reads through `CpcPauseLease::read_alone()`, which
runs the copy only while its lease is the only one and holds the pause mutex
meanwhile, so that no other lease can be taken until the copy is done. The
writes of this port use the same mechanism, as said above. (Two holders
elsewhere in the core that both change the machine at once, from two
threads, are still not kept apart: that needs the holders to hand the
machine to one another, and is tracked as beads-nhfy.)

Breakpoint edits use the editing lists and their existing locks
(`CpcStopCoordinationGuard`, the list mutators in `z80_view.cpp`). The runner
copies those lists into the probe with `subcycle_bridge_sync_probe()` before
each frame, as it does for edits made by the text IPC and DevTools.

Symbol edits touch no machine state at all: the table (`g_symfile`) is host
data. It had no lock, although the DevTools read it while they draw and the
text IPC edits it from its own thread; slice 7 gave it one (every method of
`Symfile` takes it), a generation counter and the hook `EVT_SYMBOLS_CHANGED`
is posted from. The runner never takes that lock, so a symbol request never
waits for a frame.

That lock is the pause mutex, and the runner holds it for the length of
every machine frame. `BP_LIST` and the three edits therefore wait for the
frame in flight on a running machine: a fraction of a millisecond most of the
time, up to some 15 ms on an unpaced one (the measurement that made the
machine gate read an atomic instead, see seam 2). That is fine for a request
a person causes and wrong for a poll. A client does not poll `BP_LIST`: it
asks after `EVT_BREAKPOINTS_CHANGED`, and after a stop if it shows hit
counts.

### Seam 2: the frame boundary, for reads while running

A lease is the wrong tool for a GUI that refreshes its views 50 times a
second: taking one pauses audio and waits out the rest of the frame, up to
20 ms.

So reads go through a **machine gate**, one small class
(`src/kcpc/kcpc_gate.h`):

- The reader thread builds a *read job* (for example "copy 65536 bytes of CPU
  view from address 0") and gives it to the gate.
- **If the machine is running**, the job goes on a queue. At the next frame
  boundary the runner calls `kcpc_service_frame()` from the shared frame
  epilogue in `emu_run_frame()`, next to `g_telnet.drain_input()`. That call
  runs queued jobs on the runner thread, while the machine is not being
  clocked, and wakes the readers that wait for them. Nothing is paused: not
  the machine, not the audio.
- **If the machine is stopped** (paused, with no pause lease held and the
  runner idle), no boundary is coming and none is needed. The reader runs
  the job itself through `cpc_read_while_stopped()`, which holds the pause
  mutex for the length of the copy. Every pause, resume and lease in the
  core goes through that mutex, so the machine stays stopped under the
  read, and a `run` that arrives meanwhile waits a few microseconds and then
  takes effect.

  This is deliberately not a pause lease. `cpc_resume()` does nothing while
  any lease is held (the text IPC's `run` answers `ERR 409
  pause-lease-held`), which is right for a change in progress and wrong for
  a view that refreshes 50 times a second: with three clients reading a
  paused machine through leases, 3 of 60 `run` commands were refused. Held
  through the mutex instead, none are.
- **If the machine is paused but not stopped** (somebody's lease holds it,
  or a stop was requested and the frame in flight has not ended), the reader
  waits, looking again every 100 µs. That state passes in a moment, and then
  the machine is either stopped or running again and one of the two rules
  above applies. The reader does not answer a lease with a lease of its own:
  readers doing that keep one another's leases alive.
- A reader whose job is queued looks at the pause flag every 5 ms. If the
  machine is paused under a queued job (a breakpoint, somebody's `pause`),
  the reader takes the job back and goes on as above. It does not wait for
  the fallback.
- **Fallback:** a read that has got neither a frame boundary nor a stopped
  machine within **250 ms** is run by the reader under a pause lease, through
  `read_alone()` (see seam 1): it waits up to another 250 ms to be the only
  lease holder and answers `BUSY` if it never is. This is a safety net for a runner that turns out no frames although nothing
  paused it, and for a lease that holds the machine that long. It should
  never fire on a healthy machine, because on a running one a lease costs
  exactly what the gate exists to avoid and is no quicker: it too waits for
  the frame in flight to end.
- Each job has an atomic state (queued → claimed → done), so it runs exactly
  once whichever side claims it.

"Paused" above is the flag the runner itself obeys (`g_emu_paused`), which
is set for a real stop and for a lease alike. The gate reads that atomic and
nothing else on the path of a running machine. It must not ask
`cpc_stopped()` there: that takes the pause mutex, which the runner holds
for the whole of every machine frame, and on an unpaced machine a reader
then waited about 15 ms for it on every read (measured; one frame, 0.4 ms,
with the atomic).

What the runner is allowed to do in `kcpc_service_frame()`:

- copy out of the machine into a buffer the reader thread allocated before
  it queued the job; no socket calls, no allocation at all, no waiting on a
  client. The one lock it takes guards the queue's pointers and is never
  held across a copy, an allocation or a system call;
- at most **1 MiB** of copying per frame boundary, and always at least one
  job. The budget decides how many jobs share a boundary; it never splits
  one, because all the bytes of a reply come from the same instant
  ([section 11](#11-memory)). Eight clients that each want 64 KiB are all
  served at the same boundary; a second 1 MiB read waits for the next one;
  a dump of all 4160 KiB of the largest machine is one job and one boundary;
- nothing at all when the queue is empty: it counts the frame (the `frame`
  of every `stamp`) and does one relaxed atomic load.

Read jobs never change machine state, so a run with clients attached makes
the same observations as a run without them (epic invariant 2). The test
suite checks it: 100 frames from a reset end in the same registers (all of
them, `R` included) and the same physical RAM whether nobody reads or three
clients read 64 KiB and the registers at every boundary.

Where the numbers come from (measured on an Apple M-series host, slice 2):

| What | Cost |
|------|------|
| `kcpc_service_frame()` with nothing queued | not measurable: 3 FPS in 2400 on an unpaced `-H` core, inside the run-to-run noise |
| Copying a 64 KiB view (four `memcpy`s, one per 16 KiB slot) | about 1 µs; byte by byte through the debugger's peek it was 116 µs |
| Copying 1 MiB of physical RAM | about 20 µs |
| Copying all 4160 KiB | about 80 µs |
| A paced frame / an unpaced Fast-tier frame | 20 ms / about 0.4 ms |
| A read on a running machine, request to reply | one frame: 0.4 ms unpaced, up to 20 ms paced |

So the 1 MiB budget caps a boundary at about 20 µs of copying, a thousandth
of a paced frame, plus whatever its first job costs.

The fallback was first set to 100 ms and that was too short. A paced frame
sleeps until its 50 Hz deadline, and on a loaded host that 19 ms sleep was
measured at 67 ms in the median and 107 ms at worst; the pacer then catches
up with frames back to back. With 100 ms, 14 of 301 reads of a healthy
machine fell back to a lease, each pausing the machine for nothing. With
250 ms none did. 250 ms is also where the frame pacer itself decides a host
has stalled (`frame_pacer.h`).

When the server stops, it logs where the gate took the reads of that run
(`KCPC: reads served: N at a frame boundary, N while paused, N by fallback,
N busy; N writes at a frame boundary in N changes`), so "did a client cost the machine pauses?" can be answered from a
log: only the last two numbers are leases.

### Seam 3: the bridge probe and `debug_sync`, for events

Breakpoint, watchpoint and I/O hits are already detected in one place:
`subcycle_bridge_debug_sync()` consumes the probe's latch on the runner
thread and commits the stop with `cpc_commit_breakpoint_stop()`, which
pauses the machine, announces the stop and calls the hit hook
(`KoncepcjaIpcServer::notify_breakpoint_hit()`, the text IPC's `wait bp`
latch), all under the pause mutex. The binary server hangs off that commit:
the bridge hands it what the probe latched (which kind of hit, the address
or port, the data bus) and the announcement carries it to
`EVT_STOPPED`. The design had the server on the hit hook itself; the hook
is given a `pc` and a watchpoint flag and nothing else, and the announcement
of slice 4 was already the one place a stop is pushed from, so the details
went there instead of into a second path.

Posting an event is one push into a fixed-size ring (4096 entries). It
never blocks on another thread, never allocates and makes no socket call. If
the ring is full the event is counted as lost, and every client is told with
`EVT_OVERRUN`. The fan-out thread does the rest: it takes the events out in
the order they were pushed, encodes them and queues them for the clients
that subscribed.

The ring takes **several producers**, not the one the first design named.
The runner posts frames and breakpoint stops, but a pause is settled on the
thread that asked for it, under the pause mutex, while the runner may be
finishing a frame; and a change of controller is posted by a client's reader
thread. So a producer claims its slot with a compare-and-swap (a bounded
queue after Vyukov), which keeps the push lock-free for all of them.

A producer wakes the fan-out thread only when that thread is asleep, by
taking its mutex for an instant and signalling it. The fan-out thread holds
that mutex only for the one check it makes before it sleeps, so this is the
only place a producer can wait, and for no longer than that check.

`EVT_FRAME` costs the runner nothing while nobody wants it. The server keeps
the greatest common divisor of the frame intervals clients asked for (0 when
there are none), and `kcpc_service_frame()` posts a frame event only for
frames that number divides: one relaxed atomic load per frame otherwise.

### Run state: one announced value

"Paused" and "running" are announced, not sampled. Three things settle the
pause state, all under `g_pause_mutex`: a pause (`cpc_pause()` and the
breakpoint commit), a resume that is applied, and the last pause lease
letting go, by whichever of its exits. At each of them the core compares the
state that now holds with the last state it announced and calls its one
observer only when they differ (`cpc_set_run_state_observer()` in
`koncepcja.h`). The server is that observer and posts `EVT_STOPPED` or
`EVT_RESUMED`. While any lease is held the state is not settled and nothing
is compared.

A lease that pauses a running machine and resumes it inside one request is
therefore invisible, which is what a client wants: reads and pokes do not
make the run state flicker. The test suite checks it: 60 reads and 120
writes beside a running machine produce no run-state event.

Because the announcement lives in the core and not in this server, it does
not matter who pauses. The text IPC's `pause`, `run` and `step`, the GUI's
pause and the DevTools' step buttons are pushed to the clients like a
client's own requests, with `cause_client` 0.

**A stop is announced when it has happened, not when it was asked for.** A
pause from another thread only raises a flag; the frame in flight runs to
its end first, up to 20 ms later. An event sent at once would carry a `pc`
and a `frame` that are already wrong when the client reads the registers.
So a stop that is asked for while a frame runs is announced by the runner
itself when it parks (`cpc_runner_parked()`), and `EVT_STOPPED` always
describes the machine a read then finds: same `frame`, same `pc`. A pause
that is taken back before the frame ended was never a stop and is never
announced. `RUN_STATE` reports this announced state, from two atomics, so
polling it costs a running machine nothing.

Every pause, resume and lease exit can be told who asked (`CpcRunCause`:
client, request, reason), and the announcement repeats it. The latest
request stands, as it does for the lease chain: a stop that arrives after a
deferred run leaves the machine stopped and is the one announced.

**A run is never dropped.** `cpc_resume()` does nothing while a pause lease
is held, which is right for a caller that only undoes its own pause and
wrong for a request to run the machine: with several actors a lease is held
somewhere much of the time (another client's write, a read that fell back to
a lease, the text IPC), and the run then vanished (beads-pja3). `RUN` uses
`cpc_run()`, which records the run for the lease chain: the machine runs
when the last holder lets go, unless somebody stops it first.

### Steps that take emulated time

No request waits for emulated time to pass. `STEP` in the modes that run the
machine (over a call, out of a routine, to an address, N frames) answers `OK`
as soon as the step is armed, and the result arrives as `EVT_STOPPED`. One
step runs at a time; a second `STEP` meanwhile answers `BUSY`, and `PAUSE`
ends the one in progress.

A frame step needs no thread of its own. It arms the frame counter the text
IPC's `step frame` uses (`arm_frame_step()`, counted down by the runner in
its frame epilogue) and lets go of its lease with `release_running()`; when
the count reaches zero the runner pauses the machine, and that pause is
announced with reason `STEP_DONE`. Nothing waits for it. The core remembers
which request the step in progress belongs to, so whatever stop ends it (the
count, a breakpoint, somebody's pause) is delivered to the client that
asked. A stop that ends the step early also disarms the counter: left armed,
it would stop the machine again some frames after the next `RUN`.

There is one counter, and the step armed on it owns it until it ends. A
frame step from this port while the text IPC's runs (`step frame`,
`wait vbl`, a key tap's hold, `frames dump`) answers `BUSY`; a text command
that needs the counter while this port's step runs answers
`ERR 409 frame-step-busy`. Neither replaces the other's count (before, the
second starter's count replaced the first's, which then ended with it).

The steps that walk the machine (over, out, to) need a thread, because a
walk waits: for a one-shot breakpoint to be reached, for a deadline. The
server has one **walker** thread for them. The request pauses the machine
under a lease, arms the step, lets go of the lease and answers; the walker
then runs the walk the text IPC and the DevTools already use
(`z80_run_until_ephemeral()` for *to* and for a call that *over* skips,
`z80_step_out_until()` for *out* and for a restart that *over* steps into,
`z80_step_over()` around them).

**A walk is one step, whatever it does to the run state on the way.** Step
out runs the machine to a one-shot breakpoint for every call it skips and
single-steps in between: a dozen resumes and stops for one request. The core
calls that a walk (`CpcWalk` in `koncepcja.h`), and while one is open:

- the stops on its way are not announced. The announced run state still
  follows them, which is what the walk itself waits on;
- the first time it runs a stopped machine is announced, as `EVT_RESUMED`
  naming the step. A walk that only single-steps announces no resume, and
  neither does one that starts from a running machine (the request's lease
  paused it for a moment, as a write's would);
- when it closes, the step ends with one `EVT_STOPPED`: `STEP_DONE`,
  `STEP_TIMEOUT`, `STEP_STALLED`, or whatever else stopped the machine, as
  what it was (the breakpoint or watchpoint that was hit, with its details;
  somebody's pause).

This lives in the core, not in the walker, so the walks of the text IPC
(`step over`, `step out`, `step to`, `wait pc`) and of the DevTools are one
step to a client too. Before slice 5 they were pushed as a resume and then
the hit of a breakpoint nobody had set.

**A walk waits for a stop, not for a pause** (beads-pja3). The walks used to
poll the flag the runner obeys, which a pause lease also sets: another
client's `MEM_WRITE` in the middle of a step over ended the walk short of
its target, and the lease then set the machine running again behind the
reply. Three things were wrong, and all three had to go:

- the walk now waits on the announced run state, which only a stop somebody
  asked for settles, and only once the runner has parked. It is an atomic:
  the walk never takes the pause mutex beside a running machine;
- the walk runs the machine through a call that waits a lease out (a lease
  is one operation long) instead of a resume that is dropped while one is
  held;
- a lease that pauses a running machine and puts it back no longer starts a
  new *resume epoch*. The walk remembers the epoch of its run to tell
  "somebody stopped and restarted the machine under me" from "my run goes
  on", and every lease looked like the first. A run that somebody asked for
  while a lease was held is still a new epoch.

**Somebody's pause ends a walk**, whoever pauses: `PAUSE`, the text IPC, the
GUI. The machine may be stopped already at that moment (the walk is
single-stepping), and then no stop follows that the walk could wait for; so
the pause also raises a flag the walk looks at before every instruction and
every run. The walk ends within a millisecond and its `EVT_STOPPED` is the
pause's.

So the longest any request can take is the pause lease's idle timeout. The
one exception is `CONFIG_APPLY` (section 12), which waits for the machine to
be rebuilt on the main thread.

### Shutdown

`stop()` closes the listening socket, sends `EVT_SHUTDOWN` to every client,
fails queued jobs with `SHUTTING_DOWN` (the gate is shut
first, so a reader waiting for a frame boundary answers at once), closes the
client sockets and joins every thread. No thread waits on anything without a
deadline or an abort flag.

The core calls `stop()` first thing when it quits, while the runner is still
alive, because a request in flight may be waiting for the runner to go idle.
Each connection gets 200 ms to take its last frame and 200 ms more to close
its side; the connections shut down side by side, so `stop()` returns in
well under a second however many clients there are and whatever they were
doing (measured: about 0.3 s with four connections, one of them mid-frame).

The walker is stopped after the connections, when no reader is left to arm a
step. A walk in progress is ended, not waited for: it gets the flag a pause
would raise, and stops at its next look (measured: the core exits 0.3 s
after `quit` with a walk in progress whose deadline is a minute away).

## 9. Events

### Subscribing

`SUBSCRIBE` (`0x0003`) replaces the client's subscription:

| Size | Field | Meaning |
|-----:|-------|---------|
| 4 | `classes` | Bit mask of event classes |
| 4 | `frame_interval` | Send `EVT_FRAME` every N frames; 0 = never |

The reply repeats the two values as accepted: class bits this server does
not know are cleared, not refused, so a client written for a later minor
version still subscribes to what exists. After `HELLO` a client is
subscribed to nothing.

`EVT_FRAME` is sent for the frames whose number is a multiple of
`frame_interval`, so two clients that ask for the same interval are told of
the same frames. An interval without the `FRAME` class, and the class with
interval 0, ask for nothing.

| Bit | Class | Events |
|----:|-------|--------|
| 0 | `RUN` | `EVT_STOPPED`, `EVT_RESUMED`, `EVT_RESET` |
| 1 | `FRAME` | `EVT_FRAME` |
| 2 | `MEDIA` | `EVT_MEDIA_CHANGED` |
| 3 | `DEBUG` | `EVT_BREAKPOINTS_CHANGED`, `EVT_SYMBOLS_CHANGED` |
| 4 | `MACHINE` | `EVT_MACHINE_CHANGED` |

Three events ignore the mask and reach every client: `EVT_SHUTDOWN`,
`EVT_OVERRUN`, `EVT_CONTROL_CHANGED`. And an `EVT_STOPPED` that ends a step is
always sent to the client that asked for the step.

### Delivery

- Events on one connection arrive in the order they happened, and each
  carries the next number of that connection in its `id`: 1 for the first
  event after `HELLO`, `EVT_OVERRUN` included. A gap in the numbers means a
  bug, not a loss; a loss is always reported.
- An event caused by a client's own request follows the response to that
  request ([section 4](#4-framing)). Events that happen while that one waits
  wait behind it, so the order is kept.
- An event is a notice, not the data. After `EVT_BREAKPOINTS_CHANGED` the
  client asks for `BP_LIST`; after `EVT_STOPPED` it reads the registers.
- Each client has an outgoing queue. Responses always go in. Events go in
  only while the queue holds less than 4 MiB; beyond that they are dropped
  for that client alone, and once there is room the client receives one
  `EVT_OVERRUN` with the number dropped since the last one. A client that
  sees it should assume everything changed and re-read. Events lost because
  the ring itself was full are counted for every client the same way.
  (`KONCPC_KCPC_EVENT_QUEUE_BYTES` in the environment lowers the 4 MiB. It
  exists for the tests.)
- A client whose queue reaches 32 MiB of unread responses is disconnected.
- A slow or stuck client costs the runner nothing: the runner only ever
  writes to its own ring.

### Event payloads

**`EVT_STOPPED` (`0x8001`)** — the machine is now paused.

| Size | Field | Meaning |
|-----:|-------|---------|
| 8 | `frame` | |
| 1 | `reason` | See below |
| 1 | `access` | For watch and I/O hits: 1 = read or `IN`, 2 = write or `OUT`; else 0 |
| 2 | `pc` | Program counter at the stop |
| 2 | `addr` | Breakpoint address, the address that was read or written, or the I/O port that was addressed; else 0 |
| 1 | `value` | The byte written, the byte read, or the byte an `OUT` sent; else 0 |
| 1 | `old_value` | The byte a watched write replaced; else 0 |
| 4 | `cause_client` | `client_id` whose request led to this stop, or 0 |
| 4 | `cause_request` | That request's `id`, or 0 |

| Reason | Name | When |
|-------:|------|------|
| 1 | `PAUSED` | Somebody paused: any client, the text IPC, the GUI |
| 2 | `BREAKPOINT` | Execution reached a breakpoint |
| 3 | `WATCHPOINT` | A watched memory range was read or written |
| 4 | `IO_BREAKPOINT` | A port comparator matched |
| 5 | `STEP_DONE` | A `STEP` finished where it was meant to |
| 6 | `STEP_TIMEOUT` | A walk hit its deadline; the machine is paused where it was |
| 7 | `STEP_STALLED` | A walk could make no progress (`JP $`, `HALT` with interrupts off) |

`EVT_STOPPED` is sent once the machine has stopped: its `frame` is the
`frame` of every read taken before the machine runs again, and its `pc` is
the `PC` those reads show (at a breakpoint, the breakpoint's address).

A step always ends with exactly one `EVT_STOPPED`, also when the machine was
paused before it and is paused after it. Whatever ends a step, the event is
sent to the client that asked for the step, subscribed or not:

- the step finished, timed out or stalled: `STEP_DONE`, `STEP_TIMEOUT`,
  `STEP_STALLED`, and the `cause_*` fields name the step;
- the step ran into a breakpoint, a watchpoint or an I/O breakpoint: that
  reason with its details, and the `cause_*` fields name the step;
- somebody paused: `PAUSED`, and the `cause_*` fields name the pause (a
  client's `PAUSE` request, or 0 for the text IPC and the GUI).

What a hit says, whoever set the breakpoint (this port, the text IPC, the
DevTools):

| Hit | `reason` | `addr` | `access` | `value` | `old_value` |
|-----|----------|--------|---------:|---------|-------------|
| Execution reached a breakpoint | `BREAKPOINT` | its address, which is also `pc` | 0 | 0 | 0 |
| A watched byte was written | `WATCHPOINT` | the address written | 2 | the byte written | the byte it replaced |
| A watched byte was read | `WATCHPOINT` | the address read | 1 | the byte read | 0 |
| An `OUT` to a watched port | `IO_BREAKPOINT` | the port, all 16 bits | 2 | the byte sent | 0 |
| An `IN` from a watched port | `IO_BREAKPOINT` | the port | 1 | 0 | 0 |

- `addr` is the one address or port of the access, not the start of the
  range or the comparator's value: a watch on 16 bytes says which of them,
  an I/O breakpoint on `&7Fxx` says `&7F10`.
- An `IN` has no `value`: the probe latches when the port is addressed, and
  the device has not answered yet. (For the same reason the byte *read* at
  a watchpoint is the byte in memory at that address, not the data bus,
  which still floats then. The text IPC's `value` in a watch condition used
  to be that floating `&FF` for reads; it is the byte read now.)
- A watch or an I/O hit stops the machine inside the instruction that made
  the access. `pc` is what `REGS_GET` then shows, somewhere in or just past
  that instruction; it is not an address to disassemble from. An opcode
  fetch is a read: a read watch on code stops on it.

**`EVT_RESUMED` (`0x8002`)** — `u64 frame`, `u32 cause_client`,
`u32 cause_request`.

**`EVT_RESET` (`0x8003`)** — the CPC was reset, or a snapshot was loaded
into it (`SNAPSHOT_LOAD`): either way what a client shows is another
program's, and it re-reads. `u64 frame`, `u32 cause_client`,
`u32 cause_request`.

**`EVT_FRAME` (`0x8004`)** — a frame completed. `u64 frame`, `u64 fb_seq`.
`fb_seq` is the sequence number of that frame in the frame ring
([fb-ring-v1.md](fb-ring-v1.md)), one more for each frame that ran to its
end, so a reader that wants this frame copies the slot holding `fb_seq`; 0
when the core publishes no ring. The event is posted on the frame runner
after the frame went into the ring: when it arrives, the ring holds that
frame or a later one.

**`EVT_MEDIA_CHANGED` (`0x8005`)** — `u8 slot` (0 = drive A, 1 = drive B,
2 = tape, 3 = cartridge), `u8 present`, 2 reserved bytes, `u64 generation`
(for drives, `dsk_media_generation()`), `str path` (empty if unknown).

- Sent whenever a slot gets a different medium or loses it, whoever did it:
  the text IPC's `load`, `disk eject`, `disk format` and `tape eject`, the
  menus, drag and drop, a failed load (which leaves the slot empty).
- `generation` is a counter per slot that only grows. Each event carries the
  value its change produced; the number itself means nothing.
- **A disc is announced more than once.** A load first empties the drive
  (`present` 0), then puts the new disc into the host's view (`present` 1,
  the path), and the machine takes the disc at its next frame boundary, which
  is announced again with a higher generation. A client re-reads on each;
  `CHIP_GET` of the FDC says when the machine has the disc
  (`present[unit]`, and `generation[unit]` at least the event's).
  `MEDIA_INSERT` and `MEDIA_EJECT` give the machine its half before they
  answer, so their reply's `generation` is the last of these and the events
  of the request all follow that reply ([section 12](#media)).
- `present` and `path` are what the slot holds **when the event is sent**,
  not when the change was made: the frame runner posts this event too, and
  it may neither take a lock nor carry a path. The server asks for both on
  its fan-out thread. With two changes in quick succession both events may
  describe the second; their generations still differ.
- `path` is absolute. It is the file the host loaded, so for a `.zip` it is
  the archive. It is empty for an empty slot and for a medium that has no
  file (a drive formatted in place before anything was loaded into it).

**`EVT_BREAKPOINTS_CHANGED` (`0x8006`)** — `u64 generation`
(`z80_breakpoint_generation()`). Sent for every edit of the breakpoint,
watchpoint and I/O breakpoint lists, from any source: `BP_ADD`, `BP_DEL` and
`BP_CLEAR`, the text IPC's `bp`, `wp` and `iobp`, the DevTools. An edit a
request made is sent to that client after its reply. Hit counts move without
an event, and so does the one-shot breakpoint of a walk, which is never
listed.

**`EVT_SYMBOLS_CHANGED` (`0x8007`)** — `u64 generation`, the one `SYM_LIST`
reports. Sent for every change of the symbol table, from any source:
`SYM_ADD`, `SYM_DEL` and `SYM_LOAD`, the text IPC's `sym`, the DevTools, the
assembler exporting its labels. A file of symbols and an assembly are one
change and one event, however many symbols they bring. An edit a request made
is sent to that client after its reply.

**`EVT_MACHINE_CHANGED` (`0x8008`)** — the machine was rebuilt (model, RAM
size, CRTC type, tier). Payload: the `MACHINE_INFO` reply fields, read from
the new machine before it runs its first frame; the stamp's `run_state` is
the one the rebuild puts back. Sent after every rebuild that worked:
`CONFIG_APPLY`, the text IPC's `config apply` and `profile load`, the GUI's
settings. A client should
re-read everything it shows. (`effective_tier` is the tier the new machine
starts in, chosen by the run-tier policy before its first frame.)

**`EVT_CONTROL_CHANGED` (`0x8009`)** — `u32 controller` (the new
controller's `client_id`, or 0 for none), `u32 previous` (the `client_id`
that had control before, or 0), `u8 reason` (1 = acquired while free,
2 = taken by force, 3 = released, 4 = controller disconnected or timed out),
3 reserved bytes. A client compares `previous` with its own `client_id` to
learn that it was demoted.

**`EVT_OVERRUN` (`0x800A`)** — `u32 dropped`.

**`EVT_SHUTDOWN` (`0x800B`)** — empty. The server closes the connection after
it.

## 10. Machine information

**`MACHINE_INFO` (`0x0005`)** — empty request. Reply:

| Size | Field | Meaning |
|-----:|-------|---------|
| 1 | `model` | 0 = 464, 1 = 664, 2 = 6128, 3 = 6128+ |
| 1 | `crtc_type` | 0–3 |
| 1 | `effective_tier` | 0 fast, 1 wake, 2 soldered, 3 faithful |
| 1 | `flags` | Bit 0: headless (`-H`). Bit 1: ASIC present |
| 4 | `ram_bytes` | Physical RAM: base 64 KiB plus expansion |
| 4 | `rom_slots` | Bit N set = an expansion ROM is fitted in slot N (0–31) |
| `stamp` | | |

The on-board BASIC ROM is not an expansion ROM. It answers in slot 0, and in
every slot with nothing fitted, without setting a bit; bit 0 is set only when
an expansion ROM has been fitted over it. A stock 6128 reports `0x00000080`:
AMSDOS in slot 7.

`effective_tier` is the tier the machine is running in now. At startup it
equals the `effective_tier` of the startup manifest, which is printed once
the machine is built and in the tier its first frame runs; it changes later
when the policy does, or when Auto sees a breakpoint come or go.

`ram_bytes` is what the machine has fitted, which is the size of the memory
space `PHYS_RAM` ([section 11](#11-memory)). It is the configured RAM size
unless something raises it: the Silicon Disc needs 512 KiB of expansion and
fits them whatever was asked for.

Like every read, this one goes through the machine gate
([section 8](#seam-2-the-frame-boundary-for-reads-while-running)): on a
running machine the runner takes it at the next frame boundary and nothing
is paused, so it is cheap enough to poll. (Slice 1 read it under a pause
lease.) `BUSY` only when a read had to fall back to a pause lease and the
runner did not go idle for it in time, or other lease holders kept the
machine for longer than the read waits to be alone.

## 11. Memory

### Spaces

One address means different bytes depending on which view is asked for.
`MEM_READ` and `MEM_WRITE` name the view with a `space`:

| Space | Name | Address range | What it is |
|------:|------|---------------|------------|
| 0 | `CPU` | 0–`0xFFFF` | What the Z80 would read now: ROMs where they are paged in, RAM through the current banking. The text IPC's default view |
| 1 | `CPU_RAM` | 0–`0xFFFF` | The RAM byte behind that address, ROMs ignored: where a Z80 write lands. The text IPC's `--view=ram` |
| 2 | `PHYS_RAM` | 0–`ram_bytes`−1 | Physical RAM, flat, independent of banking. 16 KiB page N of the text IPC's `--bank=N` starts at N × 16384 |
| 3 | `ROM_LOWER` | 0–`0x3FFF` | The lower (firmware) ROM, paged in or not |
| 4 | `ROM_UPPER` | 0–`0x3FFF` | Upper ROM in slot `bank` (0–31), paged in or not |
| 5 | `ASIC_PAGE` | 0–`0x3FFF` | The 6128+ ASIC register page as it appears at `0x4000` when unlocked; `NOT_PRESENT` on other models |

`bank` is only meaningful for `ROM_UPPER` and must be 0 elsewhere
(`BAD_PAYLOAD`). For `ROM_UPPER` it is the ROM number, 0 to 31
(`OUT_OF_RANGE` beyond). A number with no ROM fitted is not an error: it
reads as the on-board BASIC, which is what answers on the bus when that
number is selected.

The two ROM spaces show the ROM whether or not the Gate Array has it paged
in. On a 6128+ they show cartridge banks: `ROM_LOWER` the bank the lower
ROM window is currently mapped to, `ROM_UPPER` the bank that ROM number
selects.

`CPU` is the view of the memory device and the text IPC's `mem read`. It
does not include what an expansion or the ASIC lays over an address range;
the ASIC's register page has its own space.

A space this server does not implement answers `BAD_PAYLOAD`: a number that
is not in the table.

`ASIC_PAGE` is the ASIC's register page itself, all 16384 bytes of it,
whether or not the ASIC is unlocked and whether or not the page is paged in
at `0x4000`: a view must not depend on what the program under it has mapped.
What the bytes mean is the 6128+'s own layout (sprite pixels from offset 0,
sprite positions at `0x2000`, the palette at `0x2400`, the raster interrupt,
split and scroll registers at `0x2800`, the DMA channels at `0x2C00`). It is
read-only here: `MEM_WRITE` refuses it, because the ASIC acts on writes to
its registers and a poke that bypassed that would show a state the chip is
not in. On a machine without an ASIC a read answers `NOT_PRESENT`, after the
checks of the request itself (so a read past the end of the page is
`OUT_OF_RANGE` there too).

**`MEM_INFO` (`0x0210`)** — empty request. Reply:

| Size | Field | Meaning |
|-----:|-------|---------|
| 4 | `ram_bytes` | The size of space `PHYS_RAM` |
| 1 | `ram_config` | The RAM banking register, as last written (`11bbbccc`); 0 after a reset |
| 1 | `rom_config` | The Gate Array's mode register, as last written. Bit 2 set = lower ROM off, bit 3 set = upper ROM off. The other bits are the rest of that register (the screen mode in bits 0–1); mask them off |
| 1 | `upper_rom` | The selected upper-ROM number |
| 1 | reserved | 0 |
| 4 | `rom_slots` | As in `MACHINE_INFO` |
| `stamp` | | |

**`MEM_READ` (`0x0211`)**

Request: `u8 space`, `u8` reserved, `u16 bank`, `u32 addr`, `u32 len`.

Reply: `stamp`, then `len` bytes.

- `addr + len` must not pass the end of the space: `OUT_OF_RANGE`. Nothing
  wraps around, not at 64 KiB and not at 32 bits. A read of 0 bytes at the
  very end of a space succeeds; one byte further is `OUT_OF_RANGE`.
- `len` may be anything from 0 up to what fits the frame limit. The reply
  is 16 bytes longer than `len` (the status header and the stamp), so the
  largest read is the connection's `max_frame` less 16: `TOO_LARGE`
  otherwise. **A 64 KiB read of a CPU space is always one reply**, since
  `max_frame` is at least 128 KiB.
- `TOO_LARGE` is decided first, from the request alone and before anything
  is allocated; then `BAD_PAYLOAD` (space, bank), then `OUT_OF_RANGE`, then
  `NOT_PRESENT` (`ASIC_PAGE` without an ASIC).
- All the bytes of one reply come from the same instant, the one its stamp
  names. The server never splits a read over two frame boundaries.

**`MEM_WRITE` (`0x0212`)** — controller only.

Request: `u8 space`, `u8` reserved, `u16 bank`, `u32 addr`, then the bytes to
write. Reply: `stamp`.

- Allowed spaces: `CPU_RAM` and `PHYS_RAM`. ROM spaces, `ASIC_PAGE` and
  numbers that are not in the table answer `BAD_PAYLOAD`. `bank` must be 0
  (`BAD_PAYLOAD`).
- A `CPU_RAM` write lands where a Z80 write to that address would: in the
  RAM the current banking puts there, never in a ROM. A `PHYS_RAM` write
  lands at that offset of physical RAM whatever the banking is.
- `addr` plus the number of bytes must not pass the end of the space:
  `OUT_OF_RANGE`, and then **nothing** is written. A write is all of its
  bytes or none. No bytes at all is a valid write of nothing, also at the
  very end of a space.
- One request carries at most 1 MiB less the 8 bytes above (the request
  limit of [section 4](#4-framing)). All 64 KiB of `CPU_RAM` is one request;
  a machine with more than 1 MiB of RAM needs several to fill `PHYS_RAM`.
- Checked in this order: `NOT_CONTROLLER`, then `BAD_PAYLOAD` (too short,
  space, bank), then `OUT_OF_RANGE`, then `BUSY`.
- Host writes never trip a watchpoint, like the text IPC's `mem write`.
- The run state is put back as it was: a running machine keeps running (it
  is paused for the length of the write, between two frames), a paused one
  stays paused. The stamp carries the run state from before the write.
- `CPU` is refused too, on purpose. A write through the CPU view lands in
  RAM, but reading it back through the same view returns the ROM byte paged
  over it; the text IPC has that trap (`mem write` followed by `disasm`).
  Naming `CPU_RAM` makes the client say what it means.

## 12. Opcode payloads

Fields are listed in wire order. "Controller" marks requests an observer may
not send. Every reply starts with the 4-byte status header of section 5,
which the tables omit.

### Registers

The register block is 32 bytes:

| Offset | Size | Field |
|-------:|-----:|-------|
| 0 | 2 each | `AF`, `BC`, `DE`, `HL`, `IX`, `IY`, `SP`, `PC`, `AF'`, `BC'`, `DE'`, `HL'` |
| 24 | 1 each | `I`, `R`, `IFF1`, `IFF2`, `IM`, `HALT` |
| 30 | 2 | reserved, 0 |

- **`REGS_GET` (`0x0200`)** — empty request. Reply: `stamp`, register block.
  The values are the Z80's own (`Machine::regs()`), read now, not the
  host-side copy the in-process debugger refreshes at a stop. `IFF1`, `IFF2`
  and `HALT` are 0 or 1, `IM` is 0 to 2.

  There is one exception, `PC` at a breakpoint. A machine stopped on an
  execution breakpoint has fetched the opcode at that address and not run
  it, so the Z80's own `PC` is already one further. The block carries the
  breakpoint's address, which is where the instruction that runs next
  starts, and what the text IPC's `reg get PC` and the in-process debugger
  show. (Slice 2 reported the Z80's own value there, one too many.)
- **`REGS_SET` (`0x0201`)** — controller. Request: `u32 mask`, register
  block. Bit N of `mask` selects the Nth field of the block (bit 0 = `AF`,
  bit 7 = `PC`, bit 11 = `HL'`, bit 12 = `I`, bit 16 = `IM`); fields whose
  bit is clear are ignored, whatever they hold. Reply: `stamp`, the register
  block after the write, as `REGS_GET` would return it.
  - The first 17 fields can be set. **`HALT` (bit 17) cannot**: whether the
    CPU is halted is where it is in an instruction, not a register, and no
    path in the core can change it without restarting the CPU. A mask with
    bit 17, or with any bit from 18 up, answers `BAD_PAYLOAD`, as does a
    selected `IM` above 2 or a selected `IFF1` or `IFF2` above 1. A refused
    request sets nothing.
  - An empty mask is valid. It sets nothing and replies with the block, after
    letting the instruction in flight finish (next point).
  - The write never drops or replays part of an instruction in flight, by
    the rules of the text IPC's `reg set` and the DevTools
    (`subcycle_bridge_write_regs()`, which `subcycle_bridge_regs_to_machine()`
    now also goes through). A pause leaves the machine wherever the frame
    ended, usually inside an instruction; that instruction is finished
    first and the values are set between two instructions. So a register
    the mask did not select may differ from an earlier `REGS_GET` by what
    that one instruction did.
  - At a breakpoint the instruction shown has only been fetched. An edit
    lands before it runs, and it runs with the new values. Writing the `PC`
    that `REGS_GET` shows there is no edit, so a client may write back a
    whole block it has read. Any other `PC` drops the fetched instruction;
    the next one comes from the new address.
  - The mask says which registers to set; nothing is inferred from what a
    value was before. A selected register ends up holding the value sent
    even when that is the value it showed and the instruction that was
    finished moved it (`SP` halfway through a `PUSH`).
  - The run state is put back as it was, as for `MEM_WRITE`.

### Run control

- **`RUN_STATE` (`0x0104`)** — empty request. Reply: `stamp`,
  `u8 walk_active`, 3 reserved bytes, `u32 controller`.
  `walk_active` is 1 while a step is in progress: one of this port's, or a
  walk or frame step of the text IPC or the DevTools. The stamp is the run
  state as announced ([section 8](#run-state-one-announced-value)); nothing
  waits for the machine, so this is cheap to poll. During a walk the stamp
  follows the machine through the stops on the walk's way, which are not
  pushed as events.
- **`PAUSE` (`0x0100`)** — controller. Empty. Pauses, and ends a step in
  progress: the walk stops at its next look, within a millisecond, and its
  `EVT_STOPPED` is this pause's (reason `PAUSED`, naming this request) and
  goes to the client that stepped as well. Reply: `stamp`. The request waits for the frame in flight to
  end, so the stamp is that of the stopped machine and `EVT_STOPPED` (reason
  `PAUSED`, naming this request) follows the reply at once. Pausing a
  machine that is already paused succeeds and announces nothing. Never
  `BUSY`: a runner that will not go idle is paused all the same, the stamp
  then still says running, and the event comes when the stop has happened.
- **`RUN` (`0x0101`)** — controller. Empty. Reply: `stamp`. Never `BUSY`
  (the design said `BUSY` when a pause lease held elsewhere defers the
  resume; a run that can be lost to another actor's lease is a bug, see
  [section 8](#run-state-one-announced-value)). When a lease defers it, the
  stamp still says paused and `EVT_RESUMED` follows when the last holder
  lets go. Running a machine that is running succeeds and announces nothing.
- **`RESET` (`0x0103`)** — controller. Request: `u8 flags` (bit 0: stay
  paused afterwards), 3 reserved bytes. Reply: `stamp`. The machine runs
  afterwards, or stays paused with bit 0, whichever it was before.
  `EVT_RESET` names this request, and `EVT_STOPPED` or `EVT_RESUMED` follows
  it if the run state changed. A reset does not end a step in progress.
  `BUSY` when the runner did not go idle; nothing was reset then. A reset
  from any other source (the text IPC, the menu, a cartridge insert) is
  pushed as `EVT_RESET` too.
- **`STEP` (`0x0102`)** — controller.

  | Size | Field | Meaning |
  |-----:|-------|---------|
  | 1 | `mode` | 0 in, 1 over, 2 out, 3 to address, 4 frames |
  | 1 | reserved | 0 |
  | 2 | `addr` | Target for mode 3 |
  | 4 | `count` | Instructions (modes 0, 1) or frames (mode 4); 0 means 1. Ignored by modes 2 and 3 |
  | 4 | `timeout_ms` | Deadline for modes 1–3, for the whole step; 0 means 5000. Ignored by modes 0 and 4 |

  The machine is paused first if it was running. Reply: empty, sent once the
  step is armed. The outcome is an `EVT_STOPPED` whose `cause_request` is
  this request's `id`. Mode 0 completes before the reply is sent, and its
  event follows the reply.

  - Modes 1 to 3 come with `CAP_BREAKPOINTS`: a server that does not
    announce it answers them `BAD_PAYLOAD`, as every server does a mode
    above 4.
  - **Mode 0** single-steps `count` instructions under the pause mutex, like
    a machine write ([seam 1](#seam-1-the-pause-lease-for-every-change)), so
    every pause and resume in the core waits for it. `count` is therefore at
    most **4096** (about 10 ms): `OUT_OF_RANGE` beyond. Stepping further than
    that is what mode 3 and breakpoints are for. The stepping is blind to
    breakpoints, like the text IPC's `step`.
  - **Mode 4** runs `count` frames, exactly as many as the text IPC's
    `step frame N`, and ends with reason `STEP_DONE`. From a paused machine
    `EVT_RESUMED` comes first. `count` is at most 2147483647.
  - **Modes 1 to 3** are walks ([section 8](#steps-that-take-emulated-time)).
    The reply says the step is armed; the walker thread does the rest.
    - **Mode 1, over**, `count` times: a `CALL` (taken or not) runs to the
      instruction after it; an `RST` is stepped into and its handler run to
      its return, because a CPC firmware restart carries operand bytes after
      the opcode and does not come back to the byte after it; anything else
      is single-stepped. `count` has no limit: the deadline bounds the step.
    - **Mode 2, out**: runs until the routine the CPU is in returns, and
      stops at the instruction after the call. Calls on the way run at full
      speed; everything else is single-stepped, so a long loop in the
      routine itself is slow (some 400 000 instructions a second).
    - **Mode 3, to**: runs until an instruction at `addr` is about to
      execute.
    - They end with `STEP_DONE`; with `BREAKPOINT`, `WATCHPOINT` or
      `IO_BREAKPOINT` when one of those stops the machine first (the event
      then names the step in `cause_*` and the hit in the other fields); with
      `STEP_TIMEOUT` at the deadline, the machine paused where it was; with
      `PAUSED` when somebody pauses. `STEP_STALLED` means no machine was
      there to walk. A walk cannot tell a routine that never returns from a
      slow one, so `JP $` ends in `STEP_TIMEOUT`, never in a guess.
    - A single-stepped instruction is blind to breakpoints, as in mode 0,
      with one exception: step out stops, with `BREAKPOINT`, on a breakpoint
      it steps onto, because the same breakpoint inside a call it skips
      would stop it too.
    - A walk needs one of the probe's 32 execution comparators for its
      one-shot breakpoint: `LIMIT_REACHED` when breakpoints hold them all.
      Nothing was stepped then.
    - `EVT_RESUMED` naming the step comes first if the walk runs a machine
      that was stopped ([section 8](#steps-that-take-emulated-time)).
  - `BUSY` while a step is in progress (this client's, another's, or a walk
    or frame step of the text IPC or the DevTools), when the runner did not
    go idle, and for mode 0 when other lease holders kept the machine.
  - Checked in this order: `NOT_CONTROLLER`, `BAD_PAYLOAD` (too short,
    mode), `OUT_OF_RANGE` (count, mode 0 and 4), `BUSY`, `LIMIT_REACHED`.

### Breakpoints

One family covers execution breakpoints, memory watchpoints and I/O
breakpoints. They are the lists the text IPC (`bp`, `wp`, `iobp`) and the
DevTools edit: an entry set there is listed here, and the other way round.

`kind`: 0 = execution, 1 = memory watch, 2 = I/O.

**An entry is named by its kind and its address**, and an I/O entry by its
port and mask together. `BP_ADD` of a name that exists replaces that entry
(a new condition, pass count, access or length), and its hit count starts
again. The text IPC can hold several entries under one name (`bp add` twice
with different conditions); here they are one, and `BP_ADD` or `BP_DEL` of
that name replaces or removes all of them.

An entry on the wire:

| Size | Field | Meaning |
|-----:|-------|---------|
| 1 | `kind` | |
| 1 | `access` | Watch and I/O: 1 read/in, 2 write/out, 3 both. Execution: 0 |
| 2 | `addr` | Address, or port value for I/O |
| 2 | `len_or_mask` | Watch: length of the range, at least 1. I/O: port mask. Execution: 0 |
| 2 | reserved | 0 |
| 4 | `pass_count` | Break only from this many hits on; 0 and 1 break on the first. At most 2147483647 |
| 4 | `hit_count` | Hits counted so far. Ignored in requests |
| `str` | `condition` | Expression in the text IPC's syntax; empty = always |

- A watch covers `addr` to `addr + len_or_mask − 1` and must end by
  `0xFFFF`: nothing wraps. An I/O entry matches a port `p` when
  `(p & mask) == (addr & mask)`; mask 0 matches every port.
- A hit counts only if the entry's condition holds, and the entry breaks
  once its count has reached `pass_count`. A condition is an expression of
  the text IPC ([ipc-protocol.md](../ipc-protocol.md)): registers by name
  (`a`, `bc`, `pc`, ...), `=`, `<>`, `<`, `and`, `or`, numbers in decimal
  or `0x`/`&`/`$` hex, and for a watch or an I/O entry `address`, `value`
  and `previous`. At an execution breakpoint `pc` is the breakpoint's
  address.
- **`BP_LIST` (`0x0300`)** — anyone. Empty request. Reply:
  `u64 generation`, `u32 count`, then `count` entries: execution, then
  watch, then I/O, each in the order they were set. `generation` is the
  number `EVT_BREAKPOINTS_CHANGED` carries; a client whose list has that
  generation is up to date, hit counts apart.
- **`BP_ADD` (`0x0301`)** — controller. Request: one entry. Reply:
  `u64 generation`.
  - `BAD_PAYLOAD`: the payload is too short; `kind` is not 0 to 2; an
    execution entry with `access` or `len_or_mask` not 0; a watch or I/O
    entry whose `access` is not 1, 2 or 3; a watch of 0 bytes; a
    `pass_count` above 2147483647; a condition that does not parse (the
    message says what the parser said).
  - `OUT_OF_RANGE`: a watch that passes `0xFFFF`.
  - `LIMIT_REACHED`: the probe has no comparator left for it. The probe
    arms 32 execution addresses, 16 watch ranges and 8 port comparators,
    and the entries of every source share them; so does the one-shot
    breakpoint of a walk in progress. An entry that the probe would not arm
    is refused, here. (The text IPC accepts it, lists it, and it never
    fires: beads-zqf3. An entry this port accepted is always armed, because
    the lists are copied into the probe before anything else is.) Replacing
    an entry that exists needs no new comparator and is never refused for
    this.
  - A refused request changes nothing: not the list, not the generation.
  - Checked in this order: `NOT_CONTROLLER`, `BAD_PAYLOAD` (fields),
    `OUT_OF_RANGE`, `BAD_PAYLOAD` (condition), `LIMIT_REACHED`.
- **`BP_DEL` (`0x0302`)** — controller. Request: `u8 kind`, `u8` reserved,
  `u16 addr`, `u16 len_or_mask`, `u16` reserved. Reply: `u64 generation`.
  `len_or_mask` is the mask of an I/O entry and is ignored for the other
  kinds. `NOT_FOUND` if there is no such entry; `BAD_PAYLOAD` for a `kind`
  that is not 0 to 2.
- **`BP_CLEAR` (`0x0303`)** — controller. Request: `u8 kind`, or `0xFF` for
  all three (`BAD_PAYLOAD` for anything else). Reply: `u64 generation`. It
  succeeds, and the generation moves, also when there was nothing to clear.

A breakpoint set while the machine is paused is armed before the next frame
runs. One set while it runs is armed from the next frame on, at most 20 ms
later.

Setting any breakpoint moves the Auto run-tier policy from Fast to Wake, as
it does for breakpoints set any other way.

### Input

Keys are named by their place in the CPC keyboard matrix, which is what the
hardware has: `row` 0–9 and `bit` 0–7. Row 9 is joystick 0 (bit 0 up, 1 down,
2 left, 3 right, 4 fire 2, 5 fire 1) and row 6 carries joystick 1, so
joysticks need no opcode of their own. A frontend maps its host keys to
matrix positions itself.

None of the input requests touches the machine or takes a pause lease. A key
is written to the *pending* keyboard matrix, host state that every key source
writes (the user's keyboard, the text IPC, the `AutoTypeQueue`, this port);
the runner copies it into the matrix the firmware scans at the start of each
frame. So input works on a paused machine, where it waits for the next frame
that runs, and costs a running one nothing. Input therefore has no stamp and
is never `BUSY`.

- **`KEY` (`0x0400`)** — controller. Request: `u8 row`, `u8 bit`, `u8 down`
  (1 press, 0 release), `u8` reserved. Reply: empty. The change is written to
  the keyboard matrix under `g_kbd_matrix_mutex` and the runner picks it up
  with the next frame's keyboard snapshot, like every other input path.
  - **One key and no other.** SHIFT (row 2, bit 5) and CONTROL (row 2,
    bit 7) are keys like any other: the client presses and releases them
    itself, and pressing or releasing another key leaves them as they are.
    (The text IPC's `input keydown A` names a character and so drives the
    two modifiers too; a matrix position does not.)
  - A key stays down until it is released, by `KEY`, by `KEYS_RELEASE`, or
    by the server when the client loses control. Pressing a key that is down
    and releasing one that is up succeed and change nothing.
  - The firmware scans the keyboard once a frame and debounces: hold a key
    for a few frames, and leave a few between two presses of the same key,
    or it may not be seen.
  - `BAD_PAYLOAD` for a payload that is too short or a `down` that is not 0
    or 1; `OUT_OF_RANGE` for a `row` above 9 or a `bit` above 7. Checked in
    this order: `NOT_CONTROLLER`, `BAD_PAYLOAD`, `OUT_OF_RANGE`.
- **`KEYS_RELEASE` (`0x0401`)** — controller. Empty. Reply: empty. Releases
  **every** key of the matrix, whoever pressed it: this client, the text
  IPC's `input keydown`, the user at the keyboard of a GUI core. It is what a
  frontend sends when its window loses focus.
- **Keys held when control is lost.** The server remembers which keys the
  controller pressed with `KEY` and has not released, and which mouse buttons
  it set with `MOUSE`, and whether it pulled the light gun's trigger with
  `GUN`. When the controller disconnects, is dropped for
  silence, sends `CONTROL release` or has control taken by force, the server
  releases exactly those, so a dropped connection never leaves a key held
  down. It does not release keys another source holds: a client that only
  debugs must not let go of a key the user is holding by disconnecting. Text
  already queued by `TEXT` is typed to its end.
- **`TEXT` (`0x0402`)** — controller. Request: `str text`. Queued on the
  `AutoTypeQueue`, with the same `~KEY~` tokens as the text IPC's
  `input type` (`~RETURN~`, `~+SHIFT~` and `~-SHIFT~` to hold and release a
  key, `~PAUSE 50~` for frames, ...; `~~` is skipped). Reply: `u32 pending`,
  the number of key actions now queued, this text and whatever was queued
  before it: a character or a token is one action, a character the CPC
  keyboard cannot type is skipped and counts as none. The typing takes
  emulated time, a few frames a character; `pending` of an empty `TEXT`
  says how much is left.
  - The text is bytes, one character each: ASCII. It is not quoted or
    unquoted (the text IPC strips one pair of wrapping quotes; this port
    types what it is given).
  - `BAD_PAYLOAD` for a payload that is too short and for text the queue does
    not parse: an unknown `~KEY~` token or an unclosed `~`. The message says
    which, and nothing of that request was queued.
- **`MOUSE` (`0x0403`)** — controller. Request: `i16 dx`, `i16 dy`,
  `u8 buttons` (bit 0 left, bit 1 middle, bit 2 right), `u8 flags` (bit 0:
  `buttons` is valid). Reply: empty. Staged and applied by
  `ipc_drain_input()`, which the main loop calls once a frame, to the AMX
  mouse, the Symbiface II mouse, or both, whichever are enabled: the same
  staging as the text IPC's `input mouse`.
  - `dx` and `dy` are relative, in the mouse's own counts (*mickeys*), `dy`
    positive downwards. Movements sent before the main loop has taken them
    add up.
  - With flag bit 0 the three button bits replace the button state; without
    it `buttons` is ignored and the buttons stay as they are. The other bits
    of `buttons` and of `flags` are ignored.
  - `NOT_PRESENT` if no AMX or Symbiface mouse is enabled; `BAD_PAYLOAD` for
    a payload that is too short.
  - The movement reaches the machine a frame or two later, not with the next
    frame: the main loop hands it to the device, and the runner takes it
    from there. In a GUI core those are two threads.
- **`GUN` (`0x0B00`)** — controller, `CAP_GUN`. Request: `u16 x`,
  `u16 y`, `u8 trigger` (1 pulled, 0 let go), `u8 flags` (bit 0: `x` and `y`
  are valid; bit 1: `trigger` is valid). Reply: empty. The light gun
  (`input.lightgun`: the Amstrad Magnum Phaser or the Trojan Light Phazer)
  aimed and fired: staged and applied by `ipc_drain_input()` once a frame,
  like `MOUSE`, through the staging the text IPC's `input gun` uses
  (`ipc_stage_gun()`).
  - The aim is absolute, in pixels of the machine's frame, the one `FB_INFO`
    describes: `x` 0–767, `y` 0–271. A frontend aims at the pixel it shows,
    whatever the core's own window looks like. (The text IPC's
    `input gun move` takes pixels of the core's window and maps them through
    its video plugin; a remote frontend does not know that window.)
  - Without bit 0 the aim stays where it is and `x`, `y` are not looked at;
    without bit 1 the trigger stays as it is and `trigger` is not looked at.
    The other bits of `flags` are ignored.
  - While the trigger is pulled the gun pulses the CRTC's light pen input as
    the beam passes the aim, and the CRTC latches its address in R16 and R17
    (`CHIP_GET` CRTC). The Trojan gun's trigger is also joystick 0's fire 1,
    pressed through the keyboard matrix as the text IPC's `input gun
    trigger` does.
  - A trigger pulled with `GUN` is let go when the controller loses control,
    like a held key; the aim stays.
  - `NOT_PRESENT` if no light gun is enabled; `BAD_PAYLOAD` for a payload
    that is too short or a `trigger` that is not 0 or 1 when bit 1 is set;
    `OUT_OF_RANGE` for an aim outside the frame when bit 0 is set. Checked in
    this order: `NOT_CONTROLLER`, `BAD_PAYLOAD`, `OUT_OF_RANGE`,
    `NOT_PRESENT`.
  - Like `MOUSE`, it reaches the machine a frame or two later.
- **`INPUT_STATE` (`0x0404`)** — anyone. Empty request. Reply: 10 bytes, one
  per matrix row, bit clear = key down (the matrix's own polarity). It is the
  pending matrix, the keys as every source has asked for them, so a `KEY` is
  visible in it at once, on a paused machine too. It is what the text IPC's
  `input state` reports.

### Chip state

**`CHIP_GET` (`0x0500`)** — anyone. Request: `u8 chip`, 3 reserved bytes.
Reply: `stamp`, `u8 chip`, `u8` reserved, `u16 layout`, then the chip's
block. `BAD_PAYLOAD` for a chip that is not in the table.

`layout` is the version of that block. A new layout only appends fields, so a
client that knows layout 1 can read the start of layout 2.

Every field comes from the Device debug structures (`crtc_peek()`,
`ga_peek()`, `psg_peek()`, `fdc_peek()`, `asic_peek()` and the ASIC's debug
accessors), the ones `subcycle_bridge_sync_chip_views()` and
`subcycle_bridge_refresh_asic_view()` read. The server reads them through
the machine gate, so a read of a running machine is taken at a frame boundary
and pauses nothing; it does not read the host view structs (`CRTC`,
`GateArray`, `PSG`, `asic`) from its own threads, which the runner rewrites
once a frame.

| Chip | Id | Layout | Block |
|------|---:|-------:|-------|
| CRTC | 1 | 1 | `u8 type`, `u8 reg_select`, `u8 reg[18]`, `u8 hcc` (horizontal character counter), `u8 vcc` (character line counter), `u8 ra` (raster line within the character), `u8 hsw` (HSYNC width counter), `u8 vsw` (VSYNC width counter), `u8 vta` (vertical total adjust counter), `u16 ma` (memory address), `u16 scanline`. 30 bytes |
| Gate Array | 2 | 1 | `u8 mode`, `u8 requested_mode`, `u8 pen`, `u8 rom_config`, `u8 ram_config`, `u8 upper_rom`, `u8 sl_count` (the 6-bit HSYNC counter raster interrupts fire from), `u8 hs_count` (interrupt delay: the HSYNCs still to count after a VSYNC), `u8 ink[17]` (hardware colour numbers, pen 0–15 then border). 25 bytes |
| PSG | 3 | 1 | `u8 reg[16]`, `u8 reg_select`. 17 bytes |
| FDC | 4 | 1 | `u8 motor`, `u8 track[2]` (physical head position per drive), `u8 present[2]` (a medium is in the drive, as the machine has it), 3 reserved bytes, `u64 generation[2]` (the drive's `EVT_MEDIA_CHANGED` counter). 24 bytes |
| ASIC | 5 | 2 | See below. 154 bytes |

The ASIC's block, in wire order:

| Size | Field | Layout | Meaning |
|-----:|-------|-------:|---------|
| 1 | `unlocked` | 1 | 1 once the unlock sequence has been sent |
| 1 | `pri_line` | 1 | Programmable raster interrupt line, 0 = off |
| 2 | reserved | 1 | 0 |
| 1 | `lock_pos` | 2 | How far into the unlock sequence the ASIC is |
| 1 | `page_on` | 2 | 1 while the register page is paged in at `0x4000` |
| 1 | `hscroll` | 2 | Soft scroll, horizontal (0–15) |
| 1 | `vscroll` | 2 | Soft scroll, vertical (0–7) |
| 1 | `extend_border` | 2 | The scroll register's border bit |
| 1 | `int_vector` | 2 | Interrupt vector register |
| 1 | `split_line` | 2 | Split screen line |
| 1 | reserved | 2 | 0 |
| 2 | `split_addr` | 2 | Display address below the split |
| 2 | reserved | 2 | 0 |
| 3 × 14 | `dma[3]` | 2 | Per sound DMA channel: `u16 source` (the address it reads next), `u16 loop_addr`, `u16 loops` (iterations left), `u16 pause_ticks` (what is left of a pause), `u16 tick_cycles` (the prescaler's own counter), `u8 prescaler`, `u8 enabled`, `u8 irq` (the channel's interrupt flag), `u8` reserved |
| 16 × 6 | `sprite[16]` | 2 | Per sprite: `u16 x`, `u16 y`, `u8 mag_x`, `u8 mag_y` (0 = not shown, else 1, 2 or 4) |

Sprite pixels and the palette are not in the block; they are read raw from
the memory space `ASIC_PAGE` ([section 11](#11-memory)).

`ASIC` answers `NOT_PRESENT` unless the machine has one (a 6128+).

Layout 1 was deliberately no more than the text IPC's `regs crtc|ga|psg`
reports, so that it could be checked against it field for field; the test
suite does, between frames, in the middle of one and after a program has
written the chips. (Doing so found that `regs ga` printed `INT_DELAY` from a
host field nothing had written since the Devices took over, always `00`; it
prints `hs_count` now.)

**Which window reads what** (audited for slice 7, because phase 5 draws
these windows from this port):

| Window or surface | Reads | From |
|-------------------|-------|------|
| Video State | CRTC `reg[18]`; Gate Array mode, ROM and RAM configuration, pen | `CHIP_GET` CRTC, Gate Array |
| Audio State | PSG `reg[16]` (tone, noise, mixer, amplitudes, envelope) | `CHIP_GET` PSG |
| Disassembly, Memory | Gate Array `rom_config`, `ram_config` (the bank is its low 3 bits) | `CHIP_GET` Gate Array, or `MEM_INFO` |
| Top bar, status line | CRTC `type`, Gate Array `rom_config`, `upper_rom` | `CHIP_GET`, `MACHINE_INFO` |
| Drive status, Disc Tools | FDC `motor`, `track[2]`, the drives' generations | `CHIP_GET` FDC, `EVT_MEDIA_CHANGED` |
| Graphics finder, palette strip | Gate Array `ink[17]` | `CHIP_GET` Gate Array |
| ASIC Registers | Lock state and `lock_pos`; sprite positions and magnification; the DMA channels' address, loop, prescaler, pause, loops, interrupt flag and tick counter; raster interrupt; interrupt vector; scroll; border extension | `CHIP_GET` ASIC, layout 2 |
| Text IPC `asic palette`, `asic sprite` | The palette and the sprite pixels | `ASIC_PAGE` |

Only the ASIC needed more than layout 1. The page holds what a program
wrote, and the DMA channels' loop target, loop count, pause and tick
counters, and the position in the unlock sequence, are the ASIC's own state
and appear nowhere in it; the rest of layout 2 (scroll, vector, split, sprite
positions) is in the page too, and is sent decoded so that a client need not
know which registers are write-only on the real chip. The CRTC's and the
PSG's blocks are no wider than the windows need, and a field no window reads
(the PSG's envelope position, the FDC's command phase and status bytes) was
left out rather than guessed at: a layout can grow, and cannot shrink.

### Symbols

The table is the one the text IPC's `sym` commands and the DevTools' Symbols
window read and edit. An address has one name and a name stands for one
address: adding a name that is elsewhere moves it, and a second name for an
address replaces the first. (The table used to keep the old address of a
moved name, as an entry `SYM_DEL` could never reach.)

- **`SYM_LIST` (`0x0600`)** — anyone. Empty request. Reply: `u64 generation`,
  `u32 count`, then `count` × (`u16 addr`, `str name`), in address order.
  `TOO_LARGE` when the list does not fit the connection's `max_frame`.
- **`SYM_ADD` (`0x0601`)** — controller. Request: `u16 addr`, `str name`.
  Reply: `u64 generation`. A name is one token of a symbol file line: 1 to
  255 bytes, no blanks, no control characters and no `;` (`BAD_PAYLOAD`
  otherwise, and nothing is added).
- **`SYM_DEL` (`0x0602`)** — controller. Request: `str name`. Reply:
  `u64 generation`. `NOT_FOUND` if absent.
- **`SYM_LOAD` (`0x0603`)** — controller. Request: `str path`, a file on the
  core's machine, in the text IPC's `sym load` format. Reply:
  `u64 generation`, `u32 loaded` (the symbols the file held). Its symbols are
  added to the table as one change; breakpoints and entry points in the file
  are ignored, as `sym load` ignores them. `NOT_FOUND` when the file cannot
  be opened; `BAD_PAYLOAD` for an empty path or one that still has a `..` in
  it once it is normalised, the rule of [Media](#media).

`generation` grows with every change of the table and is the value
`EVT_SYMBOLS_CHANGED` carries. None of these requests touches the machine,
takes a pause lease or has a stamp.

### Video

**`FB_INFO` (`0x0700`)** — empty request. Reply:

| Size | Field | Value in version 1 |
|-----:|-------|--------------------|
| 2 | `width` | 768 (`kFbWidth`) |
| 2 | `height` | 272 (`kFbHeight`) |
| 1 | `format` | 1 = RGB24, three bytes per pixel, R first |
| 1 | `transport` | 1 = the shared-memory frame ring; 0 = none (the core could not create it) |
| 2 | reserved | 0 |
| 4 | `stride` | 2304 |
| 8 | `fb_seq` | Sequence number of the ring's latest frame at the time of the reply; 0 before the first, or without a ring |
| `str` | `shm_name` | The ring's OS name, to open as it is: `/kcpc-fb-<pid>` on Linux and macOS, `Local\kcpc-fb-<pid>` on Windows; empty without a ring |

This is the machine's canonical frame, before scaling or scanlines (epic
invariant 1). Everything but `fb_seq` is the same for the life of the core.
The ring's layout and how to read it without a torn frame are
[fb-ring-v1.md](fb-ring-v1.md); `kcpc_client.FbRing`
(`test/integrated/kcpc/kcpc_client.py`) and `tools/fb_dump.c` are two
readers. When the core could not create the region it logs why, runs on
without it, `transport` is 0 and `HELLO` does not announce `CAP_FB_SHM`.

The text IPC's `hash frame` is the CRC32 of exactly these bytes (`height`
rows of `stride` bytes, top row first), copied between two frames, and says
after which frame it was taken: a client that reads the frame some other way
compares its own hash with that one (docs/ipc-protocol.md, Hashes).

### Media

Paths are files on the core's machine, in the core's filesystem. A relative
path is resolved against the core's working directory and made absolute
before it is used, stored on a Recent list or reported, as the text IPC's
`load` does. A path that is empty, or that still has a `..` in it once it is
normalised, is `BAD_PAYLOAD`: the rule the text IPC's `load` and
`snapshot` apply.

The changes are controller only and run under a pause lease, while it is the
only lease held. They reuse the loaders in `src/slotshandler.cpp`
(`file_load()`, `dsk_eject()`, `tape_eject()`) and the bridge's media swap;
none of them adds a second loading path. The swap those loaders queue for the
next frame boundary is applied before the lease is let go
(`subcycle_bridge_apply_pending_media()`, as the text IPC's `disk eject`
does), so **when the reply is sent the machine holds what the request put
there**: no client has to wait for a frame, or poll `CHIP_GET`, after its own
insert.

`slot`: 0 = drive A, 1 = drive B, 2 = tape, 3 = cartridge. The same numbers
as `EVT_MEDIA_CHANGED`. A number above 3 is `BAD_PAYLOAD`.

- **`MEDIA_INSERT` (`0x0800`)** — Request: `u8 slot`, `u8 flags` (reserved:
  must be 0, `BAD_PAYLOAD` otherwise), 2 reserved bytes, `str path`. Reply:
  `u64 generation`, the slot's when the machine has the medium.
  - Drives accept `.dsk`, `.ipf`, `.raw`; drive A also accepts the flux
    images `.scp`, `.hfe`, `.a2r` (flux is drive A only: `BAD_PAYLOAD` on
    drive B).
  - Tape accepts `.cdt` and `.voc`. Cartridge accepts `.cpr` and answers
    `NOT_PRESENT` unless the model is a 6128+; inserting one resets the
    machine, as it does from the menu, and that is pushed as `EVT_RESET`.
  - A `.zip` is opened and its first member of a kind the slot takes is
    used; an archive with none is `BAD_PAYLOAD`.
  - `NOT_FOUND` if the file is missing or is not a file; `BAD_PAYLOAD` if
    the type does not fit the slot (judged by the file's name) or the image
    is malformed. Checked in this order: `NOT_CONTROLLER`, `BAD_PAYLOAD`
    (slot, flags, path), `NOT_FOUND`, `BAD_PAYLOAD` (type), `BUSY`,
    `NOT_PRESENT`, `BAD_PAYLOAD` (malformed).
  - **A disc that is refused changes nothing**: the drive keeps the disc it
    had, in the host's view and in the machine, its file stays the one
    `MEDIA_INFO` names, and no event is sent. The image is tried on copies
    before either is touched (`disc_insert()`, `src/slotshandler.h`); the
    text IPC's `load`, the menus and drag and drop go the same way. A
    cartridge is the same: a file that is no cartridge leaves the one that
    was there, and no reset happens. So is the tape deck: a file that is no
    tape leaves the tape that was there (`tape_deck_insert()`).
  - **The outgoing disc keeps its writes.** A disc that was written to is
    written back to its own file when the new one takes its place.
  - There is no write-protect flag. The FDC has no switch for it: a medium
    is write-protected when it has no image the FDC can write to (a flux
    dump without an overlay), and `MEDIA_INFO` says so. A flag the core
    could not honour would be a lie, so the bit is reserved (beads-uxle).
- **`MEDIA_EJECT` (`0x0801`)** — Request: `u8 slot`, 3 reserved bytes.
  Reply: `u64 generation`. A drive with unsaved changes is written back to
  its file first (the path the File menu's eject takes) and never raises a
  dialog. Ejecting an empty drive or deck succeeds, and is announced like
  any other eject. The cartridge cannot be ejected (`BAD_PAYLOAD`): a 6128+
  does not run without one, so a cartridge is replaced, with `MEDIA_INSERT`.
- **`MEDIA_INFO` (`0x0802`)** — anyone. Request: `u8 slot`, 3 reserved
  bytes. Reply: `u8 slot`, `u8 present`, `u8 flags`, `u8` reserved,
  `u64 generation`, `str path`. For the tape slot three more fields follow:
  `u8 playing`, `u8 motor`, 2 reserved bytes, `u32 block` (the block the
  deck is at), `u32 blocks` (blocks in the image).
  - `present`, `generation` and `path` are what `EVT_MEDIA_CHANGED` says:
    what the host last put in the slot.
  - `flags` is read off the machine, through the gate, so it pauses
    nothing: bit 0 write-protected (a disc the FDC cannot write to), bit 1
    unsaved changes (written to since it was loaded or last written back),
    bit 2 flux image. All three are 0 for the tape and the cartridge.
  - `playing` is the PLAY button, `motor` the relay the CPC switches.
- **`TAPE_CONTROL` (`0x0803`)** — Request: `u8 action` (0 stop, 1 play,
  2 rewind, 3 seek; anything else `BAD_PAYLOAD`), 3 reserved bytes,
  `u32 block` (for seek). Reply: the tape fields of `MEDIA_INFO`, as the deck
  is after the change. `NOT_PRESENT` with no tape loaded; `OUT_OF_RANGE` for
  a block past the end. The deck is moved under the request's lease, so the
  reply already shows it there. (The tape window of the GUI defers its seeks
  to a frame boundary with `subcycle_bridge_request_tape_seek()` for the same
  reason: the deck must not move under a running frame. Under a lease no
  frame is running.) It produces no event: a client that shows the deck asks
  `MEDIA_INFO`.

Every successful insert and eject, from this port or any other source,
produces `EVT_MEDIA_CHANGED`. A refused disc, tape or cartridge produces
none: nothing changed. The events of a request name it and follow its reply.

### Snapshots

The format is SNA and nothing else (epic invariant 4). Paths follow the rules
of [Media](#media).

- **`SNAPSHOT_SAVE` (`0x0900`)** — controller. Request: `str path`. Reply:
  `stamp`. Writes with `snapshot_save()`, which is
  `snapshot_save_machine()` on the live machine, under a pause lease; the run
  state is put back. It needs the controller role because it writes a file
  on the core's machine, not because it changes the CPC. `NOT_FOUND` when
  the directory does not exist, `BAD_PAYLOAD` when the path is a directory,
  `INTERNAL` when the file could not be written (the message says so).
- **`SNAPSHOT_LOAD` (`0x0901`)** — controller. Request: `u8 flags` (bit 0:
  stay paused afterwards), 3 reserved bytes, `str path`. Reply: `stamp`.
  Loads with `snapshot_load()` (`snapshot_load_machine()` on the live
  machine). The run state is restored to what it was unless bit 0 is set;
  with it the machine is left stopped, announced as `EVT_STOPPED` with
  reason `PAUSED` and this request as its cause. Clients are told with
  `EVT_RESET`, after the reply; a client re-reads everything it shows.
  `NOT_FOUND` for a missing file, `BAD_PAYLOAD` for a file that is not a
  valid SNA.
  - **A snapshot never changes the machine.** One that holds more RAM than
    the machine has is refused (`BAD_PAYLOAD`, and nothing was loaded); one
    saved on another model is loaded as it is, into the model that is
    running. So no `EVT_MACHINE_CHANGED` follows a load. A client that wants
    the snapshot's machine sets it up first with `CONFIG_SET` and
    `CONFIG_APPLY`.
  - The media stay where they are: a snapshot is not a medium.

### Configuration

The keys and values are the text IPC's `config get|set`, spelled the same:
`model`, `crtc_type`, `crtc_info`, `ram_size`, `silicon_disc`, `m4board`,
`m4_sd_path`, `m4_rom_slot`, `kbd_layout`, `kbd_layouts`, `fullscreen`,
`window`, `file`. Carrying them as strings keeps one list of keys in one
place instead of a second, numbered copy that would drift: both servers call
`ipc_config_get()`, `ipc_config_set()` and `ipc_config_apply()`
(`src/koncepcja_ipc_server.h`), and the text IPC only formats what they
return.

- **`CONFIG_GET` (`0x0A00`)** — anyone. Request: `str key`. Reply:
  `u8 flags` (bit 0: a different value is staged and waits for
  `CONFIG_APPLY` or the next frame), 3 reserved bytes, `str value`,
  `str pending` (empty unless bit 0). `NOT_FOUND` for an unknown key;
  `NOT_PRESENT` for a key that needs something this run does not have
  (`fullscreen` and `window` under `-H`). `kbd_layouts` answers with one
  file name per line. The settings a rebuild rewrites (`model`, `crtc_type`,
  `ram_size`, ...) are read through the gate, so a get pauses nothing and
  never reads beside a rebuild.
- **`CONFIG_SET` (`0x0A01`)** — controller. Request: `str key`,
  `str value`. Reply: `u8 effect` (0 = applied now, 1 = applied on the next
  frame, 2 = staged, needs `CONFIG_APPLY`), 3 reserved bytes. `NOT_FOUND`
  for an unknown key, or one that can only be read; `BAD_PAYLOAD` for a
  value the key rejects, with the reason as the message. It runs under a
  pause lease, whatever the key: `crtc_type` and `silicon_disc` change the
  machine.

  | Key | Effect |
  |-----|--------|
  | `crtc_type`, `silicon_disc`, `m4_sd_path` | 0, applied now |
  | `kbd_layout`, `fullscreen` | 1, the main loop applies it on its next frame |
  | `model`, `ram_size`, `m4board` | 2, staged |

  A staged `model` shows as `pending` in `CONFIG_GET` until it is applied.
  `ram_size` and `m4board` are settings the next rebuild reads, and
  `CONFIG_GET` shows them as set at once; what the machine has fitted is in
  `MACHINE_INFO`.
- **`CONFIG_APPLY` (`0x0A02`)** — controller. Empty. Rebuilds the machine
  with the staged settings through the main-thread rebuild request the text
  IPC's `config apply` uses (`koncpc_rebuild_machine()`, under the main
  thread's own lease; the reader thread holds none while it waits). This is
  the one request allowed to take longer than a pause lease: the reply is
  sent when the rebuild has finished, with `stamp`, or with `INTERNAL` if it
  failed (the machine is then left paused) and `BUSY` if the main loop did
  not pick it up in time or the runner did not go idle.
  `EVT_MACHINE_CHANGED` follows a successful rebuild, after the reply. The
  controller's 30 s of allowed silence count from that reply.

`CONFIG_SET` does not write the configuration file. Saving settings stays a
GUI action.

### Session

- **`PING` (`0x0002`)** — empty both ways.
- **`SUBSCRIBE` (`0x0003`)** — section 9.
- **`CONTROL` (`0x0004`)** — Request: `u8 action` (0 release, 1 acquire),
  `u8 flags` (bit 0 = force: take control even if another client has it),
  2 reserved bytes. Reply: `u8 role` now held, 3 reserved bytes,
  `u32 controller` (its `client_id`, 0 when control is free). Asking without
  force while another client has control is not an error: the reply simply
  still says observer, and names the controller. With force the reply always
  says controller. Releasing what the client does not hold, and acquiring
  what it already holds, change nothing and reply the same way. An `action`
  other than 0 or 1 is `BAD_PAYLOAD`; flag bits other than bit 0 are
  ignored.

## 13. Opcode table

| Opcode | Name | Capability | Who | Slice |
|-------:|------|------------|-----|------:|
| `0x0000` | `ERROR` | — | server only | 1 |
| `0x0001` | `HELLO` | — | anyone | 1 |
| `0x0002` | `PING` | — | anyone | 1 |
| `0x0003` | `SUBSCRIBE` | `CAP_EVENTS` | anyone | 4 |
| `0x0004` | `CONTROL` | `CAP_MACHINE_WRITE` | anyone | 3 |
| `0x0005` | `MACHINE_INFO` | — | anyone | 1 |
| `0x0100` | `PAUSE` | `CAP_RUN_CONTROL` | controller | 4 |
| `0x0101` | `RUN` | `CAP_RUN_CONTROL` | controller | 4 |
| `0x0102` | `STEP` | `CAP_RUN_CONTROL` | controller | 4, 5 |
| `0x0103` | `RESET` | `CAP_RUN_CONTROL` | controller | 4 |
| `0x0104` | `RUN_STATE` | `CAP_RUN_CONTROL` | anyone | 4 |
| `0x0200` | `REGS_GET` | `CAP_MACHINE_READ` | anyone | 2 |
| `0x0201` | `REGS_SET` | `CAP_MACHINE_WRITE` | controller | 3 |
| `0x0210` | `MEM_INFO` | `CAP_MACHINE_READ` | anyone | 2 |
| `0x0211` | `MEM_READ` | `CAP_MACHINE_READ` | anyone | 2 |
| `0x0212` | `MEM_WRITE` | `CAP_MACHINE_WRITE` | controller | 3 |
| `0x0300` | `BP_LIST` | `CAP_BREAKPOINTS` | anyone | 5 |
| `0x0301` | `BP_ADD` | `CAP_BREAKPOINTS` | controller | 5 |
| `0x0302` | `BP_DEL` | `CAP_BREAKPOINTS` | controller | 5 |
| `0x0303` | `BP_CLEAR` | `CAP_BREAKPOINTS` | controller | 5 |
| `0x0400` | `KEY` | `CAP_INPUT` | controller | 6 |
| `0x0401` | `KEYS_RELEASE` | `CAP_INPUT` | controller | 6 |
| `0x0402` | `TEXT` | `CAP_INPUT` | controller | 6 |
| `0x0403` | `MOUSE` | `CAP_INPUT` | controller | 6 |
| `0x0404` | `INPUT_STATE` | `CAP_INPUT` | anyone | 6 |
| `0x0500` | `CHIP_GET` | `CAP_CHIPS` | anyone | 7 |
| `0x0600` | `SYM_LIST` | `CAP_SYMBOLS` | anyone | 7 |
| `0x0601` | `SYM_ADD` | `CAP_SYMBOLS` | controller | 7 |
| `0x0602` | `SYM_DEL` | `CAP_SYMBOLS` | controller | 7 |
| `0x0603` | `SYM_LOAD` | `CAP_SYMBOLS` | controller | 7 |
| `0x0700` | `FB_INFO` | `CAP_FB_INFO` | anyone | 7 |
| `0x0800` | `MEDIA_INSERT` | `CAP_MEDIA` | controller | 9 |
| `0x0801` | `MEDIA_EJECT` | `CAP_MEDIA` | controller | 9 |
| `0x0802` | `MEDIA_INFO` | `CAP_MEDIA` | anyone | 9 |
| `0x0803` | `TAPE_CONTROL` | `CAP_MEDIA` | controller | 9 |
| `0x0900` | `SNAPSHOT_SAVE` | `CAP_SNAPSHOT` | controller | 9 |
| `0x0901` | `SNAPSHOT_LOAD` | `CAP_SNAPSHOT` | controller | 9 |
| `0x0A00` | `CONFIG_GET` | `CAP_CONFIG` | anyone | 9 |
| `0x0A01` | `CONFIG_SET` | `CAP_CONFIG` | controller | 9 |
| `0x0A02` | `CONFIG_APPLY` | `CAP_CONFIG` | controller | 9 |
| `0x0B00` | `GUN` | `CAP_GUN` | controller | 10 |
| `0x8001`–`0x800B` | events | `CAP_EVENTS` | server only | 4, 5, 7 |

Reserved: the unused opcodes of the media (`0x08xx`), snapshot (`0x09xx`)
and configuration (`0x0Axx`) ranges, and `0x0B01`–`0x0BFF` for other input
devices.

## 14. A session, byte by byte

A client connects, says hello as an observer and pings. Payload bytes are
shown after the `|`.

```
C→S  4B 43 50 43  00  01  01 00  01 00 00 00  14 00 00 00 |      HELLO request, id 1, 20 bytes
     00 00  00  00  00 00 00 00  00 00 00 00 00 00 00 00         minor 0, observer, max_frame default, no caps
     02 00 70 79                                                 client_name "py"

S→C  4B 43 50 43  00  02  01 00  01 00 00 00  .. .. .. .. |      HELLO response, id 1
     00 00 00 00                                                 status OK
     00 00  00  00  00 00 00 01  ...                             minor 0, observer, max_frame 16 MiB, caps, ...

C→S  4B 43 50 43  00  01  02 00  02 00 00 00  00 00 00 00        PING request, id 2, no payload

S→C  4B 43 50 43  00  02  02 00  02 00 00 00  04 00 00 00 |      PING response, id 2
     00 00 00 00                                                 status OK
```

With a `RUN` subscription and a breakpoint set, a hit arrives with no request
behind it:

```
S→C  4B 43 50 43  00  03  01 80  07 00 00 00  18 00 00 00 |      EVT_STOPPED, event number 7, 24 bytes
     2A 01 00 00 00 00 00 00                                     frame 298
     02  00  00 40  00 40  00  00                                BREAKPOINT, pc 0x4000, addr 0x4000
     00 00 00 00  00 00 00 00                                    nobody's request caused it
```

## 15. Security

The same position as the text IPC today:

- The listening socket binds `127.0.0.1` and nothing else. There is no option
  to bind another address.
- No authentication and no encryption. Any process of any local user that can
  reach the loopback port can read and change the emulated machine, make the
  core read any file the core's user can read (`SYM_LOAD`, `MEDIA_INSERT`,
  `SNAPSHOT_LOAD`) and write an SNA file anywhere that user can write
  (`SNAPSHOT_SAVE`). The `..` rule of [Media](#media), which all four
  apply, keeps a relative path inside the core's working directory; it is
  no sandbox, since an absolute path is accepted. That is already true of port 6543 (`load`,
  `snapshot save`), so this port adds no new exposure, and it must not be
  forwarded off the machine.
- `[ipc] binary_port_enabled=0` (section 3) closes the port for users who do
  not want it.
- The parser trusts nothing: every length is checked against the payload that
  actually arrived before it is used, every index against the table it
  indexes, and a request is bounded at 1 MiB before any allocation.
- The 8-client limit, the queue caps and the controller idle timeout bound
  what a misbehaving local client can cost the core.

## 16. Implementation slices

Each slice is one bead, one branch and one pull request, and each leaves the
server working with a larger `caps` mask. The Python client and its test file
start in slice 1 and grow with every slice, so each slice is written
test-first against a real `-H` core.

| # | Slice | Brings |
|--:|-------|--------|
| 1 | Server skeleton (**landed**, beads-3o1.1) | Socket shim, listener and port probing, the `[ipc] binary_port_enabled` key, the acceptor, reader and writer threads, framing codec, `HELLO`, `PING`, `ERROR`, `MACHINE_INFO`, the manifest line; built in the GUI, `-H` and UI-free builds; Python client and test wired into CI on Linux, macOS and Windows |
| 2 | Machine reads (**landed**, beads-3o1.2) | The machine gate and `kcpc_service_frame()`; `REGS_GET`, `MEM_INFO`, `MEM_READ` with the spaces `CPU`, `CPU_RAM`, `PHYS_RAM`, `ROM_LOWER`, `ROM_UPPER` (`ASIC_PAGE` comes with slice 7); `MACHINE_INFO` moved onto the gate; `CAP_MACHINE_READ` and `CAP_MEM_ROM` |
| 3 | Control and writes (**landed**, beads-3o1.3) | Roles, `CONTROL` with forced takeover, the controller idle timeout and `IDLE_TIMEOUT`; `REGS_SET`, `MEM_WRITE` to `CPU_RAM` and `PHYS_RAM`; `CAP_MACHINE_WRITE`; `REGS_GET` shows the breakpoint's address at a breakpoint |
| 4 | Run control and events (**landed**, beads-3o1.4) | The event ring and fan-out, `SUBSCRIBE`, the announced run state; `PAUSE`, `RUN`, `RESET`, `RUN_STATE`, `STEP` in and frames; `EVT_STOPPED`, `EVT_RESUMED`, `EVT_RESET`, `EVT_FRAME`, `EVT_OVERRUN`, `EVT_SHUTDOWN`, `EVT_CONTROL_CHANGED`; `CAP_RUN_CONTROL` and `CAP_EVENTS` |
| 5 | Breakpoints and pushed hits (**landed**, beads-3o1.5) | `BP_LIST`, `BP_ADD`, `BP_DEL`, `BP_CLEAR` on the editing lists the text IPC and the DevTools share; `EVT_STOPPED` says what was hit; `EVT_BREAKPOINTS_CHANGED` for edits from any source; the walker and `STEP` over, out, to; walks announced as one step, whoever walks; `CAP_BREAKPOINTS` |
| 6 | Input (**landed**, beads-3o1.6) | `KEY`, `KEYS_RELEASE`, `TEXT`, `MOUSE`, `INPUT_STATE` on the paths the text IPC's `input` commands use; the keys and mouse buttons a controller holds are released when it loses control; `CAP_INPUT` |
| 7 | DevTools data (**landed**, beads-3o1.7) | `CHIP_GET` for the CRTC, the Gate Array, the PSG, the FDC and the ASIC (layout 2), the memory space `ASIC_PAGE`; `SYM_LIST`, `SYM_ADD`, `SYM_DEL`, `SYM_LOAD` on a symbol table that now has a lock; `FB_INFO` with no transport; `EVT_MEDIA_CHANGED`, `EVT_SYMBOLS_CHANGED`, `EVT_MACHINE_CHANGED` for changes from any source; `CAP_CHIPS`, `CAP_SYMBOLS`, `CAP_FB_INFO` |
| 8 | Exit test and documentation (**landed**, beads-3o1.8) | The exit test of beads-3o1 (`test_exit_criterion.py`), a refusal checked for every opcode of [section 13](#13-opcode-table), the documentation brought in line with the code; `SYM_LOAD` takes the `..` rule of the other paths, and a refused disc changes nothing on either port. **No freeze:** the server stays at major 0 |
| 9 | Media, snapshots, configuration (**landed**, beads-rcyq) | `MEDIA_INSERT`, `MEDIA_EJECT`, `MEDIA_INFO`, `TAPE_CONTROL` on the loaders and the media swap the text IPC and the menus use, with the machine's half applied under the request's lease; `SNAPSHOT_SAVE`, `SNAPSHOT_LOAD` (SNA); `CONFIG_GET`, `CONFIG_SET`, `CONFIG_APPLY` on one table of keys shared with the text IPC; `CAP_MEDIA`, `CAP_SNAPSHOT`, `CAP_CONFIG` |
| 10 | The light gun (beads-il6h) | `GUN`, aimed in frame pixels, on the staging the text IPC's `input gun` uses; a trigger the controller pulled is let go when it loses control; `CAP_GUN` |
| 11 | The frame ring named (beads-0ir.3, phase 4) | `FB_INFO` with `transport` 1, `shm_name` and the live `fb_seq`; `EVT_FRAME` with the frame's `fb_seq`; `CAP_FB_SHM`; `tools/fb_dump.c` and the client's `FbRing`; the exit test of phase 4 (`test_fb_ring.py`, below) |

Slice 9 needs slices 3 (the controller role) and 4 (events) and nothing
later, so it can be built alongside slices 5 to 7. Slice 8 is numbered before
it because it carries the exit criterion below, which does not involve
media; phase 3 is finished when both have merged, and both have.

The `[ipc] binary_port_enabled` key and forced takeover belong to slices 1
and 3.

**After phase 3:** the protocol is frozen as major 1, minor 0 only once
phase 5 (beads-3sp) has shipped the remote GUI on top of it. That freeze is
not one of these slices.

Source layout the slices should follow:

- `src/kcpc/kcpc_proto.h` — opcodes, status codes, capability bits, and a
  bounds-checked reader and writer for the field types of section 4. Pure, no
  sockets, unit-tested in `test_runner`.
- `src/kcpc/kcpc_server.{h,cpp}` — the threads of section 8. It knows
  nothing about the emulator: what it needs from the machine comes through
  callbacks, so `test_runner` runs the real threads and sockets against a
  fake machine.
- `src/kcpc/kcpc_host.{h,cpp}` — the other side of those callbacks: the one
  server instance, started and stopped by `kon_cpc_ja.cpp`, and the code
  that reads and changes the machine for it.
- `src/kcpc/kcpc_events.h` — the event record, its encoding and the ring.
  Emulator-free and header-only; `test/kcpc_events_test.cpp` runs several
  producers against it.
- `src/koncepcja.h`, `src/kon_cpc_ja.cpp` — the announced run state:
  `CpcRunCause`, `cpc_set_run_state_observer()`, `cpc_run()`,
  `cpc_runner_parked()`. `test/run_state_announce_test.cpp` covers the
  reconciliation without any server.
- `src/koncepcja.h`, `src/kon_cpc_ja.cpp` again — walks: `CpcWalk`,
  `cpc_walk_run()`, `cpc_walk_verdict()`, `cpc_walk_interrupted()`; and what
  a hit carries to its announcement (`CpcRunCause::addr`, `access`, `value`,
  `old_value`, through `cpc_commit_breakpoint_stop()`).
- `src/z80_view.{h,cpp}` — the breakpoint lists as one family (`z80_bp_put()`,
  `z80_bp_remove()`, `z80_bp_clear()`, `z80_bp_list()`), the hook every edit
  of the lists calls (`z80_set_breakpoints_changed_hook()`), and the walks
  (`z80_run_until_ephemeral()`, `z80_step_out_until()`, `z80_step_over()`).
  `test/z80_bp_family_test.cpp` covers the lists and their limits without a
  machine.
- `src/subcycle_bridge.{h,cpp}` — `subcycle_bridge_write_regs()`, the one
  place a register edit reaches the machine from (this port by mask, the
  text IPC and the DevTools by what differs in the host view), and
  `subcycle_bridge_shown_regs()`, the registers with the breakpoint's
  address as `PC` at a breakpoint.
- `src/keyboard.{h,cpp}` — the keyboard matrix by position
  (`kbd_matrix_set_line()`, `kbd_matrix_release_all()`, `kbd_matrix_read()`):
  one line and no modifier handling, under `g_kbd_matrix_mutex`. And
  `ipc_stage_mouse()` in `src/koncepcja_ipc_server.{h,cpp}`, the staging the
  text IPC's `input mouse` and `MOUSE` share. `test/kcpc_input_test.cpp`
  covers both and the `kcpc_host_*` functions over them.
- `src/symfile.{h,cpp}` — the symbol table with its lock, its generation,
  `addSymbols()` (a batch as one change) and the one hook every change calls
  (`Symfile::set_changed_hook()`). `test/symfile.cpp` runs readers and
  writers against it.
- `src/slotshandler.{h,cpp}` — what each slot holds and when it changed:
  `media_note()`, `media_changed()`, `media_state()`, `media_generation()`
  and the hook (`media_set_changed_hook()`). The drives' counters are
  `dsk_media_generation()`'s; the tape deck and the cartridge port have
  their own. `test/disc_media_generation_test.cpp` covers the loaders.
- `src/koncepcja.h`, `src/kon_cpc_ja.cpp` once more —
  `cpc_set_machine_rebuilt_hook()`, called by `koncpc_rebuild_machine()`
  while it still holds its lease.
- `src/koncepcja_ipc_server.{h,cpp}` — the configuration keys as one table
  for both servers: `ipc_config_machine()`, `ipc_config_get()`,
  `ipc_config_set()`, `ipc_config_apply()`; and `ipc_is_safe_path()`, the
  rule a path from a client must pass.
- `src/hw/asic.{h,cpp}`, `src/hw/fdc.{h,cpp}` — `asic_page_copy()` (the
  register page in one copy) and `fdc_media_present_unit()`.
- `src/kcpc/kcpc_gate.{h,cpp}` — the machine gate. Like the server it
  knows nothing about the emulator (the pause flag and the lease are two
  callbacks), so `test/kcpc_gate_test.cpp` runs real reader and runner
  threads against a fake machine, and those tests build under
  ThreadSanitizer.
- `src/hw/memory.{h,cpp}` — `mem_peek_cpu_range()`, `mem_peek_ram_range()`,
  `mem_lower_rom()`, `mem_upper_rom()`: whole views and ROMs for
  `MEM_READ`, through the same banking decisions as the per-byte peeks.
- `src/net_socket.{h,cpp}` — the one place with `#ifdef _WIN32` around
  sockets. The text IPC's acceptor and client loop run on it too
  (beads-o3j0).
- `test/integrated/kcpc/kcpc_client.py`, `test/integrated/kcpc/test_kcpc.py`,
  `test/integrated/kcpc/test_exit_criterion.py`
  — standard library only (`socket`, `struct`, `threading`), so the Windows
  CI job needs nothing installed. `test_kcpc.py` starts its own cores:
  with `-H` by default, `--ui-free` for a `KONCPC_MODERN_UI=0` binary and
  `--gui` for the GUI build under SDL's dummy drivers. `make e2e_test` runs
  the first and the last; the UI-free CI steps run the second.
- `test/kcpc_proto_test.cpp`, `test/kcpc_server_test.cpp` — the unit tests
  in `test_runner`.

### Exit criterion of phase 3

A Python test starts one `-H` core and, with no polling loop anywhere:

1. opens **two** connections, one controller and one observer;
2. the controller sets a breakpoint and runs; **both** connections receive
   the pushed `EVT_STOPPED` with reason `BREAKPOINT`;
3. the observer reads **65536 bytes** of the `CPU` space in **one** response
   frame and the bytes match the text IPC's `mem read` of the same range.

It passes on Linux, macOS and Windows CI, against `-H` and the UI-free build.

The test is `test/integrated/kcpc/test_exit_criterion.py`. It waits only in
blocking reads (the startup manifest is read from the core's pipe line by
line, every event from the socket), and a watchdog timer kills a core that
hangs, so a hang is a failed read and never a stuck job. `make e2e_test` runs
it under `-H` (`test/integrated/kcpc/test.sh`); the UI-free step of the
Linux, macOS and MSVC workflows runs it with `--ui-free`, and the MSVC one
with `-H` too. `--gui` runs it against the threaded loop, and `--repeat N`
runs it N times, each against a core of its own.

`test_every_opcode_has_a_documented_refusal` in `test_kcpc.py` reads the
table of [section 13](#13-opcode-table) from this document, checks that it
names the client's opcodes with the client's numbers, and sends each one
the way it must be refused: as a first frame (`HELLO_REQUIRED`), from an
observer when the opcode is the controller's (`NOT_CONTROLLER`) and without
its payload when it has one (`BAD_PAYLOAD`). Every opcode's success, and
its other failures, are in the slice's own tests.

### Exit criterion of phase 4

From a running core, another process captures the machine's frame out of the
shared-memory ring, and its hash equals the core's own hash of the same frame
(beads-0ir). `test/integrated/kcpc/test_fb_ring.py`, in blocking reads only
like the test above:

1. `HELLO` announces `CAP_FB_SHM`; `FB_INFO` names the ring for the core's
   pid, with the frame's size;
2. subscribed to frames, the client is pushed `EVT_FRAME`s whose `fb_seq`
   grows with `frame`, one for one;
3. the text IPC's `wait vbl 200` runs the machine to the firmware's banner
   and pauses it; then the text IPC's `hash frame`, the client's `FbRing`
   and `tools/fb_dump` give the **same CRC32**, of the same `fb_seq`, which
   is also `FB_INFO`'s;
4. the PNG `fb_dump` writes is that frame, each row twice (768x544, as the
   core's `screenshot`), and a CPC screen: blue paper, yellow ink.

`make e2e_test` runs it under `-H` and `--gui`; the UI-free step of the
Linux, macOS and MSVC workflows with `--ui-free`, the MSVC one with the
`fb_dump.exe` CMake builds.

## 17. Engineering decisions made here

Recorded so a reviewer can disagree with one without reading the rest.

1. **Version byte in every frame**, not only in `HELLO`. One byte buys a
   stronger wrong-port check and lets a future server speak two majors on one
   port.
2. **No resynchronisation.** A framing error closes the connection.
3. **Major 0 until phase 5 has shipped** (user decision, 2026-10-11). The
   protocol is "same checkout only" until the remote GUI has used all of it;
   the freeze to major 1 comes after that.
4. **Thread per client**, not one `select()` loop. Eight clients do not need
   an event loop, and a client that sends slowly cannot delay another's
   requests.
5. **Reads at the frame boundary, changes under a pause lease.** Reads are
   the hot path of a live view and must not pause audio; changes are rare and
   reuse the path the text IPC has already proven.
6. **Nothing blocks on emulated time.** Steps complete with an event. The
   text IPC's `wait` family has no equivalent; a client waits for an event.
7. **One controller; forced takeover, newest wins** (user decision,
   2026-10-11). The previous controller is demoted to observer and told. No
   client has a special role.
8. **Events are notices.** They carry what identifies the change, and the
   client fetches the data. Lost events are counted and reported, never
   silently dropped.
9. **`MEM_WRITE` refuses the `CPU` space.**
10. **Keys are matrix positions.** Key names and host keymaps belong to the
    frontend.
11. **Chip layouts start at what the text IPC reports** and grow by layout
    version.
12. **Media, snapshots and configuration are in phase 3** as a ninth slice
    (user decision, 2026-10-11), as thin wrappers over the loaders the text
    IPC and the menus already use. Configuration keys travel as the text
    IPC's strings. There is still no disassembly opcode.
13. **The port is on by default with one key to turn it off**,
    `[ipc] binary_port_enabled` (user decision, 2026-10-11).
14. **A stopped machine is read under the pause mutex, not under a pause
    lease** (slice 2). A lease makes a Run be refused, which a view that
    polls must never cause. The lease stays the tool for changes and the
    gate's last resort.
15. **A read is never split over frame boundaries** (slice 2). The 1 MiB
    budget decides how many reads share a boundary; a single read larger
    than that still runs whole, because its bytes must come from one
    instant.
16. **`HALT` is read-only** (slice 3). The design listed it as bit 17 of
    the `REGS_SET` mask. Nothing in the core can set it without restarting
    the CPU, and the text IPC's `reg set HALT` silently did nothing for
    that reason; this port says `BAD_PAYLOAD` instead, and the text IPC now
    answers `ERR 400 reg-read-only` (beads-gpx7).
17. **A machine write runs while its lease is the only one** (slice 3), and
    answers `BUSY` when it cannot be. A bare lease would have let a
    `MEM_WRITE` run beside a machine rebuild that frees the RAM it writes.
18. **The registers of this port show the breakpoint's address at a
    breakpoint** (slice 3), on reads and in the reply to a write, so that a
    block read there can be written back unchanged.
19. **The controller's idle timeout has a status of its own**,
    `IDLE_TIMEOUT` (slice 3), so a client that was dropped for silence can
    tell that from a framing error.
20. **Run state is announced by the core, not by the server** (slice 4).
    One observer is called where the pause state settles, so a stop is
    pushed whoever caused it, and a lease's own pause and resume are
    invisible by construction instead of by filtering.
21. **A stop is announced when it has happened** (slice 4): by the runner
    when it parks, if a frame was in flight when the stop was asked for. The
    event's `frame` and `pc` are then the ones a read finds.
22. **The event ring takes several producers** (slice 4). The design said
    one, the runner; pauses are settled on the thread that pauses. The push
    stays lock-free.
23. **`RUN` is never refused** (slice 4). The design answered `BUSY` when a
    lease was held elsewhere. With several clients that is most of the time,
    and a run that can vanish is the defect of beads-pja3; `cpc_run()` defers
    it to the last lease holder instead.
24. **`PAUSE` waits for the frame in flight** (slice 4), under a pause
    lease, so that its reply is the stamp of a stopped machine.
25. **An event follows the response to the request that caused it**
    (slice 4), so a client can read "reply, then outcome" without sorting.
26. **Step in is limited to 4096 instructions a request** (slice 4): it
    runs with the pause mutex held, as the machine writes do, so that it
    never runs beside a machine rebuild.
27. **`EVT_SHUTDOWN` and then `ERROR`** (slice 4). The event is for a client
    reading events and the `ERROR` for one waiting for a response; neither
    has to know about the other.
28. **`EVT_FRAME` is sent for multiples of the interval** (slice 4), not
    every Nth frame since the subscription, so clients agree on which frames
    they saw and the runner posts only the frames somebody is due.
29. **A breakpoint is named by kind and address, and adding one that exists
    replaces it** (slice 5). The design left open what a second `BP_ADD` at
    an address does. The text IPC adds a second entry; a frontend that
    edits a condition wants the entry changed, and with names that are
    unique the probe's limits can be counted.
30. **A breakpoint the probe would not arm is refused** (slice 5), with
    `LIMIT_REACHED`, and so is a step that could not plant its one-shot
    breakpoint. Accepting what never fires is beads-zqf3.
31. **A stop's details ride on the announcement, not on the hit hook**
    (slice 5). The design hung the server on the text IPC's hit hook. One
    path that pushes stops is enough; the hook keeps its two arguments.
32. **A walk is announced as one step, in the core** (slice 5): one
    `EVT_RESUMED` at most and one `EVT_STOPPED`, for the walks of this
    port, of the text IPC and of the DevTools alike.
33. **A walk waits on the announced run state** (slice 5, beads-pja3), and
    a pause lease does not start a new resume epoch. Another actor's read or
    write must not end somebody's step.
34. **Somebody's pause ends a walk through a flag of its own** (slice 5): a
    walk single-steps a machine that is already stopped, where a pause
    changes nothing it could wait for.
35. **An `IN` reports no value, and a read watch reports the byte in
    memory** (slice 5). The probe latches before the device or the memory
    drives the data bus; the bus then reads `&FF` whatever is there.
36. **`STEP` over has no instruction limit** (slice 5). Step in is limited
    because it holds the pause mutex throughout; a walk does not, and has a
    deadline.
37. **`KEY` writes one matrix line and leaves the modifiers alone**
    (slice 6). The text IPC's key commands name characters and drive SHIFT
    and CONTROL for them: pressing a plain key there releases both. A
    frontend that sends matrix positions sends SHIFT as a key of its own,
    and a `KEY` that released it would turn `A` into `a`.
38. **The server releases the keys the controller held, not every key**
    (slice 6). The design said "releases every key" for both `KEYS_RELEASE`
    and the loss of control. The matrix is shared with the user's keyboard
    and the text IPC, and a tool that connects, debugs and disconnects must
    not let go of their keys. `KEYS_RELEASE`, which a client sends on
    purpose, still releases everything.
39. **Mouse buttons are released with the keys** (slice 6). The design
    spoke of keys only; a button stuck down after a dropped connection is
    the same defect.
40. **Input takes no pause lease and has no stamp** (slice 6). It changes
    host state the runner samples at a frame boundary, never the machine,
    so it is never `BUSY` and never pauses anything.
41. **`TEXT` counts key actions, not characters** (slice 6). The design
    said "characters now queued". The queue holds actions, a token is one
    of them, and characters the CPC cannot type are dropped before they are
    queued; the number the queue has is the one a client can wait on.
42. **The ASIC's block is at layout 2 from the start** (slice 7). The audit
    of the DevTools windows found the ASIC window showing the DMA channels'
    counters and the position in the unlock sequence, which are in no
    register and so not in `ASIC_PAGE`. The other four chips stay at
    layout 1.
43. **`ASIC_PAGE` is the page, not the view at `0x4000`** (slice 7). It
    reads the same whether the ASIC is locked, unlocked or has the page
    mapped, and it is read-only: the chip acts on writes to its registers.
44. **What an `EVT_MEDIA_CHANGED` says about the slot is read when the event
    is sent** (slice 7). The event is posted from the frame runner too (the
    machine taking a queued disc), which may not take a lock or carry a path;
    an event is a notice (decision 8), and a notice says what is true now.
45. **A change of medium is announced per step, not per load** (slice 7).
    A disc load is three: the drive emptied, the host's view filled, the
    machine given the disc. Hiding the first two would need the loaders to
    know which eject is the start of a load; the generations make the extra
    notices harmless.
46. **The symbol table got a lock, and a name is unique in it** (slice 7).
    It was read by the render thread and written by the text IPC's with
    nothing between them. And `SYM_DEL` names a symbol by its name, which
    only works if a name has one address.
47. **`EVT_MACHINE_CHANGED` is posted by the thread that rebuilt, under its
    lease** (slice 7). Its payload is read from the machine directly there:
    through the gate it would wait for that very lease.
48. **The text IPC's `tape eject` goes through the loader** (slice 7), under
    a pause lease, as `disk eject` does. It called the machine's deck from
    the IPC thread and left the host's copy of the tape in place.
49. **A media request gives the machine its half before it answers**
    (slice 9). The loaders queue the swap for the next frame boundary,
    because the menus run beside a running machine. A request of this port
    holds a lease, so it applies the swap itself: the reply is true when it
    arrives, all the events of the request are posted on the request's
    thread and can name it, and none overtakes the reply.
50. **`MEDIA_INSERT` does not eject the outgoing disc** (slice 9, revised).
    It did at first: the write back of a disc with unsaved changes went to
    the file the drive's slot named when the machine let go of the disc,
    every load path names the new file first, and the old disc's writes
    landed in the new disc's file (beads-11us, found here). The bridge now
    remembers the file of the disc each drive holds, so an insert is the
    plain load every other source does, with the same events, and both
    files stay right.
51. **No write-protect flag on `MEDIA_INSERT`** (slice 9). The design had
    one. The FDC has no such switch (a medium is write-protected when it
    has no writable image), and a flag the core accepts and ignores is worse
    than none. The bit is reserved and must be 0; `MEDIA_INFO` reports what
    the FDC would. Giving the FDC the switch is beads-uxle.
52. **The cartridge is replaced, never ejected, and a bad one leaves the old
    one** (slice 9). A 6128+ with an empty cartridge port executes nothing.
    The board holds the cartridge image by a raw pointer, so a load is a
    swap of a whole image followed by a re-attach, and a file that is
    refused changes nothing. That is `cpc_cartridge_swap()`, which the
    menus, drag and drop and the text IPC's `load` reach through
    `cpc_cartridge_insert()` (beads-lrdy, found here); this port calls the
    swap under its own lease and resets after it.
53. **A snapshot never rebuilds the machine** (slice 9). The design promised
    `EVT_MACHINE_CHANGED` "when the snapshot changed the model or RAM size".
    The SNA loader does neither: it refuses a snapshot larger than the
    machine and loads one from another model as it is. The document says
    what the loader does.
54. **`TAPE_CONTROL` moves the deck itself** (slice 9), instead of the
    deferred seek the design named. That deferral exists for callers beside
    a running machine; under a lease the deck can be moved at once, and the
    reply then shows where it is.
55. **One table of configuration keys** (slice 9). The text IPC's `config`
    command was a chain of string comparisons that built its replies in
    place. It now calls three functions that return values, and both servers
    use them. `CONFIG_GET` reads the settings a rebuild rewrites through the
    gate, in one copy of fixed size; `CONFIG_SET` runs under a lease, which
    the text IPC's `config set crtc_type` still does not (beads-7h32).
56. **`CONFIG_APPLY` names itself in `EVT_MACHINE_CHANGED`** (slice 9), so
    that the controller is sent the event after the reply. The rebuild runs
    on the main thread, so the request travels in two atomics, not in
    thread-local storage like the other causes.
57. **A refused disc changes nothing** (slice 8). A disc load emptied the
    drive before it parsed the file, so a file that was no disc left the
    drive empty, on this port and on every other; the text IPC's test said
    the opposite and checked neither. The image is now tried on copies
    first, and the slot keeps naming the disc it holds (`disc_insert()`),
    as `cpc_cartridge_swap()` already did for a cartridge. A tape did not
    yet: it emptied the deck, host and machine alike, and said so. It is now
    read and checked whole before the deck is touched, and the slot keeps
    naming it the same way (`tape_deck_insert()`, beads-ow6r).
58. **`SYM_LOAD` takes the `..` rule** (slice 8). It was the one path from
    a client that did not; it now answers `BAD_PAYLOAD`, with the message
    `MEDIA_INSERT` gives, through a message of its own on the callback.
