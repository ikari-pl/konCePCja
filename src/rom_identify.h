#ifndef ROM_IDENTIFY_H
#define ROM_IDENTIFY_H

#include "types.h"
#include <string>
#include <zlib.h>

// Known CPC ROM CRC32 → human-readable name.
// CRC32 is computed over the 16KB ROM data as loaded into memory.
struct KnownROM {
  dword crc32;
  const char* name;
};

static const KnownROM known_roms[] = {
  // ── System ROMs (OS) ──
  { 0x815752DF, "CPC 464 OS" },
  { 0x3F5A6DC4, "CPC 664 OS" },
  { 0x0219BB74, "CPC 6128 OS" },
  { 0x7F9AB3F7, "KC Compact OS" },

  // ── BASIC ROMs ──
  { 0x7D9A3BAC, "BASIC 1.0 (464)" },
  { 0x32FEE492, "BASIC 1.0 (664)" },
  { 0xCA6AF63D, "BASIC 1.1 (6128)" },

  // ── International variants (MAME-verified) ──
  // 6128 French: combined 0x1574923B → lo/hi split
  // 6128 Spanish: combined 0x588B5540 → lo/hi split
  // 6128+ / 464+: combined 0x2FA2E7D6 → lo/hi split

  // ── DOS ROMs ──
  { 0x1FE22ECD, "AMSDOS 0.5" },
  { 0xF3329AA8, "ParaDOS" },
  { 0x17445B99, "ParaDOS 1.2" },
  { 0x8FC90139, "ParaDOS 1.2+" },
  { 0x4AFF7C0A, "ParaDOS 1.2 (patched)" },  // raw CRC (with dist header)
  { 0x61EEBAD3, "ParaDOS 1.2 (patched)" },  // stripped CRC
  { 0x5700A5A7, "UniDOS" },                  // raw CRC (with dist header)
  { 0xBD745AB7, "UniDOS" },                  // stripped CRC
  { 0x623798C8, "UniTools" },                // raw CRC
  { 0x5D0F7F60, "UniTools" },                // stripped CRC

  // ── Assemblers / Dev tools ──
  { 0x7347E22D, "OrgAMS" },                  // raw CRC
  { 0x14863104, "OrgAMS" },                  // stripped CRC
  { 0xB75DCB5A, "OrgAMS Extension" },        // raw CRC
  { 0x380208B2, "OrgAMS Extension" },        // stripped CRC
  { 0xB9446948, "MonoGAMS" },                // raw CRC
  { 0xC4DC8A79, "MonoGAMS" },                // stripped CRC

  // ── Networking / Hardware ──
  { 0x20BA103F, "Nova" },                    // raw CRC
  { 0x14428C42, "Nova" },                    // stripped CRC
  { 0xB1E34D0F, "Albireo" },                 // raw CRC
  { 0xE269E682, "Albireo" },                 // stripped CRC

  // ── Utilities ──
  { 0x5A37F457, "BricBrac" },                // raw CRC
  { 0x0D67F2D4, "BricBrac" },                // stripped CRC

  // ── Multiface ──
  { 0xF36086DE, "Multiface II" },
};

// Look up a ROM by CRC32.  Returns nullptr if not found.
inline const char* rom_identify_by_crc32(dword crc) {
  for (const auto& r : known_roms) {
    if (r.crc32 == crc) return r.name;
  }
  return nullptr;
}

// Extract the ROM's self-reported name from its RSX name table.
// CPC expansion ROMs store a name table pointer at bytes 4-5 (address in
// &C000-based ROM space).  The first entry is the ROM's own name, with
// bit 7 set on the last character.
// Returns empty string if the ROM has no valid name.
inline std::string rom_extract_header_name(const byte* rom_data) {
  if (!rom_data) return "";

  // Byte 0: ROM type (0=foreground, 1=background, 2=extension)
  byte rom_type = rom_data[0];
  if (rom_type > 2) return "";  // not a valid CPC ROM

  // Bytes 4-5: name table address (little-endian, &C000-based)
  word name_table_addr = rom_data[4] | (rom_data[5] << 8);
  if (name_table_addr < 0xC000) return "";

  int offset = name_table_addr - 0xC000;
  if (offset < 0 || offset >= 16384 - 1) return "";

  // Read the first RSX name (the ROM's own name)
  std::string name;
  for (int i = offset; i < 16384 && i < offset + 32; i++) {
    byte b = rom_data[i];
    if (b == 0) break;
    char ch = static_cast<char>(b & 0x7F);
    if (ch >= 0x20 && ch < 0x7F) name += ch;
    if (b & 0x80) break;  // bit 7 set = last character
  }

  // Trim trailing spaces
  while (!name.empty() && name.back() == ' ') name.pop_back();
  return name;
}

// Identify a ROM: CRC32 lookup first, then fall back to header name.
// rom_data must point to 16384 bytes of loaded ROM data.
inline std::string rom_identify(const byte* rom_data) {
  if (!rom_data) return "";

  // Compute CRC32
  uLong crc = crc32(0L, nullptr, 0);
  crc = crc32(crc, rom_data, 16384);

  // Try CRC32 lookup
  const char* known = rom_identify_by_crc32(static_cast<dword>(crc));
  if (known) return known;

  // Fall back to ROM header name
  return rom_extract_header_name(rom_data);
}

#endif // ROM_IDENTIFY_H
