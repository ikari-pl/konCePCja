#include "z80_assembler.h"
#include "z80_opcode_table.h"
#include "z80.h"
#include "log.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

Z80Assembler g_assembler;

// ── String helpers ──

static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r");
    return s.substr(start, end - start + 1);
}

static std::string to_upper(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return r;
}

// Check if a string is a Z80 register name (used to distinguish regs from labels)
static bool is_register(const std::string& s) {
    static const char* regs[] = {
        "A","B","C","D","E","H","L","F",
        "AF","BC","DE","HL","SP","IX","IY",
        "IXH","IXL","IYH","IYL","I","R",
        "AF'",
        "(HL)","(BC)","(DE)","(SP)","(IX)","(IY)","(C)",
        nullptr
    };
    std::string u = to_upper(s);
    for (int i = 0; regs[i]; i++)
        if (u == regs[i]) return true;
    // Also check (IX+*) patterns
    if (u.size() >= 4 && u[0] == '(' && (u.substr(1,2) == "IX" || u.substr(1,2) == "IY"))
        return true;
    return false;
}

// Check if a character can start a label
static bool is_label_char(char c) {
    return isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
}

// ── Expression Evaluator (left-to-right, Maxam compatible) ──

// Tokenizer for expressions
enum class ExprTokenType { NUMBER, SYMBOL, OP, LPAREN, RPAREN, DOLLAR, TILDE, END };

struct ExprToken {
    ExprTokenType type;
    int32_t value;      // for NUMBER
    std::string text;   // for SYMBOL, OP
};

static bool tokenize_expr(const std::string& expr, std::vector<ExprToken>& tokens, std::string& error) {
    size_t i = 0;
    while (i < expr.size()) {
        char c = expr[i];
        if (isspace(static_cast<unsigned char>(c))) { i++; continue; }

        // Context-sensitive: & and % can be value prefixes or operators.
        // They are prefixes when a value is expected (at start, after op, after '(', after '~')
        bool expect_value = tokens.empty() ||
            tokens.back().type == ExprTokenType::OP ||
            tokens.back().type == ExprTokenType::LPAREN ||
            tokens.back().type == ExprTokenType::TILDE;

        // & = hex prefix (&FF) when value expected, bitwise AND otherwise
        if (c == '&' && expect_value && i + 1 < expr.size() && isxdigit(static_cast<unsigned char>(expr[i+1]))) {
            i++; // skip &
            size_t start = i;
            while (i < expr.size() && isxdigit(static_cast<unsigned char>(expr[i]))) i++;
            int32_t val = static_cast<int32_t>(std::stoul(expr.substr(start, i - start), nullptr, 16));
            tokens.push_back({ExprTokenType::NUMBER, val, ""});
            continue;
        }

        // % = binary prefix (%10110011) when value expected, modulo otherwise
        if (c == '%' && expect_value && i + 1 < expr.size() && (expr[i+1] == '0' || expr[i+1] == '1')) {
            i++; // skip %
            size_t start = i;
            while (i < expr.size() && (expr[i] == '0' || expr[i] == '1')) i++;
            int32_t val = static_cast<int32_t>(std::stoul(expr.substr(start, i - start), nullptr, 2));
            tokens.push_back({ExprTokenType::NUMBER, val, ""});
            continue;
        }

        // Single-character operators
        if (c == '+' || c == '-' || c == '/' || c == '%' ||
            c == '&' || c == '|' || c == '^') {
            tokens.push_back({ExprTokenType::OP, 0, std::string(1, c)});
            i++;
            continue;
        }
        if (c == '<' && i + 1 < expr.size() && expr[i+1] == '<') {
            tokens.push_back({ExprTokenType::OP, 0, "<<"});
            i += 2;
            continue;
        }
        if (c == '>' && i + 1 < expr.size() && expr[i+1] == '>') {
            tokens.push_back({ExprTokenType::OP, 0, ">>"});
            i += 2;
            continue;
        }
        if (c == '*') {
            tokens.push_back({ExprTokenType::OP, 0, "*"});
            i++;
            continue;
        }
        if (c == '~') {
            tokens.push_back({ExprTokenType::TILDE, 0, "~"});
            i++;
            continue;
        }
        if (c == '(') {
            tokens.push_back({ExprTokenType::LPAREN, 0, "("});
            i++;
            continue;
        }
        if (c == ')') {
            tokens.push_back({ExprTokenType::RPAREN, 0, ")"});
            i++;
            continue;
        }

        // Character literal: 'A' or 'A
        if (c == '\'') {
            if (i + 1 >= expr.size()) { error = "unterminated char literal"; return false; }
            int32_t val = static_cast<unsigned char>(expr[i + 1]);
            i += 2;
            if (i < expr.size() && expr[i] == '\'') i++; // optional closing quote
            tokens.push_back({ExprTokenType::NUMBER, val, ""});
            continue;
        }

        // $ alone = current address; $FF = hex; we need context
        if (c == '$') {
            // If next char is hex digit, it's $XX hex literal
            if (i + 1 < expr.size() && isxdigit(static_cast<unsigned char>(expr[i+1]))) {
                i++; // skip $
                size_t start = i;
                while (i < expr.size() && isxdigit(static_cast<unsigned char>(expr[i]))) i++;
                int32_t val = static_cast<int32_t>(std::stoul(expr.substr(start, i - start), nullptr, 16));
                tokens.push_back({ExprTokenType::NUMBER, val, ""});
            } else {
                // $ = current address
                tokens.push_back({ExprTokenType::DOLLAR, 0, "$"});
                i++;
            }
            continue;
        }

        // # hex prefix: #FF
        if (c == '#') {
            i++; // skip #
            size_t start = i;
            while (i < expr.size() && isxdigit(static_cast<unsigned char>(expr[i]))) i++;
            if (i == start) { error = "expected hex digit after #"; return false; }
            int32_t val = static_cast<int32_t>(std::stoul(expr.substr(start, i - start), nullptr, 16));
            tokens.push_back({ExprTokenType::NUMBER, val, ""});
            continue;
        }

        // Note: & hex prefix and % binary prefix are handled above (before operators)

        // 0x hex prefix
        if (c == '0' && i + 1 < expr.size() && (expr[i+1] == 'x' || expr[i+1] == 'X')) {
            i += 2;
            size_t start = i;
            while (i < expr.size() && isxdigit(static_cast<unsigned char>(expr[i]))) i++;
            if (i == start) { error = "expected hex digit after 0x"; return false; }
            int32_t val = static_cast<int32_t>(std::stoul(expr.substr(start, i - start), nullptr, 16));
            tokens.push_back({ExprTokenType::NUMBER, val, ""});
            continue;
        }

        // Decimal number
        if (isdigit(static_cast<unsigned char>(c))) {
            size_t start = i;
            while (i < expr.size() && isxdigit(static_cast<unsigned char>(expr[i]))) i++;
            // Check for 'h' suffix (e.g. 0FFh, 38h)
            if (i < expr.size() && (expr[i] == 'h' || expr[i] == 'H')) {
                int32_t val = static_cast<int32_t>(std::stoul(expr.substr(start, i - start), nullptr, 16));
                i++; // skip 'h'
                tokens.push_back({ExprTokenType::NUMBER, val, ""});
            } else {
                // Pure decimal (only digits, not hex letters)
                // Re-scan for just decimal digits
                i = start;
                while (i < expr.size() && isdigit(static_cast<unsigned char>(expr[i]))) i++;
                int32_t val = static_cast<int32_t>(std::stol(expr.substr(start, i - start)));
                tokens.push_back({ExprTokenType::NUMBER, val, ""});
            }
            continue;
        }

        // Symbol/label name
        if (isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '.') {
            size_t start = i;
            while (i < expr.size() && is_label_char(expr[i])) i++;
            tokens.push_back({ExprTokenType::SYMBOL, 0, expr.substr(start, i - start)});
            continue;
        }

        error = std::string("unexpected character '") + c + "' in expression";
        return false;
    }
    tokens.push_back({ExprTokenType::END, 0, ""});
    return true;
}

