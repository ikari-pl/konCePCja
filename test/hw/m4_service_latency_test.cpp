// konCePCja — the M4 must be answered at coprocessor latency, IN the frame.
//
// WHY THIS FILE EXISTS
//
// The real M4's STM32 answers a latched command in microseconds. The host used
// to answer once per frame — 20ms, four orders of magnitude slower than the
// poll loops in the M4 ROM tolerate. Every M4 exchange "worked" at the mailbox
// level while the CPC-visible result was garbage: the ROM's ready-poll timed
// out mid-command, printed junk, and retried (a two-file `cat` issued READDIR
// forty times and flooded the screen with '+').
//
// None of the 100+ M4 tests caught it, because every one constructed its
// subject directly — Device tests poked the Device, host tests poked
// g_m4board. Nothing asserted the TIMING of the wiring in between. This test
// runs the actual machine: a Z80 program sends a command and reads the reply
// window in the SAME frame, exactly like the M4 ROM does.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../../src/hw/m4.h"
#include "../../src/subcycle/machine.h"

namespace {

subcycle::Machine* g_machine = nullptr;
int g_fired = 0;
uint16_t g_last_cmd = 0;

// The host side of the mailbox, reduced to its shape: drain, answer. The
// payload is arbitrary but distinctive.
constexpr uint8_t kReply[6] = {0x01, 0x00, 0x00, 0xAA, 0x55, 0xC3};

void coproc_service(void* /*ctx*/) {
  M4Pending p{};
  if (g_machine->m4_pending(&p) == 0) return;
  g_fired++;
  g_last_cmd = p.cmd;
  g_machine->m4_respond(kReply, sizeof(kReply));
}

// A Z80 program that speaks the M4 ROM's own protocol: accumulate a command
// frame on &FE00, kick execution on &FC00, then read the reply window at
// &E800 — immediately, the way a ROM written against microsecond hardware
// does. The copy lands at 0x9000 where the test can see it.
constexpr uint16_t kProg = 0x8000;
constexpr uint16_t kDst = 0x9000;
constexpr uint8_t kProgram[] = {
    0xF3,              // di
    0x01, 0x00, 0xFE,  // ld bc,&FE00
    0x3E, 0x02,        // ld a,&02      ; size prefix
    0xED, 0x79,        // out (c),a
    0x3E, 0x06,        // ld a,&06      ; cmd lo (C_READDIR)
    0xED, 0x79,        // out (c),a
    0x3E, 0x43,        // ld a,&43      ; cmd hi
    0xED, 0x79,        // out (c),a
    0x06, 0xFC,        // ld b,&FC      ; -> &FC00
    0xED, 0x79,        // out (c),a     ; execute kick
    0x01, 0x00, 0xDF,  // ld bc,&DF00
    0x3E, 0x06,        // ld a,&06      ; upper ROM select = the M4 slot
    0xED, 0x79,        // out (c),a
    0x01, 0x00, 0x7F,  // ld bc,&7F00
    0x3E, 0x81,        // ld a,&81      ; GA RMR: both ROMs enabled, mode 1
    0xED, 0x79,        // out (c),a
    0x21, 0x00, 0xE8,  // ld hl,&E800   ; the reply window
    0x11, 0x00, 0x90,  // ld de,&9000
    0x01, 0x06, 0x00,  // ld bc,6
    0xED, 0xB0,        // ldir
    0x18, 0xFE,        // jr $
};

}  // namespace

TEST(M4ServiceLatency, ACommandIsAnsweredWithinTheSameFrame) {
  // A blank 32K ROM is enough: the program lives in RAM and runs under DI.
  std::vector<uint8_t> rom(0x8000, 0x00);
  std::vector<uint8_t> m4rom(0x4000, 0x00);
  subcycle::Machine m;
  ASSERT_TRUE(m.build(rom.data(), rom.size()));

  m.set_m4_slot(6);
  m.attach_m4_rom(m4rom.data(), m4rom.size());
  m.set_m4(true);

  g_machine = &m;
  g_fired = 0;
  g_last_cmd = 0;
  m.set_m4_service(coproc_service, nullptr);

  for (size_t i = 0; i < sizeof(kProgram); i++)
    m.poke_mem(static_cast<uint16_t>(kProg + i), kProgram[i]);
  Z80Regs regs = m.regs();
  regs.pc = kProg;
  m.set_regs(regs);

  // First run_frame is a warm-up (retires nothing); the second runs the
  // program start to parked, INCLUDING the in-frame service.
  m.run_frame();
  m.run_frame();

  EXPECT_GE(g_fired, 1) << "the coprocessor service never fired in-frame — "
                           "the CPC's read raced a 20ms answer";
  EXPECT_EQ(g_last_cmd, 0x4306) << "the frame that reached the host";
  for (size_t i = 0; i < sizeof(kReply); i++)
    EXPECT_EQ(m.peek_mem(static_cast<uint16_t>(kDst + i)), kReply[i])
        << "reply byte " << i
        << " read by the CPC in the same frame it sent the command; 0xFF "
           "here means the read hit the busy sentinel — the answer was not "
           "there yet";

  g_machine = nullptr;
  m.set_m4_service(nullptr, nullptr);
}
