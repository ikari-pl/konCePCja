#pragma once

// konCePCja — symbol-file store for the disassembler and debugger.
//
// File format (one entry per line, ';' starts a comment):
//   al $c000 .label     address label
//   b  $c000            breakpoint  ("break" also accepted)
//   d  $c000            disassembly entry point

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "types.h"

class Symfile {
 public:
  Symfile() = default;
  explicit Symfile(const std::string& filename);

  bool SaveTo(const std::string& filename);

  void addBreakpoint(word addr);
  void addEntrypoint(word addr);
  void addSymbol(word addr, const std::string& symbol);
  void delSymbol(const std::string& name);

  std::string lookupAddr(word addr) const;                    // → name or ""
  int lookupName(const std::string& name, word& addr) const;  // → 0 on success
  std::vector<std::pair<word, std::string>> listSymbols(
      const std::string& filter = "") const;
  void clear();

  const std::map<word, std::string>& Symbols() const { return symbols; }
  const std::vector<word>& Breakpoints() const { return breakpoints; }
  const std::vector<word>& Entrypoints() const { return entrypoints; }

 private:
  std::vector<word> breakpoints;
  std::vector<word> entrypoints;
  std::map<word, std::string> symbols;
  std::map<std::string, word> name_to_addr;  // reverse index of `symbols`
};

// Global symbol table
extern Symfile g_symfile;