bool Z80Assembler::eval_expr(const std::string& expr,
                              const std::map<std::string, word>& symbols,
                              word current_addr, int32_t& result,
                              std::string& error) {
    std::string trimmed = trim(expr);
    if (trimmed.empty()) { error = "empty expression"; return false; }

    std::vector<ExprToken> tokens;
    if (!tokenize_expr(trimmed, tokens, error)) return false;

    // Recursive descent with left-to-right evaluation (no operator precedence — Maxam compat)
    // Forward declarations for mutual recursion
    struct Parser {
        const std::vector<ExprToken>& tokens;
        const std::map<std::string, word>& symbols;
        word current_addr;
        size_t pos;
        std::string error;

        bool parse_atom(int32_t& val) {
            if (pos >= tokens.size()) { error = "unexpected end of expression"; return false; }
            auto& t = tokens[pos];

            if (t.type == ExprTokenType::NUMBER) {
                val = t.value;
                pos++;
                return true;
            }
            if (t.type == ExprTokenType::DOLLAR) {
                val = current_addr;
                pos++;
                return true;
            }
            if (t.type == ExprTokenType::SYMBOL) {
                std::string upper;
                for (auto c : t.text) upper += static_cast<char>(toupper(static_cast<unsigned char>(c)));
                auto it = symbols.find(upper);
                if (it == symbols.end()) {
                    error = "undefined symbol '" + t.text + "'";
                    return false;
                }
                val = it->second;
                pos++;
                return true;
            }
            if (t.type == ExprTokenType::TILDE) {
                pos++;
                int32_t operand;
                if (!parse_atom(operand)) return false;
                val = ~operand;
                return true;
            }
            if (t.type == ExprTokenType::LPAREN) {
                pos++; // skip (
                if (!parse_expr(val)) return false;
                if (pos >= tokens.size() || tokens[pos].type != ExprTokenType::RPAREN) {
                    error = "missing closing parenthesis";
                    return false;
                }
                pos++; // skip )
                return true;
            }
            // Unary minus/plus
            if (t.type == ExprTokenType::OP && (t.text == "-" || t.text == "+")) {
                pos++;
                int32_t operand;
                if (!parse_atom(operand)) return false;
                val = (t.text == "-") ? -operand : operand;
                return true;
            }
            error = "unexpected token '" + t.text + "'";
            return false;
        }

        bool parse_expr(int32_t& val) {
            if (!parse_atom(val)) return false;
            // Left-to-right: all binary operators have equal precedence
            while (pos < tokens.size() && tokens[pos].type == ExprTokenType::OP) {
                std::string op = tokens[pos].text;
                pos++;
                int32_t rhs;
                if (!parse_atom(rhs)) return false;
                if (op == "+") val += rhs;
                else if (op == "-") val -= rhs;
                else if (op == "*") val *= rhs;
                else if (op == "/") { if (rhs == 0) { error = "division by zero"; return false; } val /= rhs; }
                else if (op == "%") { if (rhs == 0) { error = "modulo by zero"; return false; } val %= rhs; }
                else if (op == "&") val &= rhs;
                else if (op == "|") val |= rhs;
                else if (op == "^") val ^= rhs;
                else if (op == "<<") val <<= rhs;
                else if (op == ">>") val >>= rhs;
                else { error = "unknown operator '" + op + "'"; return false; }
            }
            return true;
        }
    };

    Parser p{tokens, symbols, current_addr, 0, ""};
    if (!p.parse_expr(result)) {
        error = p.error;
        return false;
    }
    if (p.pos < tokens.size() && tokens[p.pos].type != ExprTokenType::END) {
        error = "unexpected token after expression";
        return false;
    }
    return true;
}

