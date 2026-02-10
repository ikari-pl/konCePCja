#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct t_drive;

// Information about a single sector on a track
struct SectorInfo {
    uint8_t C;          // Cylinder
    uint8_t H;          // Head
    uint8_t R;          // Record (sector ID)
    uint8_t N;          // Size code (sector size = 128 << N)
    unsigned int size;  // Actual data size in bytes
};

// Read raw sector data by sector ID.
// Returns sector bytes. err is set on error.
std::vector<uint8_t> disk_sector_read(t_drive* drive, unsigned int track,
                                       unsigned int side, uint8_t sector_id,
                                       std::string& err);

// Write raw sector data by sector ID.
// Returns empty string on success, error message on failure.
std::string disk_sector_write(t_drive* drive, unsigned int track,
                               unsigned int side, uint8_t sector_id,
                               const std::vector<uint8_t>& data);

// List all sectors on a track with their CHRN values and sizes.
// Returns vector of SectorInfo. err is set on error.
std::vector<SectorInfo> disk_sector_info(t_drive* drive, unsigned int track,
                                          unsigned int side, std::string& err);
