// konCePCja — symbol-file store for the disassembler and debugger.

#include "symfile.h"

#include <charconv>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

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
    readable_ = false;
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
  // Copied under the lock and written without it: a slow disk must not hold
  // up the threads that look symbols up while they draw.
  std::map<word, std::string> labels;
  std::vector<word> breaks;
  std::vector<word> entries;
  {
    std::scoped_lock const lock(mutex_);
    labels = symbols;
    breaks = breakpoints;
    entries = entrypoints;
  }
  std::ofstream outfile(filename, std::ios_base::trunc);
  if (!outfile) {
    LOG_ERROR("Couldn't write symbol file: " << filename);
    return false;
  }
  outfile << std::hex << std::setfill('0');
  outfile << "; labels\n";
  for (const auto& [addr, sym] : labels) {
    outfile << "al  $" << std::setw(4) << addr << " ." << sym << "\n";
  }
  outfile << "; breakpoints\n";
  for (auto addr : breaks) {
    outfile << "b  $" << std::setw(4) << addr << "\n";
  }
  outfile << "; entrypoints\n";
  for (auto addr : entries) {
    outfile << "d  $" << std::setw(4) << addr << "\n";
  }
  outfile.close();
  if (!outfile) {
    LOG_ERROR("I/O error while writing symbol file: " << filename);
    return false;
  }
  return true;
}

void Symfile::addBreakpoint(word addr) {
  std::scoped_lock const lock(mutex_);
  breakpoints.push_back(addr);
}

void Symfile::addEntrypoint(word addr) {
  std::scoped_lock const lock(mutex_);
  entrypoints.push_back(addr);
}

void Symfile::add_locked(word addr, const std::string& symbol) {
  // The name this address had is gone.
  auto it = symbols.find(addr);
  if (it != symbols.end()) {
    name_to_addr.erase(it->second);
  }
  // And so is the address this name had: left in `symbols`, it would be a
  // second entry of the same name that delSymbol() could never reach.
  auto named = name_to_addr.find(symbol);
  if (named != name_to_addr.end() && named->second != addr) {
    symbols.erase(named->second);
  }
  symbols[addr] = symbol;
  name_to_addr[symbol] = addr;
}

void Symfile::changed_locked() {
  ++generation_;
  if (changed_hook_ != nullptr) changed_hook_(generation_);
}

void Symfile::addSymbol(word addr, const std::string& symbol) {
  std::scoped_lock const lock(mutex_);
  add_locked(addr, symbol);
  changed_locked();
}

size_t Symfile::addSymbols(const Symfile& other) {
  if (&other == this) return 0;
  std::scoped_lock const lock(mutex_, other.mutex_);
  for (const auto& [addr, name] : other.symbols) add_locked(addr, name);
  if (!other.symbols.empty()) changed_locked();
  return other.symbols.size();
}

size_t Symfile::addSymbols(
    const std::vector<std::pair<word, std::string>>& batch) {
  std::scoped_lock const lock(mutex_);
  for (const auto& [addr, name] : batch) add_locked(addr, name);
  if (!batch.empty()) changed_locked();
  return batch.size();
}

bool Symfile::delSymbol(const std::string& name) {
  std::scoped_lock const lock(mutex_);
  auto it = name_to_addr.find(name);
  if (it == name_to_addr.end()) return false;
  symbols.erase(it->second);
  name_to_addr.erase(it);
  changed_locked();
  return true;
}

std::string Symfile::lookupAddr(word addr) const {
  std::scoped_lock const lock(mutex_);
  auto it = symbols.find(addr);
  if (it != symbols.end()) return it->second;
  return "";
}

int Symfile::lookupName(const std::string& name, word& addr) const {
  std::scoped_lock const lock(mutex_);
  auto it = name_to_addr.find(name);
  if (it != name_to_addr.end()) {
    addr = it->second;
    return 0;
  }
  return -1;
}

std::vector<std::pair<word, std::string>> Symfile::listSymbols(
    const std::string& filter) const {
  std::scoped_lock const lock(mutex_);
  std::vector<std::pair<word, std::string>> result;
  for (const auto& [addr, name] : symbols) {
    if (filter.empty() || name.find(filter) != std::string::npos) {
      result.emplace_back(addr, name);
    }
  }
  return result;
}

std::vector<std::pair<word, std::string>> Symfile::listSymbols(
    uint64_t& generation) const {
  std::scoped_lock const lock(mutex_);
  generation = generation_;
  return {symbols.begin(), symbols.end()};
}

void Symfile::clear() {
  std::scoped_lock const lock(mutex_);
  bool const had_symbols = !symbols.empty();
  symbols.clear();
  name_to_addr.clear();
  breakpoints.clear();
  entrypoints.clear();
  if (had_symbols) changed_locked();
}

uint64_t Symfile::generation() const {
  std::scoped_lock const lock(mutex_);
  return generation_;
}

void Symfile::set_changed_hook(ChangedHook hook) {
  std::scoped_lock const lock(mutex_);
  changed_hook_ = hook;
}