// ── Parser ──

std::vector<Z80Assembler::Line> Z80Assembler::parse(const std::string& source) {
    std::vector<Line> lines;
    std::istringstream stream(source);
    std::string raw_line;
    int line_num = 0;

    while (std::getline(stream, raw_line)) {
        line_num++;

        // Strip comment
        // Be careful not to strip ; inside string literals
        size_t comment_pos = std::string::npos;
        bool in_string = false;
        for (size_t i = 0; i < raw_line.size(); i++) {
            if (raw_line[i] == '"') in_string = !in_string;
            if (raw_line[i] == ';' && !in_string) { comment_pos = i; break; }
        }
        if (comment_pos != std::string::npos)
            raw_line = raw_line.substr(0, comment_pos);

        // Split on ':' for multiple statements per line
        // But not inside parentheses (to avoid splitting "LD A,(IX+5):LD B,C")
        // Actually, ':' as statement separator is after labels, so we handle label first
        std::string trimmed = trim(raw_line);
        if (trimmed.empty()) continue;

        // Extract label (if any)
        std::string label;
        std::string rest = trimmed;

        // Label: starts at column 0, followed by optional ':'
        // Could also start with '.'
        if (!rest.empty() && (isalpha(static_cast<unsigned char>(rest[0])) || rest[0] == '_' || rest[0] == '.')) {
            size_t end = 0;
            while (end < rest.size() && is_label_char(rest[end])) end++;
            std::string candidate = rest.substr(0, end);

            // If followed by ':', it's definitely a label
            // If followed by space and then a mnemonic, it might be a label or a mnemonic
            if (end < rest.size() && rest[end] == ':') {
                label = candidate;
                rest = trim(rest.substr(end + 1));
            } else {
                // Check if it's a known mnemonic keyword or directive
                std::string upper_cand = to_upper(candidate);
                if (!is_directive(upper_cand) && !z80_is_mnemonic_keyword(upper_cand)) {
                    // Not a mnemonic — treat as label
                    label = candidate;
                    rest = trim(rest.substr(end));
                }
            }
        }

        if (rest.empty()) {
            // Label-only line
            if (!label.empty()) {
                lines.push_back({line_num, label, "", ""});
            }
            continue;
        }

        // Split rest into mnemonic and operands
        // Handle ':' statement separator
        std::vector<std::string> statements;
        {
            size_t start = 0;
            int paren_depth = 0;
            bool in_str = false;
            for (size_t i = 0; i < rest.size(); i++) {
                if (rest[i] == '"') in_str = !in_str;
                if (!in_str) {
                    if (rest[i] == '(') paren_depth++;
                    if (rest[i] == ')') paren_depth--;
                    if (rest[i] == ':' && paren_depth == 0) {
                        statements.push_back(trim(rest.substr(start, i - start)));
                        start = i + 1;
                    }
                }
            }
            statements.push_back(trim(rest.substr(start)));
        }

        for (size_t si = 0; si < statements.size(); si++) {
            std::string stmt = statements[si];
            if (stmt.empty()) continue;

            // Split into mnemonic + operands
            size_t space_pos = stmt.find_first_of(" \t");
            std::string mnemonic, operands;
            if (space_pos == std::string::npos) {
                mnemonic = to_upper(stmt);
            } else {
                mnemonic = to_upper(stmt.substr(0, space_pos));
                operands = trim(stmt.substr(space_pos));
            }

            lines.push_back({line_num, (si == 0) ? label : "", mnemonic, operands});
        }
    }

    return lines;
}

