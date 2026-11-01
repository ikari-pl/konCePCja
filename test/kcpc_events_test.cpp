// konCePCja — tests for the KCPC event ring and event encoding (beads-3o1.4).
//
// The ring is what the frame runner pushes into from inside its frame, so the
// properties pinned here are the ones it relies on: a push never waits, a
// full ring loses the event and counts it, and with several producers at
// once nothing is lost and nothing is delivered twice.

#include "kcpc/kcpc_events.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using kcpc::Event;
using kcpc::EventRing;
using kcpc::Opcode;

Event frame_event(std::uint64_t frame) {
  Event e;
  e.opcode = Opcode::kEvtFrame;
  e.frame = frame;
  return e;
}

}  // namespace

TEST(KcpcEventRing, DeliversInTheOrderPushed) {
  EventRing ring;
  Event out;
  EXPECT_TRUE(ring.empty());
  EXPECT_FALSE(ring.pop(out));
  for (std::uint64_t i = 1; i <= 10; ++i)
    ASSERT_TRUE(ring.push(frame_event(i)));
  EXPECT_FALSE(ring.empty());
  for (std::uint64_t i = 1; i <= 10; ++i) {
    ASSERT_TRUE(ring.pop(out));
    EXPECT_EQ(out.frame, i);
  }
  EXPECT_FALSE(ring.pop(out));
  EXPECT_EQ(ring.take_lost(), 0U);
}

TEST(KcpcEventRing, AFullRingLosesTheEventAndCountsIt) {
  EventRing ring;
  for (std::size_t i = 0; i < EventRing::kCapacity; ++i) {
    ASSERT_TRUE(ring.push(frame_event(i))) << "slot " << i;
  }
  EXPECT_FALSE(ring.push(frame_event(9001)));
  EXPECT_FALSE(ring.push(frame_event(9002)));
  EXPECT_EQ(ring.take_lost(), 2U);
  EXPECT_EQ(ring.take_lost(), 0U) << "the count is taken, not read";

  // What was in the ring is intact, and a freed slot is usable again.
  Event out;
  ASSERT_TRUE(ring.pop(out));
  EXPECT_EQ(out.frame, 0U);
  EXPECT_TRUE(ring.push(frame_event(9003)));
  for (std::size_t i = 1; i < EventRing::kCapacity; ++i) {
    ASSERT_TRUE(ring.pop(out));
    EXPECT_EQ(out.frame, i);
  }
  ASSERT_TRUE(ring.pop(out));
  EXPECT_EQ(out.frame, 9003U);
  EXPECT_TRUE(ring.empty());
}

TEST(KcpcEventRing, WrapsAroundManyTimes) {
  EventRing ring;
  Event out;
  for (std::uint64_t i = 0; i < 5 * EventRing::kCapacity; ++i) {
    ASSERT_TRUE(ring.push(frame_event(i)));
    ASSERT_TRUE(ring.pop(out));
    ASSERT_EQ(out.frame, i);
  }
  EXPECT_EQ(ring.take_lost(), 0U);
}

// Run-state events are pushed on whichever thread paused the machine, beside
// the frame runner pushing frames. Every event pushed arrives exactly once,
// and each producer's events arrive in the order it pushed them.
TEST(KcpcEventRing, SeveralProducersLoseNothingAndKeepTheirOwnOrder) {
  constexpr int kProducers = 4;
  constexpr std::uint64_t kEach = 20000;
  EventRing ring;
  std::atomic<bool> go{false};
  std::atomic<std::uint64_t> pushed{0};
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      while (!go.load()) std::this_thread::yield();
      for (std::uint64_t i = 0; i < kEach; ++i) {
        Event e = frame_event(i);
        e.cause_client = static_cast<std::uint32_t>(p);
        // A full ring is a loss by design; here the test wants every event,
        // so it offers the event again.
        while (!ring.push(e)) std::this_thread::yield();
        pushed.fetch_add(1);
      }
    });
  }
  std::vector<std::uint64_t> next(kProducers, 0);
  std::uint64_t received = 0;
  go.store(true);
  Event out;
  while (received < kProducers * kEach) {
    if (!ring.pop(out)) {
      std::this_thread::yield();
      continue;
    }
    ASSERT_LT(out.cause_client, static_cast<std::uint32_t>(kProducers));
    ASSERT_EQ(out.frame, next[out.cause_client])
        << "producer " << out.cause_client << " out of order or duplicated";
    ++next[out.cause_client];
    ++received;
  }
  for (auto& t : producers) t.join();
  EXPECT_FALSE(ring.pop(out));
  EXPECT_EQ(pushed.load(), kProducers * kEach);
}

