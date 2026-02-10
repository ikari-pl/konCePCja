#include "disk_sector_editor.h"
#include "disk.h"

#include <cstring>
#include <sstream>

// Compute sector size from the N value in CHRN
static unsigned int sector_size_from_N(uint8_t N) {
    return 128u << N;
}

// Find a sector on a given track/side by matching CHRN[2] (the R / sector_id).
// Returns pointer to the t_sector, or nullptr if not found.
// Note: drive->sides is zero-based (0=single-sided, 1=double-sided).
static t_sector* find_sector(t_drive* drive, unsigned int track,
                              unsigned int side, uint8_t sector_id) {
    if (!drive || drive->tracks == 0) return nullptr;
    if (track >= drive->tracks) return nullptr;
    if (side > drive->sides) return nullptr;
    t_track& trk = drive->track[track][side];
    for (unsigned int s = 0; s < trk.sectors; s++) {
        if (trk.sector[s].CHRN[2] == sector_id) {
            return &trk.sector[s];
        }
    }
    return nullptr;
}

std::vector<uint8_t> disk_sector_read(t_drive* drive, unsigned int track,
                                       unsigned int side, uint8_t sector_id,
                                       std::string& err) {
    err.clear();

    if (!drive || drive->tracks == 0) {
        err = "no disc in drive";
        return {};
    }
    if (track >= drive->tracks) {
        err = "track " + std::to_string(track) + " out of range (max " +
              std::to_string(drive->tracks - 1) + ")";
        return {};
    }
    if (side > drive->sides) {
        err = "side " + std::to_string(side) + " out of range (max " +
              std::to_string(drive->sides) + ")";
        return {};
    }

    t_sector* sec = find_sector(drive, track, side, sector_id);
    if (!sec) {
        std::ostringstream oss;
        oss << "sector " << std::hex << std::uppercase
            << static_cast<unsigned>(sector_id)
            << " not found on track " << std::dec << track
            << " side " << side;
        err = oss.str();
        return {};
    }

    unsigned int size = sector_size_from_N(sec->CHRN[3]);
    unsigned char* data = sec->getDataForRead();
    return std::vector<uint8_t>(data, data + size);
}

std::string disk_sector_write(t_drive* drive, unsigned int track,
                               unsigned int side, uint8_t sector_id,
                               const std::vector<uint8_t>& data) {
    if (!drive || drive->tracks == 0) return "no disc in drive";
    if (track >= drive->tracks) {
        return "track " + std::to_string(track) + " out of range (max " +
               std::to_string(drive->tracks - 1) + ")";
    }
    if (side > drive->sides) {
        return "side " + std::to_string(side) + " out of range (max " +
               std::to_string(drive->sides) + ")";
    }

    t_sector* sec = find_sector(drive, track, side, sector_id);
    if (!sec) {
        std::ostringstream oss;
        oss << "sector " << std::hex << std::uppercase
            << static_cast<unsigned>(sector_id)
            << " not found on track " << std::dec << track
            << " side " << side;
        return oss.str();
    }

    unsigned int size = sector_size_from_N(sec->CHRN[3]);
    if (data.size() != size) {
        return "data size mismatch: expected " + std::to_string(size) +
               " bytes, got " + std::to_string(data.size());
    }

    unsigned char* dest = sec->getDataForWrite();
    std::memcpy(dest, data.data(), size);
    drive->altered = true;
    return "";
}

std::vector<SectorInfo> disk_sector_info(t_drive* drive, unsigned int track,
                                          unsigned int side, std::string& err) {
    err.clear();

    if (!drive || drive->tracks == 0) {
        err = "no disc in drive";
        return {};
    }
    if (track >= drive->tracks) {
        err = "track " + std::to_string(track) + " out of range (max " +
              std::to_string(drive->tracks - 1) + ")";
        return {};
    }
    if (side > drive->sides) {
        err = "side " + std::to_string(side) + " out of range (max " +
              std::to_string(drive->sides) + ")";
        return {};
    }

    t_track& trk = drive->track[track][side];
    std::vector<SectorInfo> result;
    result.reserve(trk.sectors);

    for (unsigned int s = 0; s < trk.sectors; s++) {
        t_sector& sec = trk.sector[s];
        SectorInfo info;
        info.C = sec.CHRN[0];
        info.H = sec.CHRN[1];
        info.R = sec.CHRN[2];
        info.N = sec.CHRN[3];
        info.size = sector_size_from_N(sec.CHRN[3]);
        result.push_back(info);
    }

    return result;
}