// ── Directives ──

bool Z80Assembler::is_directive(const std::string& mnemonic) {
    return mnemonic == "ORG" || mnemonic == "EQU" || mnemonic == "DEFL" ||
           mnemonic == "DEFB" || mnemonic == "DB" || mnemonic == "BYTE" ||
           mnemonic == "DEFW" || mnemonic == "DW" || mnemonic == "WORD" ||
           mnemonic == "DEFS" || mnemonic == "DS" || mnemonic == "RMEM" ||
           mnemonic == "END";
}

// Count how many bytes a DEFB/DB directive will produce
static int count_defb_bytes(const std::string& operands) {
    int count = 0;
    bool in_string = false;
    int item_count = 0;

    for (size_t i = 0; i < operands.size(); i++) {
        char c = operands[i];
        if (c == '"') {
            if (in_string) {
                in_string = false;
            } else {
                in_string = true;
            }
            continue;
        }
        if (in_string) {
            count++;
            continue;
        }
        if (c == ',') {
            if (item_count == 0) count++; // previous non-string item
            item_count = 0;
            continue;
        }
        if (!isspace(static_cast<unsigned char>(c))) {
            item_count++;
        }
    }
    if (!in_string && item_count > 0) count++; // last item
    return count;
}

// Count commas outside strings to determine DEFW item count
static int count_comma_items(const std::string& operands) {
    int count = 1;
    bool in_string = false;
    for (char c : operands) {
        if (c == '"') in_string = !in_string;
        if (c == ',' && !in_string) count++;
    }
    return count;
}

int Z80Assembler::directive_size(const Line& line,
                                  const std::map<std::string, word>& symbols,
                                  word current_addr, std::string& error) {
    if (line.mnemonic == "ORG" || line.mnemonic == "EQU" ||
        line.mnemonic == "DEFL" || line.mnemonic == "END") {
        return 0; // no bytes emitted
    }
    if (line.mnemonic == "DEFB" || line.mnemonic == "DB" || line.mnemonic == "BYTE") {
        return count_defb_bytes(line.operands);
    }
    if (line.mnemonic == "DEFW" || line.mnemonic == "DW" || line.mnemonic == "WORD") {
        return count_comma_items(line.operands) * 2;
    }
    if (line.mnemonic == "DEFS" || line.mnemonic == "DS" || line.mnemonic == "RMEM") {
        // First operand is count
        std::string count_str = line.operands;
        auto comma = count_str.find(',');
        if (comma != std::string::npos) count_str = trim(count_str.substr(0, comma));
        int32_t count_val;
        if (!eval_expr(count_str, symbols, current_addr, count_val, error)) {
            // In pass 1, symbols might not be resolved yet
            error.clear();
            return 0; // will be recalculated in pass 2
        }
        return count_val;
    }
    return 0;
}

bool Z80Assembler::encode_directive(const Line& line,
                                     const std::map<std::string, word>& symbols,
                                     word current_addr, std::vector<byte>& output,
                                     std::string& error) {
    if (line.mnemonic == "ORG" || line.mnemonic == "EQU" ||
        line.mnemonic == "DEFL" || line.mnemonic == "END") {
        return true; // handled externally
    }

    if (line.mnemonic == "DEFB" || line.mnemonic == "DB" || line.mnemonic == "BYTE") {
        // Parse comma-separated list of bytes and strings
        bool in_string = false;
        std::string current_expr;
        for (size_t i = 0; i <= line.operands.size(); i++) {
            char c = (i < line.operands.size()) ? line.operands[i] : ','; // sentinel
            if (c == '"' && !in_string) {
                in_string = true;
                continue;
            }
            if (c == '"' && in_string) {
                in_string = false;
                continue;
            }
            if (in_string) {
                output.push_back(static_cast<byte>(c));
                continue;
            }
            if (c == ',' || i == line.operands.size()) {
                std::string expr = trim(current_expr);
                current_expr.clear();
                if (expr.empty()) continue;
                int32_t val;
                if (!eval_expr(expr, symbols, current_addr + static_cast<word>(output.size()), val, error)) {
                    error = "in DEFB: " + error;
                    return false;
                }
                output.push_back(static_cast<byte>(val & 0xFF));
                continue;
            }
            current_expr += c;
        }
        return true;
    }

    if (line.mnemonic == "DEFW" || line.mnemonic == "DW" || line.mnemonic == "WORD") {
        // Parse comma-separated 16-bit values
        std::string current_expr;
        for (size_t i = 0; i <= line.operands.size(); i++) {
            char c = (i < line.operands.size()) ? line.operands[i] : ',';
            if (c == ',' || i == line.operands.size()) {
                std::string expr = trim(current_expr);
                current_expr.clear();
                if (expr.empty()) continue;
                int32_t val;
                if (!eval_expr(expr, symbols, current_addr + static_cast<word>(output.size()), val, error)) {
                    error = "in DEFW: " + error;
                    return false;
                }
                output.push_back(static_cast<byte>(val & 0xFF));
                output.push_back(static_cast<byte>((val >> 8) & 0xFF));
                continue;
            }
            current_expr += c;
        }
        return true;
    }

    if (line.mnemonic == "DEFS" || line.mnemonic == "DS" || line.mnemonic == "RMEM") {
        std::string count_str = line.operands;
        byte fill = 0;
        auto comma = count_str.find(',');
        if (comma != std::string::npos) {
            std::string fill_str = trim(count_str.substr(comma + 1));
            count_str = trim(count_str.substr(0, comma));
            int32_t fill_val;
            if (!eval_expr(fill_str, symbols, current_addr, fill_val, error)) {
                error = "in DEFS fill: " + error;
                return false;
            }
            fill = static_cast<byte>(fill_val & 0xFF);
        }
        int32_t count_val;
        if (!eval_expr(count_str, symbols, current_addr, count_val, error)) {
            error = "in DEFS count: " + error;
            return false;
        }
        for (int i = 0; i < count_val; i++)
            output.push_back(fill);
        return true;
    }

    error = "unknown directive " + line.mnemonic;
    return false;
}

