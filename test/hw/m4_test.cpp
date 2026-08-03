/* m4_test.cpp — the M4 Board Device: the &FE00 command accumulator, the &FCxx
 * execute-latch into the pending mailbox, the busy handshake, and the &E800
 * response-window overlay served under romdis when the M4 ROM slot is paged
 * in. Proves the deferred-execution protocol end to end (the host executes
 * between latch and complete). See docs/hardware/m4-device.md. */

#include "hw/m4.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "hw/board.h"
#include "hw/memory.h"

namespace {

struct M4Rig {
  std::vector<uint8_t> mmem = std::vector<uint8_t>(mem_state_size());
  std::vector<uint8_t> m4mem = std::vector<uint8_t>(m4_state_size());
  std::vector<uint8_t> rom = std::vector<uint8_t>(0x4000, 0x5A);  // M4 ROM body
  std::vector<uint8_t> basic = std::vector<uint8_t>(0x4000, 0x33);  // onboard
  Board board;
  Device mem;
  Device m4;
};

void make_rig(M4Rig& rig) {
  rig.mem = mem_init(rig.mmem.data());
  rig.m4 = m4_init(rig.m4mem.data());
  board_init(&rig.board);
  board_add(&rig.board, rig.mem);
  board_add(&rig.board, rig.m4);
  board_reset(&rig.board);
  mem_load_upper_rom(&rig.mem, rig.basic.data(), rig.basic.size());  // slot 0
  mem_attach_rom(&rig.mem, 6, rig.rom.data());  // M4 ROM in slot 6
  m4_attach_rom(&rig.m4, rig.rom.data(), rig.rom.size());
  m4_set_plugged(&rig.m4, 1);
}

void idle(M4Rig& rig) {
  rig.board.bus = bus_resting();
  board_tick(&rig.board);
}

void io_write(M4Rig& rig, uint16_t addr, uint8_t val) {
  rig.board.bus = bus_resting();
  rig.board.bus.cpu.iorq = true;
  rig.board.bus.cpu.wr = true;
  rig.board.bus.cpu.addr = addr;
  rig.board.bus.cpu.data = val;
  board_tick(&rig.board);
  idle(rig);
}

// A held ROM read (sample at the end — the T3 discipline the overlay relies
// on).
uint8_t rom_read(M4Rig& rig, uint16_t addr) {
  for (int i = 0; i < 6; ++i) {
    Bus b = rig.board.bus;
    b.cpu.m1 = b.cpu.iorq = b.cpu.wr = false;
    b.cpu.mreq = b.cpu.rd = true;
    b.cpu.addr = addr;
    rig.board.bus = b;
    board_tick(&rig.board);
  }
  const uint8_t v = rig.board.bus.cpu.data;
  idle(rig);
  return v;
}

// Page the M4 ROM in: enable the upper ROM (GA cfg fn2, bit3=0) + select
// slot 6.
void page_m4_rom(M4Rig& rig) {
  io_write(rig, 0x7F00, 0x80);  // GA mode/ROM config: upper ROM enabled
  io_write(rig, 0xDF00, 6);     // upper-ROM select 6
}

}  // namespace

TEST(M4, AccumulateExecuteLatchAndBusy) {
  M4Rig rig;
  make_rig(rig);

  // Send a command frame: [size, cmd_lo, cmd_hi, data...] = 0x4321 with a byte.
  io_write(rig, 0xFE00, 0x01);  // size prefix
  io_write(rig, 0xFE00, 0x21);  // cmd lo
  io_write(rig, 0xFE00, 0x43);  // cmd hi
  io_write(rig, 0xFE00, 0x99);  // one data byte
  M4Regs r{};
  m4_peek(&rig.m4, &r);
  EXPECT_EQ(r.cmd_count, 4) << "four bytes accumulated";
  EXPECT_EQ(r.busy, 0) << "not executing yet";

  io_write(rig, 0xFC00, 0x00);  // execute kick
  m4_peek(&rig.m4, &r);
  EXPECT_EQ(r.busy, 1) << "latched: awaiting the host coprocessor";
  EXPECT_EQ(r.cmd_count, 0) << "the accumulator drained on execute";
  EXPECT_EQ(r.last_cmd, 0x4321);

  // The host drains the frame.
  M4Pending p{};
  ASSERT_EQ(m4_pending_command(&rig.m4, &p), 1);
  EXPECT_EQ(p.cmd, 0x4321);
  EXPECT_EQ(p.len, 4);
  EXPECT_EQ(p.frame[3], 0x99);
  EXPECT_EQ(m4_pending_command(&rig.m4, &p), 0) << "mailbox drained once";
}

