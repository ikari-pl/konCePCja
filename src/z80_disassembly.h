#ifndef Z80_DISASSEMBLY_H
#define Z80_DISASSEMBLY_H

#include "types.h"
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

class DisassembledLine {
  public:
    DisassembledLine(word address, uint64_t opcode, std::string&& instruction, int64_t ref_address = -1);

    int Size() const;

    friend bool operator<(const DisassembledLine& l, const DisassembledLine& r);
    friend bool operator==(const DisassembledLine& l, const DisassembledLine& r);

    word address_;
    uint64_t opcode_;
    std::string instruction_;
    word ref_address_ = 0;
    std::string ref_address_string_;
};

std::ostream& operator<<(std::ostream& os, const DisassembledLine& line);

class DisassembledCode {
  public:
    DisassembledCode() = default;

    std::optional<DisassembledLine> LineAt(word address) const;

    uint64_t hash() const;

    std::set<DisassembledLine> lines;
};

std::ostream& operator<<(std::ostream& os, const DisassembledCode& code);

DisassembledLine disassemble_one(dword pos, DisassembledCode& result, std::vector<dword>& entry_points);
DisassembledCode disassemble(const std::vector<word>& entry_points);

// konCePCja debug helpers
int z80_instruction_length(word pc);    // disassemble one instruction, return its size in bytes
bool z80_is_call_or_rst(word pc);       // true for CALL/CALL cc/RST opcodes

#endif
