// konCePCja — symbol-file store for the disassembler and debugger.

#include "symfile.h"

#include <charconv>
#include <fstream>
#include <iomanip>
#include <string>

#include "log.h"
#include "stringutils.h"

namespace {

// Parses a "$xxxx" hex address field. Returns false when the field is not
// exactly a '$' followed by a valid 16-bit hex number.
bool parse_hex_addr(const std::string& field, word& addr) {
  if (field.size() < 2 || field[0] != '$') return false;
  const char* first = field.data() + 1;
  const char* last = field.data() + field.size();
  unsigned value = 0;
  auto [ptr, ec] = std::from_chars(first, last, value, 16);
  if (ec != std::errc() || ptr != last || value > 0xffff) return false;
  addr = static_cast<word>(value);
  return true;
}

}  // namespace

Symfile::Symfile(const std::string& filename) {
  std::ifstream infile(filename);
  if (!infile) {
    LOG_VERBOSE("Symbol file not readable: " << filename);
    return;
  }
  std::string line;
  while (std::getline(infile, line)) {
    // Strip any ';' comment, then tokenize.
    auto fields = stringutils::split(
        stringutils::trim(line.substr(0, line.find(';')), ' '), ' ',
        /*ignore_empty=*/true);
    if (fields.empty()) continue;

    const std::string& kind = fields[0];
    word addr = 0;
    if (kind == "al") {
      if (fields.size() < 3 || !parse_hex_addr(fields[1], addr) ||
          fields[2][0] != '.') {
        LOG_ERROR("Invalid `al` entry in " << filename << ": " << line);
        continue;
      }
      addSymbol(addr, fields[2].substr(1));
    } else if (kind == "b" || kind == "break") {
      if (fields.size() < 2 || !parse_hex_addr(fields[1], addr)) {
        LOG_ERROR("Invalid `" << kind << "` entry in " << filename << ": "
                              << line);
        continue;
      }
      addBreakpoint(addr);
    } else if (kind == "d") {
      if (fields.size() < 2 || !parse_hex_addr(fields[1], addr)) {
        LOG_ERROR("Invalid `d` entry in " << filename << ": " << line);
        continue;
      }
      addEntrypoint(addr);
    }
  }
}

bool Symfile::SaveTo(const std::string& filename) {
  std::ofstream outfile(filename, std::ios_base::trunc);
  if (!outfile) {
    LOG_ERROR("Couldn't write symbol file: " << filename);
    return false;
  }
  outfile << std::hex << std::setfill('0');
  outfile << "; labels\n";
  for (const auto& [addr, sym] : symbols) {
    outfile << "al  $" << std::setw(4) << addr << " ." << sym << "\n";
  }
  outfile << "; breakpoints\n";
  for (auto addr : breakpoints) {
    outfile << "b  $" << std::setw(4) << addr << "\n";
  }
  outfile << "; entrypoints\n";
  for (auto addr : entrypoints) {
    outfile << "d  $" << std::setw(4) << addr << "\n";
  }
  outfile.close();
  if (!outfile) {
    LOG_ERROR("I/O error while writing symbol file: " << filename);
    return false;
  }
  return true;
}

void Symfile::addBreakpoint(word addr) { breakpoints.push_back(addr); }

void Symfile::addEntrypoint(word addr) { entrypoints.push_back(addr); }

void Symfile::addSymbol(word addr, const std::string& symbol) {
  // Remove old reverse mapping if this address already had a symbol
  auto it = symbols.find(addr);
  if (it != symbols.end()) {
    name_to_addr.erase(it->second);
  }
  symbols[addr] = symbol;
  name_to_addr[symbol] = addr;
}

void Symfile::delSymbol(const std::string& name) {
  auto it = name_to_addr.find(name);
  if (it != name_to_addr.end()) {
    symbols.erase(it->second);
    name_to_addr.erase(it);
  }
}

std::string Symfile::lookupAddr(word addr) const {
  auto it = symbols.find(addr);
  if (it != symbols.end()) return it->second;
  return "";
}

int Symfile::lookupName(const std::string& name, word& addr) const {
  auto it = name_to_addr.find(name);
  if (it != name_to_addr.end()) {
    addr = it->second;
    return 0;
  }
  return -1;
}

std::vector<std::pair<word, std::string>> Symfile::listSymbols(
    const std::string& filter) const {
  std::vector<std::pair<word, std::string>> result;
  for (const auto& [addr, name] : symbols) {
    if (filter.empty() || name.find(filter) != std::string::npos) {
      result.emplace_back(addr, name);
    }
  }
  return result;
}

void Symfile::clear() {
  symbols.clear();
  name_to_addr.clear();
  breakpoints.clear();
  entrypoints.clear();
}
