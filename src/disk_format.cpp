#include "disk_format.h"
#include "koncepcja.h"
#include "slotshandler.h"
#include "log.h"

#include <algorithm>
#include <cctype>
#include <map>

extern t_drive driveA;
extern t_drive driveB;
extern t_disk_format disk_format[];

// Short name -> disk_format[] index.
// Built-in formats are at indices 0 and 1; indices 2..MAX_DISK_FORMAT-1 are
// user-customisable and may or may not be populated.
static const std::map<std::string, int> builtin_format_names = {
    {"data",   0},
    {"vendor", 1},
};

static std::string to_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

int disk_format_index_by_name(const std::string& name) {
    if (name.empty()) return -1;
    auto it = builtin_format_names.find(to_lower(name));
    if (it != builtin_format_names.end()) return it->second;

    // Also allow matching by full label (case-insensitive prefix match).
    std::string lower_name = to_lower(name);
    for (int i = 0; i < MAX_DISK_FORMAT; i++) {
        if (disk_format[i].label.empty()) continue;
        std::string lower_label = to_lower(disk_format[i].label);
        if (lower_label.find(lower_name) == 0) return i;
    }
    return -1;
}

std::vector<std::string> disk_format_names() {
    std::vector<std::string> names;
    // Return the short names for built-in formats.
    names.push_back("data");
    names.push_back("vendor");
    // Also include any populated custom formats by their label.
    for (int i = FIRST_CUSTOM_DISK_FORMAT; i < MAX_DISK_FORMAT; i++) {
        if (!disk_format[i].label.empty()) {
            names.push_back(disk_format[i].label);
        }
    }
    return names;
}

std::string disk_create_new(const std::string& path,
                            const std::string& format_name) {
    int idx = disk_format_index_by_name(format_name);
    if (idx < 0) {
        return "unknown format: " + format_name;
    }

    // Create a temporary drive struct, format it, then save to file.
    t_drive tmp_drive{};

    int rc = dsk_format(&tmp_drive, idx);
    if (rc != 0) {
        dsk_eject(&tmp_drive);
        return "format error code " + std::to_string(rc);
    }

    rc = dsk_save(path, &tmp_drive);
    if (rc != 0) {
        // Clean up the formatted drive's allocated memory.
        dsk_eject(&tmp_drive);
        return "write error for " + path;
    }

    // Clean up.
    dsk_eject(&tmp_drive);
    return "";
}

std::string disk_format_drive(char drive_letter,
                              const std::string& format_name) {
    t_drive* drive = nullptr;
    char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(drive_letter)));
    if (upper == 'A') {
        drive = &driveA;
    } else if (upper == 'B') {
        drive = &driveB;
    } else {
        return "invalid drive letter: " + std::string(1, drive_letter);
    }

    int idx = disk_format_index_by_name(format_name);
    if (idx < 0) {
        return "unknown format: " + format_name;
    }

    // Eject any existing disc content before formatting.
    dsk_eject(drive);

    int rc = dsk_format(drive, idx);
    if (rc != 0) {
        return "format error code " + std::to_string(rc);
    }
    return "";
}
