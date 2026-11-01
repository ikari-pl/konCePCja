#pragma once

// The fake machine the KCPC server and client tests run kcpc::Server
// against: each Fake* fills in one group of ServerConfig callbacks and
// records what the server handed it. Moved out of kcpc_server_test.cpp so
// kcpc_client_test.cpp drives the same machine (beads-3sp.1).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kcpc/kcpc_events.h"
#include "kcpc/kcpc_proto.h"
#include "kcpc/kcpc_server.h"

namespace kcpc_fake {

using kcpc::Opcode;
using kcpc::Status;

inline kcpc::MachineInfo fake_machine() {
  kcpc::MachineInfo m;
  m.model = 2;
  m.crtc_type = 1;
  m.effective_tier = 0;
  m.flags = kcpc::kMachineFlagHeadless;
  m.ram_bytes = 128U * 1024U;
  m.rom_slots = 0x81;
  m.stamp = {1234, false};
  return m;
}

// A machine with something to read: 64 KiB in which no two neighbouring bytes
// are alike, registers that are all different, and a record of what the
// server asked for.
struct FakeReads {
  static constexpr std::uint32_t kSize = 0x10000;

  FakeReads() : memory(kSize) {
    for (std::uint32_t a = 0; a < kSize; ++a) {
      memory[a] = static_cast<std::uint8_t>((a * 7) ^ (a >> 8));
    }
  }

  static kcpc::Regs regs() {
    kcpc::Regs g;
    g.af = 0x0102;
    g.bc = 0x0304;
    g.de = 0x0506;
    g.hl = 0x0708;
    g.ix = 0x090A;
    g.iy = 0x0B0C;
    g.sp = 0x0D0E;
    g.pc = 0x0F10;
    g.af_alt = 0x1112;
    g.bc_alt = 0x1314;
    g.de_alt = 0x1516;
    g.hl_alt = 0x1718;
    g.i = 0x19;
    g.r = 0x1A;
    g.iff1 = 1;
    g.iff2 = 0;
    g.im = 2;
    g.halt = 1;
    return g;
  }

  void attach(kcpc::ServerConfig& c) {
    c.regs_get = [this](kcpc::Stamp& stamp, kcpc::Regs& out) {
      if (refuse != Status::kOk) return refuse.load();
      stamp = {77, true};
      out = regs();
      return Status::kOk;
    };
    c.mem_info = [this](kcpc::MemInfo& info) {
      if (refuse != Status::kOk) return refuse.load();
      info.ram_bytes = kSize;
      info.ram_config = 0xC4;
      info.rom_config = 0x89;
      info.upper_rom = 7;
      info.rom_slots = 0x80;
      info.stamp = {78, false};
      return Status::kOk;
    };
    c.mem_read = [this](const kcpc::MemReadRequest& request, kcpc::Stamp& stamp,
                        std::uint8_t* dst) {
      mem_reads.fetch_add(1);
      last = request;
      if (refuse != Status::kOk) return refuse.load();
      if (!kcpc::mem_range_ok(request.addr, request.len, kSize)) {
        return Status::kOutOfRange;
      }
      std::copy_n(memory.begin() + request.addr, request.len, dst);
      stamp = {79, false};
      return Status::kOk;
    };
  }

  std::vector<std::uint8_t> memory;
  std::atomic<Status> refuse{Status::kOk};
  std::atomic<int> mem_reads{0};
  kcpc::MemReadRequest last;  // written on the one reader thread of a test
};

// A machine that can be written: it records what the server handed it.
struct FakeWrites {
  void attach(kcpc::ServerConfig& c) {
    c.regs_set = [this](std::uint32_t mask, const kcpc::Regs& values,
                        kcpc::Stamp& stamp, kcpc::Regs& after) {
      regs_sets.fetch_add(1);
      if (refuse != Status::kOk) return refuse.load();
      last_mask = mask;
      last_regs = values;
      stamp = {90, true};
      after = FakeReads::regs();
      after.hl = values.hl;
      return Status::kOk;
    };
    c.mem_write = [this](const kcpc::MemWriteRequest& request,
                         const std::uint8_t* data, std::size_t len,
                         kcpc::Stamp& stamp) {
      mem_writes.fetch_add(1);
      if (refuse != Status::kOk) return refuse.load();
      if (!kcpc::mem_range_ok(request.addr, static_cast<std::uint32_t>(len),
                              FakeReads::kSize)) {
        return Status::kOutOfRange;
      }
      last_write = request;
      written.assign(data, data + len);
      stamp = {91, false};
      return Status::kOk;
    };
  }

