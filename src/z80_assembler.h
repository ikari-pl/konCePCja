#ifndef Z80_ASSEMBLER_H
#define Z80_ASSEMBLER_H

#include "types.h"
#include <string>
#include <vector>
#include <map>
#include <cstdint>

struct AsmError {
    int line;           // 1-based source line number
    std::string message;
};

struct AsmResult {
    bool success;
    std::vector<AsmError> errors;
    std::map<std::string, word> symbols;
    word start_addr;    // lowest address written
    word end_addr;      // highest address written + 1
    int bytes_written;
};

class Z80Assembler {
public:
    // Assemble source text, write to CPC memory
    AsmResult assemble(const std::string& source);

    // Assemble but don't write to memory (dry run)
    AsmResult check(const std::string& source);

    // Expression evaluator — left-to-right (Maxam compat)
    // Public for testing
    static bool eval_expr(const std::string& expr,
                          const std::map<std::string, word>& symbols,
                          word current_addr, int32_t& result,
                          std::string& error);

private:
    struct Line {
        int number;           // 1-based source line number
        std::string label;
        std::string mnemonic; // uppercased
        std::string operands; // raw operand string (whitespace-trimmed)
    };

    std::vector<Line> parse(const std::string& source);

    bool pass1(std::vector<Line>& lines,
               std::map<std::string, word>& symbols,
               std::vector<AsmError>& errors);

    bool pass2(const std::vector<Line>& lines,
               const std::map<std::string, word>& symbols,
               std::vector<AsmError>& errors,
               bool write_memory,
               word& start_addr, word& end_addr, int& bytes_written);

    // Instruction size calculation (pass 1) — returns byte count, 0 on error
    int instruction_size(const std::string& mnemonic, const std::string& operands,
                         std::string& error);

    // Instruction encoding (pass 2) — returns encoded bytes
    bool encode_instruction(const std::string& mnemonic, const std::string& operands,
                            const std::map<std::string, word>& symbols,
                            word current_addr, std::vector<byte>& output,
                            std::string& error);

    // Directive helpers
    static bool is_directive(const std::string& mnemonic);
    int directive_size(const Line& line, const std::map<std::string, word>& symbols,
                       word current_addr, std::string& error);
    bool encode_directive(const Line& line, const std::map<std::string, word>& symbols,
                          word current_addr, std::vector<byte>& output,
                          std::string& error);
};

extern Z80Assembler g_assembler;

#endif // Z80_ASSEMBLER_H
