#include "expr_parser.h"
#include "z80.h"
#include "koncepcja.h"
#include "debug_timers.h"

#include <cctype>
#include <cstring>
#include <algorithm>

// ─── Tokenizer ──────────────────────────────────────────────────────

enum class TokenType {
  NUMBER, IDENT, LPAREN, RPAREN, COMMA,
  PLUS, MINUS, STAR, SLASH,
  LT, LE, GT, GE, EQ, NE,
  END, ERROR,
};

struct Token {
  TokenType type;
  int32_t num_value = 0;
  std::string str_value;
};

class Tokenizer {
public:
  explicit Tokenizer(const std::string& input) : src(input), pos(0) {}

  Token next() {
    skip_ws();
    if (pos >= src.size()) return {TokenType::END};

    char ch = src[pos];

    // Parentheses and comma
    if (ch == '(') { pos++; return {TokenType::LPAREN}; }
    if (ch == ')') { pos++; return {TokenType::RPAREN}; }
    if (ch == ',') { pos++; return {TokenType::COMMA}; }
    if (ch == '+') { pos++; return {TokenType::PLUS}; }
    if (ch == '-') { pos++; return {TokenType::MINUS}; }
    if (ch == '*') { pos++; return {TokenType::STAR}; }
    if (ch == '/') { pos++; return {TokenType::SLASH}; }

    // Comparison operators
    if (ch == '<') {
      pos++;
      if (pos < src.size() && src[pos] == '=') { pos++; return {TokenType::LE}; }
      if (pos < src.size() && src[pos] == '>') { pos++; return {TokenType::NE}; }
      return {TokenType::LT};
    }
    if (ch == '>') {
      pos++;
      if (pos < src.size() && src[pos] == '=') { pos++; return {TokenType::GE}; }
      return {TokenType::GT};
    }
    if (ch == '=') {
      pos++;
      if (pos < src.size() && src[pos] == '=') pos++; // accept both = and ==
      return {TokenType::EQ};
    }
    if (ch == '!' && pos + 1 < src.size() && src[pos + 1] == '=') {
      pos += 2;
      return {TokenType::NE};
    }

    // Number: decimal, #hex, &hex, 0xhex, %binary
    if (ch == '#' || ch == '&') {
      pos++;
      return parse_hex();
    }
    if (ch == '0' && pos + 1 < src.size() && (src[pos + 1] == 'x' || src[pos + 1] == 'X')) {
      pos += 2;
      return parse_hex();
    }
    if (ch == '%') {
      pos++;
      return parse_binary();
    }
    if (std::isdigit(static_cast<unsigned char>(ch))) {
      return parse_decimal();
    }

    // Identifier (register, variable, keyword, function name)
    if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_' || ch == '\'') {
      return parse_ident();
    }

    Token err;
    err.type = TokenType::ERROR;
    err.str_value = "unexpected character: ";
    err.str_value += ch;
    pos++;
    return err;
  }

private:
  const std::string& src;
  size_t pos;

  void skip_ws() {
    while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos]))) pos++;
  }

  Token parse_hex() {
    size_t start = pos;
    while (pos < src.size() && std::isxdigit(static_cast<unsigned char>(src[pos]))) pos++;
    if (pos == start) return {TokenType::ERROR, 0, "expected hex digits"};
    Token t;
    t.type = TokenType::NUMBER;
    t.num_value = static_cast<int32_t>(std::stoul(src.substr(start, pos - start), nullptr, 16));
    return t;
  }

  Token parse_binary() {
    size_t start = pos;
    while (pos < src.size() && (src[pos] == '0' || src[pos] == '1')) pos++;
    if (pos == start) return {TokenType::ERROR, 0, "expected binary digits"};
    Token t;
    t.type = TokenType::NUMBER;
    t.num_value = static_cast<int32_t>(std::stoul(src.substr(start, pos - start), nullptr, 2));
    return t;
  }

  Token parse_decimal() {
    size_t start = pos;
    while (pos < src.size() && std::isdigit(static_cast<unsigned char>(src[pos]))) pos++;
    Token t;
    t.type = TokenType::NUMBER;
    t.num_value = static_cast<int32_t>(std::stoul(src.substr(start, pos - start), nullptr, 10));
    return t;
  }

  Token parse_ident() {
    size_t start = pos;
    // Allow alphanumeric, underscore, and trailing apostrophe (for AF' etc.)
    while (pos < src.size() &&
           (std::isalnum(static_cast<unsigned char>(src[pos])) ||
            src[pos] == '_' || src[pos] == '\'')) {
      pos++;
    }
    Token t;
    t.type = TokenType::IDENT;
    t.str_value = src.substr(start, pos - start);
    return t;
  }
};

