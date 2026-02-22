/* konCePCja - Amstrad CPC Emulator
   M4 Board - Virtual filesystem expansion

   Command/response protocol via I/O ports:
   - OUT &FE00: accumulate command bytes
   - OUT &FC00: execute accumulated command
   - Response written to ROM overlay at &E800

   Virtual SD card is backed by a host directory.
*/

#ifndef M4BOARD_H
#define M4BOARD_H

#include "types.h"
#include <string>
#include <vector>
#include <cstdio>

struct M4Board {
   bool enabled = false;
   std::string sd_root_path;       // host directory = virtual SD
   std::string current_dir = "/";

   // Command state machine
   std::vector<uint8_t> cmd_buf;   // accumulates OUT bytes
   bool cmd_pending = false;

   // Response buffer (written to ROM overlay at &E800)
   static constexpr int RESPONSE_SIZE = 0x600; // 1.5KB
   uint8_t response[RESPONSE_SIZE] = {};
   int response_len = 0;

   // File handles (up to 4 concurrent)
   FILE* open_files[4] = {};

   // ROM slot and auto-load tracking
   int rom_slot = 7;
   bool rom_auto_loaded = false;   // true if we loaded the ROM (vs. user)
};

extern M4Board g_m4board;

void m4board_reset();
void m4board_cleanup();
void m4board_data_out(byte val);
void m4board_execute();

// Write response into upper ROM overlay memory at given base
void m4board_write_response(byte* rom_base);

// Auto-load M4 ROM into the configured slot (called by emulator_init)
// rom_map: the memmap_ROM[] array, rom_path: CPC.rom_path search directory
void m4board_load_rom(byte** rom_map, const std::string& rom_path);

// Unload M4 ROM if we auto-loaded it (called by emulator_shutdown)
void m4board_unload_rom(byte** rom_map);

#endif
