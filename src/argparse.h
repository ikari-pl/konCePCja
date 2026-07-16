#pragma once

// konCePCja — command-line parsing for the emulator binary.

#include <map>
#include <string>
#include <vector>

class CapriceArgs {
 public:
  CapriceArgs() = default;
  std::string autocmd;
  std::string cfgFilePath;
  std::string binFile;
  size_t binOffset = 0x6000;  // documented default for -i without -o
  std::map<std::string, std::map<std::string, std::string>> cfgOverrides;
  std::string symFilePath;
  bool headless = false;
  std::string exitAfter;  // e.g. "100f", "5s", "3000ms"
  bool exitOnBreak = false;
  bool debug = false;
  bool fps = false;  // --fps: log once-per-second FPS to stdout
};

// Expands KONCPC_*/CPC_* keywords in an autocmd string into the internal
// keystroke escape sequences understood by the autotype queue.
std::string replaceKoncpcKeys(std::string command);

// Parses argv into `args`; every non-option argument lands in `slot_list`.
// Exits the process for --help/--version/--list-plugins and on bad usage.
void parseArguments(int argc, char** argv, std::vector<std::string>& slot_list,
                    CapriceArgs& args);
