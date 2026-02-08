#ifndef _SYMFILE_H
#define _SYMFILE_H

#include <map>
#include <string>
#include <utility>
#include <vector>
#include "types.h"

class Symfile
{
  public:
    Symfile() = default;
    explicit Symfile(const std::string& filename);

    bool SaveTo(const std::string& filename);

    void addBreakpoint(word addr);
    void addEntrypoint(word addr);
    void addSymbol(word addr, const std::string& symbol);
    void delSymbol(const std::string& name);

    std::string lookupAddr(word addr) const;                          // → name or ""
    int lookupName(const std::string& name, word& addr) const;       // → 0 on success
    std::vector<std::pair<word, std::string>> listSymbols(const std::string& filter = "") const;
    void clear();

    std::map<word, std::string> Symbols() { return symbols; };
    std::vector<word> Breakpoints() { return breakpoints; };
    std::vector<word> Entrypoints() { return entrypoints; };

  private:
    std::vector<word> breakpoints;
    std::vector<word> entrypoints;
    std::map<word, std::string> symbols;
    std::map<std::string, word> name_to_addr;
};

// Global symbol table
extern Symfile g_symfile;

#endif