// A second command arriving before the host drained the first must be dropped
// WHOLE. The bug this pins: latch_execute returned early on a full mailbox
// without clearing the accumulator, so the dropped frame's bytes stayed put and
// the NEXT frame was built from them plus the new ones. Spliced frames decoded
// to a nonsense opcode, the host answered with an error, and every M4 directory
// listing came back as garbage — reproduced live as 23/52/20-byte frames whose
// size prefix disagreed with their length.
//
// Fails before the fix: the third frame arrives 6 bytes long, carrying the
// undrained command's tail, instead of the 3 bytes actually sent.
// While the coprocessor is working, the WHOLE response window must read as
// not-ready — not just the status byte at &E800. The M4 ROM's ready-poll
// watches the TAIL of the response it expects (the last byte of a directory
// entry). When only &E800 was blanked, a previous same-length response left
// its terminator under the tail address, the poll passed instantly, and the
// ROM copied five stale bytes — "READM" printed where "GAMES" belonged, byte
// pattern captured live on beads-bx36.
//
// Fails before the fix: rom_read(0xE816) returns the STALE previous response
// byte while busy, instead of 0xFF.
TEST(M4, BusyBlanketsTheWholeResponseWindow) {
  M4Rig rig;
  make_rig(rig);
  page_m4_rom(rig);

  // A completed 23-byte response — a directory entry's exact shape.
  uint8_t first[23] = {};
  first[0] = 0x01;
  first[22] = 0x00;  // the tail byte a ready-poll watches
  m4_complete_response(&rig.m4, first, sizeof(first));
  ASSERT_EQ(rom_read(rig, 0xE816), 0x00) << "tail served while idle";

  // The CPC latches the next command: the coprocessor is now working.
  io_write(rig, 0xFE00, 0x02);
  io_write(rig, 0xFE00, 0x06);
  io_write(rig, 0xFE00, 0x43);
  io_write(rig, 0xFC00, 0x00);

  EXPECT_EQ(rom_read(rig, 0xE800), 0xFF) << "status byte while busy";
  EXPECT_EQ(rom_read(rig, 0xE816), 0xFF)
      << "the tail byte still serves the PREVIOUS response while busy — a "
         "same-length reply satisfies the ROM's ready-poll instantly and it "
         "copies stale bytes";
  EXPECT_EQ(rom_read(rig, 0xE80A), 0xFF) << "mid-window while busy";

  // The host answers: the window serves the fresh bytes.
  M4Pending drain{};
  m4_pending_command(&rig.m4, &drain);
  uint8_t second[23] = {};
  second[0] = 0x01;
  second[3] = 0x47;  // 'G'
  m4_complete_response(&rig.m4, second, sizeof(second));
  EXPECT_EQ(rom_read(rig, 0xE800), 0x01);
  EXPECT_EQ(rom_read(rig, 0xE803), 0x47) << "fresh payload after the answer";
}

TEST(M4, AFrameArrivingWhileTheMailboxIsFullIsDroppedWhole) {
  M4Rig rig;
  make_rig(rig);

  // First command: latched, and deliberately NOT drained.
  io_write(rig, 0xFE00, 0x02);
  io_write(rig, 0xFE00, 0x06);
  io_write(rig, 0xFE00, 0x43);
  io_write(rig, 0xFC00, 0x00);

  M4Regs r{};
  m4_peek(&rig.m4, &r);
  ASSERT_EQ(r.busy, 1) << "first frame is latched and awaiting the host";

  // Second command arrives while the mailbox is still full. It cannot be
  // delivered — but it must not poison what comes next either.
  io_write(rig, 0xFE00, 0xAA);
  io_write(rig, 0xFE00, 0xBB);
  io_write(rig, 0xFE00, 0xCC);
  io_write(rig, 0xFC00, 0x00);

  m4_peek(&rig.m4, &r);
  EXPECT_EQ(r.cmd_count, 0)
      << "the dropped frame left bytes behind; the next command will be built "
         "from them and both commands are lost";

  // The host drains the first frame, unchanged.
  M4Pending p{};
  ASSERT_EQ(m4_pending_command(&rig.m4, &p), 1);
  EXPECT_EQ(p.cmd, 0x4306) << "the first frame survived intact";
  EXPECT_EQ(p.len, 3);

  // Now a third command must arrive exactly as sent — 3 bytes, not 6.
  io_write(rig, 0xFE00, 0x02);
  io_write(rig, 0xFE00, 0x09);
  io_write(rig, 0xFE00, 0x43);
  io_write(rig, 0xFC00, 0x00);

  ASSERT_EQ(m4_pending_command(&rig.m4, &p), 1);
  EXPECT_EQ(p.len, 3) << "frame was spliced onto the dropped one's remains";
  EXPECT_EQ(p.cmd, 0x4309) << "decoded a command the CPC never sent";
}