// ── Instruction encoding ──

// Build a mnemonic pattern from parsed mnemonic+operands for lookup in opcode table.
// E.g., mnemonic="LD", operands="A,(IX+5)" → tries patterns like "LD A,(IX+*)"
// Returns the matched opcode entry and the evaluated operand values.

struct OperandMatch {
    const Z80Opcode* opcode;
    std::vector<int32_t> operand_values; // resolved operand values in order
};

// Split operands by comma, respecting parentheses
static std::vector<std::string> split_operands(const std::string& operands) {
    std::vector<std::string> parts;
    if (operands.empty()) return parts;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < operands.size(); i++) {
        if (operands[i] == '(') depth++;
        if (operands[i] == ')') depth--;
        if (operands[i] == ',' && depth == 0) {
            parts.push_back(trim(operands.substr(start, i - start)));
            start = i + 1;
        }
    }
    parts.push_back(trim(operands.substr(start)));
    return parts;
}

// Try to match a mnemonic pattern from the opcode table.
// The idea: take "LD A,5" → try "LD A,*" → lookup.
// For indexed: "LD A,(IX+5)" → "LD A,(IX+*)"
// For 16-bit immediate: "LD BC,1234" → "LD BC,**"
// For indirect: "LD (1234),A" → "LD (**),A"
static bool try_match_opcode(const std::string& mnemonic, const std::string& operands,
                              const std::map<std::string, word>& symbols,
                              word current_addr,
                              OperandMatch& match, std::string& error) {
    z80_opcode_table_init();

    auto ops = split_operands(operands);

    // Build candidate patterns by trying to replace operand expressions with * or **
    // Strategy: for each operand part, try:
    //   1. Keep as-is (register name, flag)
    //   2. Replace with * (8-bit immediate/displacement)
    //   3. Replace with ** (16-bit immediate/address)
    //   4. For (expr), try (**) and (*)
    //   5. For (IX+expr), try (IX+*)
    //   6. For (IY+expr), try (IY+*)

    // First, try the mnemonic alone (no operands — NOP, RET, HALT, etc.)
    if (operands.empty()) {
        auto& results = z80_asm_lookup(mnemonic);
        if (!results.empty()) {
            match.opcode = results[0];
            return true;
        }
        error = "unknown instruction: " + mnemonic;
        return false;
    }

    // Generate all possible patterns
    struct PatternOp {
        std::string pattern;  // the text for this operand in the lookup pattern
        int32_t value;        // evaluated value (if this operand has one)
        bool has_value;       // whether this operand contributes an operand byte
    };

    // For each operand, generate possible replacements
    std::vector<std::vector<PatternOp>> candidates;
    for (auto& op_str : ops) {
        std::vector<PatternOp> alts;
        std::string upper_op = to_upper(op_str);

        // 1. As-is (for register names, conditions like NZ, Z, NC, C, PO, PE, P, M)
        alts.push_back({upper_op, 0, false});

        // 2. Check for (IX+expr) or (IY+expr)
        if (upper_op.size() >= 5 && upper_op[0] == '(' &&
            (upper_op.substr(1,2) == "IX" || upper_op.substr(1,2) == "IY")) {
            std::string reg = upper_op.substr(1, 2);
            // Find the +/- sign
            size_t sign_pos = 3;
            if (sign_pos < upper_op.size() && upper_op[sign_pos] == '+') {
                // (IX+expr) → (IX+*)
                std::string expr_part = op_str.substr(4, op_str.size() - 5); // strip (IX+ and )
                int32_t val;
                std::string eval_err;
                if (Z80Assembler::eval_expr(expr_part, symbols, current_addr, val, eval_err)) {
                    alts.push_back({"(" + reg + "+*)", val, true});
                }
            } else if (sign_pos < upper_op.size() && upper_op[sign_pos] == '-') {
                // (IX-expr) → (IX+*) with negated value
                std::string expr_part = op_str.substr(4, op_str.size() - 5);
                int32_t val;
                std::string eval_err;
                if (Z80Assembler::eval_expr(expr_part, symbols, current_addr, val, eval_err)) {
                    alts.push_back({"(" + reg + "+*)", -val, true});
                }
            } else if (upper_op.back() == ')') {
                // (IX) with no displacement → (IX+*) with 0
                alts.push_back({"(" + reg + "+*)", 0, true});
            }
        }

        // 3. Check for (expr) — indirect addressing
        if (upper_op.size() >= 3 && upper_op[0] == '(' && upper_op.back() == ')') {
            std::string inner = op_str.substr(1, op_str.size() - 2);
            std::string upper_inner = to_upper(inner);
            // Skip if it's a register indirect: (HL), (BC), (DE), (SP), (C), (IX), (IY)
            if (upper_inner != "HL" && upper_inner != "BC" && upper_inner != "DE" &&
                upper_inner != "SP" && upper_inner != "C" &&
                upper_inner.substr(0,2) != "IX" && upper_inner.substr(0,2) != "IY") {
                int32_t val;
                std::string eval_err;
                if (Z80Assembler::eval_expr(inner, symbols, current_addr, val, eval_err)) {
                    alts.push_back({"(**)", val, true});
                    alts.push_back({"(*)", val, true});
                }
            }
        }

        // 4. Try as numeric expression → * or **
        if (!upper_op.empty() && upper_op[0] != '(') {
            // Not a register?
            if (!is_register(upper_op)) {
                int32_t val;
                std::string eval_err;
                if (Z80Assembler::eval_expr(op_str, symbols, current_addr, val, eval_err)) {
                    alts.push_back({"*", val, true});
                    alts.push_back({"**", val, true});
                }
            }
        }

        candidates.push_back(alts);
    }

    // Try all combinations of candidate patterns
    // For 1 operand: try each alt
    // For 2 operands: try each alt[0] × alt[1]
    // For 3 operands: rare but possible (e.g. "ld (ix+*),*" has 3 parts after split)

    auto try_lookup = [&](const std::string& pattern, const std::vector<PatternOp>& selected) -> bool {
        auto& results = z80_asm_lookup(pattern);
        if (results.empty()) return false;

        // Collect operand values in order
        std::vector<int32_t> vals;
        for (auto& sel : selected) {
            if (sel.has_value) vals.push_back(sel.value);
        }
        match.opcode = results[0];
        match.operand_values = vals;
        return true;
    };

    if (candidates.size() == 1) {
        for (auto& alt : candidates[0]) {
            std::string pattern = mnemonic + " " + alt.pattern;
            if (try_lookup(pattern, {alt})) return true;
        }
    } else if (candidates.size() == 2) {
        for (auto& a0 : candidates[0]) {
            for (auto& a1 : candidates[1]) {
                std::string pattern = mnemonic + " " + a0.pattern + "," + a1.pattern;
                if (try_lookup(pattern, {a0, a1})) return true;
            }
        }
    } else if (candidates.size() == 3) {
        for (auto& a0 : candidates[0]) {
            for (auto& a1 : candidates[1]) {
                for (auto& a2 : candidates[2]) {
                    std::string pattern = mnemonic + " " + a0.pattern + "," + a1.pattern + "," + a2.pattern;
                    if (try_lookup(pattern, {a0, a1, a2})) return true;
                }
            }
        }
    }

    error = "cannot match instruction: " + mnemonic + " " + operands;
    return false;
}

