// konCePCja — INI-style configuration store.

#include "configuration.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>

#include "log.h"

namespace config {
namespace {

// Delimiter sets of the historical parser, kept for format compatibility:
// keys stop at whitespace or '='; values stop at tab, '=' or an inline
// '#' comment.
constexpr std::string_view kKeyDelims = "\t =";
constexpr std::string_view kValueDelims = "\t=#";

std::string_view strip_spaces(std::string_view s) {
  size_t const first = s.find_first_not_of(' ');
  if (first == std::string_view::npos) return {};
  size_t const last = s.find_last_not_of(' ');
  return s.substr(first, last - first + 1);
}

// "[section] # comment" → "section" (the token between bracket chars).
std::string_view parse_section_name(std::string_view line) {
  size_t const start = line.find_first_not_of("[]");
  if (start == std::string_view::npos) return {};
  size_t const end = line.find_first_of("[]", start);
  return line.substr(start, end == std::string_view::npos ? end : end - start);
}

// The key a line assigns, or empty when the line carries no key/value pair.
// Deliberately mirrors parseStream's tokenizer step for step: if the writer
// and the reader ever disagreed about which lines hold a value, a save would
// either duplicate a key or leave a stale one behind.
std::string_view line_key(std::string_view line) {
  if (!line.empty() && line.front() == '[') return {};  // section header
  size_t const key_start = line.find_first_not_of(kKeyDelims);
  if (key_start == std::string_view::npos) return {};  // blank
  if (line[key_start] == '#') return {};               // comment
  size_t const key_end = line.find_first_of(kKeyDelims, key_start);
  if (key_end == std::string_view::npos) return {};  // no separator
  return line.substr(key_start, key_end - key_start);
}

// The stored value, or nullptr when the map has no such section/key.
const std::string* find_in(const ConfigMap& map, const std::string& section,
                           const std::string& key) {
  auto sec = map.find(section);
  if (sec == map.end()) return nullptr;
  auto entry = sec->second.find(key);
  return entry == sec->second.end() ? nullptr : &entry->second;
}

// Rewrite `line`'s value, keeping its key spelling, its spacing around the
// separator and any inline '#' comment.
std::string rewrite_value(const std::string& line, const std::string& value) {
  size_t const key_end = line.find_first_of(
      kKeyDelims, line.find_first_not_of(kKeyDelims));  // past the key
  size_t const eq = line.find('=', key_end);
  size_t const sep_end = (eq == std::string::npos) ? key_end : eq + 1;
  // Everything up to and including the separator plus the blanks after it.
  size_t value_start = line.find_first_not_of(" \t", sep_end);
  if (value_start == std::string::npos) value_start = line.size();
  std::string out = line.substr(0, value_start);
  if (eq == std::string::npos) out += "=";
  out += value;

  // An empty value drops the comment: "key= # note" re-parses as the value
  // "note", because parseStream skips '#' when hunting for the value start.
  if (value.empty()) return out;
  size_t const hash = line.find('#', value_start);
  if (hash != std::string::npos) {
    // max(): guards a line that had no value at all, just "key= # note".
    size_t const gap =
        std::max(value_start, line.find_last_not_of(" \t", hash - 1) + 1);
    out += line.substr(gap);
  }
  return out;
}

}  // namespace

bool hasValue(const ConfigMap& config, const std::string& section,
              const std::string& key) {
  auto sec = config.find(section);
  return sec != config.end() && sec->second.find(key) != sec->second.end();
}

std::istream& Config::parseStream(std::istream& configStream) {
  std::string line;
  std::string section;
  while (std::getline(configStream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();  // CRLF files
    // Kept verbatim (comments and all) so toStream can rewrite this file
    // rather than regenerate it. Appended, not replaced: parsing a second
    // stream adds to this Config, and its text has to come along.
    lines_.push_back(line);
    std::string_view rest = line;

    if (!rest.empty() && rest.front() == '[') {
      std::string_view const name = parse_section_name(rest);
      if (!name.empty()) section = std::string(name);
      continue;
    }

    size_t const key_start = rest.find_first_not_of(kKeyDelims);
    if (key_start == std::string_view::npos) continue;  // blank line
    if (rest[key_start] == '#') continue;               // comment line
    size_t const key_end = rest.find_first_of(kKeyDelims, key_start);
    if (key_end == std::string_view::npos) continue;  // no '=': not a pair
    std::string_view const key = rest.substr(key_start, key_end - key_start);

    rest = rest.substr(key_end);
    // The value begins after the '=' separator (surrounding blanks eaten)
    // and runs until an inline '#' comment, a tab, or a second '='.
    size_t const value_start = rest.find_first_not_of("\t=# ");
    if (value_start == std::string_view::npos) continue;  // empty value
    size_t const value_end = rest.find_first_of(kValueDelims, value_start);
    std::string_view const value =
        strip_spaces(rest.substr(value_start, value_end - value_start));

    config_[section][std::string(key)] = std::string(value);
  }
  return configStream;
}

void Config::parseString(const std::string& configString) {
  std::istringstream configStream(configString);
  parseStream(configStream);
}

void Config::parseFile(const std::string& configFilename) {
  std::ifstream configStream(configFilename);
  parseStream(configStream);
}

std::ostream& Config::toStream(std::ostream& out) const {
  auto dump_section = [&out](const std::string& section,
                             const ConfigSection& entries) {
    out << "[" << section << "]\n";
    for (const auto& [key, value] : entries) out << key << "=" << value << "\n";
  };

  if (lines_.empty()) {  // nothing was parsed: generate from scratch
    for (const auto& [section, entries] : config_) dump_section(section, entries);
    return out;
  }

  // Rewrite the parsed text in place. `pending` starts as the full model and
  // loses each key as its existing line is rewritten; whatever is left is
  // genuinely new and gets appended to its section (or to a new one).
  ConfigMap pending = config_;
  std::string section;
  std::vector<std::string> buffered;

  // Emit the section just finished, appending its new keys after its last
  // non-blank line so trailing blank lines stay trailing.
  auto flush = [&] {
    auto sec = pending.find(section);
    if (sec != pending.end() && !sec->second.empty()) {
      size_t at = buffered.size();
      while (at > 0 && buffered[at - 1].empty()) --at;
      std::vector<std::string> added;
      added.reserve(sec->second.size());
      for (const auto& [key, value] : sec->second) {
        std::string entry = key;
        entry += "=";
        entry += value;
        added.push_back(std::move(entry));
      }
      buffered.insert(buffered.begin() + static_cast<long>(at), added.begin(),
                      added.end());
    }
    pending.erase(section);
    for (const auto& line : buffered) out << line << "\n";
    buffered.clear();
  };

  for (const std::string& line : lines_) {
    if (!line.empty() && line.front() == '[') {
      std::string_view const name = parse_section_name(line);
      if (!name.empty()) {
        flush();
        section = std::string(name);
      }
      buffered.push_back(line);
      continue;
    }

    std::string_view const key = line_key(line);
    const std::string* value =
        key.empty() ? nullptr : find_in(config_, section, std::string(key));
    if (value != nullptr) {
      // Every occurrence is rewritten, not just the first: a file that
      // repeats a key must not re-parse to the stale duplicate.
      buffered.push_back(rewrite_value(line, *value));
      auto p = pending.find(section);
      if (p != pending.end()) p->second.erase(std::string(key));
      continue;
    }
    buffered.push_back(line);
  }
  flush();

  // Sections the file never had.
  for (const auto& [name, entries] : pending) {
    if (!entries.empty()) dump_section(name, entries);
  }
  return out;
}

bool Config::saveToFile(const std::string& configFilename) const {
  std::ofstream configStream(configFilename);
  toStream(configStream);
  configStream.close();
  if (!configStream.good()) {
    LOG_ERROR("Couldn't save configuration to '" << configFilename
                                                 << "'. Is the file writable?");
    return false;
  }
  return true;
}

void Config::setOverrides(const ConfigMap& overrides) {
  overrides_ = overrides;
}

const std::string* Config::find(const std::string& section,
                                const std::string& key) const {
  if (const std::string* value = find_in(overrides_, section, key)) return value;
  return find_in(config_, section, key);
}

int Config::getIntValue(const std::string& section, const std::string& key,
                        const int defaultValue) const {
  const std::string* value = find(section, key);
  // atoi semantics kept: leading number parsed, garbage yields 0, no throw.
  return value ? std::atoi(value->c_str()) : defaultValue;
}

std::string Config::getStringValue(const std::string& section,
                                   const std::string& key,
                                   const std::string& defaultValue) const {
  const std::string* value = find(section, key);
  return value ? *value : defaultValue;
}

void Config::setStringValue(const std::string& section, const std::string& key,
                            const std::string& value) {
  overrides_[section][key] = value;
  config_[section][key] = value;
}

void Config::setIntValue(const std::string& section, const std::string& key,
                         const int value) {
  setStringValue(section, key, std::to_string(value));
}

ConfigMap Config::getConfigMapForTests() const { return config_; }
}  // namespace config