TEST(M4, ResponseWindowOverlaysTheRomWhilePagedIn) {
  M4Rig rig;
  make_rig(rig);
  page_m4_rom(rig);

  // Before any response, &E800 reads the plain ROM body.
  EXPECT_EQ(rom_read(rig, 0xE800), 0x5A) << "plain ROM until a response lands";

  // The host completes a command: bytes appear in the &E800 window.
  const uint8_t resp[5] = {0x00, 0x02, 0x00, 0xAB, 0xCD};  // status,len,data
  m4_complete_response(&rig.m4, resp, 5);
  M4Regs r{};
  m4_peek(&rig.m4, &r);
  EXPECT_EQ(r.busy, 0) << "the coprocessor answered";
  EXPECT_EQ(rom_read(rig, 0xE800), 0x00) << "status byte";
  EXPECT_EQ(rom_read(rig, 0xE803), 0xAB) << "command data at +3";
  EXPECT_EQ(rom_read(rig, 0xE804), 0xCD);
  EXPECT_EQ(rom_read(rig, 0xE900), 0x5A)
      << "past the response length: the ROM shows through";

  // Deselect the M4 ROM (another slot): the overlay vanishes, ROM is normal.
  io_write(rig, 0xDF00, 0);  // select slot 0 (onboard BASIC)
  EXPECT_EQ(rom_read(rig, 0xE803), 0x33)
      << "M4 not paged in: neither the overlay nor the M4 ROM body shows";
}

// Busy sentinel (beads-315e): while a command is latched and the host
// coprocessor hasn't answered, the &E800 status byte reads 0xFF ("not ready").
// The M4 ROM polls it; once the response lands (busy cleared) it reads the real
// status. This takes precedence over any stale prior response in the window.
TEST(M4, BusyStatusByteReadsFfUntilTheResponseLands) {
  M4Rig rig;
  make_rig(rig);
  page_m4_rom(rig);

  // A first command completes so the window holds a stale OK status (0x00).
  io_write(rig, 0xFE00, 0x00);
  io_write(rig, 0xFE00, 0x21);
  io_write(rig, 0xFE00, 0x43);
  io_write(rig, 0xFC00, 0x00);
  M4Pending drain{};
  m4_pending_command(&rig.m4, &drain);
  const uint8_t ok[3] = {0x00, 0x00, 0x00};
  m4_complete_response(&rig.m4, ok, 3);
  EXPECT_EQ(rom_read(rig, 0xE800), 0x00) << "prior command left an OK status";

  // A second command latches: busy rises and the status byte must read 0xFF,
  // masking the stale 0x00 still sitting under response_len.
  io_write(rig, 0xFE00, 0x00);
  io_write(rig, 0xFE00, 0x22);
  io_write(rig, 0xFE00, 0x43);
  io_write(rig, 0xFC00, 0x00);
  M4Regs r{};
  m4_peek(&rig.m4, &r);
  ASSERT_EQ(r.busy, 1) << "the coprocessor is working";
  EXPECT_EQ(rom_read(rig, 0xE800), 0xFF) << "busy sentinel at the status byte";

  // The host answers: busy clears and the real status shows through again.
  m4_pending_command(&rig.m4, &drain);
  m4_complete_response(&rig.m4, ok, 3);
  m4_peek(&rig.m4, &r);
  EXPECT_EQ(r.busy, 0);
  EXPECT_EQ(rom_read(rig, 0xE800), 0x00) << "answer landed: real status";
}

TEST(M4, ConfigWindowOverlaysAtF400) {
  M4Rig rig;
  make_rig(rig);
  page_m4_rom(rig);

  const uint8_t cfg[4] = {0x11, 0x22, 0x33, 0x44};
  m4_write_config(&rig.m4, cfg, 4);
  EXPECT_EQ(rom_read(rig, 0xF400), 0x11);
  EXPECT_EQ(rom_read(rig, 0xF403), 0x44);
  EXPECT_EQ(rom_read(rig, 0xF480), 0x5A) << "past the 128-byte config: ROM";
}

TEST(M4, ResetDrainsProtocolButKeepsWiring) {
  M4Rig rig;
  make_rig(rig);
  io_write(rig, 0xFE00, 0x00);
  io_write(rig, 0xFE00, 0x21);
  io_write(rig, 0xFE00, 0x43);
  io_write(rig, 0xFC00, 0x00);
  const uint8_t resp[2] = {0x00, 0x00};
  m4_complete_response(&rig.m4, resp, 2);

  board_reset(&rig.board);
  M4Regs r{};
  m4_peek(&rig.m4, &r);
  EXPECT_EQ(r.busy, 0);
  EXPECT_EQ(r.cmd_count, 0);
  EXPECT_EQ(r.response_len, 0);
  EXPECT_EQ(r.slot, 6) << "slot persists across reset";
  EXPECT_EQ(r.plugged, 1) << "still plugged";
  EXPECT_EQ(m4_pending_command(&rig.m4, nullptr), 0) << "mailbox cleared";
}
