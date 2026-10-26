#pragma once

// Brings the sub-cycle board up on a synthetic 32K system ROM for one test
// and tears it down again, so no other test sees an active bridge. The ROM is
// `code` at &0000 and zeros after it; the model is pinned to the 6128
// (chROMFile[2] == "cpc6128.rom").

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "autotype.h"
#include "koncepcja.h"
#include "subcycle_bridge.h"

extern t_CPC CPC;

class TestBridgeRom {
 public:
  explicit TestBridgeRom(const std::vector<unsigned char>& code)
      : dir_(std::filesystem::temp_directory_path() /
             "koncepcja-test-bridge-rom") {
    std::filesystem::create_directories(dir_);
    std::vector<unsigned char> rom(0x8000, 0);
    for (size_t i = 0; i < code.size() && i < rom.size(); ++i) rom[i] = code[i];
    std::filesystem::path const rom_file = dir_ / "cpc6128.rom";
    FILE* f = fopen(rom_file.string().c_str(), "wb");
    if (f == nullptr) return;
    bool const written = fwrite(rom.data(), 1, rom.size(), f) == rom.size();
    if (fclose(f) != 0 || !written) return;
    saved_rom_path_ = CPC.rom_path;
    saved_model_ = CPC.model;
    saved_ram_ = CPC.ram_size;
    saved_limit_ = CPC.limit_speed;
    CPC.rom_path = dir_.string();
    CPC.model = 2;
    CPC.ram_size = 128;
    CPC.limit_speed = 0;
    saved_ = true;
    // A queue another (shuffled) test left behind would be typed, by the
    // frames this test runs, through CPC.InputMapper, which the test binary
    // never builds.
    g_autotype_queue.clear();
    started_ = subcycle_bridge_start();
  }
  ~TestBridgeRom() {
    if (started_) subcycle_bridge_stop();
    g_autotype_queue.clear();
    if (saved_) {
      CPC.rom_path = saved_rom_path_;
      CPC.model = saved_model_;
      CPC.ram_size = saved_ram_;
      CPC.limit_speed = saved_limit_;
    }
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  TestBridgeRom(const TestBridgeRom&) = delete;
  TestBridgeRom& operator=(const TestBridgeRom&) = delete;

  bool started() const { return started_; }

 private:
  std::filesystem::path dir_;
  std::string saved_rom_path_;
  unsigned int saved_model_ = 0;
  unsigned int saved_ram_ = 0;
  unsigned int saved_limit_ = 0;
  bool saved_ = false;
  bool started_ = false;
};

// Paints the border bright red through the Gate Array (pen 16, hardware
// colour &4C), programs the CRTC with the firmware's 50 Hz screen (R0-R9),
// then DI : JR $. A frame of it costs well under a millisecond, and every
// frame after the first is the same picture.
inline std::vector<unsigned char> test_rom_red_border() {
  return {
      0xF3,              // DI
      0x01, 0x10, 0x7F,  // LD BC,&7F10   select pen 16 (border)
      0xED, 0x49,        // OUT (C),C
      0x0E, 0x4C,        // LD C,&4C      bright red
      0xED, 0x49,        // OUT (C),C
      0x21, 0x21, 0x00,  // LD HL,&0021   the CRTC table below
      0x16, 0x00,        // LD D,0
      0x06, 0xBC,        // loop: LD B,&BC
      0xED, 0x51,        // OUT (C),D     register number
      0x7E, 0x23,        // LD A,(HL) : INC HL
      0x06, 0xBD,        // LD B,&BD
      0xED, 0x79,        // OUT (C),A     register value
      0x14, 0x7A,        // INC D : LD A,D
      0xFE, 0x0A,        // CP 10
      0x20, 0xF0,        // JR NZ,loop
      0x18, 0xFE,        // JR $
      63,   40,   46,   0x8E, 38, 0, 25, 30, 0, 7,
  };
}