int Z80Assembler::instruction_size(const std::string& mnemonic, const std::string& operands,
                                    std::string& error) {
    // Quick approach: try to match and use the opcode entry's length field
    OperandMatch match;
    std::map<std::string, word> dummy_syms;
    // Use a dummy match — if symbols aren't resolved, we can still determine
    // size from the pattern if we try with 0 for unknown values
    if (try_match_opcode(mnemonic, operands, dummy_syms, 0, match, error)) {
        return match.opcode->length;
    }
    // If match failed (probably forward ref), try heuristic
    // Most instructions are 1-4 bytes. For forward refs, we need at least
    // the mnemonic to guess size. Clear error — we'll catch real errors in pass 2.
    error.clear();

    // Try all patterns with dummy values substituted
    // Replace any non-register operand with a number
    auto ops = split_operands(operands);
    std::string new_operands;
    for (size_t i = 0; i < ops.size(); i++) {
        if (i > 0) new_operands += ",";
        std::string upper = to_upper(ops[i]);
        if (is_register(upper) || upper == "NZ" || upper == "Z" || upper == "NC" ||
            upper == "C" || upper == "PO" || upper == "PE" || upper == "P" || upper == "M" ||
            upper == "(C)" || upper == "(HL)" || upper == "(BC)" || upper == "(DE)" || upper == "(SP)") {
            new_operands += upper;
        } else if (upper[0] == '(' && (upper.substr(1,2) == "IX" || upper.substr(1,2) == "IY")) {
            new_operands += upper.substr(0, 3) + "+0)";
        } else if (upper[0] == '(') {
            new_operands += "(0)";
        } else {
            new_operands += "0";
        }
    }

    // Create a dummy symbol table with the missing symbols set to 0
    std::map<std::string, word> dummy_with_zeros;
    for (auto& op : ops) {
        std::string t = trim(op);
        if (!t.empty() && (isalpha(static_cast<unsigned char>(t[0])) || t[0] == '_' || t[0] == '.')) {
            if (!is_register(to_upper(t))) {
                dummy_with_zeros[to_upper(t)] = 0;
            }
        }
    }

    if (try_match_opcode(mnemonic, new_operands, dummy_with_zeros, 0, match, error)) {
        error.clear();
        return match.opcode->length;
    }
    error.clear();
    return 0; // unknown — will be caught in pass 2
}

