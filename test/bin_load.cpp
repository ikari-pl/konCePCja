#include <gtest/gtest.h>

#include "koncepcja.h"
#include "z80_view.h"

// -i/--inject hands the program to the firmware's own launcher, MC START
// PROGRAM (&BD16), the way RUN" does once a file is in memory: HL = entry,
// C = &FF (a RAM program selects no ROM). Setting PC to the entry point
// behind the firmware's back left the interrupt-driven keyboard scan dead
// (beads-scrl; test/integrated/ipc_harness.py test_inject_launches_like_run
// proves the key arrives end to end). Without a firmware jumpblock — a
// prepared test ROM, or an inject that fires before the firmware built it —
// the program is entered directly, as before.

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

TEST(FirmwareLaunch, JumpblockIsRecognisedByItsEntryOpcode) {
  EXPECT_TRUE(koncpc_firmware_jumpblock_present(0xCF)) << "RST 1 (LOW JUMP)";
  EXPECT_TRUE(koncpc_firmware_jumpblock_present(0xC3)) << "JP";
  EXPECT_FALSE(koncpc_firmware_jumpblock_present(0x00)) << "zeroed RAM";
  EXPECT_FALSE(koncpc_firmware_jumpblock_present(0xFF)) << "unwritten RAM";
  EXPECT_FALSE(koncpc_firmware_jumpblock_present(0x76)) << "arbitrary code";
}

TEST(FirmwareLaunch, InjectGoesThroughTheFirmwareWhenItsJumpblockIsThere) {
  t_z80regs regs;
  koncpc_inject_launch_regs(regs, 0x6000, 0xCF);
  EXPECT_EQ(0xBD16, regs.PC.w.l);
  EXPECT_EQ(0x6000, regs.HL.w.l);
  EXPECT_EQ(0xFF, regs.BC.b.l);
}

TEST(FirmwareLaunch, InjectEntersDirectlyWithoutAFirmwareJumpblock) {
  t_z80regs regs;
  regs.HL.w.l = 0x1111;
  regs.BC.w.l = 0x2222;
  koncpc_inject_launch_regs(regs, 0x6000, 0x00);
  EXPECT_EQ(0x6000, regs.PC.w.l) << "no firmware to hand the program to";
  EXPECT_EQ(0x1111, regs.HL.w.l) << "registers left alone on the direct path";
  EXPECT_EQ(0x2222, regs.BC.w.l);
}