TEST(KcpcEvents, StoppedIsTwentyFourBytesInWireOrder) {
  Event e;
  e.opcode = Opcode::kEvtStopped;
  e.frame = 0x0102030405060708ULL;
  e.reason = static_cast<std::uint8_t>(kcpc::StopReason::kStepDone);
  e.access = 2;
  e.pc = 0x4000;
  e.addr = 0x4001;
  e.value = 0xAA;
  e.old_value = 0xBB;
  e.cause_client = 7;
  e.cause_request = 9;
  e.step_client = 7;  // not on the wire
  std::vector<std::uint8_t> const payload = kcpc::encode_event(e);
  ASSERT_EQ(payload.size(), 24U);
  kcpc::Reader r(payload);
  std::uint64_t frame = 0;
  std::uint8_t reason = 0, access = 0, value = 0, old_value = 0;
  std::uint16_t pc = 0, addr = 0;
  std::uint32_t client = 0, request = 0;
  r.u64(frame);
  r.u8(reason);
  r.u8(access);
  r.u16(pc);
  r.u16(addr);
  r.u8(value);
  r.u8(old_value);
  r.u32(client);
  ASSERT_TRUE(r.u32(request));
  EXPECT_EQ(frame, 0x0102030405060708ULL);
  EXPECT_EQ(reason, 5);
  EXPECT_EQ(access, 2);
  EXPECT_EQ(pc, 0x4000);
  EXPECT_EQ(addr, 0x4001);
  EXPECT_EQ(value, 0xAA);
  EXPECT_EQ(old_value, 0xBB);
  EXPECT_EQ(client, 7U);
  EXPECT_EQ(request, 9U);
}

TEST(KcpcEvents, AFrameEventCarriesItsSequenceInTheRing) {
  Event e = frame_event(0x1122);
  e.fb_seq = 0x0807060504030201ULL;
  std::vector<std::uint8_t> const payload = kcpc::encode_event(e);
  ASSERT_EQ(payload.size(), 16U);
  EXPECT_EQ(payload[0], 0x22) << "u64 frame first";
  EXPECT_EQ(payload[1], 0x11);
  EXPECT_EQ(payload[8], 0x01) << "then u64 fb_seq, little-endian";
  EXPECT_EQ(payload[15], 0x08);
}

TEST(KcpcEvents, PayloadSizesMatchTheDocument) {
  Event e;
  e.opcode = Opcode::kEvtResumed;
  EXPECT_EQ(kcpc::encode_event(e).size(), 16U);
  e.opcode = Opcode::kEvtReset;
  EXPECT_EQ(kcpc::encode_event(e).size(), 16U);
  e.opcode = Opcode::kEvtFrame;
  EXPECT_EQ(kcpc::encode_event(e).size(), 16U);
  e.opcode = Opcode::kEvtControlChanged;
  EXPECT_EQ(kcpc::encode_event(e).size(), 12U);
  e.opcode = Opcode::kEvtOverrun;
  EXPECT_EQ(kcpc::encode_event(e).size(), 4U);
  e.opcode = Opcode::kEvtShutdown;
  EXPECT_TRUE(kcpc::encode_event(e).empty());
  e.opcode = Opcode::kEvtBreakpointsChanged;
  e.generation = 0x0807060504030201ULL;
  std::vector<std::uint8_t> const changed = kcpc::encode_event(e);
  ASSERT_EQ(changed.size(), 8U);
  EXPECT_EQ(changed.front(), 0x01) << "the generation, little-endian";
  EXPECT_EQ(changed.back(), 0x08);
  e.opcode = Opcode::kEvtSymbolsChanged;
  EXPECT_EQ(kcpc::encode_event(e), changed) << "the same payload";
}

