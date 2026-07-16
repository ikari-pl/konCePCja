/* state_roundtrip_test.cpp — Gate B1 (beads-yjpy) state-completeness guard.
 *
 * device.h:38 requires every Device's save()/load() to serialize LOGICAL state
 * only: pointer-free, deterministic, and a faithful round-trip. The deep
 * differential harness (Gate B3) and any future "drop legacy" proof both stand
 * on this. This test drives each device to a non-trivial state and asserts two
 * properties that, together, catch every known failure class:
 *
 *   1. Round-trip idempotence (expect_roundtrip): a mutation must change the
 *      blob (so a stub save() or empty load() cannot pass), and load(save(x))
 *      must reproduce save(x).  -> catches the z80/video "version byte only"
 *      stubs.
 *   2. Instance-independence (expect_instance_independent): two independently
 *      allocated devices driven through the SAME operations must save to
 *      BYTE-IDENTICAL blobs. The two save buffers are pre-filled with different
 *      canaries, so this also fails on a short write.  -> catches raw host
 *      pointers (mem/video wiring), uninitialised padding, and under-writes.
 *
 * Adding a device to B1: give it a make() + a deterministic mutate(), register
 * a TEST here; a still-broken save() shows up red until B1 completes it.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

#include "hw/amdrum.h"
#include "hw/amx.h"
#include "hw/asic.h"
#include "hw/board.h"
#include "hw/buses.h"
#include "hw/crtc.h"
#include "hw/device.h"
#include "hw/fdc.h"
#include "hw/gate_array.h"
#include "hw/m4.h"
#include "hw/memory.h"
#include "hw/mf2.h"
#include "hw/ppi.h"
#include "hw/printer.h"
#include "hw/probe.h"
#include "hw/psg.h"
#include "hw/smartwatch.h"
#include "hw/symbiface.h"
#include "hw/tape.h"
#include "hw/video.h"
#include "hw/z80.h"

namespace {

std::vector<uint8_t> save_blob(const Device& d) {
  std::vector<uint8_t> b(d.state_size(d.self), 0);
  d.save(d.self, b.data());
  return b;
}

// A mutation must be observable in the blob, and load() must be its inverse.
void expect_roundtrip(const Device& d, const std::function<void()>& mutate) {
  const std::vector<uint8_t> before = save_blob(d);
  mutate();
  const std::vector<uint8_t> after = save_blob(d);
  ASSERT_NE(before, after)
      << d.name << ": mutation left the blob unchanged — save() ignores state";
  d.load(d.self, before.data());
  EXPECT_EQ(save_blob(d), before)
      << d.name << ": load(initial blob) did not restore the initial state";
  d.load(d.self, after.data());
  EXPECT_EQ(save_blob(d), after) << d.name << ": load(save(x)) != x";
}

// Two independent instances, identical history → byte-identical blob. Different
// canary prefills (0x00 vs 0xFF) also make a short write diverge.
void expect_instance_independent(const Device& a, const Device& b) {
  std::vector<uint8_t> ba(a.state_size(a.self), 0x00);
  std::vector<uint8_t> bb(b.state_size(b.self), 0xFF);
  ASSERT_EQ(ba.size(), bb.size())
      << a.name << ": state_size differs by instance";
  a.save(a.self, ba.data());
  b.save(b.self, bb.data());
  EXPECT_EQ(ba, bb) << a.name
                    << ": two instances differ — a raw pointer, uninitialised "
                       "padding, or a short write leaked into save()";
}

/* --- Z80: distinctive architectural state via z80_poke. --- */

Z80Regs distinctive_regs() {
  Z80Regs r{};
  r.af = 0x1234;
  r.bc = 0x5678;
  r.de = 0x9abc;
  r.hl = 0xdef0;
  r.af_ = 0x0f0f;
  r.bc_ = 0xf0f0;
  r.de_ = 0x1111;
  r.hl_ = 0x2222;
  r.ix = 0x3333;
  r.iy = 0x4444;
  r.sp = 0xfffe;
  r.pc = 0xa000;
  r.wz = 0x0bad;
  r.i = 0x7e;
  r.r = 0x39;
  r.im = 2;
  r.iff1 = 1;
  r.iff2 = 1;
  r.q = 0x55;
  r.halted = 0;
  r.tstates = 123456;
  r.instr_count = 789;
  return r;
}