  std::atomic<Status> refuse{Status::kOk};
  std::atomic<int> regs_sets{0};
  std::atomic<int> mem_writes{0};
  // Written on the controller's reader thread, read after its reply.
  std::uint32_t last_mask = 0;
  kcpc::Regs last_regs;
  kcpc::MemWriteRequest last_write;
  std::vector<std::uint8_t> written;
};

// Run control that records what the server handed it. A request can be made
// to post an event that names it before it returns, as the real machine does
// under the pause mutex.
struct FakeRun {
  void attach(kcpc::ServerConfig& c, kcpc::Server& server) {
    c.run_state = [this](kcpc::Stamp& stamp, bool& walk_active) {
      stamp = {55, paused.load()};
      walk_active = walking.load();
      return Status::kOk;
    };
    c.pause = [this, &server](const kcpc::Cause& cause, kcpc::Stamp& stamp) {
      pauses.fetch_add(1);
      last_cause = cause;
      if (refuse != Status::kOk) return refuse.load();
      paused = true;
      if (post_from_request) {
        kcpc::Event e;
        e.opcode = Opcode::kEvtStopped;
        e.frame = 56;
        e.reason = static_cast<std::uint8_t>(kcpc::StopReason::kPaused);
        e.pc = 0x1234;
        e.cause_client = cause.client;
        e.cause_request = cause.request;
        server.post(e);
        // Long enough for the fan-out thread to have the event at the
        // connection before the response exists.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      stamp = {56, true};
      return Status::kOk;
    };
    c.run = [this](const kcpc::Cause& cause, kcpc::Stamp& stamp) {
      runs.fetch_add(1);
      last_cause = cause;
      if (refuse != Status::kOk) return refuse.load();
      paused = false;
      stamp = {57, false};
      return Status::kOk;
    };
    c.reset = [this](const kcpc::Cause& cause, bool stay_paused,
                     kcpc::Stamp& stamp) {
      resets.fetch_add(1);
      last_cause = cause;
      last_stay_paused = stay_paused;
      if (refuse != Status::kOk) return refuse.load();
      stamp = {58, stay_paused};
      return Status::kOk;
    };
    c.step = [this](const kcpc::Cause& cause, const kcpc::StepRequest& step) {
      steps.fetch_add(1);
      last_cause = cause;
      last_step = step;
      return refuse.load();
    };
  }

  std::atomic<Status> refuse{Status::kOk};
  std::atomic<bool> paused{false};
  std::atomic<bool> walking{false};
  std::atomic<bool> post_from_request{false};
  std::atomic<int> pauses{0};
  std::atomic<int> runs{0};
  std::atomic<int> resets{0};
  std::atomic<int> steps{0};
  // Written on the controller's reader thread, read after its reply.
  kcpc::Cause last_cause;
  bool last_stay_paused = false;
  kcpc::StepRequest last_step;
};

// The breakpoint lists of a machine that records what the server handed it.
// An edit posts EVT_BREAKPOINTS_CHANGED naming the request before it
// returns, as the real lists do from under their lock.
struct FakeBreakpoints {
  void attach(kcpc::ServerConfig& c, kcpc::Server& server) {
    c.bp_list = [this](std::uint64_t& out, std::vector<kcpc::BpEntry>& list) {
      lists.fetch_add(1);
      out = generation;
      list = entries;
      return Status::kOk;
    };
    c.bp_add = [this, &server](const kcpc::Cause& cause,
                               const kcpc::BpEntry& entry, std::uint64_t& out,
                               std::string& message) {
      adds.fetch_add(1);
      last_cause = cause;
      last_entry = entry;
      if (refuse != Status::kOk) {
        message = refusal;
        return refuse.load();
      }
      entries.push_back(entry);
      changed(server, cause, out);
      return Status::kOk;
    };
    c.bp_del = [this, &server](const kcpc::Cause& cause,
                               const kcpc::BpDelRequest& del,
                               std::uint64_t& out) {
      dels.fetch_add(1);
      last_cause = cause;
      last_del = del;
      if (refuse != Status::kOk) return refuse.load();
      changed(server, cause, out);
      return Status::kOk;
    };
    c.bp_clear = [this, &server](const kcpc::Cause& cause, std::uint8_t kind,
                                 std::uint64_t& out) {
      clears.fetch_add(1);
      last_cause = cause;
      last_clear_kind = kind;
      entries.clear();
      changed(server, cause, out);
      return Status::kOk;
    };
  }

  void changed(kcpc::Server& server, const kcpc::Cause& cause,
               std::uint64_t& out) {
    out = ++generation;
    kcpc::Event e;
    e.opcode = Opcode::kEvtBreakpointsChanged;
    e.generation = out;
    e.cause_client = cause.client;
    e.cause_request = cause.request;
    server.post(e);
  }