// ─── Parser (recursive descent) ─────────────────────────────────────

class Parser {
public:
  explicit Parser(const std::string& input) : tok(input) {}

  std::unique_ptr<ExprNode> parse(std::string& error) {
    advance();
    auto node = parse_or_expr();
    if (!node) { error = err; return nullptr; }
    if (cur.type != TokenType::END) {
      error = "unexpected token after expression";
      return nullptr;
    }
    return node;
  }

private:
  Tokenizer tok;
  Token cur;
  std::string err;

  void advance() { cur = tok.next(); }

  // Helper to make nodes
  static std::unique_ptr<ExprNode> make_literal(int32_t v) {
    auto n = std::make_unique<ExprNode>();
    n->type = ExprNodeType::LITERAL;
    n->value = v;
    return n;
  }
  static std::unique_ptr<ExprNode> make_var(const std::string& name) {
    auto n = std::make_unique<ExprNode>();
    n->type = ExprNodeType::VARIABLE;
    n->name = name;
    return n;
  }
  static std::unique_ptr<ExprNode> make_binop(BinaryOp op,
      std::unique_ptr<ExprNode> lhs, std::unique_ptr<ExprNode> rhs) {
    auto n = std::make_unique<ExprNode>();
    n->type = ExprNodeType::BINARY_OP;
    n->op = op;
    n->left = std::move(lhs);
    n->right = std::move(rhs);
    return n;
  }
  static std::unique_ptr<ExprNode> make_not(std::unique_ptr<ExprNode> operand) {
    auto n = std::make_unique<ExprNode>();
    n->type = ExprNodeType::UNARY_NOT;
    n->left = std::move(operand);
    return n;
  }
  static std::unique_ptr<ExprNode> make_func(const std::string& name,
      std::unique_ptr<ExprNode> arg) {
    auto n = std::make_unique<ExprNode>();
    n->type = ExprNodeType::FUNCTION_CALL;
    n->name = name;
    n->arg = std::move(arg);
    return n;
  }

