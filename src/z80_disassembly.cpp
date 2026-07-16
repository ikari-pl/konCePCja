// Z80 disassembler.
//
// RE-AUTHORED 2026-07-13 (Gate C cutover): rewritten from the inherited
// Caprice32 decode ladder into a table-driven disassembler that reads directly
// from the shared master opcode table (z80_opcode_table.{h,cpp}) — the same
// single source of truth the assembler consumes. There is no second opcode
// table and no per-call std::map rebuild anymore: decode is an O(1) lookup and
// a single formatting pass into stack buffers.
//
// The prefix stream is decoded explicitly (NONE / CB / ED / DD / FD / DDCB /
// FDCB), which also fixes a latent bug in the old byte-accumulator: for DD CB
// and FD CB instructions the displacement byte precedes the final opcode byte,
// so the old code keyed the table on the displacement and produced the wrong
// mnemonic for any non-zero displacement. Output format for every other case
// is preserved byte-for-byte (see test/z80_disassembly.cpp golden
// expectations).

#include "z80_disassembly.h"

#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <ostream>
#include <string>
#include <string_view>

#include "log.h"
#include "z80_opcode_table.h"
#include "z80_view.h"

extern t_z80regs z80;

namespace {

// A Z80 instruction is at most 4 bytes: prefix, prefix, displacement, opcode.
constexpr int kMaxInstrLen = 4;

// Result of decoding the raw byte stream at an address, before any text
// formatting. When `op` is null the bytes did not form a known instruction and
// the caller emits a single `db` byte.
struct RawInstr {
  const Z80Opcode* op = nullptr;  // matched table entry, or null (unknown)
  byte length = 1;  // total bytes consumed (1 for the db fallback)
  byte bytes[kMaxInstrLen] = {};   // instruction bytes in memory order
  const byte* operands = nullptr;  // start of the immediate/displacement run
  byte operand_count = 0;          // number of operand bytes at `operands`
};

// Decode the prefix stream and look the opcode up in the master table. Reads
// only the bytes the instruction actually needs.
RawInstr decode_at(word pos) {
  z80_opcode_table_init();  // idempotent; guarantees the lookup map is built

  RawInstr r;
  const byte b0 = z80_read_mem(pos);
  r.bytes[0] = b0;

  OpcodePrefix prefix;
  byte opcode;
  byte opcode_index;  // index of the opcode byte within bytes[]

  switch (b0) {
    case 0xCB:
      prefix = OpcodePrefix::CB;
      opcode = z80_read_mem(pos + 1);
      opcode_index = 1;
      break;
    case 0xED:
      prefix = OpcodePrefix::ED;
      opcode = z80_read_mem(pos + 1);
      opcode_index = 1;
      break;
    case 0xDD:
    case 0xFD: {
      const byte b1 = z80_read_mem(pos + 1);
      if (b1 == 0xCB) {
        // DD CB disp opcode / FD CB disp opcode — displacement precedes opcode.
        prefix = (b0 == 0xDD) ? OpcodePrefix::DDCB : OpcodePrefix::FDCB;
        opcode = z80_read_mem(pos + 3);
        opcode_index = 3;
      } else {
        prefix = (b0 == 0xDD) ? OpcodePrefix::DD : OpcodePrefix::FD;
        opcode = b1;
        opcode_index = 1;
      }
      break;
    }
    default:
      prefix = OpcodePrefix::NONE;
      opcode = b0;
      opcode_index = 0;
      break;
  }

  const Z80Opcode* entry = z80_opcode_lookup(prefix, opcode);
  if (entry == nullptr) {
    // Unknown encoding: consume a single byte so the caller can resync.
    r.length = 1;
    return r;
  }

  r.op = entry;
  r.length = entry->length;
  for (byte i = 0; i < r.length && i < kMaxInstrLen; ++i)
    r.bytes[i] = z80_read_mem(pos + i);

  r.operand_count = entry->operand_bytes;
  if (prefix == OpcodePrefix::DDCB || prefix == OpcodePrefix::FDCB) {
    // The lone operand is the displacement, sitting *before* the opcode byte.
    r.operands = &r.bytes[2];
  } else {
    r.operands = &r.bytes[opcode_index + 1];
  }
  return r;
}

// Pack the instruction bytes big-endian into a single value. This mirrors the
// historical `opcode_` encoding that callers and golden tests compare against.
uint64_t pack_bytes(const RawInstr& r) {
  uint64_t packed = 0;
  for (byte i = 0; i < r.length && i < kMaxInstrLen; ++i)
    packed = (packed << 8) | r.bytes[i];
  return packed;
}

// Queue `address` for recursive disassembly if it hasn't been emitted yet.
void add_if_new(word address, const DisassembledCode& result,
                std::vector<dword>& worklist, const char* why, word from) {
  const DisassembledLine probe(address, 0, "");
  if (result.lines.count(probe) == 0) {
    worklist.push_back(address);
    LOG_VERBOSE("Adding " << std::hex << address << " from " << why << " at "
                          << from);
  }
}

// Append the "  ; $xxxx" absolute-target annotation used for relative jumps.
void append_target_comment(std::string& text, word address) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "  ; $%04x", address);
  text += buf;
}

}  // namespace

// ── DisassembledLine ────────────────────────────────────────────────────────

