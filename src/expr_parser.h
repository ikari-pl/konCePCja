#ifndef EXPR_PARSER_H
#define EXPR_PARSER_H

#include <string>
#include <memory>
#include <cstdint>
#include "types.h"

class t_z80regs;

// Context for expression evaluation — provides access to emulator state.
// Hardware struct pointers are void* because the typedef'd anonymous structs
// from koncepcja.h can't be forward-declared. The evaluator casts them back.
struct ExprContext {
  t_z80regs* z80 = nullptr;
  void* crtc = nullptr;       // t_CRTC*
  void* ga = nullptr;         // t_GateArray*
  void* psg = nullptr;        // t_PSG*
  int32_t address = 0;        // breakpoint/watchpoint address
  int32_t value = 0;          // data value at breakpoint
  int32_t previous = 0;       // previous value (for watchpoints)
  int32_t mode = 0;           // access mode (read/write/io)
};

// AST node types
enum class ExprNodeType {
  LITERAL,
  VARIABLE,
  UNARY_NOT,
  BINARY_OP,
  FUNCTION_CALL,
};

enum class BinaryOp {
  ADD, SUB, MUL, DIV, MOD,
  AND, OR, XOR,
  LT, LE, EQ, GE, GT, NE,
};

// AST node
struct ExprNode {
  ExprNodeType type;
  int32_t value = 0;            // for LITERAL
  std::string name;             // for VARIABLE or FUNCTION_CALL
  BinaryOp op = BinaryOp::ADD; // for BINARY_OP
  std::unique_ptr<ExprNode> left;
  std::unique_ptr<ExprNode> right;
  std::unique_ptr<ExprNode> arg; // single-arg function call
};

// Parse an expression string into an AST. Returns nullptr on error, with
// error message stored in `error`.
std::unique_ptr<ExprNode> expr_parse(const std::string& input, std::string& error);

// Evaluate an AST node with the given context. All operations are 32-bit
// signed integers. Division by zero returns 0. Comparisons return -1 (true)
// or 0 (false).
int32_t expr_eval(const ExprNode* node, const ExprContext& ctx);

// Convert an AST back to a human-readable string.
std::string expr_to_string(const ExprNode* node);

#endif