TEST(StateRoundtrip, Z80RoundTrips) {
  std::vector<uint8_t> mem(z80_state_size(), 0);
  Device d = z80_init(mem.data());
  expect_roundtrip(d, [&] {
    Z80Regs r = distinctive_regs();
    z80_poke(&d, &r);
  });
}

TEST(StateRoundtrip, Z80InstanceIndependent) {
  std::vector<uint8_t> ma(z80_state_size(), 0), mb(z80_state_size(), 0);
  Device a = z80_init(ma.data());
  Device b = z80_init(mb.data());
  Z80Regs r = distinctive_regs();
  z80_poke(&a, &r);
  z80_poke(&b, &r);
  expect_instance_independent(a, b);
}

/* --- Video: mutate the beam via crafted hsync/vsync edges (no phase 13/15, so
 *     no RAM fetch / pixel render — keeps beam_row's negative back-porch out of
 *     the framebuffer index). The framebuffer + GA pointers are wiring and must
 *     NOT appear in the blob — the instance-independence test (different fb/GA
 *     addresses, identical blob) is the proof they're excluded. --- */

struct VideoRig {
  std::vector<uint8_t> ga_mem = std::vector<uint8_t>(ga_state_size(), 0);
  std::vector<uint8_t> vid_mem = std::vector<uint8_t>(video_state_size(), 0);
  std::vector<uint8_t> fb = std::vector<uint8_t>(16 * 8 * 3, 0);
  Device ga = ga_init(ga_mem.data());
  Device vid = video_init(vid_mem.data());
  VideoRig() { video_attach(&vid, &ga, fb.data(), 16, 8); }
};

void drive(const Device& vid, bool vs, bool hs) {
  Bus in{};
  Bus out{};
  in.vid.vsync = vs;
  in.vid.hsync = hs;
  vid.tick(vid.self, &in, &out);
}

void mutate_beam(const Device& vid) {
  drive(vid, /*vs=*/true,
        /*hs=*/false);  // vsync rise → frames++, beam_row reset
  for (int i = 0; i < 5; ++i) {
    drive(vid, false, /*hs=*/true);   // hsync rise → beam_row++
    drive(vid, false, /*hs=*/false);  // hsync fall → beam_col reset + refresh
  }
}

TEST(StateRoundtrip, VideoRoundTrips) {
  VideoRig rig;
  expect_roundtrip(rig.vid, [&] { mutate_beam(rig.vid); });
}

TEST(StateRoundtrip, VideoInstanceIndependentExcludesWiring) {
  VideoRig a;
  VideoRig b;  // distinct fb / GA / backing addresses
  mutate_beam(a.vid);
  mutate_beam(b.vid);
  expect_instance_independent(a.vid, b.vid);
}

/* --- Memory: base RAM + banking config (logical, in-struct) AND the expansion
 *     RAM contents (logical, but living behind a caller-owned pointer). The
 *     expansion/roms/cart/asic pointers themselves are wiring and must be
 *     excluded — proven by the instance-independence case (distinct expansion
 *     buffer addresses, identical blob). --- */

struct MemRig {
  std::vector<uint8_t> mem = std::vector<uint8_t>(mem_state_size(), 0);
  std::vector<uint8_t> exp =
      std::vector<uint8_t>(128 * 1024, 0);  // 2×64K banks
  Board board;
  Device dev = mem_init(mem.data());
  MemRig() {
    board_init(&board);
    board_add(&board, dev);
    board_reset(&board);
    mem_attach_expansion(&dev, exp.data(), exp.size());
  }
  void wr(uint16_t addr, uint8_t val) {  // CPU RAM write (two-phase commit)
    board.bus = bus_resting();
    board.bus.cpu.mreq = true;
    board.bus.cpu.wr = true;
    board.bus.cpu.addr = addr;
    board.bus.cpu.data = val;
    board_tick(&board);
    board.bus = bus_resting();
    board_tick(&board);  // commit cycle
  }
  void io(uint8_t data) {  // PAL RAM-banking latch write (&7Fxx, 11xxxxxx)
    board.bus = bus_resting();
    board.bus.cpu.iorq = true;
    board.bus.cpu.wr = true;
    board.bus.cpu.addr = 0x7F00;
    board.bus.cpu.data = data;
    board_tick(&board);
  }
};

