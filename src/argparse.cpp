// konCePCja — command-line parsing for the emulator binary.
//
// Self-contained C++17 parser (no getopt dependency, no global parser
// state).  Behaviour kept from the historical CLI:
//   * short options may cluster (-vD) and take attached values (-ofoo),
//   * long options accept --name=value or --name value, and unambiguous
//     prefixes (--head → --headless),
//   * options and slot files may interleave; "--" ends option parsing.

#include "argparse.h"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "keyboard.h"
#include "koncepcja.h"
#include "log.h"
#include "stringutils.h"
#include "video_host.h"

namespace {

struct OptionSpec {
  char short_name;        // unique key, also the short flag
  std::string_view long_name;
  bool takes_value;
};

constexpr OptionSpec kOptions[] = {
    {'a', "autocmd", true},      {'B', "exit-on-break", false},
    {'c', "cfg_file", true},     {'D', "debug", false},
    {'E', "exit-after", true},   {'F', "fps", false},
    {'H', "headless", false},    {'h', "help", false},
    {'i', "inject", true},       {'L', "list-plugins", false},
    {'o', "offset", true},       {'O', "override", true},
    {'s', "sym_file", true},     {'V', "version", false},
    {'v', "verbose", false},
};

const OptionSpec* findShort(char name) {
  for (const auto& spec : kOptions) {
    if (spec.short_name == name) return &spec;
  }
  return nullptr;
}

// Exact match wins; otherwise a unique prefix matches (getopt_long parity).
const OptionSpec* findLong(std::string_view name) {
  const OptionSpec* prefix_match = nullptr;
  bool ambiguous = false;
  for (const auto& spec : kOptions) {
    if (spec.long_name == name) return &spec;
    if (spec.long_name.substr(0, name.size()) == name) {
      if (prefix_match) ambiguous = true;
      prefix_match = &spec;
    }
  }
  return ambiguous ? nullptr : prefix_match;
}

void usage(std::ostream& os, const char* progPath, int errcode) {
  std::string progname, dirname;

  stringutils::splitPath(progPath, dirname, progname);

  os << "Usage: " << progname << " [options] <slotfile(s)>\n";
  os << "\nSupported options are:\n";
  os << "   -a/--autocmd=<command>: execute command after CPC boots "
        "(repeatable).\n";
  os << "      Supports WinAPE ~KEY~ syntax: ~ENTER~, ~CLR~, ~PAUSE 50~, "
        "~+SHIFT~, etc.\n";
  os << "      Also accepts KONCPC_EXIT, KONCPC_WAITBREAK, KONCPC_RESET and "
        "other\n";
  os << "      emulator commands (see docs/ipc-protocol.md for the full "
        "list).\n";
  os << "   -B/--exit-on-break:     exit with code 1 when a breakpoint is hit "
        "(instead of pausing).\n";
  os << "   -c/--cfg_file=<file>:   use <file> as the emulator configuration "
        "file instead of the default.\n";
  os << "   -E/--exit-after=<spec>: exit after N frames (e.g. 100f), seconds "
        "(5s), or milliseconds (3000ms).\n";
  os << "   -H/--headless:          run without display or audio (IPC and "
        "emulation only).\n";
  os << "   -h/--help:              shows this help\n";
  os << "   -i/--inject=<file>:     inject a binary in memory after the CPC "
        "startup finishes\n";
  os << "   -L/--list-plugins:      list all video plugins (index: name) and "
        "exit\n";
  os << "   -o/--offset=<address>:  offset at which to inject the binary "
        "provided with -i (default: 0x6000)\n";
  os << "   -O/--override:          override an option from the config. Can be "
        "repeated. (example: -O system.model=3)\n";
  os << "   -s/--sym_file=<file>:   use <file> as a source of symbols and "
        "entry points for disassembling in developers' tools.\n";
  os << "   -V/--version:           outputs version and exit\n";
  os << "   -v/--verbose:           be talkative\n";
  os << "   -D/--debug:             show frame timing and audio diagnostics in "
        "DevTools bar\n";
  os << "   --fps:                  log once-per-second FPS to stdout (e.g. "
        "'[fps] 50 FPS 100% speed')\n";
  os << "\nslotfiles is an optional list of files giving the content of the "
        "various CPC ports.\n";
  os << "Ports files are identified by their extension. Supported formats are "
        ".dsk (disk), .cdt or .voc (tape), .cpr (cartridge), .sna (snapshot), "
        "or .zip (archive containing one or more of the supported ports "
        "files).\n";
  os << "\nExample: " << progname << " sorcery.dsk\n";
  os << "\nPress F1 when the emulator is running to show the in-application "
        "option menu.\n";
  os << "\nSee https://github.com/ikari/konCePCja or check the man page (man "
        "koncepcja) for more extensive information.\n";
  exit(errcode);
}

void listVideoPlugins() {
  // List all video plugins (index: name).  Helps users recover from
  // scr_style index shifts after plugin-list edits (e.g. Phase 7b removed
  // the legacy GL CRT plugins, shifting every plugin after them down by
  // three slots).
  size_t last = video_plugin_list.empty() ? 0 : video_plugin_list.size() - 1;
  int width = 1;
  for (size_t v = last; v >= 10; v /= 10) ++width;
  for (size_t i = 0; i < video_plugin_list.size(); ++i) {
    std::cout << std::setw(width) << i << ": " << video_plugin_list[i].name
              << '\n';
  }
  exit(0);
}

void printVersion() {
  std::cout << "konCePCja " << VERSION_STRING;
#ifdef HASH
  std::cout << (std::string(HASH).empty() ? "" : "-" + std::string(HASH));
#endif
  std::cout << "\n";

  std::cout << "APP_PATH: ";
#ifdef APP_PATH
  std::cout << APP_PATH;
#else
  std::cout << "Not provided";
#endif
  std::cout << std::endl;

  std::cout << "Compiled with:"
#ifdef DEBUG
            << " DEBUG"
#endif
            << "\n";

  std::cout << "Number of video plugins available: "
            << video_plugin_list.size() << std::endl;
  exit(0);
}

// -O section.item=value
void applyOverride(std::string_view opt, CapriceArgs& args) {
  // Validate both separators before slicing so we never index past a
  // missing '=' / '.'.
  auto key_value_separator = opt.find('=');
  std::string_view key = key_value_separator == std::string_view::npos
                             ? std::string_view()
                             : opt.substr(0, key_value_separator);
  auto section_item_separator = key.find('.');
  if (key_value_separator == std::string_view::npos ||
      section_item_separator == std::string_view::npos) {
    LOG_ERROR("Couldn't parse override: '" << opt << "'");
    return;
  }
  std::string section(key.substr(0, section_item_separator));
  std::string item(key.substr(section_item_separator + 1));
  std::string value(opt.substr(key_value_separator + 1));
  if (section.empty() || item.empty()) {
    LOG_ERROR("Couldn't parse override: '" << opt << "'");
  } else {
    LOG_INFO("Override configuration: " << section << "." << item << " = "
                                        << value);
    args.cfgOverrides[std::move(section)][std::move(item)] = std::move(value);
  }
}

// Applies one recognized option. `value` is empty for flag options.
void applyOption(char name, const std::string& value, char* progname,
                 CapriceArgs& args) {
  switch (name) {
    case 'a':
      LOG_VERBOSE("Append to autocmd: " << value);
      args.autocmd += replaceKoncpcKeys(value);
      args.autocmd += "\n";
      break;
    case 'B':
      args.exitOnBreak = true;
      break;
    case 'c':
      args.cfgFilePath = value;
      break;
    case 'D':
      args.debug = true;
      break;
    case 'E':
      args.exitAfter = value;
      break;
    case 'F':
      args.fps = true;
      break;
    case 'H':
      args.headless = true;
      break;
    case 'h':
      usage(std::cout, progname, 0);
      break;
    case 'i':
      args.binFile = value;
      break;
    case 'L':
      listVideoPlugins();
      break;
    case 'o': {
      // Base auto-detected: 0x/0 prefixes work (strtol base 0).
      char* end = nullptr;
      long offset = std::strtol(value.c_str(), &end, 0);
      if (end == value.c_str() || *end != '\0' || offset < 0) {
        LOG_ERROR("Invalid injection offset: '" << value << "'");
        usage(std::cerr, progname, 1);
      }
      args.binOffset = static_cast<size_t>(offset);
      break;
    }
    case 'O':
      applyOverride(value, args);
      break;
    case 's':
      args.symFilePath = value;
      break;
    case 'V':
      printVersion();
      break;
    case 'v':
      log_verbose = true;
      break;
    default:
      usage(std::cerr, progname, 1);
      break;
  }
}

std::string koncpc_keystroke(KONCPC_KEYS key) {
  return std::string("\f") + static_cast<char>(key);
}

std::string cpc_keystroke(CPC_KEYS key) {
  return std::string("\a") + static_cast<char>(key);
}

}  // namespace