bool Z80Assembler::encode_instruction(const std::string& mnemonic, const std::string& operands,
                                       const std::map<std::string, word>& symbols,
                                       word current_addr, std::vector<byte>& output,
                                       std::string& error) {
    OperandMatch match;
    if (!try_match_opcode(mnemonic, operands, symbols, current_addr, match, error))
        return false;

    auto* op = match.opcode;

    // Emit prefix bytes
    switch (op->prefix) {
        case OpcodePrefix::NONE:
            break;
        case OpcodePrefix::CB:
            output.push_back(0xCB);
            break;
        case OpcodePrefix::ED:
            output.push_back(0xED);
            break;
        case OpcodePrefix::DD:
            output.push_back(0xDD);
            break;
        case OpcodePrefix::FD:
            output.push_back(0xFD);
            break;
        case OpcodePrefix::DDCB:
            output.push_back(0xDD);
            output.push_back(0xCB);
            break;
        case OpcodePrefix::FDCB:
            output.push_back(0xFD);
            output.push_back(0xCB);
            break;
    }

    // For DDCB/FDCB: displacement comes BEFORE the opcode byte
    if (op->prefix == OpcodePrefix::DDCB || op->prefix == OpcodePrefix::FDCB) {
        if (match.operand_values.empty()) {
            error = "DDCB/FDCB instruction missing displacement";
            return false;
        }
        output.push_back(static_cast<byte>(match.operand_values[0] & 0xFF));
        output.push_back(op->opcode);
        return true;
    }

    // Emit opcode byte
    output.push_back(op->opcode);

    // Emit operand bytes
    if (op->is_relative && !match.operand_values.empty()) {
        // Relative jump: compute offset from (current_addr + instruction_length)
        int32_t target = match.operand_values[0];
        int32_t offset = target - (current_addr + op->length);
        if (offset < -128 || offset > 127) {
            error = "relative jump out of range (" + std::to_string(offset) + ")";
            return false;
        }
        output.push_back(static_cast<byte>(offset & 0xFF));
    } else {
        // Non-relative operands
        size_t val_idx = 0;
        // Count *'s in mnemonic to determine operand layout
        const char* m = op->mnemonic;
        while (*m) {
            if (*m == '*' && *(m+1) == '*') {
                // 16-bit operand
                if (val_idx >= match.operand_values.size()) {
                    error = "missing operand value";
                    return false;
                }
                int32_t val = match.operand_values[val_idx++];
                output.push_back(static_cast<byte>(val & 0xFF));
                output.push_back(static_cast<byte>((val >> 8) & 0xFF));
                m += 2;
            } else if (*m == '*') {
                // 8-bit operand
                if (val_idx >= match.operand_values.size()) {
                    error = "missing operand value";
                    return false;
                }
                int32_t val = match.operand_values[val_idx++];
                output.push_back(static_cast<byte>(val & 0xFF));
                m++;
            } else {
                m++;
            }
        }
    }

    return true;
}

// ── Two-pass assembly ──