void mutate_mem(MemRig& rig) {
  rig.wr(0x8000, 0xA5);
  rig.wr(0x4000, 0x3C);
  rig.io(0xC4);  // ram_config latch
  for (size_t i = 0; i < rig.exp.size(); i += 997)
    rig.exp[i] = static_cast<uint8_t>(i);  // pattern into the expansion RAM
}

TEST(StateRoundtrip, MemoryRoundTripsIncludingExpansionRam) {
  MemRig rig;
  expect_roundtrip(rig.dev, [&] { mutate_mem(rig); });
}

TEST(StateRoundtrip, MemoryInstanceIndependentExcludesWiring) {
  MemRig a;
  MemRig b;  // distinct expansion-buffer / backing addresses
  mutate_mem(a);
  mutate_mem(b);
  expect_instance_independent(a.dev, b.dev);
}

// The expansion RAM lives behind a pointer; before B1 its contents were never
// serialised (only the pointer was). Wipe the live buffer and prove load()
// repaints the pattern from the blob — the check the generic cases can't make.
TEST(StateRoundtrip, MemoryExpansionRamContentsSurviveLoad) {
  MemRig rig;
  mutate_mem(rig);  // fills the expansion RAM with a pattern
  const std::vector<uint8_t> blob = save_blob(rig.dev);
  std::fill(rig.exp.begin(), rig.exp.end(), 0);  // wipe the live expansion
  rig.dev.load(rig.dev.self, blob.data());
  for (size_t i = 0; i < rig.exp.size(); i += 997)
    ASSERT_EQ(rig.exp[i], static_cast<uint8_t>(i))
        << "expansion byte " << i << " not restored by load()";
}

/* --- CRTC: register file + internal beam/sync counters (logical), plus an
 *     embedded ASIC Device* that is wiring. Each rig attaches its OWN asic
 *     (distinct address) so the instance-independence case proves the pointer
 * is excluded — and keeps crtc_tick's guarded asic deref safe. crtc_save also
 *     used to overflow its blob by the version byte (dev_state_size lacked the
 *     +1); state_size / dev_state_size were swapped. --- */

struct CrtcRig {
  std::vector<uint8_t> asic_mem = std::vector<uint8_t>(asic_state_size(), 0);
  std::vector<uint8_t> mem = std::vector<uint8_t>(crtc_state_size(), 0);
  Board board;
  Device asic = asic_init(asic_mem.data());
  Device dev = crtc_init(mem.data());
  CrtcRig() {
    board_init(&board);
    board_add(&board, dev);
    board_reset(&board);
    crtc_attach_asic(&dev, &asic);  // distinct asic address per instance
  }
  void tick_crtc(int n) {
    for (int i = 0; i < n; ++i) {
      board.bus = bus_resting();
      board.bus.clk.crtc = true;
      board_tick(&board);
    }
  }
};

void mutate_crtc(CrtcRig& rig) {
  static const uint8_t regs[10] = {63, 40, 46, 0x8E, 38, 0, 25, 30, 0, 7};
  for (uint8_t i = 0; i < 10; ++i) crtc_poke_reg(&rig.dev, i, regs[i]);
  rig.tick_crtc(200);  // advance the beam / hsync / vsync counters
}

TEST(StateRoundtrip, CrtcRoundTrips) {
  CrtcRig rig;
  expect_roundtrip(rig.dev, [&] { mutate_crtc(rig); });
}

TEST(StateRoundtrip, CrtcInstanceIndependentExcludesAsicPointer) {
  CrtcRig a;
  CrtcRig b;  // distinct asic Device addresses
  mutate_crtc(a);
  mutate_crtc(b);
  expect_instance_independent(a.dev, b.dev);
}

/* --- Gate Array: a pointer-free POD device whose save() is a whole-struct
 *     memcpy. Registering it validates that the memcpy path is deterministic
 *     (no uninitialised padding diverges between two zero-backed instances) —
 *     the property the B3 differential harness relies on for every POD chip.
 * --- */

