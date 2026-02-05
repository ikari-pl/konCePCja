// imgui_ui_testable.h - Extracted pure-logic functions for unit testing
// These functions have no ImGui dependencies and can be tested independently.

#ifndef IMGUI_UI_TESTABLE_H
#define IMGUI_UI_TESTABLE_H

#include "types.h"
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstring>

// ─────────────────────────────────────────────────
// Hex parsing
// ─────────────────────────────────────────────────

// Parse hex string with validation. Returns true if valid, false otherwise.
// On success, *out contains the parsed value. On failure, *out is unchanged.
inline bool parse_hex(const char* str, unsigned long* out, unsigned long max_val)
{
  if (!str || !str[0]) return false;
  char* end;
  unsigned long val = strtoul(str, &end, 16);
  if (*end != '\0' || val > max_val) return false;
  *out = val;
  return true;
}

// ─────────────────────────────────────────────────
// Safe memory read helpers
// ─────────────────────────────────────────────────

// Safe read for unaligned TZX block parsing - returns false if read would overflow
inline bool safe_read_word(byte* p, byte* end, size_t offset, word& out) {
  if (p + offset + sizeof(word) > end) return false;
  memcpy(&out, p + offset, sizeof(word));
  return true;
}

inline bool safe_read_dword(byte* p, byte* end, size_t offset, dword& out) {
  if (p + offset + sizeof(dword) > end) return false;
  memcpy(&out, p + offset, sizeof(dword));
  return true;
}

// ─────────────────────────────────────────────────
// Configuration lookup helpers
// ─────────────────────────────────────────────────

// RAM size options in KB
constexpr unsigned int RAM_SIZES[] = { 64, 128, 192, 256, 320, 576 };
constexpr int RAM_SIZE_COUNT = sizeof(RAM_SIZES) / sizeof(RAM_SIZES[0]);

// Sample rate options in Hz
constexpr unsigned int SAMPLE_RATES[] = { 11025, 22050, 44100, 48000, 96000 };
constexpr int SAMPLE_RATE_COUNT = sizeof(SAMPLE_RATES) / sizeof(SAMPLE_RATES[0]);

// Find index of RAM size in options array, returns 2 (192 KB default) if not found
inline int find_ram_index(unsigned int ram) {
  for (int i = 0; i < RAM_SIZE_COUNT; i++) {
    if (RAM_SIZES[i] == ram) return i;
  }
  return 2; // default to 192 KB
}

// Find index of sample rate in options array, returns 2 (44100 Hz default) if not found
inline int find_sample_rate_index(unsigned int rate) {
  for (int i = 0; i < SAMPLE_RATE_COUNT; i++) {
    if (SAMPLE_RATES[i] == rate) return i;
  }
  return 2; // default to 44100 Hz
}

// ─────────────────────────────────────────────────
// Memory display formatting
// ─────────────────────────────────────────────────

// Format memory line into stack buffer - zero heap allocations
// format: 0 = hex only, 1 = hex + ASCII, 2 = hex + decimal
// Returns number of characters written (excluding null terminator)
// Requires: pbRAM pointer to be valid 64KB memory
inline int format_memory_line(char* buf, size_t buf_size, unsigned int base_addr,
                              int bytes_per_line, int format, const byte* ram)
{
  if (buf_size == 0 || !ram) return 0;

  int offset = 0;
  int remaining = static_cast<int>(buf_size);

  // Helper to safely advance after snprintf
  auto advance = [&](int written) {
    if (written < 0) return false;
    int actual = (written < remaining) ? written : remaining - 1;
    offset += actual;
    remaining -= actual;
    return remaining > 1;
  };

  // Address
  if (!advance(snprintf(buf + offset, remaining, "%04X : ", base_addr & 0xFFFF))) {
    buf[offset] = '\0';
    return offset;
  }

  // Hex bytes
  for (int j = 0; j < bytes_per_line && remaining > 1; j++) {
    if (!advance(snprintf(buf + offset, remaining, "%02X ", ram[(base_addr + j) & 0xFFFF])))
      break;
  }

  // Extended formats
  if (format == 1 && remaining > 1) { // Hex & char
    if (advance(snprintf(buf + offset, remaining, " | "))) {
      for (int j = 0; j < bytes_per_line && remaining > 1; j++) {
        byte b = ram[(base_addr + j) & 0xFFFF];
        buf[offset++] = (b >= 32 && b < 127) ? static_cast<char>(b) : '.';
        remaining--;
      }
    }
  } else if (format == 2 && remaining > 1) { // Hex & u8
    if (advance(snprintf(buf + offset, remaining, " | "))) {
      for (int j = 0; j < bytes_per_line && remaining > 1; j++) {
        if (!advance(snprintf(buf + offset, remaining, "%3u ", ram[(base_addr + j) & 0xFFFF])))
          break;
      }
    }
  }

  buf[offset] = '\0';
  return offset;
}

#endif // IMGUI_UI_TESTABLE_H