std::string replaceKoncpcKeys(std::string command) {
  static const std::map<std::string, std::string> keyNames = {
      {"KONCPC_EXIT", koncpc_keystroke(KONCPC_EXIT)},
      {"KONCPC_FPS", koncpc_keystroke(KONCPC_FPS)},
      {"KONCPC_FULLSCRN", koncpc_keystroke(KONCPC_FULLSCRN)},
      {"KONCPC_GUI", koncpc_keystroke(KONCPC_GUI)},
      {"KONCPC_VKBD", koncpc_keystroke(KONCPC_VKBD)},
      {"KONCPC_JOY", koncpc_keystroke(KONCPC_JOY)},
      {"KONCPC_PHAZER", koncpc_keystroke(KONCPC_PHAZER)},
      {"KONCPC_MF2STOP", koncpc_keystroke(KONCPC_MF2STOP)},
      {"KONCPC_RESET", koncpc_keystroke(KONCPC_RESET)},
      {"KONCPC_SCRNSHOT", koncpc_keystroke(KONCPC_SCRNSHOT)},
      {"KONCPC_SPEED", koncpc_keystroke(KONCPC_SPEED)},
      {"KONCPC_TAPEPLAY", koncpc_keystroke(KONCPC_TAPEPLAY)},
      {"KONCPC_DEBUG", koncpc_keystroke(KONCPC_DEBUG)},
      {"KONCPC_WAITBREAK", koncpc_keystroke(KONCPC_WAITBREAK)},
      {"KONCPC_DELAY", koncpc_keystroke(KONCPC_DELAY)},
      {"KONCPC_PASTE", koncpc_keystroke(KONCPC_PASTE)},
      {"KONCPC_DEVTOOLS", koncpc_keystroke(KONCPC_DEVTOOLS)},
      {"CPC_F1", cpc_keystroke(CPC_F1)},
      {"CPC_F2", cpc_keystroke(CPC_F2)},
  };
  for (const auto& [keyword, keystroke] : keyNames) {
    size_t pos;
    while ((pos = command.find(keyword)) != std::string::npos) {
      command.replace(pos, keyword.size(), keystroke);
      LOG_VERBOSE("Recognized keyword: " << keyword);
    }
  }
  return command;
}