struct GaRig {
  std::vector<uint8_t> mem = std::vector<uint8_t>(ga_state_size(), 0);
  std::vector<uint8_t> asic_mem = std::vector<uint8_t>(asic_state_size(), 0);
  Board board;
  Device dev = ga_init(mem.data());
  Device asic = asic_init(asic_mem.data());
  GaRig() {
    board_init(&board);
    board_add(&board, dev);
    board_reset(&board);
    // Attach a live, per-instance ASIC so g->asic is a real heap pointer — the
    // board wiring ga_save must EXCLUDE from the blob. Without it both rigs
    // carry a null asic, so the instance-independence check cannot see a leaked
    // pointer (the leak the differential harness caught that this test missed).
    ga_attach_asic(&dev, &asic);
  }
  void out(uint8_t data) {  // GA port &7Fxx write
    board.bus = bus_resting();
    board.bus.cpu.iorq = true;
    board.bus.cpu.wr = true;
    board.bus.cpu.addr = 0x7F00;
    board.bus.cpu.data = data;
    board_tick(&board);
  }
  void hsync_pulses(int n) {  // advance the raster-interrupt line counter
    for (int i = 0; i < n; ++i) {
      board.bus = bus_resting();
      board.bus.vid.hsync = true;
      board_tick(&board);
      board.bus = bus_resting();
      board.bus.vid.hsync = false;
      board_tick(&board);
    }
  }
};

void mutate_ga(GaRig& rig) {
  rig.out(0x00);         // select pen 0
  rig.out(0x40 | 0x1A);  //   ink
  rig.out(0x05);         // select pen 5
  rig.out(0x40 | 0x03);  //   ink
  rig.out(0x10);         // select border
  rig.out(0x40 | 0x0B);  //   border ink
  rig.out(0x89);         // RMR: screen mode + ROM enables + interrupt control
  rig.hsync_pulses(40);  // move the interrupt counter off its reset value
}

TEST(StateRoundtrip, GateArrayRoundTrips) {
  GaRig rig;
  expect_roundtrip(rig.dev, [&] { mutate_ga(rig); });
}

TEST(StateRoundtrip, GateArrayInstanceIndependent) {
  GaRig a;
  GaRig b;
  mutate_ga(a);
  mutate_ga(b);
  expect_instance_independent(a.dev, b.dev);
}

/* --- Reusable single-device board rig for the I/O-driven peripherals. --- */

struct IoRig {
  std::vector<uint8_t> mem;
  Board board;
  Device dev;
  IoRig(size_t backing, Device (*init)(void*)) : mem(backing, 0) {
    dev = init(mem.data());
    board_init(&board);
    board_add(&board, dev);
    board_reset(&board);
  }
  void io_write(uint16_t addr, uint8_t val) {
    board.bus = bus_resting();
    board.bus.cpu.iorq = true;
    board.bus.cpu.wr = true;
    board.bus.cpu.addr = addr;
    board.bus.cpu.data = val;
    board_tick(&board);
    board.bus = bus_resting();
    board_tick(&board);  // owned-access edge settles a cycle later
  }
};

/* --- Probe (ICE bus probe): pointer-free whole-struct save with the version
 *     byte added in B1. Mutated via its comparator-table API. --- */

void mutate_probe(const Device& dev) {
  probe_add_exec(&dev, 0x0038);
  probe_add_exec(&dev, 0x8000);
  probe_add_watch(&dev, 0xC000, 1, /*on_read=*/1, /*on_write=*/0);
}

TEST(StateRoundtrip, ProbeRoundTrips) {
  std::vector<uint8_t> mem(probe_state_size(), 0);
  Device d = probe_init(mem.data());
  expect_roundtrip(d, [&] { mutate_probe(d); });
}

TEST(StateRoundtrip, ProbeInstanceIndependent) {
  std::vector<uint8_t> ma(probe_state_size(), 0), mb(probe_state_size(), 0);
  Device a = probe_init(ma.data());
  Device b = probe_init(mb.data());
  mutate_probe(a);
  mutate_probe(b);
  expect_instance_independent(a, b);
}

/* --- PPI 8255: register file mutated via the four I/O ports (&F4xx-&F7xx). ---
 */

void mutate_ppi(IoRig& rig) {
  rig.io_write(0xF700, 0x82);  // control: port A output, B input
  rig.io_write(0xF400, 0x5A);  // port A data
  rig.io_write(0xF600, 0x3C);  // port C data
}

TEST(StateRoundtrip, PpiRoundTrips) {
  IoRig rig(ppi_state_size(), ppi_init);
  expect_roundtrip(rig.dev, [&] { mutate_ppi(rig); });
}