  // Lowercase helper
  static std::string lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return r;
  }

  // Grammar: or_expr := xor_expr ('or' xor_expr)*
  std::unique_ptr<ExprNode> parse_or_expr() {
    auto node = parse_xor_expr();
    if (!node) return nullptr;
    while (cur.type == TokenType::IDENT && lower(cur.str_value) == "or") {
      advance();
      auto rhs = parse_xor_expr();
      if (!rhs) return nullptr;
      node = make_binop(BinaryOp::OR, std::move(node), std::move(rhs));
    }
    return node;
  }

  // xor_expr := and_expr ('xor' and_expr)*
  std::unique_ptr<ExprNode> parse_xor_expr() {
    auto node = parse_and_expr();
    if (!node) return nullptr;
    while (cur.type == TokenType::IDENT && lower(cur.str_value) == "xor") {
      advance();
      auto rhs = parse_and_expr();
      if (!rhs) return nullptr;
      node = make_binop(BinaryOp::XOR, std::move(node), std::move(rhs));
    }
    return node;
  }

  // and_expr := cmp_expr ('and' cmp_expr)*
  std::unique_ptr<ExprNode> parse_and_expr() {
    auto node = parse_cmp_expr();
    if (!node) return nullptr;
    while (cur.type == TokenType::IDENT && lower(cur.str_value) == "and") {
      advance();
      auto rhs = parse_cmp_expr();
      if (!rhs) return nullptr;
      node = make_binop(BinaryOp::AND, std::move(node), std::move(rhs));
    }
    return node;
  }

  // cmp_expr := add_expr (('<'|'<='|'='|'>='|'>'|'<>'|'!=') add_expr)?
  std::unique_ptr<ExprNode> parse_cmp_expr() {
    auto node = parse_add_expr();
    if (!node) return nullptr;
    BinaryOp op;
    bool found = true;
    switch (cur.type) {
      case TokenType::LT: op = BinaryOp::LT; break;
      case TokenType::LE: op = BinaryOp::LE; break;
      case TokenType::EQ: op = BinaryOp::EQ; break;
      case TokenType::GE: op = BinaryOp::GE; break;
      case TokenType::GT: op = BinaryOp::GT; break;
      case TokenType::NE: op = BinaryOp::NE; break;
      default: found = false;
    }
    if (found) {
      advance();
      auto rhs = parse_add_expr();
      if (!rhs) return nullptr;
      node = make_binop(op, std::move(node), std::move(rhs));
    }
    return node;
  }

  // add_expr := mul_expr (('+'|'-') mul_expr)*
  std::unique_ptr<ExprNode> parse_add_expr() {
    auto node = parse_mul_expr();
    if (!node) return nullptr;
    while (cur.type == TokenType::PLUS || cur.type == TokenType::MINUS) {
      BinaryOp op = (cur.type == TokenType::PLUS) ? BinaryOp::ADD : BinaryOp::SUB;
      advance();
      auto rhs = parse_mul_expr();
      if (!rhs) return nullptr;
      node = make_binop(op, std::move(node), std::move(rhs));
    }
    return node;
  }

  // mul_expr := unary (('*'|'/'|'mod') unary)*
  std::unique_ptr<ExprNode> parse_mul_expr() {
    auto node = parse_unary();
    if (!node) return nullptr;
    while (true) {
      BinaryOp op;
      if (cur.type == TokenType::STAR) op = BinaryOp::MUL;
      else if (cur.type == TokenType::SLASH) op = BinaryOp::DIV;
      else if (cur.type == TokenType::IDENT && lower(cur.str_value) == "mod") op = BinaryOp::MOD;
      else break;
      advance();
      auto rhs = parse_unary();
      if (!rhs) return nullptr;
      node = make_binop(op, std::move(node), std::move(rhs));
    }
    return node;
  }

  // unary := 'not' unary | '-' unary | atom
  std::unique_ptr<ExprNode> parse_unary() {
    if (cur.type == TokenType::IDENT && lower(cur.str_value) == "not") {
      advance();
      auto operand = parse_unary();
      if (!operand) return nullptr;
      return make_not(std::move(operand));
    }
    if (cur.type == TokenType::MINUS) {
      advance();
      auto operand = parse_unary();
      if (!operand) return nullptr;
      // Negate: 0 - operand
      return make_binop(BinaryOp::SUB, make_literal(0), std::move(operand));
    }
    return parse_atom();
  }

  // atom := number | variable | function '(' expr ')' | '(' expr ')'
  std::unique_ptr<ExprNode> parse_atom() {
    if (cur.type == TokenType::NUMBER) {
      auto node = make_literal(cur.num_value);
      advance();
      return node;
    }
    if (cur.type == TokenType::LPAREN) {
      advance();
      auto node = parse_or_expr();
      if (!node) return nullptr;
      if (cur.type != TokenType::RPAREN) {
        err = "expected ')'";
        return nullptr;
      }
      advance();
      return node;
    }
    if (cur.type == TokenType::IDENT) {
      std::string name = cur.str_value;
      advance();
      // Check if it's a function call
      if (cur.type == TokenType::LPAREN) {
        advance();
        auto arg = parse_or_expr();
        if (!arg) return nullptr;
        if (cur.type != TokenType::RPAREN) {
          err = "expected ')' after function argument";
          return nullptr;
        }
        advance();
        return make_func(lower(name), std::move(arg));
      }
      // It's a variable
      return make_var(name);
    }
    if (cur.type == TokenType::ERROR) {
      err = cur.str_value;
    } else {
      err = "unexpected token";
    }
    return nullptr;
  }
};

