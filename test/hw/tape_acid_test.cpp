/* tape_acid_test.cpp — the tape parity acid test: a CDT synthesized in the
 * CPC firmware's OWN cassette block format (SOFT968 / CPCWiki "Cassette data
 * information", verified in docs/hardware/tape-device.md §6) is loaded and
 * RUN through the real firmware on the sub-cycle machine. The firmware's CAS
 * READ routine is the referee: it waits out its motor spin-up delay,
 * calibrates on the leader, demands the 0x2C/0x16 sync bytes, and verifies a
 * complemented CRC-16 per 256-byte segment. Passing means the deck's pulse
 * timing, bit order and block sequencing are right end to end — the first
 * full peripheral parity proof of the replacement ledger.
 * Skipped without rom/cpc6128.rom. */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "subcycle/machine.h"

namespace {

std::vector<uint8_t> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
}

/* ---- the firmware cassette format, synthesized -------------------------- */

// CRC-16, polynomial X^16+X^12+X^5+1, preset 0xFFFF, result complemented —
// exactly the check CAS READ runs over each 256-byte segment.
uint16_t cas_crc16(const uint8_t* p, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; ++i) {
    crc = static_cast<uint16_t>(crc ^ (static_cast<uint16_t>(p[i]) << 8));
    for (int b = 0; b < 8; ++b)
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                           : static_cast<uint16_t>(crc << 1);
  }
  return static_cast<uint16_t>(~crc);
}

void put16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(static_cast<uint8_t>(x & 0xFF));
  v.push_back(static_cast<uint8_t>(x >> 8));
}

// One firmware record as a CDT turbo block (0x11): leader of 2048 one-bits,
// a single zero start bit (the block's sync pulses), the sync byte, then the
// payload in 256-byte segments each followed by its CRC (high byte first),
// and a trailer of 32 one-bits. Timings are SPEED WRITE 1 (2000 baud):
// half-a-zero 583 T-states at the CDT's 3.5MHz time base, ones twice that.
void append_record(std::vector<uint8_t>& cdt, uint8_t sync_byte,
                   const std::vector<uint8_t>& payload, uint8_t pad_byte,
                   uint16_t pause_ms) {
  const uint16_t kZero = 583, kOne = 1166;

  std::vector<uint8_t> data;
  data.push_back(sync_byte);
  for (size_t off = 0; off < payload.size(); off += 256) {
    uint8_t seg[256];
    for (size_t i = 0; i < 256; ++i)
      seg[i] = (off + i < payload.size()) ? payload[off + i] : pad_byte;
    data.insert(data.end(), seg, seg + 256);
    uint16_t crc = cas_crc16(seg, 256);
    data.push_back(static_cast<uint8_t>(crc >> 8));  // high byte first
    data.push_back(static_cast<uint8_t>(crc & 0xFF));
  }
  for (int i = 0; i < 4; ++i) data.push_back(0xFF);  // trailer: 32 one-bits

  cdt.push_back(0x11);
  put16(cdt, kOne);   // pilot pulse = a one-bit half-wave
  put16(cdt, kZero);  // sync pulses = the single zero start bit
  put16(cdt, kZero);
  put16(cdt, kZero);  // zero-bit pulse
  put16(cdt, kOne);   // one-bit pulse
  put16(cdt, 4096);   // pilot tone: 2048 one-bits = 4096 pulses
  cdt.push_back(8);   // all bits of the last byte used
  put16(cdt, pause_ms);
  cdt.push_back(static_cast<uint8_t>(data.size() & 0xFF));
  cdt.push_back(static_cast<uint8_t>((data.size() >> 8) & 0xFF));
  cdt.push_back(static_cast<uint8_t>((data.size() >> 16) & 0xFF));
  cdt.insert(cdt.end(), data.begin(), data.end());
}