TEST(StateRoundtrip, PpiInstanceIndependent) {
  IoRig a(ppi_state_size(), ppi_init);
  IoRig b(ppi_state_size(), ppi_init);
  mutate_ppi(a);
  mutate_ppi(b);
  expect_instance_independent(a.dev, b.dev);
}

/* --- Printer (Digiblaster DAC): kSaveBytes prefix (latch/access_prev/now); the
 *     strobe-edge event ring after the boundary is telemetry, excluded. --- */

void mutate_printer(IoRig& rig) {
  rig.io_write(0xEF7E, 0x41);         // 'A' present (A12 low → selected)
  rig.io_write(0xEF7E, 0x41 | 0x80);  // strobe: falling edge clocks it
  rig.io_write(0xEF7E, 0x42);         // 'B' present
}

TEST(StateRoundtrip, PrinterRoundTrips) {
  IoRig rig(printer_state_size(), printer_init);
  expect_roundtrip(rig.dev, [&] { mutate_printer(rig); });
}

TEST(StateRoundtrip, PrinterInstanceIndependent) {
  IoRig a(printer_state_size(), printer_init);
  IoRig b(printer_state_size(), printer_init);
  mutate_printer(a);
  mutate_printer(b);
  expect_instance_independent(a.dev, b.dev);
}

/* --- AmDrum (Cheetah DAC on &FFxx): plugged flag + DAC latch. --- */

void mutate_amdrum(IoRig& rig) {
  amdrum_set_plugged(&rig.dev, 1);
  rig.io_write(0xFF12, 0x20);  // any &FFxx decodes
  rig.io_write(0xFF34, 0x77);
}

TEST(StateRoundtrip, AmdrumRoundTrips) {
  IoRig rig(amdrum_state_size(), amdrum_init);
  expect_roundtrip(rig.dev, [&] { mutate_amdrum(rig); });
}

TEST(StateRoundtrip, AmdrumInstanceIndependent) {
  IoRig a(amdrum_state_size(), amdrum_init);
  IoRig b(amdrum_state_size(), amdrum_init);
  mutate_amdrum(a);
  mutate_amdrum(b);
  expect_instance_independent(a.dev, b.dev);
}

/* --- Devices mutated purely through their public setter API (no bus traffic):
 *     one helper runs both checks. --- */

void check_device(size_t backing, Device (*init)(void*),
                  const std::function<void(const Device&)>& mutate) {
  std::vector<uint8_t> mem(backing, 0);
  Device d = init(mem.data());
  expect_roundtrip(d, [&] { mutate(d); });

  std::vector<uint8_t> ma(backing, 0), mb(backing, 0);
  Device a = init(ma.data());
  Device b = init(mb.data());
  mutate(a);
  mutate(b);
  expect_instance_independent(a, b);
}

// Smartwatch (DS1216 phantom RTC): plugged flag + latched host time.
TEST(StateRoundtrip, Smartwatch) {
  check_device(smartwatch_state_size(), smartwatch_init, [](const Device& d) {
    smartwatch_set_plugged(&d, 1);
    const uint8_t bcd[8] = {0x00, 0x30, 0x12, 0x05, 0x07, 0x07, 0x26, 0x01};
    smartwatch_set_time(&d, bcd);
  });
}

// Symbiface II: plugged + the RTC register file (before the img[] wiring).
TEST(StateRoundtrip, Symbiface) {
  check_device(sf2_state_size(), sf2_init, [](const Device& d) {
    sf2_set_plugged(&d, 1);
    const uint8_t regs[10] = {0x30, 0x00, 0x45, 0x00, 0x12,
                              0x00, 0x02, 0x07, 0x07, 0x26};
    sf2_rtc_set_time(&d, regs);
  });
}

// Multiface II: plugged + a freeze session (frozen/nmi_hold/active).
TEST(StateRoundtrip, Mf2) {
  check_device(mf2_state_size(), mf2_init, [](const Device& d) {
    mf2_set_plugged(&d, 1);
    mf2_stop(&d);
  });
}

// M4 board: plugged + ROM slot (both in the serialized prefix, before `rom`).
TEST(StateRoundtrip, M4) {
  check_device(m4_state_size(), m4_init, [](const Device& d) {
    m4_set_plugged(&d, 1);
    m4_set_slot(&d, 3);
  });
}