  std::atomic<Status> refuse{Status::kOk};
  std::string refusal;
  std::atomic<int> lists{0};
  std::atomic<int> adds{0};
  std::atomic<int> dels{0};
  std::atomic<int> clears{0};
  // Written on the controller's reader thread, read after its reply.
  std::uint64_t generation = 40;
  std::vector<kcpc::BpEntry> entries;
  kcpc::Cause last_cause;
  kcpc::BpEntry last_entry;
  kcpc::BpDelRequest last_del;
  std::uint8_t last_clear_kind = 0;
};

// A keyboard matrix, a typing queue and a mouse, and a record of what the
// server did to them. The callbacks come from several reader threads (a key
// is released by whichever thread took control away), so everything is read
// and written under one mutex.
struct FakeInput {
  FakeInput() { matrix.fill(0xFF); }

  void attach(kcpc::ServerConfig& c) {
    c.key = [this](std::uint8_t row, std::uint8_t bit, bool down) {
      std::scoped_lock const lock(mutex);
      (down ? presses : releases) += 1;
      auto const mask = static_cast<std::uint8_t>(1U << bit);
      if (down) {
        matrix[row] &= static_cast<std::uint8_t>(~mask);
      } else {
        matrix[row] |= mask;
      }
      return Status::kOk;
    };
    c.keys_release = [this] {
      std::scoped_lock const lock(mutex);
      release_alls += 1;
      matrix.fill(0xFF);
      return Status::kOk;
    };
    c.text = [this](const std::string& text, std::uint32_t& pending,
                    std::string& message) {
      std::scoped_lock const lock(mutex);
      if (text.find("~NOPE~") != std::string::npos) {
        message = "unknown key: NOPE";
        return Status::kBadPayload;
      }
      typed += text;
      pending = static_cast<std::uint32_t>(typed.size());
      return Status::kOk;
    };
    c.mouse = [this](int dx, int dy, bool set_buttons, std::uint8_t buttons) {
      std::scoped_lock const lock(mutex);
      mouse_calls += 1;
      if (!mouse_fitted) return Status::kNotPresent;
      mouse_x += dx;
      mouse_y += dy;
      if (set_buttons) mouse_buttons = buttons;
      return Status::kOk;
    };
    c.input_state = [this](kcpc::KeyMatrix& rows) {
      std::scoped_lock const lock(mutex);
      rows = matrix;
      return Status::kOk;
    };
    c.gun = [this](bool aim, std::uint16_t x, std::uint16_t y, bool set_trigger,
                   bool pulled) {
      std::scoped_lock const lock(mutex);
      gun_calls += 1;
      if (!gun_fitted) return Status::kNotPresent;
      if (aim) {
        gun_x = x;
        gun_y = y;
      }
      if (set_trigger) gun_pulled = pulled;
      return Status::kOk;
    };
  }

  std::uint8_t row(std::size_t index) {
    std::scoped_lock const lock(mutex);
    return matrix[index];
  }
  // A key some other source holds down: the text IPC, the user's keyboard.
  void press_from_elsewhere(std::uint8_t row_index, std::uint8_t bit) {
    std::scoped_lock const lock(mutex);
    matrix[row_index] &= static_cast<std::uint8_t>(~(1U << bit));
  }
  int count(int FakeInput::* counter) {
    std::scoped_lock const lock(mutex);
    return this->*counter;
  }
  std::uint8_t buttons() {
    std::scoped_lock const lock(mutex);
    return mouse_buttons;
  }
  bool pulled() {
    std::scoped_lock const lock(mutex);
    return gun_pulled;
  }

