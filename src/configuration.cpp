// konCePCja — INI-style configuration store.

#include "configuration.h"

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
  for (const auto& [section, entries] : config_) {
    out << "[" << section << "]\n";
    for (const auto& [key, value] : entries) {
      out << key << "=" << value << "\n";
    }
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
  for (const ConfigMap* map : {&overrides_, &config_}) {
    auto sec = map->find(section);
    if (sec == map->end()) continue;
    auto entry = sec->second.find(key);
    if (entry != sec->second.end()) return &entry->second;
  }
  return nullptr;
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