// AMX mouse: pointer-free POD; the plugged flag is enough to exercise the
// memcpy path (padding determinism is the property under test for a POD
// device).
TEST(StateRoundtrip, Amx) {
  check_device(amx_state_size(), amx_init,
               [](const Device& d) { amx_set_plugged(&d, 1); });
}

// PSG AY-3-8912: the 14 register file + selection latch + keyboard-scan row.
TEST(StateRoundtrip, Psg) {
  check_device(psg_state_size(), psg_init, [](const Device& d) {
    for (uint8_t n = 0; n < 14; ++n)
      psg_poke_reg(&d, n, static_cast<uint8_t>(0x11 * n + 3));
    psg_poke_sel(&d, 7);
    psg_set_key_row(&d, 3, 0x5A);
  });
}

/* --- FDC uPD765A: controller state mutated via the motor latch (&FA7E). --- */

TEST(StateRoundtrip, FdcRoundTrips) {
  IoRig rig(fdc_state_size(), fdc_init);
  expect_roundtrip(rig.dev, [&] { rig.io_write(0xFA7E, 0x0D); });
}

TEST(StateRoundtrip, FdcInstanceIndependent) {
  IoRig a(fdc_state_size(), fdc_init);
  IoRig b(fdc_state_size(), fdc_init);
  a.io_write(0xFA7E, 0x0D);
  b.io_write(0xFA7E, 0x0D);
  expect_instance_independent(a.dev, b.dev);
}

/* --- ASIC (6128+): the 17-byte unlock knock advances the lock state machine.
 * --- */

void mutate_asic(IoRig& rig) {
  asic_set_plugged(&rig.dev, 1);
  const uint8_t seq[17] = {0xFF, 0x00, 0xFF, 0x77, 0xB3, 0x51, 0xA8, 0xD4, 0x62,
                           0x39, 0x9C, 0x46, 0x2B, 0x15, 0x8A, 0xCD, 0x01};
  for (uint8_t v : seq)
    rig.io_write(0xBC00, v);  // &BC00: where the knock lands
}

TEST(StateRoundtrip, AsicRoundTrips) {
  IoRig rig(asic_state_size(), asic_init);
  expect_roundtrip(rig.dev, [&] { mutate_asic(rig); });
}

TEST(StateRoundtrip, AsicInstanceIndependent) {
  IoRig a(asic_state_size(), asic_init);
  IoRig b(asic_state_size(), asic_init);
  mutate_asic(a);
  mutate_asic(b);
  expect_instance_independent(a.dev, b.dev);
}

/* --- Tape deck: attach a minimal CDT and play it, advancing the playback
 * cursor (the serialized prefix). The CDT image is wiring behind the `cdt`
 * pointer; the instance-independence case gives each deck its OWN CDT buffer
 * (distinct address) to prove that pointer is excluded. --- */

std::vector<uint8_t> minimal_cdt() {
  std::vector<uint8_t> c = {'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1a, 1, 20};
  const std::vector<uint8_t> payload(16, 0xA5);
  c.push_back(0x10);  // standard-speed data block
  c.push_back(100);
  c.push_back(0);  // pause ms
  c.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
  c.push_back(static_cast<uint8_t>(payload.size() >> 8));
  c.insert(c.end(), payload.begin(), payload.end());
  return c;
}

void play_tape(const Device& d, const std::vector<uint8_t>& cdt) {
  tape_attach_cdt(&d, cdt.data(), cdt.size());
  tape_play(&d, 1);
  Bus in = bus_resting();
  in.tape.motor = true;
  for (int i = 0; i < 4000; ++i) {
    Bus out = bus_resting();
    d.tick(d.self, &in, &out);
  }
}

TEST(StateRoundtrip, TapeRoundTrips) {
  std::vector<uint8_t> mem(tape_state_size(), 0);
  Device d = tape_init(mem.data());
  const std::vector<uint8_t> cdt = minimal_cdt();
  expect_roundtrip(d, [&] { play_tape(d, cdt); });
}

TEST(StateRoundtrip, TapeInstanceIndependentExcludesCdtPointer) {
  std::vector<uint8_t> ma(tape_state_size(), 0), mb(tape_state_size(), 0);
  Device a = tape_init(ma.data());
  Device b = tape_init(mb.data());
  const std::vector<uint8_t> ca = minimal_cdt(),
                             cb = minimal_cdt();  // distinct
  play_tape(a, ca);
  play_tape(b, cb);
  expect_instance_independent(a, b);
}

}  // namespace