// A complete one-block file: blank-tape lead-in, header record (sync 0x2C,
// 64-byte header padded into one segment), then its data record (sync 0x16).
std::vector<uint8_t> make_firmware_cdt(const char* name, uint8_t file_type,
                                       const std::vector<uint8_t>& body) {
  std::vector<uint8_t> hdr(64, 0);
  const size_t name_len = strlen(name);
  for (size_t i = 0; i < 16; ++i)
    hdr[i] = (i < name_len) ? static_cast<uint8_t>(name[i]) : ' ';
  hdr[16] = 1;     // block number
  hdr[17] = 0xFF;  // last block
  hdr[18] = file_type;
  hdr[19] = static_cast<uint8_t>(body.size() & 0xFF);  // block data length
  hdr[20] = static_cast<uint8_t>(body.size() >> 8);
  hdr[21] = 0x70;  // data location 0x0170 (BASIC's program area)
  hdr[22] = 0x01;
  hdr[23] = 0xFF;     // first block
  hdr[24] = hdr[19];  // logical length
  hdr[25] = hdr[20];

  std::vector<uint8_t> cdt = {'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1A, 1, 20};
  // A real SAVE leaves ~2s of blank tape before the leader: CAS MOTOR ON
  // waits for the tape to come up to speed (the 6128 OS delay loop at
  // &2BE2, ~200 ticks) before writing OR reading. Without this silence the
  // reader is still in its spin-up wait while the leader plays past.
  cdt.push_back(0x20);  // pause block: 2500 ms of blank tape
  cdt.push_back(2500 & 0xFF);
  cdt.push_back(2500 >> 8);
  append_record(cdt, 0x2C, hdr, 0x00, 600);
  append_record(cdt, 0x16, body, 0x1A, 2000);
  return cdt;
}

/* ---- machine-side helpers ----------------------------------------------- */

// Tap one packed (row << 4 | bit) key: hold across scans, release with a gap.
void tap(subcycle::Machine& m, uint8_t code) {
  m.key(code, true);
  for (int i = 0; i < 4; ++i) m.run_frame();
  m.key(code, false);
  for (int i = 0; i < 4; ++i) m.run_frame();
}

void tap_shifted(subcycle::Machine& m, uint8_t code) {
  m.key(0x25, true);  // SHIFT
  for (int i = 0; i < 2; ++i) m.run_frame();
  tap(m, code);
  m.key(0x25, false);
  for (int i = 0; i < 2; ++i) m.run_frame();
}

std::string g_text;
subcycle::Machine* g_machine = nullptr;
void collect_txt(void*, uint16_t) {
  if (g_machine != nullptr)
    g_text += static_cast<char>(g_machine->regs().af >> 8);
}

}  // namespace

