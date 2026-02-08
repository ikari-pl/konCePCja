#include "symfile.h"

#include <fstream>
#include <iomanip>
#include <string>
#include "stringutils.h"
#include "log.h"

Symfile::Symfile(const std::string& filename)
{
  std::ifstream infile(filename);
  std::string line;
  while (std::getline(infile, line))
  {
    // Remove any comment
    line = stringutils::trim(line.substr(0, line.find(';')), ' ');
    auto elems = stringutils::split(line, ' ', /*ignore_empty=*/true);
    if (elems.empty()) continue;
    if (elems[0] == "al") {
      if (elems.size() < 3 || elems[1][0] != '$' || elems[2][0] != '.') {
        LOG_ERROR("Invalid `al` entry in " << filename << ": " << line);
        continue;
      }
      word addr = std::stol(elems[1].substr(1), nullptr, 16);
      addSymbol(addr, elems[2].substr(1));
    }
    if (elems[0] == "b" or elems[0] == "break") {
      if (elems.size() < 2 || elems[1][0] != '$') {
        LOG_ERROR("Invalid `al` entry in " << filename << ": " << line);
        continue;
      }
      word addr = std::stol(elems[1].substr(1), nullptr, 16);
      addBreakpoint(addr);
    }
    if (elems[0] == "d") {
      if (elems.size() < 2 || elems[1][0] != '$') {
        LOG_ERROR("Invalid `al` entry in " << filename << ": " << line);
        continue;
      }
      word addr = std::stol(elems[1].substr(1), nullptr, 16);
      addEntrypoint(addr);
    }
  }
}

bool Symfile::SaveTo(const std::string& filename)
{
  std::ofstream outfile;
  outfile.open(filename, std::ios_base::trunc);
  outfile << "; labels" << std::endl;
  for (const auto& [addr, sym] : symbols)
  {
    outfile << "al  $" << std::hex << std::setw(4) << std::setfill('0') << addr << " ." << sym << std::endl;
  }
  outfile << "; breakpoints" << std::endl;
  for (auto addr : breakpoints)
  {
    outfile << "b  $" << std::hex << std::setw(4) << std::setfill('0') << addr << std::endl;
  }
  outfile << "; entrypoints" << std::endl;
  for (auto addr : entrypoints)
  {
    outfile << "d  $" << std::hex << std::setw(4) << std::setfill('0') << addr << std::endl;
  }
  outfile.close();
  return true;
}

void Symfile::addBreakpoint(word addr)
{
  breakpoints.push_back(addr);
}

void Symfile::addEntrypoint(word addr)
{
  entrypoints.push_back(addr);
}

void Symfile::addSymbol(word addr, const std::string& symbol)
{
  // Remove old reverse mapping if this address already had a symbol
  auto it = symbols.find(addr);
  if (it != symbols.end()) {
    name_to_addr.erase(it->second);
  }
  symbols[addr] = symbol;
  name_to_addr[symbol] = addr;
}

void Symfile::delSymbol(const std::string& name)
{
  auto it = name_to_addr.find(name);
  if (it != name_to_addr.end()) {
    symbols.erase(it->second);
    name_to_addr.erase(it);
  }
}

std::string Symfile::lookupAddr(word addr) const
{
  auto it = symbols.find(addr);
  if (it != symbols.end()) return it->second;
  return "";
}

int Symfile::lookupName(const std::string& name, word& addr) const
{
  auto it = name_to_addr.find(name);
  if (it != name_to_addr.end()) {
    addr = it->second;
    return 0;
  }
  return -1;
}

std::vector<std::pair<word, std::string>> Symfile::listSymbols(const std::string& filter) const
{
  std::vector<std::pair<word, std::string>> result;
  for (const auto& [addr, name] : symbols) {
    if (filter.empty() || name.find(filter) != std::string::npos) {
      result.emplace_back(addr, name);
    }
  }
  return result;
}

void Symfile::clear()
{
  symbols.clear();
  name_to_addr.clear();
  breakpoints.clear();
  entrypoints.clear();
}