bool Z80Assembler::pass1(std::vector<Line>& lines,
                          std::map<std::string, word>& symbols,
                          std::vector<AsmError>& errors) {
    word current_addr = 0;
    bool hit_end = false;

    for (auto& line : lines) {
        if (hit_end) break;

        // Handle label
        if (!line.label.empty()) {
            std::string upper_label = to_upper(line.label);
            if (line.mnemonic == "EQU" || line.mnemonic == "DEFL") {
                // Value will be set by EQU/DEFL handling below
            } else {
                if (symbols.count(upper_label) && line.mnemonic != "DEFL") {
                    errors.push_back({line.number, "duplicate label: " + line.label});
                } else {
                    symbols[upper_label] = current_addr;
                }
            }
        }

        if (line.mnemonic.empty()) continue;

        // Handle directives
        if (line.mnemonic == "ORG") {
            int32_t val;
            std::string err;
            if (eval_expr(line.operands, symbols, current_addr, val, err)) {
                current_addr = static_cast<word>(val);
            } else {
                errors.push_back({line.number, "ORG: " + err});
            }
            continue;
        }

        if (line.mnemonic == "EQU") {
            int32_t val;
            std::string err;
            if (eval_expr(line.operands, symbols, current_addr, val, err)) {
                std::string upper_label = to_upper(line.label);
                if (!upper_label.empty()) {
                    symbols[upper_label] = static_cast<word>(val);
                }
            } else {
                // Might have forward ref — defer to pass 2
            }
            continue;
        }

        if (line.mnemonic == "DEFL") {
            int32_t val;
            std::string err;
            if (eval_expr(line.operands, symbols, current_addr, val, err)) {
                std::string upper_label = to_upper(line.label);
                if (!upper_label.empty()) {
                    symbols[upper_label] = static_cast<word>(val);
                }
            }
            continue;
        }

        if (line.mnemonic == "END") {
            hit_end = true;
            continue;
        }

        // Calculate size
        std::string err;
        int size = 0;
        if (is_directive(line.mnemonic)) {
            size = directive_size(line, symbols, current_addr, err);
        } else {
            size = instruction_size(line.mnemonic, line.operands, err);
        }
        if (size < 0) {
            errors.push_back({line.number, err});
            continue;
        }
        current_addr += static_cast<word>(size);
    }

    return errors.empty();
}

bool Z80Assembler::pass2(const std::vector<Line>& lines,
                          const std::map<std::string, word>& symbols,
                          std::vector<AsmError>& errors,
                          bool write_memory,
                          word& start_addr, word& end_addr, int& bytes_written) {
    word current_addr = 0;
    bool first_byte = true;
    start_addr = 0;
    end_addr = 0;
    bytes_written = 0;
    bool hit_end = false;

    for (auto& line : lines) {
        if (hit_end) break;
        if (line.mnemonic.empty()) continue;

        // Handle ORG
        if (line.mnemonic == "ORG") {
            int32_t val;
            std::string err;
            if (eval_expr(line.operands, symbols, current_addr, val, err)) {
                current_addr = static_cast<word>(val);
            }
            continue;
        }

        // Skip EQU/DEFL/END (no output)
        if (line.mnemonic == "EQU" || line.mnemonic == "DEFL") continue;
        if (line.mnemonic == "END") { hit_end = true; continue; }

        // Encode
        std::vector<byte> output;
        std::string err;
        bool ok;

        if (is_directive(line.mnemonic)) {
            ok = encode_directive(line, symbols, current_addr, output, err);
        } else {
            ok = encode_instruction(line.mnemonic, line.operands, symbols, current_addr, output, err);
        }

        if (!ok) {
            errors.push_back({line.number, err});
            // Still need to advance address — try to estimate size
            std::string dummy_err;
            int size = is_directive(line.mnemonic)
                ? directive_size(line, symbols, current_addr, dummy_err)
                : instruction_size(line.mnemonic, line.operands, dummy_err);
            current_addr += static_cast<word>(std::max(size, 1));
            continue;
        }

        // Write output bytes
        if (!output.empty()) {
            if (first_byte) {
                start_addr = current_addr;
                first_byte = false;
            }
            for (byte b : output) {
                if (write_memory) {
                    z80_write_mem(current_addr, b);
                }
                current_addr++;
                bytes_written++;
            }
            end_addr = current_addr;
        }
    }

    return errors.empty();
}

// ── Public API ──

AsmResult Z80Assembler::assemble(const std::string& source) {
    z80_opcode_table_init();

    AsmResult result;
    result.success = false;
    result.start_addr = 0;
    result.end_addr = 0;
    result.bytes_written = 0;

    auto lines = parse(source);
    if (lines.empty()) {
        result.success = true;
        return result;
    }

    // Pass 1: resolve labels
    if (!pass1(lines, result.symbols, result.errors)) {
        return result;
    }

    // Pass 2: encode and write
    if (!pass2(lines, result.symbols, result.errors, true,
               result.start_addr, result.end_addr, result.bytes_written)) {
        return result;
    }

    result.success = true;
    return result;
}

AsmResult Z80Assembler::check(const std::string& source) {
    z80_opcode_table_init();

    AsmResult result;
    result.success = false;
    result.start_addr = 0;
    result.end_addr = 0;
    result.bytes_written = 0;

    auto lines = parse(source);
    if (lines.empty()) {
        result.success = true;
        return result;
    }

    if (!pass1(lines, result.symbols, result.errors)) {
        return result;
    }

    if (!pass2(lines, result.symbols, result.errors, false,
               result.start_addr, result.end_addr, result.bytes_written)) {
        return result;
    }

    result.success = true;
    return result;
}
