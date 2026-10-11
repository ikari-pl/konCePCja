# KCPC binary frontend protocol, version 1

**Status: design. Nothing in this document is implemented yet.** It is the
contract for phase 3 of the headless-core epic (beads-cv2, bead beads-3o1).
The implementation lands in the slices listed in
[section 16](#16-implementation-slices). The server announces protocol
version 0, and the layouts below may still change, until phase 5 (the remote
GUI) has shipped; see [section 6](#6-versioning-and-hello).

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

- **Video frames.** They travel through shared memory in phase 4 (beads-0ir).
  This protocol only describes the frame (`FB_INFO`) and announces each new
  one (`EVT_FRAME`).
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

  In `StartupManifest` the field is `binary_port`, filled through
  `startup_manifest_await_port()` like the others.
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
  except 0, and it should not reuse one that is still unanswered.
- **Responses** go from server to client and carry the `opcode` and `id` of
  the request they answer. Every request gets exactly one response.
- **Events** go from server to client. Their `id` is a per-connection sequence
  number that starts at 1 and grows by 1 for every event sent on that
  connection.
- Events may arrive between a request and its response. Match responses by
  `id`, never by position in the stream.
- The server answers one connection's requests in the order they arrived, one
  at a time. A client may send several requests without waiting (pipelining).
- Maximum payload a client may send: **1 MiB**. Maximum payload the server
  sends: the smaller of 16 MiB and the `max_frame` the client gave in `HELLO`.
- There is no resynchronisation. TCP already delivers bytes in order; the
  magic exists to catch a client that connected to the wrong port. On a bad
  magic, a wrong `version`, an unknown `kind` or an oversized `length`, the
  server sends one `ERROR` frame (section 5) and closes the connection.

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
| 8 | `BUSY` | The runner did not go idle in time, or a step is already in progress | stays open |
| 9 | `OUT_OF_RANGE` | Address, length, bank or index outside what exists | stays open |
| 10 | `TOO_LARGE` | The reply would exceed the frame limit | stays open |
| 11 | `NOT_PRESENT` | The hardware is not fitted (ASIC on a 6128, a mouse that is off) | stays open |
| 12 | `NOT_FOUND` | No such breakpoint, symbol or file | stays open |
| 13 | `LIMIT_REACHED` | A table is full (the probe holds 32 exec, 16 watch, 8 I/O comparators) | stays open |
| 14 | `SHUTTING_DOWN` | The core is quitting | closed |
| 15 | `INTERNAL` | A bug; the message says what | stays open |

`BUSY` maps to the text IPC's `ERR 409 z80-not-idle`: the request changed
nothing and the machine runs or stays paused as before.

**`ERROR` (opcode `0x0000`)** is a response the server sends on its own,
with `id` 0, right before it closes a connection it cannot serve: a bad
frame, a ninth client, shutdown. Its payload is the same `status` and
`message`.

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
| 9 | `CAP_FB_SHM` | Phase 4: `FB_INFO` names a shared-memory ring |
| 10 | `CAP_MEM_ROM` | Memory spaces `ROM_LOWER` and `ROM_UPPER` |
| 11 | `CAP_MEDIA` | `MEDIA_*`, `TAPE_CONTROL` |
| 12 | `CAP_SNAPSHOT` | `SNAPSHOT_SAVE`, `SNAPSHOT_LOAD` |
| 13 | `CAP_CONFIG` | `CONFIG_GET`, `CONFIG_SET`, `CONFIG_APPLY` |

`PING` (empty request, empty reply) needs no capability.

## 7. Clients, roles and control

Up to **8** clients. A ninth connection receives `ERROR` with
`TOO_MANY_CLIENTS` and is closed.

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
  `NOT_CONTROLLER`, and the keys it was holding down are released. It may
  force control back the same way. No client is special: the GUI, a test
  script and a tool all follow this one rule.
- Control is also released when the controller disconnects, sends
  `CONTROL release`, or goes quiet: a controller that has sent no frame for
  **30 s** is disconnected. A controller with nothing to say sends `PING`
  every 10 s. Observers are never timed out.
- Every change of controller is announced to **all** clients, the demoted
  one included, with `EVT_CONTROL_CHANGED` carrying the new controller's
  `client_id` (0 when control became free). The event ignores subscription
  masks, so a client always learns that it lost control.
- A step that the previous controller started keeps running across a
  takeover. Its `EVT_STOPPED` still goes to the client that asked for it, and
  the new controller may end it with `PAUSE`.

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
| Acceptor | 1 | `accept()`, enforces the client limit |
| Reader | 1 per client | Reads frames, runs requests, queues responses |
| Writer | 1 per client | Sends that client's queued responses and events |
| Fan-out | 1 | Takes events from the runner's ring and copies them to subscribed clients |
| Walker | 1 | Runs steps that take emulated time (step over, out, to, frame) |

At most 19 threads with 8 clients. They block in `select()`/`recv()` or on a
condition variable; none of them spins.

### Seam 1: the pause lease, for every change

Every request that changes the machine runs on the client's reader thread
under a `CpcPauseLease`, the same way the text IPC's `reg set` and `reset`
do:

1. Take the lease. If `idle()` is false, call `restore_run_state()` and
   answer `BUSY`.
2. Make the change through the existing host functions:
   `subcycle_bridge_regs_to_machine()` for registers, `z80_write_mem()` and
   `Machine::ram_write()` for memory, `emulator_reset()`,
   `z80_step_instruction()`.
3. Restore the run state the lease found (`restore_run_state()`), unless the
   request itself is meant to leave the machine paused.

Breakpoint and symbol edits use the editing lists and their existing locks
(`CpcStopCoordinationGuard`, the list mutators in `z80_view.cpp`). The runner
copies those lists into the probe with `subcycle_bridge_sync_probe()` before
each frame, as it does for edits made by the text IPC and DevTools.

### Seam 2: the frame boundary, for reads while running

A lease is the wrong tool for a GUI that refreshes its views 50 times a
second: taking one pauses audio and waits out the rest of the frame, up to
20 ms.

So reads go through a **machine gate**, one small class:

- The reader thread builds a *read job* (for example "copy 65536 bytes of CPU
  view from address 0") and gives it to the gate.
- **If the machine is paused**, the reader takes a pause lease, which is
  immediate because the runner is already idle, runs the job itself and
  releases the lease. The machine stays paused.
- **If the machine is running**, the job goes on a queue. At the next frame
  boundary the runner calls `kcpc_service_frame()` from the shared frame
  epilogue in `emu_run_frame()`, next to `g_telnet.drain_input()`. That call
  runs queued jobs on the runner thread, while the machine is not being
  clocked, and wakes the readers that wait for them.
- If a queued job is not served within 100 ms (the machine was paused in the
  meantime, or the tier is too slow to finish a frame), the reader takes a
  pause lease and runs it itself. Each job has an atomic state
  (queued → claimed → done), so it runs exactly once whichever side claims
  it.

What the runner is allowed to do in `kcpc_service_frame()`:

- copy out of the machine into a buffer owned by the job; no socket calls, no
  allocation proportional to anything but the job, no waiting on a client;
- at most **1 MiB** of copying per frame boundary, and always at least one
  job. A 4 MiB dump of expansion RAM takes four frames;
- nothing at all when the queue is empty: one relaxed atomic load.

Read jobs never change machine state, so a run with clients attached makes
the same observations as a run without them (epic invariant 2).

### Seam 3: the bridge probe and `debug_sync`, for events

Breakpoint, watchpoint and I/O hits are already detected in one place:
`subcycle_bridge_debug_sync()` consumes the probe's latch on the runner
thread and calls the hit hook, which today ends in
`KoncepcjaIpcServer::notify_breakpoint_hit()`. The binary server hangs off
the same hook. `emu_handle_stop()` classifies the stop, and that is where the
event gets its reason.

The runner's part of an event is one push into a fixed-size ring (4096
entries, single producer). It never blocks and never allocates. If the ring
is full the event is counted as lost. The fan-out thread does the rest.

### Run state: one announced value

"Paused" and "running" are announced, not sampled. Three functions settle the
pause state today, all under `g_pause_mutex`: `cpc_pause()`, `cpc_resume()`
when it is applied, and `CpcPauseLease::release()` when the last lease goes.
At each of them the server compares the real state with the last state it
announced and posts `EVT_STOPPED` or `EVT_RESUMED` only when they differ.

A lease that pauses a running machine and resumes it inside one request is
therefore invisible, which is what a client wants: reads and pokes do not
make the run state flicker.

### Steps that take emulated time

No request waits for emulated time to pass. `STEP` in the modes that run the
machine (over a call, out of a routine, to an address, N frames) answers `OK`
as soon as the step is armed. The walker thread runs it with the existing
`z80_run_until_ephemeral()` and frame-step machinery and the result arrives
as `EVT_STOPPED`. One walk runs at a time; a second `STEP` meanwhile answers
`BUSY`, and `PAUSE` aborts the walk through its abort predicate.

So the longest any request can take is the pause lease's idle timeout. The
one exception is `CONFIG_APPLY` (section 12), which waits for the machine to
be rebuilt on the main thread.

### Shutdown

`stop()` closes the listening socket, sends `EVT_SHUTDOWN` to every client,
aborts the walk, fails queued jobs with `SHUTTING_DOWN`, closes the client
sockets and joins every thread. No thread waits on anything without a
deadline or an abort flag.

## 9. Events

### Subscribing

`SUBSCRIBE` replaces the client's subscription:

| Size | Field | Meaning |
|-----:|-------|---------|
| 4 | `classes` | Bit mask of event classes |
| 4 | `frame_interval` | Send `EVT_FRAME` every N frames; 0 = never |

The reply repeats the two values as accepted. After `HELLO` a client is
subscribed to nothing.

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

- Events on one connection arrive in the order they happened.
- An event is a notice, not the data. After `EVT_BREAKPOINTS_CHANGED` the
  client asks for `BP_LIST`; after `EVT_STOPPED` it reads the registers.
- Each client has an outgoing queue. Responses always go in. Events go in
  only while the queue holds less than 4 MiB; beyond that they are dropped
  for that client alone, and once there is room the client receives one
  `EVT_OVERRUN` with the number dropped. A client that sees it should assume
  everything changed and re-read.
- A client whose queue reaches 32 MiB of unread responses is disconnected.
- A slow or stuck client costs the runner nothing: the runner only ever
  writes to its own ring.

### Event payloads

**`EVT_STOPPED` (`0x8001`)** — the machine is now paused.

| Size | Field | Meaning |
|-----:|-------|---------|
| 8 | `frame` | |
| 1 | `reason` | See below |
| 1 | `access` | For watch and I/O hits: 1 = read, 2 = write; else 0 |
| 2 | `pc` | Program counter at the stop |
| 2 | `addr` | Breakpoint address, watched address or I/O port; else 0 |
| 1 | `value` | Data bus at a watch or I/O hit; else 0 |
| 1 | `old_value` | Previous memory byte at a watch write; else 0 |
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

A walk that runs into a breakpoint reports `BREAKPOINT`, with the `cause_*`
fields still naming the step.

**`EVT_RESUMED` (`0x8002`)** — `u64 frame`, `u32 cause_client`,
`u32 cause_request`.

**`EVT_RESET` (`0x8003`)** — the CPC was reset. `u64 frame`,
`u32 cause_client`, `u32 cause_request`.

**`EVT_FRAME` (`0x8004`)** — a frame completed. `u64 frame`, `u64 fb_seq`.
`fb_seq` is 0 until phase 4, then the sequence number of that frame in the
shared-memory ring.

**`EVT_MEDIA_CHANGED` (`0x8005`)** — `u8 slot` (0 = drive A, 1 = drive B,
2 = tape, 3 = cartridge), `u8 present`, 2 reserved bytes, `u64 generation`
(for drives, `dsk_media_generation()`), `str path` (empty if unknown).

**`EVT_BREAKPOINTS_CHANGED` (`0x8006`)** — `u64 generation`
(`z80_breakpoint_generation()`). Sent for edits from any source.

**`EVT_SYMBOLS_CHANGED` (`0x8007`)** — `u64 generation`.

**`EVT_MACHINE_CHANGED` (`0x8008`)** — the machine was rebuilt (model, RAM
size, CRTC type, tier). Payload: the `MACHINE_INFO` reply fields.

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

`bank` is only meaningful for `ROM_UPPER` and must be 0 elsewhere.

**`MEM_INFO` (`0x0210`)** — empty request. Reply: `u32 ram_bytes`,
`u8 ram_config` (Gate Array RAM banking register), `u8 rom_config` (bit 2 set
= lower ROM off, bit 3 set = upper ROM off, as the Gate Array holds them),
`u8 upper_rom` (selected slot), `u8` reserved, `u32 rom_slots`, `stamp`.

**`MEM_READ` (`0x0211`)**

Request: `u8 space`, `u8` reserved, `u16 bank`, `u32 addr`, `u32 len`.

Reply: `stamp`, then `len` bytes.

- `addr + len` must not pass the end of the space: `OUT_OF_RANGE`. Nothing
  wraps around.
- `len` may be anything from 0 up to what fits the frame limit: `TOO_LARGE`
  otherwise. **A 64 KiB read of a CPU space is always one reply.**
- All the bytes of one reply come from the same instant.

**`MEM_WRITE` (`0x0212`)** — controller only.

Request: `u8 space`, `u8` reserved, `u16 bank`, `u32 addr`, then the bytes to
write. Reply: `stamp`.

- Allowed spaces: `CPU_RAM` and `PHYS_RAM`. ROM spaces and `ASIC_PAGE` answer
  `BAD_PAYLOAD`.
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
- **`REGS_SET` (`0x0201`)** — controller. Request: `u32 mask`, register
  block. Bit N of `mask` selects the Nth field of the block (bit 0 = `AF`,
  bit 11 = `HL'`, bit 12 = `I`, bit 17 = `HALT`); fields whose bit is clear
  are ignored. Reply: `stamp`, the register block after the write. The write
  follows `subcycle_bridge_regs_to_machine()`: it never drops or replays part
  of an instruction in flight.

### Run control

- **`RUN_STATE` (`0x0104`)** — empty request. Reply: `stamp`,
  `u8 walk_active`, 3 reserved bytes, `u32 controller`.
- **`PAUSE` (`0x0100`)** — controller. Empty. Pauses, aborts a walk in
  progress. Reply: `stamp`.
- **`RUN` (`0x0101`)** — controller. Empty. Reply: `stamp`. `BUSY` if a
  pause lease held elsewhere defers the resume.
- **`RESET` (`0x0103`)** — controller. Request: `u8 flags` (bit 0: stay
  paused afterwards), 3 reserved bytes. Reply: `stamp`.
- **`STEP` (`0x0102`)** — controller.

  | Size | Field | Meaning |
  |-----:|-------|---------|
  | 1 | `mode` | 0 in, 1 over, 2 out, 3 to address, 4 frames |
  | 1 | reserved | 0 |
  | 2 | `addr` | Target for mode 3 |
  | 4 | `count` | Instructions (modes 0, 1) or frames (mode 4); 0 means 1 |
  | 4 | `timeout_ms` | Deadline for modes 1–3; 0 means 5000 |

  The machine is paused first if it was running. Reply: empty, sent once the
  step is armed. The outcome is an `EVT_STOPPED` whose `cause_request` is
  this request's `id`. Mode 0 completes before the reply is sent, and its
  event follows the reply.

### Breakpoints

One family covers execution breakpoints, memory watchpoints and I/O
breakpoints. Entries are identified the way the editing lists identify them:
by kind and address (and mask, for I/O).

`kind`: 0 = execution, 1 = memory watch, 2 = I/O.

An entry on the wire:

| Size | Field | Meaning |
|-----:|-------|---------|
| 1 | `kind` | |
| 1 | `access` | Watch and I/O: 1 read/in, 2 write/out, 3 both. Execution: 0 |
| 2 | `addr` | Address, or port value for I/O |
| 2 | `len_or_mask` | Watch: length of the range. I/O: port mask. Execution: 0 |
| 2 | reserved | 0 |
| 4 | `pass_count` | Break only after this many hits |
| 4 | `hit_count` | Ignored in requests |
| `str` | `condition` | Expression in the text IPC's syntax; empty = always |

- **`BP_LIST` (`0x0300`)** — empty request. Reply: `u64 generation`,
  `u32 count`, then `count` entries.
- **`BP_ADD` (`0x0301`)** — controller. Request: one entry. Reply:
  `u64 generation`. `BAD_PAYLOAD` if the condition does not parse,
  `LIMIT_REACHED` if the probe table for that kind is full.
- **`BP_DEL` (`0x0302`)** — controller. Request: `u8 kind`, `u8` reserved,
  `u16 addr`, `u16 len_or_mask`, `u16` reserved. Reply: `u64 generation`.
  `NOT_FOUND` if there is no such entry.
- **`BP_CLEAR` (`0x0303`)** — controller. Request: `u8 kind`, or `0xFF` for
  all three. Reply: `u64 generation`.

Setting any breakpoint moves the Auto run-tier policy from Fast to Wake, as
it does for breakpoints set any other way.

### Input

Keys are named by their place in the CPC keyboard matrix, which is what the
hardware has: `row` 0–9 and `bit` 0–7. Row 9 is joystick 0 and row 6 carries
joystick 1, so joysticks need no opcode of their own. A frontend maps its
host keys to matrix positions itself.

- **`KEY` (`0x0400`)** — controller. Request: `u8 row`, `u8 bit`, `u8 down`
  (1 press, 0 release), `u8` reserved. Reply: empty. The change is written to
  the keyboard matrix under `g_kbd_matrix_mutex` and the runner picks it up
  with the next frame's keyboard snapshot, like every other input path.
- **`KEYS_RELEASE` (`0x0401`)** — controller. Empty. Releases every key.
  The server does this by itself when the controller disconnects or loses
  control, so a dropped connection never leaves a key held down.
- **`TEXT` (`0x0402`)** — controller. Request: `str text`. Queued on the
  `AutoTypeQueue`, with the same `~KEY~` tokens as the text IPC's
  `input type`. Reply: `u32 pending` (characters now queued).
- **`MOUSE` (`0x0403`)** — controller. Request: `i16 dx`, `i16 dy`,
  `u8 buttons` (bit 0 left, bit 1 middle, bit 2 right), `u8 flags` (bit 0:
  `buttons` is valid). Staged and applied by `ipc_drain_input()` at the next
  frame. `NOT_PRESENT` if no AMX or Symbiface mouse is enabled.
- **`INPUT_STATE` (`0x0404`)** — empty request. Reply: 10 bytes, one per
  matrix row, bit clear = key down (the matrix's own polarity).

### Chip state

**`CHIP_GET` (`0x0500`)** — Request: `u8 chip`, 3 reserved bytes. Reply:
`stamp`, `u8 chip`, `u8` reserved, `u16 layout`, then the chip's block.

`layout` is the version of that block. A new layout only appends fields, so a
client that knows layout 1 can read the start of layout 2.

Every field comes from the Device debug structures that
`subcycle_bridge_sync_chip_views()` and `subcycle_bridge_refresh_asic_view()`
already read. The server reads them through the machine gate; it does not
read the host view structs (`CRTC`, `GateArray`, `PSG`) from its own threads.

| Chip | Id | Block, layout 1 |
|------|---:|-----------------|
| CRTC | 1 | `u8 type`, `u8 reg_select`, `u8 reg[18]`, `u8 hcc` (horizontal character counter), `u8 vcc` (character line counter), `u8 ra` (raster line within the character), `u8 hsw` (HSYNC width counter), `u8 vsw` (VSYNC width counter), `u8 vta` (vertical total adjust counter), `u16 ma` (memory address), `u16 scanline` |
| Gate Array | 2 | `u8 mode`, `u8 requested_mode`, `u8 pen`, `u8 rom_config`, `u8 ram_config`, `u8 upper_rom`, `u8 sl_count` (the 6-bit HSYNC counter raster interrupts fire from), `u8 hs_count` (interrupt delay), `u8 ink[17]` (hardware colour numbers, pen 0–15 then border) |
| PSG | 3 | `u8 reg[16]`, `u8 reg_select` |
| FDC | 4 | `u8 motor`, `u8 track[2]` (physical head position per drive), `u8 present[2]`, 3 reserved bytes, `u64 generation[2]` |
| ASIC | 5 | `u8 unlocked`, `u8 pri_line` (programmable raster interrupt line, 0 = off), 2 reserved bytes. Everything else (sprites, palette, DMA channels, split screen) is read raw from memory space `ASIC_PAGE` |

`ASIC` answers `NOT_PRESENT` unless the model is a 6128+.

Layout 1 is deliberately no more than the text IPC's `regs crtc|ga|psg`
already reports, so the first version can be checked against it byte for
byte. The DevTools slice may raise a layout when a window needs a field that
is not here.

### Symbols

- **`SYM_LIST` (`0x0600`)** — empty request. Reply: `u64 generation`,
  `u32 count`, then `count` × (`u16 addr`, `str name`).
- **`SYM_ADD` (`0x0601`)** — controller. Request: `u16 addr`, `str name`.
  Reply: `u64 generation`.
- **`SYM_DEL` (`0x0602`)** — controller. Request: `str name`. Reply:
  `u64 generation`. `NOT_FOUND` if absent.
- **`SYM_LOAD` (`0x0603`)** — controller. Request: `str path`, a file on the
  core's machine. Reply: `u64 generation`, `u32 loaded`.

### Video

**`FB_INFO` (`0x0700`)** — empty request. Reply:

| Size | Field | Value in version 1 |
|-----:|-------|--------------------|
| 2 | `width` | 768 (`kFbWidth`) |
| 2 | `height` | 272 (`kFbHeight`) |
| 1 | `format` | 1 = RGB24, three bytes per pixel, R first |
| 1 | `transport` | 0 = none; phase 4 defines 1 = shared-memory ring |
| 2 | reserved | 0 |
| 4 | `stride` | 2304 |
| 8 | `fb_seq` | 0 until phase 4 |
| `str` | `shm_name` | Empty until phase 4 |

This is the machine's canonical frame, before scaling or scanlines (epic
invariant 1).

### Media

Paths are files on the core's machine, in the core's filesystem. A relative
path is resolved against the core's working directory and made absolute
before it is stored on a Recent list, as the text IPC's `load` does. All of
these requests are controller only, run under a pause lease, and reuse the
loaders in `src/slotshandler.cpp` and the bridge's deferred media swap
(`subcycle_bridge_insert_media`, `subcycle_bridge_apply_pending_media`);
none of them adds a second loading path.

`slot`: 0 = drive A, 1 = drive B, 2 = tape, 3 = cartridge. The same numbers
as `EVT_MEDIA_CHANGED`.

- **`MEDIA_INSERT` (`0x0800`)** — Request: `u8 slot`, `u8 flags` (bit 0:
  write-protect, drives only), 2 reserved bytes, `str path`. Reply:
  `u64 generation`.
  - Drives accept `.dsk`, `.ipf`, `.raw`; drive A also accepts the flux
    images `.scp`, `.hfe`, `.a2r` (flux is drive A only: `BAD_PAYLOAD` on
    drive B).
  - Tape accepts `.cdt` and `.voc`. Cartridge accepts `.cpr` and answers
    `NOT_PRESENT` unless the model is a 6128+; inserting one resets the
    machine, as it does from the menu.
  - A `.zip` is opened and its first supported member is used.
  - `NOT_FOUND` if the file is missing; `BAD_PAYLOAD` if the type does not
    fit the slot or the image is malformed.
- **`MEDIA_EJECT` (`0x0801`)** — Request: `u8 slot`, 3 reserved bytes.
  Reply: `u64 generation`. A drive with unsaved changes follows the
  File-menu flush-on-eject path and never raises a dialog. Ejecting an empty
  slot succeeds.
- **`MEDIA_INFO` (`0x0802`)** — anyone. Request: `u8 slot`, 3 reserved
  bytes. Reply: `u8 slot`, `u8 present`, `u8 flags` (bit 0 write-protected,
  bit 1 unsaved changes, bit 2 flux image), `u8` reserved, `u64 generation`,
  `str path`. For the tape slot three more fields follow: `u8 playing`,
  `u8 motor`, 2 reserved bytes, `u32 block` (current block), `u32 blocks`
  (blocks in the image).
- **`TAPE_CONTROL` (`0x0803`)** — Request: `u8 action` (0 stop, 1 play,
  2 rewind, 3 seek), 3 reserved bytes, `u32 block` (for seek). Reply: the
  tape fields of `MEDIA_INFO`. `NOT_PRESENT` with no tape loaded;
  `OUT_OF_RANGE` for a block past the end. Seek uses the deferred
  `subcycle_bridge_request_tape_seek()`.

Every successful insert and eject, from this port or any other source,
produces `EVT_MEDIA_CHANGED`.

### Snapshots

The format is SNA and nothing else (epic invariant 4).

- **`SNAPSHOT_SAVE` (`0x0900`)** — controller. Request: `str path`. Reply:
  `stamp`. Writes with `snapshot_save_machine()`. It needs the controller
  role because it writes a file on the core's machine, not because it
  changes the CPC.
- **`SNAPSHOT_LOAD` (`0x0901`)** — controller. Request: `u8 flags` (bit 0:
  stay paused afterwards), 3 reserved bytes, `str path`. Reply: `stamp`.
  Loads with `snapshot_load_machine()`. The run state is restored to what it
  was unless bit 0 is set. Clients are told with `EVT_RESET`, plus
  `EVT_MACHINE_CHANGED` when the snapshot changed the model or RAM size;
  either way a client should re-read everything it shows.

Both answer `NOT_FOUND` for a missing file or directory and `BAD_PAYLOAD`
for a file that is not a valid SNA.

### Configuration

The keys and values are the text IPC's `config get|set`, spelled the same
(`model`, `crtc_type`, `ram_size`, `silicon_disc`, `kbd_layout`, ...).
Carrying them as strings keeps one list of keys in one place
(`koncepcja_ipc_server.cpp` today; the slice moves the table somewhere both
servers call) instead of a second, numbered copy that would drift.

- **`CONFIG_GET` (`0x0A00`)** — anyone. Request: `str key`. Reply:
  `u8 flags` (bit 0: a different value is staged and waits for
  `CONFIG_APPLY` or the next frame), 3 reserved bytes, `str value`,
  `str pending` (empty unless bit 0). `NOT_FOUND` for an unknown key;
  `NOT_PRESENT` for a key that needs a window under `-H`.
- **`CONFIG_SET` (`0x0A01`)** — controller. Request: `str key`,
  `str value`. Reply: `u8 effect` (0 = applied now, 1 = applied on the next
  frame, 2 = staged, needs `CONFIG_APPLY`), 3 reserved bytes. `BAD_PAYLOAD`
  for a value the key rejects.
- **`CONFIG_APPLY` (`0x0A02`)** — controller. Empty. Rebuilds the machine
  with the staged settings through the existing main-thread rebuild request.
  This is the one request allowed to take longer than a pause lease: the
  reply is sent when the rebuild has finished, with `stamp`, or with
  `INTERNAL` if it failed and `BUSY` if the main loop did not pick it up in
  time. `EVT_MACHINE_CHANGED` follows a successful rebuild.

`CONFIG_SET` does not write the configuration file. Saving settings stays a
GUI action.

### Session

- **`PING` (`0x0002`)** — empty both ways.
- **`SUBSCRIBE` (`0x0003`)** — section 9.
- **`CONTROL` (`0x0004`)** — Request: `u8 action` (0 release, 1 acquire),
  `u8 flags` (bit 0 = force: take control even if another client has it),
  2 reserved bytes. Reply: `u8 role` now held, 3 reserved bytes,
  `u32 controller`. Asking without force while another client has control is
  not an error: the reply simply still says observer. With force the reply
  always says controller.

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
| `0x8001`–`0x800B` | events | `CAP_EVENTS` | server only | 4, 5, 7 |

Reserved: the unused opcodes of the media (`0x08xx`), snapshot (`0x09xx`)
and configuration (`0x0Axx`) ranges, and `0x0B00`–`0x0BFF` for the light gun
and other input devices.

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
  (`SNAPSHOT_SAVE`). That is already true of port 6543 (`load`,
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
| 1 | Server skeleton | Socket shim, listener and port probing, the `[ipc] binary_port_enabled` key, the thread model, framing codec, `HELLO`, `PING`, `ERROR`, `MACHINE_INFO`, the manifest line; built in the GUI, `-H` and UI-free builds; Python client and test wired into CI on Linux, macOS and Windows |
| 2 | Machine reads | The machine gate and `kcpc_service_frame()`; `REGS_GET`, `MEM_INFO`, `MEM_READ` with every space |
| 3 | Control and writes | Roles, `CONTROL` with forced takeover, the controller idle timeout; `REGS_SET`, `MEM_WRITE` |
| 4 | Run control and events | The event ring and fan-out, `SUBSCRIBE`, the announced run state; `PAUSE`, `RUN`, `RESET`, `RUN_STATE`, `STEP` in and frames; `EVT_STOPPED`, `EVT_RESUMED`, `EVT_RESET`, `EVT_FRAME`, `EVT_OVERRUN`, `EVT_SHUTDOWN`, `EVT_CONTROL_CHANGED` |
| 5 | Breakpoints and pushed hits | `BP_*`, hits through the probe hook, `EVT_BREAKPOINTS_CHANGED`; the walker and `STEP` over, out, to |
| 6 | Input | `KEY`, `KEYS_RELEASE`, `TEXT`, `MOUSE`, `INPUT_STATE`; keys released when control is lost |
| 7 | DevTools data | `CHIP_GET`, `ASIC_PAGE`, `SYM_*`, `FB_INFO`, `EVT_MEDIA_CHANGED`, `EVT_SYMBOLS_CHANGED`, `EVT_MACHINE_CHANGED` |
| 8 | Exit test and documentation | The exit test of beads-3o1, the documentation brought in line with the code. **No freeze:** the server stays at major 0 |
| 9 | Media, snapshots, configuration | `MEDIA_INSERT`, `MEDIA_EJECT`, `MEDIA_INFO`, `TAPE_CONTROL`, `SNAPSHOT_SAVE`, `SNAPSHOT_LOAD`, `CONFIG_GET`, `CONFIG_SET`, `CONFIG_APPLY` |

Slice 9 needs slices 3 (the controller role) and 4 (events) and nothing
later, so it can be built alongside slices 5 to 7. Slice 8 is numbered before
it because it carries the exit criterion below, which does not involve
media; phase 3 is finished when both have merged.

The `[ipc] binary_port_enabled` key and forced takeover belong to slices 1
and 3.

**After phase 3:** the protocol is frozen as major 1, minor 0 only once
phase 5 (beads-3sp) has shipped the remote GUI on top of it. That freeze is
not one of these slices.

Source layout the slices should follow:

- `src/kcpc/kcpc_proto.h` — opcodes, status codes, capability bits, and a
  bounds-checked reader and writer for the field types of section 4. Pure, no
  sockets, unit-tested in `test_runner`.
- `src/kcpc/kcpc_server.{h,cpp}` — the threads of section 8.
- `src/kcpc/kcpc_gate.{h,cpp}` — the machine gate.
- `src/net_socket.{h,cpp}` — the one place with `#ifdef _WIN32` around
  sockets. The text IPC keeps its own two copies of `run()` for now; moving
  it onto the shim is a separate clean-up.
- `test/integrated/kcpc/kcpc_client.py`, `test/integrated/kcpc/test_kcpc.py`
  — standard library only (`socket`, `struct`, `threading`), so the Windows
  CI job needs nothing installed.

### Exit criterion of phase 3

A Python test starts one `-H` core and, with no polling loop anywhere:

1. opens **two** connections, one controller and one observer;
2. the controller sets a breakpoint and runs; **both** connections receive
   the pushed `EVT_STOPPED` with reason `BREAKPOINT`;
3. the observer reads **65536 bytes** of the `CPU` space in **one** response
   frame and the bytes match the text IPC's `mem read` of the same range.

It passes on Linux, macOS and Windows CI, against `-H` and the UI-free build.

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