// ─── Public API: parse ──────────────────────────────────────────────

std::unique_ptr<ExprNode> expr_parse(const std::string& input, std::string& error) {
  Parser p(input);
  return p.parse(error);
}

// ─── Evaluator ──────────────────────────────────────────────────────

// Resolve a register variable name to its value (case-insensitive)
static int32_t resolve_variable(const std::string& name, const ExprContext& ctx) {
  if (!ctx.z80) return 0;
  const t_z80regs& z = *ctx.z80;

  // Normalize to lowercase for uniform matching
  std::string n = name;
  std::transform(n.begin(), n.end(), n.begin(),
                 [](unsigned char c){ return std::tolower(c); });

  // 8-bit registers
  if (n == "a") return z.AF.b.h;
  if (n == "f") return z.AF.b.l;
  if (n == "b") return z.BC.b.h;
  if (n == "c") return z.BC.b.l;
  if (n == "d") return z.DE.b.h;
  if (n == "e") return z.DE.b.l;
  if (n == "h") return z.HL.b.h;
  if (n == "l") return z.HL.b.l;
  if (n == "i") return z.I;
  if (n == "r") return z.R;

  // 16-bit registers
  if (n == "af") return z.AF.w.l;
  if (n == "bc") return z.BC.w.l;
  if (n == "de") return z.DE.w.l;
  if (n == "hl") return z.HL.w.l;
  if (n == "ix") return z.IX.w.l;
  if (n == "iy") return z.IY.w.l;
  if (n == "sp") return z.SP.w.l;
  if (n == "pc") return z.PC.w.l;

  // IX/IY halves
  if (n == "ixh") return z.IX.b.h;
  if (n == "ixl") return z.IX.b.l;
  if (n == "iyh") return z.IY.b.h;
  if (n == "iyl") return z.IY.b.l;

  // Shadow registers (accept both AF' and AFx forms)
  if (n == "af'" || n == "afx") return z.AFx.w.l;
  if (n == "bc'" || n == "bcx") return z.BCx.w.l;
  if (n == "de'" || n == "dex") return z.DEx.w.l;
  if (n == "hl'" || n == "hlx") return z.HLx.w.l;

  // Status
  if (n == "im") return z.IM;
  if (n == "iff1") return z.IFF1;
  if (n == "iff2") return z.IFF2;

  // Context variables
  if (n == "address") return ctx.address;
  if (n == "value") return ctx.value;
  if (n == "previous") return ctx.previous;
  if (n == "mode") return ctx.mode;

  return 0; // unknown variable
}

// Resolve a function call
static int32_t resolve_function(const std::string& name, int32_t arg, const ExprContext& ctx) {
  if (name == "peek") {
    return z80_read_mem(static_cast<word>(arg));
  }
  if (name == "byte") {
    return arg & 0xFF;
  }
  if (name == "hibyte") {
    return (arg >> 8) & 0xFF;
  }
  if (name == "word") {
    return arg & 0xFFFF;
  }
  if (name == "hiword") {
    return (arg >> 16) & 0xFFFF;
  }
  if (name == "ay") {
    if (ctx.psg && arg >= 0 && arg < 16) {
      auto* psg = static_cast<t_PSG*>(ctx.psg);
      return psg->RegisterAY.Index[arg];
    }
    return 0;
  }
  if (name == "crtc") {
    if (ctx.crtc && arg >= 0 && arg < 18) {
      auto* crtc = static_cast<t_CRTC*>(ctx.crtc);
      return crtc->registers[arg];
    }
    return 0;
  }
  if (name == "timer_start") {
    extern uint64_t g_tstate_counter;
    g_debug_timers.timer_start(arg, g_tstate_counter);
    return 0;
  }
  if (name == "timer_stop") {
    extern uint64_t g_tstate_counter;
    return g_debug_timers.timer_stop(arg, g_tstate_counter);
  }
  return 0;
}

