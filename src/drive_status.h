#ifndef DRIVE_STATUS_H
#define DRIVE_STATUS_H

#include <string>

// Brief one-line-per-drive summary (for `status` command)
std::string drive_status_summary();

// Detailed multi-line per-drive output (for `status drives` command)
std::string drive_status_detailed();

// Overall emulator state line (paused, model, speed)
std::string emulator_status_summary();

#endif