  std::mutex mutex;
  kcpc::KeyMatrix matrix{};  // the matrix's polarity: a clear bit is down
  int presses = 0;
  int releases = 0;
  int release_alls = 0;
  int mouse_calls = 0;
  std::string typed;
  bool mouse_fitted = true;
  int mouse_x = 0;
  int mouse_y = 0;
  std::uint8_t mouse_buttons = 0;
  int gun_calls = 0;
  bool gun_fitted = true;
  int gun_x = -1;
  int gun_y = -1;
  bool gun_pulled = false;
};

// A machine with chips, a symbol table and a frame. The table posts
// EVT_SYMBOLS_CHANGED for every change, as the real one does.
struct FakeDevtools {
  void attach(kcpc::ServerConfig& c, kcpc::Server& server) {
    c.chip_get = [this](kcpc::ChipState& state) {
      std::scoped_lock const lock(mutex);
      chip_calls += 1;
      auto const chip = static_cast<kcpc::Chip>(state.chip);
      if (chip == kcpc::Chip::kAsic && !has_asic) return Status::kNotPresent;
      std::uint8_t const asked = state.chip;
      state = chips;
      state.chip = asked;
      state.stamp.frame = 77;
      state.stamp.paused = true;
      return chip_status;
    };
    c.sym_list = [this](std::uint64_t& generation,
                        std::vector<kcpc::Symbol>& out) {
      std::scoped_lock const lock(mutex);
      generation = sym_generation;
      out = symbols;
      return Status::kOk;
    };
    c.sym_add = [this, &server](const kcpc::Cause& cause,
                                const kcpc::Symbol& symbol,
                                std::uint64_t& generation) {
      std::scoped_lock const lock(mutex);
      symbols.push_back(symbol);
      generation = changed(server, cause);
      return Status::kOk;
    };
    c.sym_del = [this, &server](const kcpc::Cause& cause,
                                const std::string& name,
                                std::uint64_t& generation) {
      std::scoped_lock const lock(mutex);
      for (auto it = symbols.begin(); it != symbols.end(); ++it) {
        if (it->name != name) continue;
        symbols.erase(it);
        generation = changed(server, cause);
        return Status::kOk;
      }
      return Status::kNotFound;
    };
    c.sym_load = [this, &server](
                     const kcpc::Cause& cause, const std::string& path,
                     std::uint64_t& generation, std::uint32_t& loaded,
                     std::string& /*message*/) {
      std::scoped_lock const lock(mutex);
      if (path != "game.sym") return Status::kNotFound;
      symbols.push_back(kcpc::Symbol{0xC000, "rom_entry"});
      symbols.push_back(kcpc::Symbol{0xC003, "rom_name"});
      loaded = 2;
      generation = changed(server, cause);
      return Status::kOk;
    };
    c.fb_info = [this](kcpc::FbInfo& info) {
      info.width = 768;
      info.height = 272;
      info.stride = 2304;
      if (fb_ring) {  // as a core whose frame ring runs
        info.transport = kcpc::kFbTransportShm;
        info.fb_seq = fb_seq.load();
        info.shm_name = "/kcpc-fb-4242";
      }
      return Status::kOk;
    };
    c.media_state = [this](std::uint8_t slot, kcpc::MediaNote& note) {
      std::scoped_lock const lock(mutex);
      media_asked.push_back(slot);
      if (slot == 0) {
        note.present = true;
        note.path = "/discs/game.dsk";
      }
    };
  }

  // Under `mutex`. One change of the table: a new generation, and the event.
  std::uint64_t changed(kcpc::Server& server, const kcpc::Cause& cause) {
    sym_generation += 1;
    kcpc::Event event;
    event.opcode = Opcode::kEvtSymbolsChanged;
    event.generation = sym_generation;
    event.cause_client = cause.client;
    event.cause_request = cause.request;
    server.post(event);
    return sym_generation;
  }