TEST(KcpcEvents, MediaChangedCarriesTheSlotTheGenerationAndThePath) {
  Event e;
  e.opcode = Opcode::kEvtMediaChanged;
  e.slot = 1;
  e.generation = 0x0102030405060708ULL;
  kcpc::MediaNote note;
  note.present = true;
  note.path = "/d/b.dsk";
  std::vector<std::uint8_t> const bytes = kcpc::encode_event(e, note);
  ASSERT_EQ(bytes.size(), 1U + 1U + 2U + 8U + 2U + 8U);
  EXPECT_EQ(bytes[0], 1) << "slot";
  EXPECT_EQ(bytes[1], 1) << "present";
  EXPECT_EQ(bytes[2], 0);
  EXPECT_EQ(bytes[3], 0);
  EXPECT_EQ(bytes[4], 0x08) << "generation, little-endian";
  EXPECT_EQ(bytes[11], 0x01);
  EXPECT_EQ(bytes[12], 8) << "the path's length";
  EXPECT_EQ(bytes[13], 0);
  EXPECT_EQ(bytes[14], '/');
  EXPECT_EQ(bytes.back(), 'k');

  // With nothing known about the slot: not present, and an empty path.
  std::vector<std::uint8_t> const bare = kcpc::encode_event(e);
  ASSERT_EQ(bare.size(), 14U);
  EXPECT_EQ(bare[1], 0);
}

TEST(KcpcEvents, MachineChangedIsTheMachineInfoFields) {
  Event e;
  e.opcode = Opcode::kEvtMachineChanged;
  e.machine.model = 3;
  e.machine.crtc_type = 3;
  e.machine.effective_tier = 1;
  e.machine.flags = kcpc::kMachineFlagAsic;
  e.machine.ram_bytes = 0x00090000;
  e.machine.rom_slots = 0x80;
  e.machine.stamp.frame = 42;
  e.machine.stamp.paused = true;
  kcpc::Writer expected;
  kcpc::write_machine_info(expected, e.machine);
  std::vector<std::uint8_t> const bytes = kcpc::encode_event(e);
  EXPECT_EQ(bytes, expected.data());
  EXPECT_EQ(bytes.size(), 24U);
  EXPECT_EQ(bytes[0], 3) << "model";
}

TEST(KcpcEvents, OnlyTheThreeMasklessEventsHaveNoClass) {
  EXPECT_EQ(kcpc::event_class(Opcode::kEvtStopped), kcpc::kEventClassRun);
  EXPECT_EQ(kcpc::event_class(Opcode::kEvtResumed), kcpc::kEventClassRun);
  EXPECT_EQ(kcpc::event_class(Opcode::kEvtReset), kcpc::kEventClassRun);
  EXPECT_EQ(kcpc::event_class(Opcode::kEvtFrame), kcpc::kEventClassFrame);
  EXPECT_EQ(kcpc::event_class(Opcode::kEvtMediaChanged),
            kcpc::kEventClassMedia);
  EXPECT_EQ(kcpc::event_class(Opcode::kEvtBreakpointsChanged),
            kcpc::kEventClassDebug);
  EXPECT_EQ(kcpc::event_class(Opcode::kEvtSymbolsChanged),
            kcpc::kEventClassDebug);
  EXPECT_EQ(kcpc::event_class(Opcode::kEvtMachineChanged),
            kcpc::kEventClassMachine);
  EXPECT_EQ(kcpc::event_class(Opcode::kEvtShutdown), 0U);
  EXPECT_EQ(kcpc::event_class(Opcode::kEvtOverrun), 0U);
  EXPECT_EQ(kcpc::event_class(Opcode::kEvtControlChanged), 0U);
}