TEST(TapeAcid, FirmwareLoadsAndRunsAnAsciiProgramFromTheDeck) {
  std::vector<uint8_t> rom = read_file("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read_file("../rom/cpc6128.rom");
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";

  // An ASCII BASIC listing: type 0x16 files are read line by line and
  // tokenized by BASIC itself — no hand-rolled tokens to get wrong.
  const char* listing = "10 PRINT \"ACID TEST OK\"\r\n";
  std::vector<uint8_t> body(listing, listing + strlen(listing));
  std::vector<uint8_t> cdt = make_firmware_cdt("ACID", 0x16, body);

  subcycle::Machine m;
  ASSERT_TRUE(m.build(rom.data(), rom.size()));
  g_machine = &m;
  g_text.clear();
  // BASIC prints through the TXT_OUTPUT jumpblock (&BB5A); a tap there
  // collects the program's output. (The cassette manager's own messages —
  // "Press PLAY...", "Loading ACID block 1" — are printed by the lower ROM
  // through its internal routine and never cross the RAM jumpblock.)
  ASSERT_TRUE(m.add_tap(0xBB5A, collect_txt, nullptr));

  for (int i = 0; i < 150; ++i) m.run_frame();  // to the Ready screen
  ASSERT_TRUE(m.insert_tape(cdt.data(), cdt.size()));
  m.tape_play_button(true);

  // RUN" <RETURN> — no AMSDOS ROM on this board, so cassette is the default
  // filesystem and the firmware prompts "Press PLAY then any key".
  tap(m, 0x62);          // R
  tap(m, 0x52);          // U
  tap(m, 0x56);          // N
  tap_shifted(m, 0x81);  // " (SHIFT+2)
  tap(m, 0x22);          // RETURN
  for (int i = 0; i < 25; ++i) m.run_frame();
  tap(m, 0x57);  // SPACE = "any key"

  // 3s of blank lead-in plus two 2000-baud records is ~9s of CPC time; give
  // it slack and stop the moment the program's own output appears.
  // While the firmware reads, the deck must drive the live cassette wires the
  // engine=1 bridge mirrors back to the host tape scope/SFX
  // (subcycle_bridge.cpp reads Machine::tape_motor / tape_read_level each
  // frame). If either froze, the status-bar waveform, the procedural hiss, and
  // the auto-armed data monitor would all go dark under engine=1 — the
  // regression those accessors fix.
  bool ran = false;
  bool saw_motor = false, saw_rdata_toggle = false;
  const bool rd0 = m.tape_read_level();
  std::vector<uint8_t>
      tape_bits;           // decoded bits the engine=1 BITS scope drains
  uint32_t max_block = 0;  // the deck's own block ordinal, high-water
  for (int i = 0; i < 900 && !ran; ++i) {
    m.run_frame();
    if (m.tape_motor()) saw_motor = true;
    if (m.tape_read_level() != rd0) saw_rdata_toggle = true;
    uint8_t bit_buf[256];
    const int nb =
        m.tape_drain_bits(bit_buf, static_cast<int>(sizeof(bit_buf)));
    tape_bits.insert(tape_bits.end(), bit_buf, bit_buf + nb);
    TapeRegs tr{};
    tape_peek(m.tape(), &tr);
    if (tr.block > max_block) max_block = tr.block;
    ran = g_text.find("ACID TEST OK") != std::string::npos;
  }
  g_machine = nullptr;

  EXPECT_TRUE(ran) << "the loaded program printed through TXT_OUTPUT; got: "
                   << g_text.substr(g_text.size() > 400 ? g_text.size() - 400
                                                        : 0);
  EXPECT_TRUE(saw_motor)
      << "Machine::tape_motor() never latched during a firmware tape read — "
         "the "
         "engine=1 bridge scope/SFX gate (CPC.tape_motor) would stay frozen";
  EXPECT_TRUE(saw_rdata_toggle)
      << "Machine::tape_read_level() never changed — the engine=1 bridge tape "
         "scope (bTapeLevel) would be a flat line";

  // Machine::tape_drain_bits feeds the scope's decoded-BITS view under engine=1
  // (the legacy tape.cpp writer never runs here). Prove the drained bits are
  // the REAL decoded data, not noise: reconstructing bytes at the right bit
  // alignment must contain the loaded program text. (Bits dropped before the
  // drain steadied only shift the alignment, so try all 8.)
  EXPECT_FALSE(tape_bits.empty()) << "no decoded tape bits were drained";
  bool found_program = false;
  for (int align = 0; align < 8 && !found_program; ++align) {
    std::string bytes;
    for (size_t i = align; i + 8 <= tape_bits.size(); i += 8) {
      uint8_t v = 0;
      for (int b = 0; b < 8; ++b)
        v = static_cast<uint8_t>((v << 1) | (tape_bits[i + b] & 1));
      bytes.push_back(static_cast<char>(v));
    }
    if (bytes.find("PRINT") != std::string::npos) found_program = true;
  }
  EXPECT_TRUE(found_program)
      << "decoded tape bits didn't reconstruct the program text ("
      << tape_bits.size() << " bits drained)";

  // The deck's block ordinal must advance as it plays through the tape's blocks
  // (pause + two records here). The engine=1 tape UI drives its block counter
  // and Prev/Next enable-state off this ordinal; if it stayed 0 the counter
  // would freeze and Prev would gray out mid-tape.
  EXPECT_GT(max_block, 0u)
      << "deck block ordinal never advanced past 0 — the engine=1 tape counter "
         "and Prev/Next buttons would stay stuck at block 0";
}

// Machine::tape_seek is the deck primitive the engine=1 Prev/Next block buttons
// drive (via the bridge). It walks the deck's OWN cdt to the Nth block by size
// (NOT a legacy pbTapeImage byte offset, whose header-stripped layout differs),
// and ignores out-of-range ordinals. The acid CDT here has 3 blocks: a pause
// (0x20), the header record (0x11) and the data record (0x11).
TEST(TapeSeek, DeckWalksToBlockOrdinalAndIgnoresOutOfRange) {
  std::vector<uint8_t> rom = read_file("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read_file("../rom/cpc6128.rom");
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";

  std::vector<uint8_t> body(64, 0x2A);
  std::vector<uint8_t> cdt = make_firmware_cdt("SEEK", 0x16, body);

  subcycle::Machine m;
  ASSERT_TRUE(m.build(rom.data(), rom.size()));
  ASSERT_TRUE(m.insert_tape(cdt.data(), cdt.size()));

  TapeRegs tr{};
  tape_peek(m.tape(), &tr);
  EXPECT_EQ(tr.pos,
            10u);  // attach rewinds to just past "ZXTape!\x1A" + version
  EXPECT_EQ(tr.block, 0u);

  // Jump to the data record (block 2): the walk lands on that block's header,
  // well past the pause + header record.
  m.tape_seek(2);
  tape_peek(m.tape(), &tr);
  EXPECT_EQ(tr.block, 2u);
  const uint32_t pos_block2 = tr.pos;
  EXPECT_GT(pos_block2, 10u);

  // Back to the header record (block 1): a distinct, smaller position.
  m.tape_seek(1);
  tape_peek(m.tape(), &tr);
  EXPECT_EQ(tr.block, 1u);
  EXPECT_GT(tr.pos, 10u);
  EXPECT_LT(tr.pos, pos_block2);

  // Out of range (only 3 blocks): ignored, position unchanged.
  const uint32_t blk_before = tr.block, pos_before = tr.pos;
  m.tape_seek(99);
  tape_peek(m.tape(), &tr);
  EXPECT_EQ(tr.block, blk_before);
  EXPECT_EQ(tr.pos, pos_before);
}
