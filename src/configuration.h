#pragma once

// konCePCja — INI-style configuration store.
//
// On-disk format (kept compatible with historical koncepcja.cfg files):
//   [section]          # '#' starts a comment
//   key=value
//
// Values set via setIntValue/setStringValue land in both the persisted map
// and the override map; overrides (e.g. from -O on the command line) win
// over parsed file content on reads.
//
// An -O override is one-run intent, and it never persists by echo: at save
// time the live state carries the override's value back through the setters,
// and writing that value into the file would make a temporary flag permanent
// (a scratch-dir m4_sd_path ended up in a user's config exactly this way —
// beads-iorb). A setter call that merely repeats what an override forced is
// therefore a no-op on the persisted map; a call with a DIFFERENT value is a
// genuine change — it persists, and that key's override protection ends.
//
// Writing is round-trip preserving: a Config that parsed a file remembers its
// text and rewrites values in place, so comments, blank lines, key order and
// keys this build knows nothing about all survive a save. Only a Config that
// never parsed anything falls back to generating a bare key=value dump.

#include <map>
#include <string>
#include <vector>

namespace config {
using ConfigSection = std::map<std::string, std::string>;
using ConfigMap = std::map<std::string, ConfigSection>;

bool hasValue(const ConfigMap& configMap, const std::string& section,
              const std::string& key);

class Config {
 public:
  std::istream& parseStream(std::istream& configStream);
  void parseString(const std::string& configString);
  void parseFile(const std::string& configFilename);

  std::ostream& toStream(std::ostream& out) const;
  bool saveToFile(const std::string& configFilename) const;

  void setOverrides(const ConfigMap& overrides);

  int getIntValue(const std::string& section, const std::string& key,
                  const int defaultValue) const;
  void setIntValue(const std::string& section, const std::string& key,
                   const int value);

  std::string getStringValue(const std::string& section, const std::string& key,
                             const std::string& defaultValue) const;
  void setStringValue(const std::string& section, const std::string& key,
                      const std::string& value);

  // Do not use this for anything else than testing.
  ConfigMap getConfigMapForTests() const;

 private:
  // Returns the stored value (override first, then parsed config), or
  // nullptr when neither map has it.
  const std::string* find(const std::string& section,
                          const std::string& key) const;

  ConfigMap config_;
  ConfigMap overrides_;

  // The overrides exactly as launched (-O / setOverrides), untouched by the
  // setters. The echo guard compares against THIS map: overrides_ absorbs
  // setter writes to keep reads coherent, so it cannot tell an echo from an
  // edit.
  ConfigMap launch_overrides_;

  // Verbatim text of everything parsed into this Config, in order. Empty for
  // a Config built purely by setIntValue/setStringValue (e.g. a brand-new
  // config file), which is the only case that still generates output from
  // scratch.
  std::vector<std::string> lines_;
};
}  // namespace config