  std::mutex mutex;
  int chip_calls = 0;
  bool has_asic = true;
  Status chip_status = Status::kOk;
  kcpc::ChipState chips;
  std::uint64_t sym_generation = 10;
  std::vector<kcpc::Symbol> symbols;
  std::vector<std::uint8_t> media_asked;
  // FB_INFO: whether the frame ring runs, and its latest frame.
  std::atomic<bool> fb_ring{true};
  std::atomic<std::uint64_t> fb_seq{0};
};

// A machine with two drives, a deck, snapshots and one setting. A change of
// a slot and a rebuild post their events named after the request, as the
// real machine's side does.
struct FakeMedia {
  void attach(kcpc::ServerConfig& c, kcpc::Server& server) {
    c.media_insert = [this, &server](const kcpc::Cause& cause,
                                     const kcpc::MediaInsertRequest& insert,
                                     std::uint64_t& generation,
                                     std::string& message) {
      std::scoped_lock const lock(mutex);
      calls += 1;
      if (insert.path == "missing.dsk") return Status::kNotFound;
      if (insert.path == "junk.dsk") {
        message = "this slot takes .dsk.ipf.raw";
        return Status::kBadPayload;
      }
      path[insert.slot] = insert.path;
      generation = changed(server, insert.slot, cause);
      return Status::kOk;
    };
    c.media_eject = [this, &server](const kcpc::Cause& cause, std::uint8_t slot,
                                    std::uint64_t& generation,
                                    std::string& /*message*/) {
      std::scoped_lock const lock(mutex);
      calls += 1;
      path[slot].clear();
      generation = changed(server, slot, cause);
      return Status::kOk;
    };
    c.media_info = [this](kcpc::MediaInfo& info) {
      std::scoped_lock const lock(mutex);
      info.present = path[info.slot].empty() ? 0 : 1;
      info.flags = kcpc::kMediaFlagUnsaved;
      info.generation = generations[info.slot];
      info.path = path[info.slot];
      info.tape = tape;
      // The server keeps the slot that was asked for.
      info.slot = 0xEE;
      return Status::kOk;
    };
    c.tape_control = [this](const kcpc::TapeControlRequest& control,
                            kcpc::TapeState& out) {
      std::scoped_lock const lock(mutex);
      calls += 1;
      if (path[2].empty()) return Status::kNotPresent;
      if (control.action ==
          static_cast<std::uint8_t>(kcpc::TapeAction::kSeek)) {
        if (control.block >= tape.blocks) return Status::kOutOfRange;
        tape.block = control.block;
      }
      tape.playing =
          control.action == static_cast<std::uint8_t>(kcpc::TapeAction::kPlay);
      out = tape;
      return Status::kOk;
    };
    c.snapshot_save = [this](const std::string& file, kcpc::Stamp& stamp,
                             std::string& message) {
      std::scoped_lock const lock(mutex);
      calls += 1;
      if (file == "/nowhere/state.sna") {
        message = "no such directory";
        return Status::kNotFound;
      }
      saved = file;
      stamp.frame = 500;
      stamp.paused = true;
      return Status::kOk;
    };
    c.snapshot_load = [this, &server](const kcpc::Cause& cause,
                                      const kcpc::SnapshotLoadRequest& load,
                                      kcpc::Stamp& stamp,
                                      std::string& /*message*/) {
      std::scoped_lock const lock(mutex);
      calls += 1;
      if (load.path != "state.sna") return Status::kBadPayload;
      loaded_flags = load.flags;
      stamp.frame = 501;
      stamp.paused = (load.flags & kcpc::kSnapshotFlagStayPaused) != 0;
      kcpc::Event event;
      event.opcode = Opcode::kEvtReset;
      event.frame = 501;
      event.cause_client = cause.client;
      event.cause_request = cause.request;
      server.post(event);
      return Status::kOk;
    };
    c.config_get = [this](const std::string& key, kcpc::ConfigValue& value) {
      std::scoped_lock const lock(mutex);
      if (key == "fullscreen") return Status::kNotPresent;
      if (key != "model") return Status::kNotFound;
      value.value = std::to_string(model);
      value.has_pending = staged != model;
      value.pending = std::to_string(staged);
      return Status::kOk;
    };
    c.config_set = [this](const std::string& key, const std::string& value,
                          kcpc::ConfigEffect& effect, std::string& message) {
      std::scoped_lock const lock(mutex);
      calls += 1;
      if (key != "model") return Status::kNotFound;
      if (value.size() != 1 || value[0] < '0' || value[0] > '3') {
        message = "model must be 0-3";
        return Status::kBadPayload;
      }
      staged = value[0] - '0';
      effect = kcpc::ConfigEffect::kStaged;
      return Status::kOk;
    };
    c.config_apply = [this, &server](const kcpc::Cause& cause,
                                     kcpc::Stamp& stamp, std::string& message) {
      std::scoped_lock const lock(mutex);
      calls += 1;
      if (apply_status != Status::kOk) {
        message = "the machine could not be rebuilt (code 7)";
        return apply_status;
      }
      model = staged;
      kcpc::Event event;
      event.opcode = Opcode::kEvtMachineChanged;
      event.machine.model = static_cast<std::uint8_t>(model);
      event.cause_client = cause.client;
      event.cause_request = cause.request;
      server.post(event);
      stamp.frame = 900;
      return Status::kOk;
    };
  }

  // Under `mutex`.
  std::uint64_t changed(kcpc::Server& server, std::uint8_t slot,
                        const kcpc::Cause& cause) {
    generations[slot] += 1;
    kcpc::Event event;
    event.opcode = Opcode::kEvtMediaChanged;
    event.slot = slot;
    event.generation = generations[slot];
    event.cause_client = cause.client;
    event.cause_request = cause.request;
    server.post(event);
    return generations[slot];
  }

  std::mutex mutex;
  int calls = 0;  // requests that reached a callback that changes something
  std::string path[kcpc::kMediaSlots];
  std::uint64_t generations[kcpc::kMediaSlots] = {20, 30, 40, 50};
  kcpc::TapeState tape{0, 1, 0, 3};
  std::string saved;
  std::uint8_t loaded_flags = 0xFF;
  int model = 2;
  int staged = 2;
  Status apply_status = Status::kOk;
};

}  // namespace kcpc_fake
