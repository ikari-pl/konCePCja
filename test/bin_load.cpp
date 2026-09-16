#include <gtest/gtest.h>

#include "koncepcja.h"
#include "z80_view.h"

// -i/--inject hands the program to the firmware's own launcher, MC START
// PROGRAM (&BD16), the way RUN" does once a file is in memory: HL = entry,
// C = &FF (a RAM program selects no ROM). Setting PC to the entry point
// behind the firmware's back left the interrupt-driven keyboard scan dead
// (beads-scrl; test/integrated/ipc_harness.py test_inject_launches_like_run
// proves the key arrives end to end).

TEST(FirmwareLaunch, EntersMcStartProgramWithTheEntryInHl) {
  t_z80regs regs;
  regs.PC.w.l = 0x1234;
  regs.SP.w.l = 0xBFFE;

  koncpc_firmware_launch_regs(regs, 0x6000);

  EXPECT_EQ(0xBD16, regs.PC.w.l) << "MC START PROGRAM, not the entry point";
  EXPECT_EQ(0x6000, regs.HL.w.l);
  EXPECT_EQ(0xFF, regs.BC.b.l) << "C = &FF: no ROM to select for RAM";
  EXPECT_EQ(0xBFFE, regs.SP.w.l)
      << "the stack is the firmware's to reset, not ours to fake";
}

TEST(FirmwareLaunch, DoesNotClobberTheHighByteOfBc) {
  t_z80regs regs;
  regs.BC.w.l = 0x1234;
  koncpc_firmware_launch_regs(regs, 0x4000);
  EXPECT_EQ(0x12, regs.BC.b.h);
  EXPECT_EQ(0xFF, regs.BC.b.l);
}