void parseArguments(int argc, char** argv, std::vector<std::string>& slot_list,
                    CapriceArgs& args) {
  slot_list.clear();

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];

    if (arg == "--") {  // end of options: the rest is slot files
      slot_list.insert(slot_list.end(), argv + i + 1, argv + argc);
      break;
    }

    if (arg.size() >= 3 && arg.substr(0, 2) == "--") {  // long option
      std::string_view body = arg.substr(2);
      auto eq = body.find('=');
      std::string_view name =
          eq == std::string_view::npos ? body : body.substr(0, eq);
      const OptionSpec* spec = findLong(name);
      if (!spec) {
        LOG_ERROR("Unrecognized or ambiguous option: '" << arg << "'");
        usage(std::cerr, argv[0], 1);
        return;
      }
      std::string value;
      if (spec->takes_value) {
        if (eq != std::string_view::npos) {
          value = std::string(body.substr(eq + 1));
        } else if (i + 1 < argc) {
          value = argv[++i];
        } else {
          LOG_ERROR("Option '" << arg << "' requires a value");
          usage(std::cerr, argv[0], 1);
          return;
        }
      } else if (eq != std::string_view::npos) {
        LOG_ERROR("Option '--" << name << "' doesn't take a value");
        usage(std::cerr, argv[0], 1);
        return;
      }
      applyOption(spec->short_name, value, argv[0], args);
      continue;
    }

    if (arg.size() >= 2 && arg[0] == '-') {  // short option(s), may cluster
      for (size_t pos = 1; pos < arg.size(); ++pos) {
        const OptionSpec* spec = findShort(arg[pos]);
        if (!spec) {
          LOG_ERROR("Unrecognized option: '-" << arg[pos] << "'");
          usage(std::cerr, argv[0], 1);
          return;
        }
        if (!spec->takes_value) {
          applyOption(spec->short_name, "", argv[0], args);
          continue;
        }
        std::string value;
        if (pos + 1 < arg.size()) {  // attached value: -ofoo
          value = std::string(arg.substr(pos + 1));
        } else if (i + 1 < argc) {
          value = argv[++i];
        } else {
          LOG_ERROR("Option '-" << arg[pos] << "' requires a value");
          usage(std::cerr, argv[0], 1);
          return;
        }
        applyOption(spec->short_name, value, argv[0], args);
        break;  // the value consumed the rest of this argv element
      }
      continue;
    }

    slot_list.emplace_back(arg);  // non-option argument → slot file
  }

  LOG_DEBUG("slot_list: " << stringutils::join(slot_list, ","))
}
