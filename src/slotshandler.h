#pragma once

// konCePCja — media manager: file-to-slot routing (see slotshandler.cpp).

#include <cstdio>
#include <string>
#include <vector>

#include "hw_views.h"
#include "koncepcja.h"

// Extract 'filename' from 'zipfile'. Filename must end with one of the
// extensions listed in 'ext'. The FILE handle returned must be closed once
// finished with; nullptr when the file couldn't be extracted.
FILE* extractFile(const std::string& zipfile, const std::string& filename,
                  const std::string& ext);

int snapshot_load(FILE* pfile);
int snapshot_load(const std::string& filename);
int snapshot_save(const std::string& filename);

int dsk_load(FILE* pfile, t_drive* drive);
int dsk_load(const std::string& filename, t_drive* drive);
int dsk_save(const std::string& filename, t_drive* drive);
// Serialize a formatted drive to EXTENDED-DSK bytes in memory (the same bytes
// dsk_save writes to a file). Returns 0, or ERR_DSK_WRITE when the drive has no
// tracks. Used by the flux New-disk path to synthesize an SCP from a blank DSK.
int dsk_to_bytes(t_drive* drive, std::vector<uint8_t>& out);
void dsk_eject(t_drive* drive);
int dsk_format(t_drive* drive, int iFormat);

int tape_insert(FILE* pfile);
int tape_insert(const std::string& filename);
int tape_insert_cdt(FILE* pfile);
int tape_insert_voc(FILE* pfile);
void tape_eject();

void cartridge_load();
int cartridge_load(const std::string& filepath);
int cartridge_load(FILE* file);

// Smart load: DSK, SNA, CDT, VOC, CPR, or a zip containing one of these.
// slot.drive must match the type of file being loaded.
int file_load(t_slot& slot);

/* SNA <-> the sub-cycle machine (Wave 1); the plain snapshot_load/save route
 * here automatically when the engine is active. Exposed for tests. */
namespace subcycle {
class Machine;
}
int snapshot_load_machine(subcycle::Machine& m, FILE* pfile);
int snapshot_save_machine(subcycle::Machine& m, const std::string& filename);

// Retrieve files that are passed as argument and update CPC fields so that
// they will be loaded properly
void fillSlots(std::vector<std::string> slot_list, t_CPC& CPC);
// Loads slot content in memory
void loadSlots();

inline constexpr int MAX_DISK_FORMAT = 8;
inline constexpr int DEFAULT_DISK_FORMAT = 0;
inline constexpr int FIRST_CUSTOM_DISK_FORMAT = 2;

t_disk_format parseDiskFormat(const std::string& format);
std::string serializeDiskFormat(const t_disk_format& format);
