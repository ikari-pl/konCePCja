#pragma once

// KCPC events on their way from whoever causes them to the clients.
//
// The contract is docs/protocol/kcpc-binary-v1.md, sections 8 (seam 3) and 9.
// An event is posted wherever it happens: on the frame runner (a frame
// completed, a breakpoint), under the pause mutex on whichever thread paused
// or resumed the machine, on a client's reader thread (a change of
// controller). None of those may wait for a client, so posting is one push
// into a fixed ring; the server's fan-out thread takes the events out and
// copies them to the clients that subscribed.
//
// Emulator-free, like the rest of the server: test_runner drives it alone.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "kcpc/kcpc_proto.h"

namespace kcpc {

// One event, before it is encoded for a connection. Plain data of fixed size:
// it is copied into the ring by value and nothing in it is allocated.
struct Event {
  Opcode opcode = Opcode::kEvtFrame;
  std::uint64_t frame = 0;
  // EVT_STOPPED.
  std::uint8_t reason = 0;  // a StopReason; for EVT_CONTROL_CHANGED its reason
  std::uint8_t access = 0;
  std::uint16_t pc = 0;
  std::uint16_t addr = 0;
  std::uint8_t value = 0;
  std::uint8_t old_value = 0;
  // The request that led to the event, or 0. On the wire for EVT_STOPPED,
  // EVT_RESUMED and EVT_RESET; for every event it is also what makes the
  // server send the event after the response to that request, never before.
  std::uint32_t cause_client = 0;
  std::uint32_t cause_request = 0;
  // EVT_STOPPED: the client whose step this stop ends. It is sent the event
  // whatever it subscribed to. Not on the wire.
  std::uint32_t step_client = 0;
  // EVT_CONTROL_CHANGED.
  std::uint32_t controller = 0;
  std::uint32_t previous = 0;
  // EVT_OVERRUN.
  std::uint32_t dropped = 0;
  // EVT_FRAME: the frame's sequence number in the frame ring, 0 without one.
  std::uint64_t fb_seq = 0;
  // EVT_BREAKPOINTS_CHANGED: z80_breakpoint_generation() after the edit.
  // EVT_SYMBOLS_CHANGED and EVT_MEDIA_CHANGED: the table's, the slot's.
  std::uint64_t generation = 0;
  // EVT_MEDIA_CHANGED. Whether a medium is in the slot, and its path, are
  // not here: a path does not fit a record of fixed size, and the frame
  // runner posts this event too. The fan-out thread asks for both when it
  // sends the event (ServerConfig::media_state).
  std::uint8_t slot = 0;
  // EVT_MACHINE_CHANGED: the machine as rebuilt.
  MachineInfo machine;
};

// What the fan-out thread adds to an EVT_MEDIA_CHANGED before it is encoded.
struct MediaNote {
  bool present = false;
  std::string path;  // empty if unknown
};

// The payload of the event frame, per section 9. `media` is for
// EVT_MEDIA_CHANGED alone.
inline std::vector<std::uint8_t> encode_event(const Event& e,
                                              const MediaNote& media = {}) {
  Writer w;
  switch (e.opcode) {
    case Opcode::kEvtStopped:
      w.u64(e.frame);
      w.u8(e.reason);
      w.u8(e.access);
      w.u16(e.pc);
      w.u16(e.addr);
      w.u8(e.value);
      w.u8(e.old_value);
      w.u32(e.cause_client);
      w.u32(e.cause_request);
      break;
    case Opcode::kEvtResumed:
    case Opcode::kEvtReset:
      w.u64(e.frame);
      w.u32(e.cause_client);
      w.u32(e.cause_request);
      break;
    case Opcode::kEvtFrame:
      w.u64(e.frame);
      w.u64(e.fb_seq);
      break;
    case Opcode::kEvtControlChanged:
      w.u32(e.controller);
      w.u32(e.previous);
      w.u8(e.reason);
      w.zeros(3);
      break;
    case Opcode::kEvtOverrun:
      w.u32(e.dropped);
      break;
    case Opcode::kEvtBreakpointsChanged:
    case Opcode::kEvtSymbolsChanged:
      w.u64(e.generation);
      break;
    case Opcode::kEvtMediaChanged:
      w.u8(e.slot);
      w.u8(media.present ? 1 : 0);
      w.zeros(2);
      w.u64(e.generation);
      w.str(media.path);
      break;
    case Opcode::kEvtMachineChanged:
      write_machine_info(w, e.machine);
      break;
    default:  // EVT_SHUTDOWN has no payload
      break;
  }
  return w.take();
}

// encode_event() read back: what a client makes of an event frame. False
// when `opcode` is no event or the payload is shorter than its fields; bytes
// beyond them are ignored (section 4, "Growing a payload"). `media` is
// filled for EVT_MEDIA_CHANGED alone.
inline bool decode_event(std::uint16_t opcode,
                         const std::vector<std::uint8_t>& payload, Event& e,
                         MediaNote& media) {
  Reader r(payload);
  e = Event{};
  media = MediaNote{};
  e.opcode = static_cast<Opcode>(opcode);
  switch (e.opcode) {
    case Opcode::kEvtStopped:
      r.u64(e.frame);
      r.u8(e.reason);
      r.u8(e.access);
      r.u16(e.pc);
      r.u16(e.addr);
      r.u8(e.value);
      r.u8(e.old_value);
      r.u32(e.cause_client);
      return r.u32(e.cause_request);
    case Opcode::kEvtResumed:
    case Opcode::kEvtReset:
      r.u64(e.frame);
      r.u32(e.cause_client);
      return r.u32(e.cause_request);
    case Opcode::kEvtFrame:
      r.u64(e.frame);
      return r.u64(e.fb_seq);
    case Opcode::kEvtControlChanged:
      r.u32(e.controller);
      r.u32(e.previous);
      r.u8(e.reason);
      return r.skip(3);
    case Opcode::kEvtOverrun:
      return r.u32(e.dropped);
    case Opcode::kEvtBreakpointsChanged:
    case Opcode::kEvtSymbolsChanged:
      return r.u64(e.generation);
    case Opcode::kEvtMediaChanged: {
      std::uint8_t present = 0;
      r.u8(e.slot);
      r.u8(present);
      r.skip(2);
      r.u64(e.generation);
      media.present = present != 0;
      return r.str(media.path);
    }
    case Opcode::kEvtMachineChanged:
      if (!read_machine_info(r, e.machine)) return false;
      e.frame = e.machine.stamp.frame;
      return true;
    case Opcode::kEvtShutdown:
      return true;
    default:
      return false;
  }
}

// The class bit of SUBSCRIBE an event belongs to, or 0 for the events that
// ignore the mask and reach every client.
inline std::uint32_t event_class(Opcode opcode) {
  switch (opcode) {
    case Opcode::kEvtStopped:
    case Opcode::kEvtResumed:
    case Opcode::kEvtReset:
      return kEventClassRun;
    case Opcode::kEvtFrame:
      return kEventClassFrame;
    case Opcode::kEvtMediaChanged:
      return kEventClassMedia;
    case Opcode::kEvtBreakpointsChanged:
    case Opcode::kEvtSymbolsChanged:
      return kEventClassDebug;
    case Opcode::kEvtMachineChanged:
      return kEventClassMachine;
    default:
      return 0;
  }
}

// A bounded queue with any number of producers and one consumer (after
// Vyukov's bounded queue). push() never blocks, never allocates and never
// waits for another thread: a full ring loses the event and counts it. That
// is what lets the frame runner post from inside its frame.
//
// The design asked for a single producer, the runner. Run-state events do
// not happen there: a pause is settled on the thread that paused, under the
// pause mutex, and the runner may be finishing a frame at that moment. So
// producers claim their slot with a compare-and-swap.
class EventRing {
 public:
  static constexpr std::size_t kCapacity = 4096;  // a power of two

