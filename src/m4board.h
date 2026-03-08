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

struct t_drive;

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
   int rom_slot = 6;
   bool rom_auto_loaded = false;   // true if we loaded the ROM (vs. user)

   // Config buffer — mirrors ROM data area at offset 0x3400
   // Populated by C_CONFIG commands from the M4 ROM init code
   static constexpr int CONFIG_SIZE = 128;
   uint8_t config_buf[CONFIG_SIZE] = {};

   // Directory listing state — the ROM calls C_READDIR once per entry
   struct DirEntry {
      std::string name;     // original filename
      bool is_dir;
      uint32_t size;        // file size in bytes
   };
   std::vector<DirEntry> dir_entries;
   size_t dir_index = 0;

   // ── Container browsing (cd into DSK/CPR files) ──
   // The real M4 firmware lets users |cd,"game.dsk" to browse a DSK image
   // as a virtual directory. Files inside can be listed and loaded.
   enum class ContainerType { NONE, DSK };
   ContainerType container_type = ContainerType::NONE;
   std::string container_host_path;  // host path of the opened container file
   std::string container_parent_dir; // current_dir before entering the container
   t_drive* container_drive = nullptr; // parsed DSK image (heap-allocated)

   // ── TCP socket bridging (v1.0.9+ protocol) ──
   // The real M4 firmware exposes raw TCP sockets via the ESP8266.
   // We bridge these to host sockets so CPC software (IRC clients, etc.)
   // can make real TCP connections through the emulator.
   static constexpr int MAX_SOCKETS = 4;
#ifdef _WIN32
   using socket_t = uintptr_t; // SOCKET is UINT_PTR on Windows
   static constexpr socket_t INVALID_SOCK = ~static_cast<socket_t>(0);
#else
   using socket_t = int;
   static constexpr socket_t INVALID_SOCK = -1;
#endif
   socket_t sockets[MAX_SOCKETS] = {INVALID_SOCK, INVALID_SOCK, INVALID_SOCK, INVALID_SOCK};

   // Activity tracking for UI display
   int activity_frames = 0;       // countdown timer for LED (frames at 50fps)
   enum class LastOp { NONE, READ, WRITE, DIR, CMD } last_op = LastOp::NONE;
   std::string last_filename;     // last opened file (for display)
   int cmd_count = 0;             // total commands processed
};

extern M4Board g_m4board;

void m4board_reset();
void m4board_cleanup();
void m4board_data_out(byte val);
void m4board_execute();

// Write response into upper ROM overlay memory at given base
void m4board_write_response(byte* rom_base);

// Auto-load M4 ROM into the configured slot (called by emulator_init)
// rom_map: the memmap_ROM[] array, rom_path: CPC.rom_path, resources_path: CPC.resources_path
void m4board_load_rom(byte** rom_map, const std::string& rom_path, const std::string& resources_path);

// Unload M4 ROM if we auto-loaded it (called by emulator_shutdown)
void m4board_unload_rom(byte** rom_map);

// I/O dispatch registration
void m4board_register_io();

#endif
