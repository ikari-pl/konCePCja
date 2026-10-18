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

namespace kcpc {
struct WriteOp;  // kcpc_server.h
}  // namespace kcpc

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
// A run of the two, in order, as one change: one frame boundary or one
// lease for all. Each fills in its own status, stamp and register block.
void kcpc_host_writes(std::vector<kcpc::WriteOp>& ops);

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

// Input (section 12): KEY, KEYS_RELEASE, TEXT, MOUSE, INPUT_STATE. None of
// them touches the machine or takes a pause lease, so all of them work on a
// paused machine and cost a running one nothing: a key is a write to the
// pending keyboard matrix under g_kbd_matrix_mutex, which the runner copies
// at the start of each frame; text goes onto the AutoTypeQueue; a mouse
// movement is staged for ipc_drain_input(). These are the paths the text
// IPC's `input` commands use.
//
// One matrix line, by position, and no other: SHIFT and CONTROL are keys the
// client presses itself.
kcpc::Status kcpc_host_key(std::uint8_t row, std::uint8_t bit, bool down);
kcpc::Status kcpc_host_keys_release();
// kBadPayload, with the parser's words in `message`, for text the queue does
// not parse (an unknown ~KEY~ token); nothing was queued then. `pending` is
// the number of key actions queued afterwards.
kcpc::Status kcpc_host_text(const std::string& text, std::uint32_t& pending,
                            std::string& message);
// kNotPresent when neither the AMX nor the Symbiface mouse is enabled.
kcpc::Status kcpc_host_mouse(int dx, int dy, bool set_buttons,
                             std::uint8_t buttons);
kcpc::Status kcpc_host_input_state(kcpc::KeyMatrix& rows);

// Chip state (section 12): the block of state.chip, read off the Devices
// through the gate like the other machine reads. kNotPresent for the ASIC of
// a machine that has none; the memory space ASIC_PAGE of kcpc_host_mem_read()
// answers the same way.
kcpc::Status kcpc_host_chip_get(kcpc::ChipState& state);

// Symbols (section 12): g_symfile, the table the text IPC and the DevTools
// read and edit. None of these touches the machine. A change from any source
// is pushed as EVT_SYMBOLS_CHANGED.
kcpc::Status kcpc_host_sym_list(std::uint64_t& generation,
                                std::vector<kcpc::Symbol>& symbols);
kcpc::Status kcpc_host_sym_add(std::uint32_t client, std::uint32_t request,
                               const kcpc::Symbol& symbol,
                               std::uint64_t& generation);
// kNotFound when there is no symbol of that name.
kcpc::Status kcpc_host_sym_del(std::uint32_t client, std::uint32_t request,
                               const std::string& name,
                               std::uint64_t& generation);
// Adds the symbols of a symbol file as one change. kNotFound when the file
// cannot be opened, kBadPayload (with `message`) for a path that leads out
// through '..', the rule MEDIA_INSERT and the snapshots apply.
kcpc::Status kcpc_host_sym_load(std::uint32_t client, std::uint32_t request,
                                const std::string& path,
                                std::uint64_t& generation,
                                std::uint32_t& loaded, std::string& message);

// FB_INFO: the machine's canonical frame, with no transport until phase 4.
kcpc::Status kcpc_host_fb_info(kcpc::FbInfo& info);

// Media (section 12): MEDIA_INSERT, MEDIA_EJECT, MEDIA_INFO, TAPE_CONTROL.
// The changes run on the calling (socket) thread under a pause lease, while
// it is the only lease held, through the loaders of slotshandler.h and the
// bridge's media swap, which is applied before the lease is let go: when the
// reply is sent the machine holds what the request put there. Every change is
// pushed as EVT_MEDIA_CHANGED, named after the request.
//
// kBadPayload for a path with `..` in it, a type that does not fit the slot
// or a malformed image (`message` says which); kNotFound for a missing file;
// kNotPresent for a cartridge on a machine that is not a 6128+. A failed load
// leaves a drive or the deck empty and the cartridge port as it was.
kcpc::Status kcpc_host_media_insert(std::uint32_t client, std::uint32_t request,
                                    const kcpc::MediaInsertRequest& insert,
                                    std::uint64_t& generation,
                                    std::string& message);
// A drive with unsaved changes is written back to its file first. The
// cartridge cannot be ejected (kBadPayload): a 6128+ does not run without
// one.
kcpc::Status kcpc_host_media_eject(std::uint32_t client, std::uint32_t request,
                                   std::uint8_t slot, std::uint64_t& generation,
                                   std::string& message);
// What info.slot holds: read through the gate, so it pauses nothing.
kcpc::Status kcpc_host_media_info(kcpc::MediaInfo& info);
// kNotPresent with no tape in the deck, kOutOfRange for a block past the
// last. `tape` is the deck after the change.
kcpc::Status kcpc_host_tape_control(const kcpc::TapeControlRequest& control,
                                    kcpc::TapeState& tape);

// Snapshots (section 12): SNA files, through snapshot_save() and
// snapshot_load() of slotshandler.h, under a pause lease. Never
// Machine::save_devices(). kNotFound for a missing file or directory,
// kBadPayload for a file that is no SNA or holds more RAM than the machine.
kcpc::Status kcpc_host_snapshot_save(const std::string& path,
                                     kcpc::Stamp& stamp, std::string& message);
// Announced as EVT_RESET. The run state is put back, or the machine is left
// stopped when load.flags says so.
kcpc::Status kcpc_host_snapshot_load(std::uint32_t client,
                                     std::uint32_t request,
                                     const kcpc::SnapshotLoadRequest& load,
                                     kcpc::Stamp& stamp, std::string& message);

// Configuration (section 12): the keys of the text IPC's `config`, through
// the functions that command uses (ipc_config_get() and the others in
// koncepcja_ipc_server.h). A get is read through the gate; a set runs under
// a pause lease; an apply has the main thread rebuild the machine
// (koncpc_rebuild_machine()) and returns when it has.
kcpc::Status kcpc_host_config_get(const std::string& key,
                                  kcpc::ConfigValue& value);
kcpc::Status kcpc_host_config_set(const std::string& key,
                                  const std::string& value,
                                  kcpc::ConfigEffect& effect,
                                  std::string& message);
kcpc::Status kcpc_host_config_apply(std::uint32_t client, std::uint32_t request,
                                    kcpc::Stamp& stamp, std::string& message);
