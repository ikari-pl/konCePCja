#pragma once

// konCePCja — fixed-width integer aliases for hardware-facing code.
//
// byte/word/dword/qword mirror the 8/16/32/64-bit widths of the emulated
// machine's buses and registers. Prefer these in device- and bus-level code;
// plain <cstdint> types are fine everywhere else.

#include <cstdint>

using byte = std::uint8_t;
using word = std::uint16_t;
using dword = std::uint32_t;
using qword = std::uint64_t;

static_assert(sizeof(byte) == 1 && sizeof(word) == 2 && sizeof(dword) == 4 &&
                  sizeof(qword) == 8,
              "fixed-width aliases must match their bus widths");