DisassembledLine::DisassembledLine(word address, uint64_t opcode,
                                   std::string&& instruction,
                                   int64_t ref_address)
    : address_(address), opcode_(opcode), instruction_(std::move(instruction)) {
  if (ref_address >= 0) {
    ref_address_ = static_cast<word>(ref_address);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "$%04x", ref_address_);
    ref_address_string_ = buf;
  }
}

int DisassembledLine::Size() const {
  // Byte count is the number of significant bytes in the packed opcode.
  int size = 1;
  for (uint64_t limit = 0x100; opcode_ >= limit && size < 8; limit <<= 8)
    ++size;
  return size;
}

bool operator<(const DisassembledLine& l, const DisassembledLine& r) {
  return l.address_ < r.address_;
}

bool operator==(const DisassembledLine& l, const DisassembledLine& r) {
  return l.address_ == r.address_ && l.opcode_ == r.opcode_ &&
         l.instruction_ == r.instruction_;
}

std::ostream& operator<<(std::ostream& os, const DisassembledLine& line) {
  os << std::setfill('0') << std::setw(4) << std::hex << line.address_ << ": ";
  os << std::setfill(' ') << std::setw(8) << line.opcode_ << " "
     << line.instruction_;
  return os;
}

// ── DisassembledCode ────────────────────────────────────────────────────────

uint64_t DisassembledCode::hash() const {
  // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated
  // (out-param/compound-assign/loop/reference)
  uint64_t h = 0;
  // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated
  // (out-param/compound-assign/loop/reference)
  int i = 0;
  for (const auto& line : lines) {
    h += i * (line.address_ + line.opcode_);
    i += 1;
  }
  return h;
}

std::optional<DisassembledLine> DisassembledCode::LineAt(word address) const {
  for (const auto& l : lines) {
    if (l.address_ == address) return l;
  }
  return {};
}

std::ostream& operator<<(std::ostream& os, const DisassembledCode& code) {
  for (const auto& line : code.lines) os << line << "\n";
  return os;
}

// ── Disassembly core ────────────────────────────────────────────────────────

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other
// translation units/tests; internal linkage would break the link
DisassembledLine disassemble_one(dword start_address, DisassembledCode& result,
                                 std::vector<dword>& called_points) {
  const word start = static_cast<word>(start_address);
  const RawInstr r = decode_at(start);

  if (r.op == nullptr) {
    LOG_VERBOSE("No opcode found at " << std::hex << start);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "db $%x", r.bytes[0]);
    return {start, r.bytes[0], buf};
  }

  const uint64_t opcode = pack_bytes(r);
  std::string text(r.op->mnemonic);
  int64_t ref_address = -1;
  byte oi = 0;  // operand cursor

  // Word operand `**` (little-endian). Only absolute jp/call targets are
  // recorded for recursive following.
  const size_t word_pos = text.find("**");
  if (word_pos != std::string::npos) {
    const word value = static_cast<word>(r.operands[0] | (r.operands[1] << 8));
    oi = 2;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "$%04x", value);
    text.replace(word_pos, 2, buf);
    if (text.rfind("call", 0) == 0 || text.rfind("jp", 0) == 0) {
      add_if_new(value, result, called_points, "call/jp", start);
      ref_address = value;
    }
  }

  // Byte operands `*` (displacement and/or immediate), left to right.
  for (size_t p = text.find('*'); p != std::string::npos; p = text.find('*')) {
    const byte value = r.operands[oi++];
    char buf[8];
    std::snprintf(buf, sizeof(buf), "$%02x", value);
    text.replace(p, 1, buf);
    if (r.op->is_relative) {
      // JR/DJNZ: the byte is a signed displacement from the next instruction.
      const word target =
          static_cast<word>(start + r.length + static_cast<int8_t>(value));
      append_target_comment(text, target);
      add_if_new(target, result, called_points, "jr/djnz", start);
      ref_address = target;
    }
  }

  return {start, opcode, std::move(text), ref_address};
}

// `pos` is a dword so we can detect running off the top of memory.
namespace {
void disassemble_from(dword pos, DisassembledCode& result,
                      std::vector<dword>& to_disassemble_from) {
  while (pos <= 0xFFFF) {
    auto line = disassemble_one(pos, result, to_disassemble_from);
    pos += line.Size();
    const bool is_ret = line.instruction_ == "ret";
    result.lines.insert(std::move(line));
    if (is_ret) return;
  }
}
}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other
// translation units/tests; internal linkage would break the link
DisassembledCode disassemble(const std::vector<word>& entry_points) {
  DisassembledCode code;
  std::vector<dword> to_disassemble_from(entry_points.begin(),
                                         entry_points.end());
  while (!to_disassemble_from.empty()) {
    const auto next_pos = to_disassemble_from.back();
    to_disassemble_from.pop_back();
    disassemble_from(next_pos, code, to_disassemble_from);
  }
  return code;
}

// ── konCePCja debug helpers ─────────────────────────────────────────────────
//
// These no longer format any text — they decode straight off the table, which
// makes the DevTools stepper / memory navigator hot paths allocation-free.

int z80_instruction_length(word pc) { return decode_at(pc).length; }

bool z80_is_call_or_rst(word pc) {
  const RawInstr r = decode_at(pc);
  if (r.op == nullptr) return false;
  const std::string_view m(r.op->mnemonic);
  return m.rfind("call", 0) == 0 || m.rfind("rst", 0) == 0;
}
