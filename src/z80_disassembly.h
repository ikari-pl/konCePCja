#pragma once

// Table-driven Z80 disassembler (re-authored 2026-07-13, Gate C). Decodes
// directly off the shared master opcode table in z80_opcode_table.{h,cpp};
// see z80_disassembly.cpp for the architecture note.

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "types.h"

class DisassembledLine {
 public:
  DisassembledLine(word address, uint64_t opcode, std::string&& instruction,
                   int64_t ref_address = -1);

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

DisassembledLine disassemble_one(dword start_address, DisassembledCode& result,
                                 std::vector<dword>& called_points);
DisassembledCode disassemble(const std::vector<word>& entry_points);

// konCePCja debug helpers
int z80_instruction_length(
    word pc);  // disassemble one instruction, return its size in bytes
bool z80_is_call(word pc);           // CALL/CALL cc only — NOT RST
bool z80_is_rst(word pc);            // RST vectors only
bool z80_is_ret(word pc);            // RET/RET cc/RETI/RETN
bool z80_is_indirect_jump(word pc);  // JP (HL)/(IX)/(IY)

// Everything a step walker needs about one instruction, from a SINGLE decode.
// The predicates above each decode independently, so asking three of them
// about the same PC — which the step-out walk does once per retired
// instruction — decoded that instruction three times over.
struct Z80StepClass {
  bool is_call = false;           // CALL / CALL cc
  bool is_rst = false;            // RST vector
  bool is_ret = false;            // RET / RET cc / RETI / RETN
  bool is_indirect_jump = false;  // JP (HL)/(IX)/(IY)
  int length = 1;                 // bytes the instruction consumes
};
Z80StepClass z80_classify_at(word pc);
