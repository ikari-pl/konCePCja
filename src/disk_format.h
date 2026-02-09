#pragma once

#include <string>
#include <vector>

// Look up a disk format index by short name (e.g. "data", "vendor").
// Returns -1 if the name is not recognized.
int disk_format_index_by_name(const std::string& name);

// Return a list of recognized short format names.
std::vector<std::string> disk_format_names();

// Create a new blank formatted DSK file at the given path.
// format_name: short name like "data" or "vendor".
// Returns empty string on success, error message on failure.
std::string disk_create_new(const std::string& path,
                            const std::string& format_name = "data");

// Format (or re-format) the disc currently in the given drive.
// drive_letter: 'A' or 'B'.
// format_name: short name like "data" or "vendor".
// Returns empty string on success, error message on failure.
std::string disk_format_drive(char drive_letter,
                              const std::string& format_name);