int32_t expr_eval(const ExprNode* node, const ExprContext& ctx) {
  if (!node) return 0;

  switch (node->type) {
    case ExprNodeType::LITERAL:
      return node->value;

    case ExprNodeType::VARIABLE:
      return resolve_variable(node->name, ctx);

    case ExprNodeType::UNARY_NOT:
      return ~expr_eval(node->left.get(), ctx);

    case ExprNodeType::FUNCTION_CALL:
      return resolve_function(node->name, expr_eval(node->arg.get(), ctx), ctx);

    case ExprNodeType::BINARY_OP: {
      int32_t lv = expr_eval(node->left.get(), ctx);
      int32_t rv = expr_eval(node->right.get(), ctx);
      switch (node->op) {
        case BinaryOp::ADD: return lv + rv;
        case BinaryOp::SUB: return lv - rv;
        case BinaryOp::MUL: return lv * rv;
        case BinaryOp::DIV: return rv == 0 ? 0 : lv / rv;
        case BinaryOp::MOD: return rv == 0 ? 0 : lv % rv;
        case BinaryOp::AND: return lv & rv;
        case BinaryOp::OR:  return lv | rv;
        case BinaryOp::XOR: return lv ^ rv;
        case BinaryOp::LT:  return lv < rv  ? -1 : 0;
        case BinaryOp::LE:  return lv <= rv ? -1 : 0;
        case BinaryOp::EQ:  return lv == rv ? -1 : 0;
        case BinaryOp::GE:  return lv >= rv ? -1 : 0;
        case BinaryOp::GT:  return lv > rv  ? -1 : 0;
        case BinaryOp::NE:  return lv != rv ? -1 : 0;
      }
      return 0;
    }
  }
  return 0;
}

// ─── Stringify ──────────────────────────────────────────────────────

static const char* binop_str(BinaryOp op) {
  switch (op) {
    case BinaryOp::ADD: return "+";
    case BinaryOp::SUB: return "-";
    case BinaryOp::MUL: return "*";
    case BinaryOp::DIV: return "/";
    case BinaryOp::MOD: return " mod ";
    case BinaryOp::AND: return " and ";
    case BinaryOp::OR:  return " or ";
    case BinaryOp::XOR: return " xor ";
    case BinaryOp::LT:  return "<";
    case BinaryOp::LE:  return "<=";
    case BinaryOp::EQ:  return "=";
    case BinaryOp::GE:  return ">=";
    case BinaryOp::GT:  return ">";
    case BinaryOp::NE:  return "<>";
  }
  return "?";
}

std::string expr_to_string(const ExprNode* node) {
  if (!node) return "";
  switch (node->type) {
    case ExprNodeType::LITERAL: {
      char buf[16];
      if (node->value < 0 || node->value > 255) {
        snprintf(buf, sizeof(buf), "#%X", static_cast<unsigned>(node->value) & 0xFFFFFFFF);
      } else {
        snprintf(buf, sizeof(buf), "%d", node->value);
      }
      return buf;
    }
    case ExprNodeType::VARIABLE:
      return node->name;
    case ExprNodeType::UNARY_NOT:
      return "not " + expr_to_string(node->left.get());
    case ExprNodeType::BINARY_OP:
      return "(" + expr_to_string(node->left.get()) +
             binop_str(node->op) +
             expr_to_string(node->right.get()) + ")";
    case ExprNodeType::FUNCTION_CALL:
      return node->name + "(" + expr_to_string(node->arg.get()) + ")";
  }
  return "";
}
