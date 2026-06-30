/* z80_smoke_test.cpp — Round 1 standalone conformance check.
 *
 * Drives the z80core.h contract through a flat-RAM bus and verifies the Round 1
 * skeleton: reset state, cycle-stepped fetch timing, PC advance, and HALT.
 *
 * Standalone (no gtest dep yet) so each round is provably green from a one-line
 * compile. Wiring into the project test_runner comes when the build integrates
 * the new core (Phase 1).
 *
 *   c++ -std=c++17 -Wall -Wextra -I src/z80core \
 *       test/z80_conformance/z80_smoke_test.cpp src/z80core/z80_cpp.cpp -o /tmp/z80smoke
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "z80core.h"

namespace {

struct FlatMem {
  uint8_t ram[0x10000] = {};
  uint64_t ticks = 0;
};

uint8_t mem_read(void* ctx, uint16_t addr) {
  return static_cast<FlatMem*>(ctx)->ram[addr];
}
void mem_write(void* ctx, uint16_t addr, uint8_t val) {
  static_cast<FlatMem*>(ctx)->ram[addr] = val;
}
uint8_t io_read(void*, uint16_t) { return 0xFF; }
void io_write(void*, uint16_t, uint8_t) {}
void on_tick(void* ctx, int tstates) {
  static_cast<FlatMem*>(ctx)->ticks += static_cast<uint64_t>(tstates);
}

int failures = 0;
void check(bool ok, const char* what) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++failures;
}

}  // namespace

int main() {
  FlatMem mem;
  // Program: NOP ; NOP ; HALT
  mem.ram[0] = 0x00;
  mem.ram[1] = 0x00;
  mem.ram[2] = 0x76;

  z80core_bus bus{&mem, mem_read, mem_write, io_read, io_write, on_tick};
  z80core_state* cpu = z80core_create(&bus);

  check(std::strcmp(z80core_impl_name(), "cpp-cleanroom") == 0, "impl name is cpp-cleanroom");

  z80core_regs r;
  z80core_snapshot(cpu, &r);
  check(r.pc == 0x0000, "reset: PC = 0x0000");
  check(r.af == 0xFFFF, "reset: AF = 0xFFFF");
  check(r.sp == 0xFFFF, "reset: SP = 0xFFFF");
  check(r.im == 0 && r.iff1 == 0 && r.iff2 == 0, "reset: IM 0, interrupts off");
  check(r.halted == 0, "reset: not halted");

  int t1 = z80core_step(cpu);  // NOP
  z80core_snapshot(cpu, &r);
  check(t1 == 4, "NOP takes 4 T-states");
  check(r.pc == 0x0001, "NOP: PC advanced to 1");
  check(r.r == 0x01, "NOP: R refreshed to 1");

  z80core_step(cpu);  // NOP
  z80core_snapshot(cpu, &r);
  check(r.pc == 0x0002, "second NOP: PC = 2");

  z80core_step(cpu);  // HALT
  z80core_snapshot(cpu, &r);
  check(r.halted == 1, "HALT sets halted");
  check(r.pc == 0x0003, "HALT: PC past the opcode (3)");

  uint16_t pc_before = r.pc;
  uint8_t r_before = r.r;
  int th = z80core_step(cpu);  // halted: internal NOP
  z80core_snapshot(cpu, &r);
  check(th == 4, "halted step burns 4 T-states");
  check(r.pc == pc_before, "halted: PC frozen");
  check(r.r == static_cast<uint8_t>((r_before & 0x80) | ((r_before + 1) & 0x7F)),
        "halted: R still refreshes");

  check(r.tstates == mem.ticks, "tick callback total matches tstates counter");

  // snapshot/restore round-trip
  z80core_regs saved = r;
  z80core_set_reg(cpu, Z80CORE_BC, 0x1234);
  z80core_restore(cpu, &saved);
  check(z80core_get_reg(cpu, Z80CORE_BC) == saved.bc, "restore overwrites a poked register");

  z80core_destroy(cpu);

  std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILURES",
              failures, failures == 1 ? "" : "s");
  return failures == 0 ? 0 : 1;
}
