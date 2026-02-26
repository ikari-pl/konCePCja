#ifndef Z80_OPCODE_TABLE_H
#define Z80_OPCODE_TABLE_H

#include "types.h"
#include <string>
#include <vector>
#include <unordered_map>

// Prefix group for the opcode
enum class OpcodePrefix : byte {
    NONE = 0,   // base opcodes (0x00-0xFF)
    CB,         // CB xx
    ED,         // ED xx
    DD,         // DD xx (IX)
    FD,         // FD xx (IY)
    DDCB,       // DD CB disp xx
    FDCB,       // FD CB disp xx
};

struct Z80Opcode {
    OpcodePrefix prefix;
    byte opcode;            // the final opcode byte
    const char* mnemonic;   // template e.g. "LD A,*", "BIT 7,(IX+*)"
    byte length;            // total instruction length in bytes
    byte operand_bytes;     // 0, 1 (*), or 2 (**)
    byte t_states;          // base cycle count (T-states)
    byte t_states_extra;    // extra cycles when condition taken (0 if N/A)
    bool is_relative;       // true for JR/DJNZ (operand is PC-relative)
};

// The master table
extern const Z80Opcode g_z80_opcodes[];
extern const int g_z80_opcode_count;

// ── Lookup functions ──

// Disassembler direction: prefix+opcode → entry pointer (or nullptr)
const Z80Opcode* z80_opcode_lookup(OpcodePrefix prefix, byte opcode);

// Assembler direction: normalized mnemonic pattern → matching entries
const std::vector<const Z80Opcode*>& z80_asm_lookup(const std::string& pattern);

// Initialize lookup tables (call once at startup)
void z80_opcode_table_init();

// Check if a word is a known Z80 mnemonic keyword (e.g. "LD", "NOP", "BIT")
bool z80_is_mnemonic_keyword(const std::string& word);

// Generate z80_opcodes.txt format from the master table
std::string z80_opcode_table_to_txt();

// Produce the combined key used by the old disassembler's map (prefix bytes << 8 | opcode)
int z80_opcode_to_legacy_key(const Z80Opcode& op);

#endif // Z80_OPCODE_TABLE_H