// A client reads back exactly what the server wrote (beads-3sp.1).
TEST(KcpcEvents, DecodeReadsBackEveryEventEncodeWrites) {
  Event stopped;
  stopped.opcode = Opcode::kEvtStopped;
  stopped.frame = 0x0102030405060708ULL;
  stopped.reason = static_cast<std::uint8_t>(kcpc::StopReason::kWatchpoint);
  stopped.access = kcpc::kBpAccessWrite;
  stopped.pc = 0x4001;
  stopped.addr = 0xBE80;
  stopped.value = 0x5A;
  stopped.old_value = 0xA5;
  stopped.cause_client = 3;
  stopped.cause_request = 99;
  Event control;
  control.opcode = Opcode::kEvtControlChanged;
  control.controller = 7;
  control.previous = 2;
  control.reason = kcpc::kControlTaken;
  Event frame = frame_event(500);
  frame.fb_seq = 499;
  Event machine;
  machine.opcode = Opcode::kEvtMachineChanged;
  machine.machine.model = 3;
  machine.machine.flags = kcpc::kMachineFlagAsic;
  machine.machine.ram_bytes = 128U * 1024U;
  machine.machine.stamp = {321, true};
  Event media;
  media.opcode = Opcode::kEvtMediaChanged;
  media.slot = 1;
  media.generation = 12;
  kcpc::MediaNote const note{true, "/discs/b.dsk"};
  Event resumed;
  resumed.opcode = Opcode::kEvtResumed;
  resumed.frame = 10;
  resumed.cause_client = 5;
  resumed.cause_request = 6;
  Event reset;
  reset.opcode = Opcode::kEvtReset;
  reset.frame = 11;
  reset.cause_client = 7;
  reset.cause_request = 8;
  Event overrun;
  overrun.opcode = Opcode::kEvtOverrun;
  overrun.dropped = 42;
  Event bps;
  bps.opcode = Opcode::kEvtBreakpointsChanged;
  bps.generation = 7;
  Event syms;
  syms.opcode = Opcode::kEvtSymbolsChanged;
  syms.generation = 8;
  Event shutdown;
  shutdown.opcode = Opcode::kEvtShutdown;

  for (Event const& in : {stopped, control, frame, machine, media, resumed,
                          reset, overrun, bps, syms, shutdown}) {
    Event out;
    kcpc::MediaNote out_note;
    ASSERT_TRUE(kcpc::decode_event(static_cast<std::uint16_t>(in.opcode),
                                   kcpc::encode_event(in, note), out,
                                   out_note));
    EXPECT_EQ(out.opcode, in.opcode);
    EXPECT_EQ(out.frame, in.opcode == Opcode::kEvtMachineChanged
                             ? in.machine.stamp.frame
                             : in.frame);
    EXPECT_EQ(out.reason, in.reason);
    EXPECT_EQ(out.access, in.access);
    EXPECT_EQ(out.pc, in.pc);
    EXPECT_EQ(out.addr, in.addr);
    EXPECT_EQ(out.value, in.value);
    EXPECT_EQ(out.old_value, in.old_value);
    EXPECT_EQ(out.controller, in.controller);
    EXPECT_EQ(out.previous, in.previous);
    EXPECT_EQ(out.fb_seq, in.fb_seq);
    EXPECT_EQ(out.generation, in.generation);
    EXPECT_EQ(out.slot, in.slot);
    EXPECT_EQ(out.machine.model, in.machine.model);
    EXPECT_EQ(out.machine.flags, in.machine.flags);
    EXPECT_EQ(out.machine.ram_bytes, in.machine.ram_bytes);
    EXPECT_EQ(out.dropped, in.dropped);
    // The cause is on the wire for a stop, a resume and a reset only.
    bool const wire_cause = in.opcode == Opcode::kEvtStopped ||
                            in.opcode == Opcode::kEvtResumed ||
                            in.opcode == Opcode::kEvtReset;
    EXPECT_EQ(out.cause_client, wire_cause ? in.cause_client : 0U);
    EXPECT_EQ(out.cause_request, wire_cause ? in.cause_request : 0U);
    if (in.opcode == Opcode::kEvtMediaChanged) {
      EXPECT_TRUE(out_note.present);
      EXPECT_EQ(out_note.path, note.path);
    }
  }
}

TEST(KcpcEvents, DecodeRefusesAShortPayloadAndANonEvent) {
  Event e;
  kcpc::MediaNote note;
  std::vector<std::uint8_t> payload = kcpc::encode_event(frame_event(1));
  payload.pop_back();
  EXPECT_FALSE(kcpc::decode_event(static_cast<std::uint16_t>(Opcode::kEvtFrame),
                                  payload, e, note));
  EXPECT_FALSE(kcpc::decode_event(static_cast<std::uint16_t>(Opcode::kPing), {},
                                  e, note));
  EXPECT_TRUE(kcpc::decode_event(
      static_cast<std::uint16_t>(Opcode::kEvtShutdown), {}, e, note));
  // Fields a later minor version appends are ignored.
  payload = kcpc::encode_event(frame_event(9));
  payload.push_back(0xEE);
  ASSERT_TRUE(kcpc::decode_event(static_cast<std::uint16_t>(Opcode::kEvtFrame),
                                 payload, e, note));
  EXPECT_EQ(e.frame, 9U);
}
