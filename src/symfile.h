#pragma once

// konCePCja — symbol-file store for the disassembler and debugger.
//
// File format (one entry per line, ';' starts a comment):
//   al $c000 .label     address label
//   b  $c000            breakpoint  ("break" also accepted)
//   d  $c000            disassembly entry point

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "types.h"

// The table is shared by threads: the DevTools read it while they draw, the
// text IPC and the KCPC server edit it from their own. Every method takes the
// table's lock, so a call is safe from any thread; the three accessors that
// hand out references are for a table no other thread can reach (one just
// read from a file).
class Symfile {
 public:
  Symfile() = default;
  explicit Symfile(const std::string& filename);

  // False when the file the table was built from could not be opened.
  bool readable() const { return readable_; }

  bool SaveTo(const std::string& filename);

  void addBreakpoint(word addr);
  void addEntrypoint(word addr);
  // A name stands for one address and an address has one name: adding a
  // name that is elsewhere moves it, and a second name for an address
  // replaces the first.
  void addSymbol(word addr, const std::string& symbol);
  // Adds every symbol of `other` as one change: one generation, one call of
  // the hook. Returns how many were added.
  size_t addSymbols(const Symfile& other);
  size_t addSymbols(const std::vector<std::pair<word, std::string>>& batch);
  // False when there is no symbol of that name; nothing changed then.
  bool delSymbol(const std::string& name);

  std::string lookupAddr(word addr) const;                    // → name or ""
  int lookupName(const std::string& name, word& addr) const;  // → 0 on success
  std::vector<std::pair<word, std::string>> listSymbols(
      const std::string& filter = "") const;
  // The symbols and the generation they belong to, taken together.
  std::vector<std::pair<word, std::string>> listSymbols(
      uint64_t& generation) const;
  void clear();

  // Grows with every change of the symbols; 0 for a table nothing was ever
  // added to.
  uint64_t generation() const;
  // Called after every change of the symbols with the new generation, under
  // the table's lock, on the thread that made the change: it must be quick
  // and must not call back into the table. One hook; nullptr removes it.
  using ChangedHook = void (*)(uint64_t generation);
  void set_changed_hook(ChangedHook hook);

  const std::map<word, std::string>& Symbols() const { return symbols; }
  const std::vector<word>& Breakpoints() const { return breakpoints; }
  const std::vector<word>& Entrypoints() const { return entrypoints; }

 private:
  void add_locked(word addr, const std::string& symbol);
  void changed_locked();

  mutable std::mutex mutex_;
  bool readable_ = true;
  uint64_t generation_ = 0;
  ChangedHook changed_hook_ = nullptr;
  std::vector<word> breakpoints;
  std::vector<word> entrypoints;
  std::map<word, std::string> symbols;
  std::map<std::string, word> name_to_addr;  // reverse index of `symbols`
};

// Global symbol table
extern Symfile g_symfile;
