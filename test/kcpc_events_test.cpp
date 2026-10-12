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