  EventRing() : cells_(new Cell[kCapacity]) {
    for (std::size_t i = 0; i < kCapacity; ++i) {
      cells_[i].seq.store(i, std::memory_order_relaxed);
    }
  }
  EventRing(const EventRing&) = delete;
  EventRing& operator=(const EventRing&) = delete;

  // False when the ring was full: the event is lost, and counted.
  bool push(const Event& event) {
    std::size_t pos = head_.load(std::memory_order_relaxed);
    for (;;) {
      Cell& cell = cells_[pos & kMask];
      std::size_t const seq = cell.seq.load(std::memory_order_acquire);
      auto const ahead =
          static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);
      if (ahead == 0) {
        if (head_.compare_exchange_weak(pos, pos + 1,
                                        std::memory_order_relaxed)) {
          cell.event = event;
          cell.seq.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (ahead < 0) {
        lost_.fetch_add(1, std::memory_order_relaxed);
        return false;
      } else {
        pos = head_.load(std::memory_order_relaxed);
      }
    }
  }

  // The consumer's half; one thread only. False when nothing is ready.
  bool pop(Event& out) {
    Cell& cell = cells_[tail_ & kMask];
    if (cell.seq.load(std::memory_order_acquire) != tail_ + 1) return false;
    out = cell.event;
    cell.seq.store(tail_ + kCapacity, std::memory_order_release);
    ++tail_;
    return true;
  }

  // Consumer only: whether pop() would find an event.
  bool empty() const {
    return cells_[tail_ & kMask].seq.load(std::memory_order_acquire) !=
           tail_ + 1;
  }

  // Events lost to a full ring since the last call.
  std::uint64_t take_lost() {
    return lost_.exchange(0, std::memory_order_relaxed);
  }

 private:
  static constexpr std::size_t kMask = kCapacity - 1;
  struct Cell {
    std::atomic<std::size_t> seq{0};
    Event event;
  };

  std::unique_ptr<Cell[]> cells_;
  std::atomic<std::size_t> head_{0};  // next slot a producer claims
  std::size_t tail_ = 0;              // consumer only
  std::atomic<std::uint64_t> lost_{0};
};

}  // namespace kcpc
