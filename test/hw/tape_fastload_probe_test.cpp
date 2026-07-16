/* tape_fastload_probe_test.cpp — SCRATCH investigation (delete after use).
 * Loads a real firmware CDT and histograms the Z80 PC while the tape motor is
 * on, to LOCATE the lower-ROM cassette-read timing loop — the candidate hook
 * point for a fast-load shortcut. Prints the top PCs; not an assertion test.
 * Reuses tape_acid_test's firmware-CDT synthesis. Skipped without cpc6128.rom.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "subcycle/machine.h"

namespace {

std::vector<uint8_t> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
}

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
    data.push_back(static_cast<uint8_t>(crc >> 8));
    data.push_back(static_cast<uint8_t>(crc & 0xFF));
  }
  for (int i = 0; i < 4; ++i) data.push_back(0xFF);
  cdt.push_back(0x11);
  put16(cdt, kOne);
  put16(cdt, kZero);
  put16(cdt, kZero);
  put16(cdt, kZero);
  put16(cdt, kOne);
  put16(cdt, 4096);
  cdt.push_back(8);
  put16(cdt, pause_ms);
  cdt.push_back(static_cast<uint8_t>(data.size() & 0xFF));
  cdt.push_back(static_cast<uint8_t>((data.size() >> 8) & 0xFF));
  cdt.push_back(static_cast<uint8_t>((data.size() >> 16) & 0xFF));
  cdt.insert(cdt.end(), data.begin(), data.end());
}
std::vector<uint8_t> make_firmware_cdt(const char* name, uint8_t file_type,
                                       const std::vector<uint8_t>& body) {
  std::vector<uint8_t> hdr(64, 0);
  const size_t name_len = strlen(name);
  for (size_t i = 0; i < 16; ++i)
    hdr[i] = (i < name_len) ? static_cast<uint8_t>(name[i]) : ' ';
  hdr[16] = 1;
  hdr[17] = 0xFF;
  hdr[18] = file_type;
  hdr[19] = static_cast<uint8_t>(body.size() & 0xFF);
  hdr[20] = static_cast<uint8_t>(body.size() >> 8);
  hdr[21] = 0x70;
  hdr[22] = 0x01;
  hdr[23] = 0xFF;
  hdr[24] = hdr[19];
  hdr[25] = hdr[20];
  std::vector<uint8_t> cdt = {'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1A, 1, 20};
  cdt.push_back(0x20);
  cdt.push_back(2500 & 0xFF);
  cdt.push_back(2500 >> 8);
  append_record(cdt, 0x2C, hdr, 0x00, 600);
  append_record(cdt, 0x16, body, 0x1A, 2000);
  return cdt;
}

void tap(subcycle::Machine& m, uint8_t code) {
  m.key(code, true);
  for (int i = 0; i < 4; ++i) m.run_frame();
  m.key(code, false);
  for (int i = 0; i < 4; ++i) m.run_frame();
}
void tap_shifted(subcycle::Machine& m, uint8_t code) {
  m.key(0x25, true);
  for (int i = 0; i < 2; ++i) m.run_frame();
  tap(m, code);
  m.key(0x25, false);
  for (int i = 0; i < 2; ++i) m.run_frame();
}

std::map<uint16_t, uint64_t>* g_hist = nullptr;
bool g_hist_on = false;
void histogram(void*, const Z80Regs* r) {
  if (g_hist_on && g_hist) (*g_hist)[r->pc]++;
}

}  // namespace

TEST(TapeFastloadProbe, LocatesTheCassetteReadTimingLoop) {
  std::vector<uint8_t> rom = read_file("rom/cpc6128.rom");
  if (rom.size() < 0x8000) rom = read_file("../rom/cpc6128.rom");
  if (rom.size() < 0x8000) GTEST_SKIP() << "rom/cpc6128.rom not found";

  const char* listing = "10 PRINT \"FASTLOAD PROBE\"\r\n";
  std::vector<uint8_t> body(listing, listing + strlen(listing));
  std::vector<uint8_t> cdt = make_firmware_cdt("PROBE", 0x16, body);

  subcycle::Machine m;
  ASSERT_TRUE(m.build(rom.data(), rom.size()));
  std::map<uint16_t, uint64_t> hist;
  g_hist = &hist;
  m.set_instr_hook(histogram, nullptr);

  for (int i = 0; i < 150; ++i) m.run_frame();
  ASSERT_TRUE(m.insert_tape(cdt.data(), cdt.size()));
  m.tape_play_button(true);
  tap(m, 0x62);          // R
  tap(m, 0x52);          // U
  tap(m, 0x56);          // N
  tap_shifted(m, 0x81);  // "
  tap(m, 0x22);          // RETURN
  for (int i = 0; i < 25; ++i) m.run_frame();
  tap(m, 0x57);  // SPACE = "any key"

  // Histogram PCs only while the motor is actually reading.
  uint64_t motor_frames = 0;
  for (int i = 0; i < 500; ++i) {
    g_hist_on = m.tape_motor();
    if (g_hist_on) motor_frames++;
    m.run_frame();
  }
  g_hist_on = false;
  m.set_instr_hook(nullptr, nullptr);
  g_hist = nullptr;

  std::vector<std::pair<uint16_t, uint64_t>> top(hist.begin(), hist.end());
  std::sort(top.begin(), top.end(),
            [](auto& a, auto& b) { return a.second > b.second; });
  printf("\n=== PC histogram during motor-on (%llu frames) ===\n",
         static_cast<unsigned long long>(motor_frames));
  uint64_t total = 0;
  for (auto& kv : top) total += kv.second;
  for (int i = 0; i < 30 && i < static_cast<int>(top.size()); ++i)
    printf("  PC=%04X  %10llu  (%.1f%%)\n", top[i].first,
           static_cast<unsigned long long>(top[i].second),
           total ? 100.0 * top[i].second / total : 0.0);
  fflush(stdout);
  EXPECT_GT(motor_frames, 0u) << "motor never turned on — no read to probe";
}
