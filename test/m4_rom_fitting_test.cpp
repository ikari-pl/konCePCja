// konCePCja — the M4 board ROM must be findable, and findable by ONE lookup.
//
// WHY THIS FILE EXISTS
//
// The host loader searched rom_path and resources/roms for "m4board.rom" or
// "M4ROM.BIN". The sub-cycle bridge kept its OWN list, and that list named
// "m4.rom" — a filename this project has never shipped. So b.m4rom stayed
// empty, attach_m4_rom() and set_m4(true) never ran, and the machine had no M4
// in it. Everything else agreed that it did: the config said m4board=1, the
// ROMs dialog showed slot 6 occupied, and `rom info 6` reported a CRC. The only
// visible symptoms were the ones a user hits — no RSX commands registered, and
// drive A stayed the default.
//
// Two hand-kept lists that must agree is the same failure this project keeps
// paying for. These tests pin the lookup and pin the fact that there is only
// one of it.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "m4board.h"

namespace {

std::string source_dir() {
#ifdef KONCPC_SOURCE_DIR
  return KONCPC_SOURCE_DIR;
#else
  return ".";
#endif
}

std::string read_source_file(const std::string& relative) {
  std::ifstream f(source_dir() + "/" + relative);
  std::stringstream buf;
  buf << f.rdbuf();
  return buf.str();
}

}  // namespace

// The ROM this project ships must be reachable through the resolver. If the
// file is renamed or moved without updating the search, this fails instead of
// the M4 silently not existing at run time.
TEST(M4RomFitting, ResolverFindsTheShippedRom) {
  const std::string resources = source_dir() + "/resources";
  const std::string found = m4board_find_rom(source_dir() + "/rom", resources);

  ASSERT_FALSE(found.empty())
      << "no M4 ROM found under " << source_dir() + "/rom" << " or "
      << resources + "/roms";
  EXPECT_TRUE(std::filesystem::exists(found)) << found;
  EXPECT_GE(std::filesystem::file_size(found), 0x4000u)
      << "an M4 ROM smaller than 16K is rejected by attach_m4_rom, so the "
         "Device would silently not be fitted";
}

TEST(M4RomFitting, ResolverReportsAbsenceRatherThanGuessing) {
  EXPECT_EQ(m4board_find_rom("/nonexistent/roms", "/nonexistent/resources"),
            "");
}

// The regression guard proper, and it has moved: the bridge must not load this
// ROM AT ALL. m4board_load_rom() patches a boot stage into the image at 0x3800
// (the shipped file is 0xFF there) and that stage is what registers the RSX
// commands, so any second read of the file hands the machine an unpatched ROM.
// The bridge must fit the host-prepared image out of memmap_ROM instead.
//
// Fails on the pre-fix tree, which read the file itself under a name this
// project does not ship.
TEST(M4RomFitting, TheBridgeFitsTheHostPreparedImage) {
  const std::string bridge = read_source_file("src/subcycle_bridge.cpp");
  ASSERT_FALSE(bridge.empty()) << "could not read src/subcycle_bridge.cpp";

  const size_t m4_block = bridge.find("g_m4board.enabled && !b.m4_loaded");
  ASSERT_NE(m4_block, std::string::npos) << "M4 fitting block not found";
  const std::string block = bridge.substr(m4_block, 1400);

  EXPECT_NE(block.find("memmap_ROM[g_m4board.rom_slot]"), std::string::npos)
      << "the bridge must fit the image the host prepared, not its own read";
  EXPECT_EQ(block.find("read_file"), std::string::npos)
      << "the bridge is reading an M4 ROM file again; a fresh read misses the "
         "boot stage m4board_load_rom patches in, and the M4 goes inert";
  EXPECT_EQ(bridge.find("\"/m4.rom\""), std::string::npos)
      << "the bridge is hardcoding an M4 ROM filename again";
}

// The patched boot stage is the whole point: prove the shipped file does NOT
// already contain it, so a future "just read the file" refactor cannot look
// harmless.
TEST(M4RomFitting, TheShippedRomHasNoBootStageOfItsOwn) {
  const std::string rom =
      m4board_find_rom(source_dir() + "/rom", source_dir() + "/resources");
  ASSERT_FALSE(rom.empty());

  std::ifstream f(rom, std::ios::binary);
  ASSERT_TRUE(f) << rom;
  std::string bytes((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
  ASSERT_GE(bytes.size(), 0x3810u);

  EXPECT_EQ(bytes.find("Emulated M4"), std::string::npos)
      << "the banner is patched in by m4board_load_rom, not shipped";
  bool all_blank = true;
  for (size_t i = 0x3800; i < 0x3810; i++) {
    if (static_cast<unsigned char>(bytes[i]) != 0xFF) all_blank = false;
  }
  EXPECT_TRUE(all_blank)
      << "0x3800 is the boot-stage area the host patches; if the file now "
         "carries its own code, revisit how the ROM is prepared";
}

// Same trap, one file over: the host loader must not re-grow its own lookup.
// Whichever side drifts, the M4 goes quietly missing. A filename may still
// appear in a log message or a comment — that is prose, not a second search;
// what must not reappear is another filesystem probe built from the ROM paths.
TEST(M4RomFitting, TheHostLoaderHasOnlyOneRomSearch) {
  const std::string host = read_source_file("src/m4board.cpp");
  ASSERT_FALSE(host.empty()) << "could not read src/m4board.cpp";

  const size_t resolver = host.find("std::string m4board_find_rom(");
  ASSERT_NE(resolver, std::string::npos) << "resolver definition not found";
  const size_t resolver_end = host.find("\n}", resolver);
  ASSERT_NE(resolver_end, std::string::npos);

  // Walk the file outside the resolver; flag any line that both names an M4
  // ROM file and probes for it.
  size_t line_start = 0;
  int offenders = 0;
  std::string first;
  while (line_start < host.size()) {
    const size_t line_end = host.find('\n', line_start);
    const std::string line = host.substr(
        line_start, line_end == std::string::npos ? std::string::npos
                                                  : line_end - line_start);
    const bool inside_resolver =
        line_start > resolver && line_start < resolver_end;
    const bool names_rom = line.find("m4board.rom") != std::string::npos ||
                           line.find("M4ROM.BIN") != std::string::npos;
    const bool probes = line.find("filesystem::exists") != std::string::npos ||
                        line.find("read_file") != std::string::npos ||
                        line.find("fopen") != std::string::npos;
    if (!inside_resolver && names_rom && probes) {
      if (offenders++ == 0) first = line;
    }
    if (line_end == std::string::npos) break;
    line_start = line_end + 1;
  }
  EXPECT_EQ(offenders, 0)
      << "a second M4 ROM lookup has appeared outside m4board_find_rom(): "
      << first;
}
