#include "disk_file_editor.h"
#include "disk.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>

// CP/M DATA format constants
static constexpr unsigned int CPM_BLOCK_SIZE = 1024;      // 1K blocks for DATA format
static constexpr unsigned int CPM_SECTOR_SIZE = 512;
static constexpr unsigned int CPM_SECTORS_PER_TRACK = 9;
static constexpr unsigned int CPM_DIR_BLOCKS = 2;          // Blocks 0-1 are directory
static constexpr unsigned int CPM_DIR_ENTRIES = 64;
static constexpr unsigned int CPM_DIR_ENTRY_SIZE = 32;
static constexpr unsigned int CPM_RECORDS_PER_EXTENT = 128; // 128 records of 128 bytes = 16K
static constexpr unsigned int CPM_RECORD_SIZE = 128;
static constexpr unsigned int CPM_TOTAL_BLOCKS = 180;       // DATA format: 180 blocks total
static constexpr unsigned int CPM_EXTENT_SIZE = 16384;      // 16K per extent
static constexpr uint8_t CPM_DELETED_ENTRY = 0xE5;

// Find sector data by sector ID within a track
static uint8_t* find_sector_data(t_drive* drive, unsigned int track, unsigned int side,
                                  uint8_t sector_id) {
    if (track >= drive->tracks) return nullptr;
    if (side > drive->sides) return nullptr;
    t_track& trk = drive->track[track][side];
    for (unsigned int s = 0; s < trk.sectors; s++) {
        if (trk.sector[s].CHRN[2] == sector_id) {
            return trk.sector[s].getDataForWrite();
        }
    }
    return nullptr;
}

// Read a logical block (1K) from the disc.
// Block 0 starts at track 0, blocks map to sectors sequentially.
// DATA format: 9 sectors/track * 512 bytes = 4608 bytes/track
// 1K block = 2 sectors. Block N maps to:
//   track = (N * 2) / 9  (each track has 9 sectors)
//   sector_offset = (N * 2) % 9
static bool read_block(t_drive* drive, unsigned int block, uint8_t* out) {
    if (block >= CPM_TOTAL_BLOCKS) return false;

    // Each block is 2 sectors (2 * 512 = 1024)
    unsigned int first_sector = block * 2;

    // DATA format sector IDs: C1..C9 (0xC1..0xC9)
    for (int half = 0; half < 2; half++) {
        unsigned int abs_sector = first_sector + half;
        unsigned int track = abs_sector / CPM_SECTORS_PER_TRACK;
        unsigned int sec_in_track = abs_sector % CPM_SECTORS_PER_TRACK;

        // Map logical sector index to sector ID
        uint8_t sector_id = static_cast<uint8_t>(0xC1 + sec_in_track);

        uint8_t* data = find_sector_data(drive, track, 0, sector_id);
        if (!data) return false;
        std::memcpy(out + half * CPM_SECTOR_SIZE, data, CPM_SECTOR_SIZE);
    }
    return true;
}

// Write a logical block (1K) to the disc.
static bool write_block(t_drive* drive, unsigned int block, const uint8_t* in) {
    if (block >= CPM_TOTAL_BLOCKS) return false;

    unsigned int first_sector = block * 2;

    for (int half = 0; half < 2; half++) {
        unsigned int abs_sector = first_sector + half;
        unsigned int track = abs_sector / CPM_SECTORS_PER_TRACK;
        unsigned int sec_in_track = abs_sector % CPM_SECTORS_PER_TRACK;

        uint8_t sector_id = static_cast<uint8_t>(0xC1 + sec_in_track);

        uint8_t* data = find_sector_data(drive, track, 0, sector_id);
        if (!data) return false;
        std::memcpy(data, in + half * CPM_SECTOR_SIZE, CPM_SECTOR_SIZE);
    }
    drive->altered = true;
    return true;
}

// Read the entire directory (blocks 0-1, 2K, 64 entries of 32 bytes)
static bool read_directory(t_drive* drive, uint8_t dir[CPM_DIR_ENTRIES * CPM_DIR_ENTRY_SIZE]) {
    for (unsigned int b = 0; b < CPM_DIR_BLOCKS; b++) {
        if (!read_block(drive, b, dir + b * CPM_BLOCK_SIZE)) return false;
    }
    return true;
}

// Write the entire directory back
static bool write_directory(t_drive* drive, const uint8_t dir[CPM_DIR_ENTRIES * CPM_DIR_ENTRY_SIZE]) {
    for (unsigned int b = 0; b < CPM_DIR_BLOCKS; b++) {
        if (!write_block(drive, b, dir + b * CPM_BLOCK_SIZE)) return false;
    }
    return true;
}

// Format a CP/M filename from directory entry bytes 1-11 into "NAME.EXT"
// Strips high bits (used for R/O, SYS flags) and trims spaces.
static std::string format_cpm_name(const uint8_t* entry) {
    char name[9], ext[4];
    for (int i = 0; i < 8; i++) name[i] = entry[1 + i] & 0x7F;
    name[8] = 0;
    for (int i = 0; i < 3; i++) ext[i] = entry[9 + i] & 0x7F;
    ext[3] = 0;

    // Trim trailing spaces
    int nlen = 8;
    while (nlen > 0 && name[nlen - 1] == ' ') nlen--;
    int elen = 3;
    while (elen > 0 && ext[elen - 1] == ' ') elen--;

    std::string result(name, nlen);
    if (elen > 0) {
        result += '.';
        result += std::string(ext, elen);
    }
    return result;
}

// Build a padded 11-byte CP/M filename from "NAME.EXT" format
static bool parse_cpm_name(const std::string& display, uint8_t out[11]) {
    std::memset(out, ' ', 11);

    std::string upper;
    for (char c : display) upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    auto dot = upper.find('.');
    std::string name_part, ext_part;
    if (dot != std::string::npos) {
        name_part = upper.substr(0, dot);
        ext_part = upper.substr(dot + 1);
    } else {
        name_part = upper;
    }

    if (name_part.size() > 8 || ext_part.size() > 3) return false;

    for (size_t i = 0; i < name_part.size(); i++) out[i] = static_cast<uint8_t>(name_part[i]);
    for (size_t i = 0; i < ext_part.size(); i++) out[8 + i] = static_cast<uint8_t>(ext_part[i]);
    return true;
}

// Check if two directory entries refer to the same file (same user + name)
static bool same_file(const uint8_t* a, const uint8_t* b) {
    if (a[0] != b[0]) return false; // different user
    for (int i = 1; i <= 11; i++) {
        if ((a[i] & 0x7F) != (b[i] & 0x7F)) return false;
    }
    return true;
}

// Compute the size of a file from its directory extents.
// Each extent covers up to 16K (128 records * 128 bytes).
// The last extent's RC field tells how many records are actually used.
static uint32_t compute_file_size(const uint8_t dir[], const uint8_t* first_entry) {
    // Collect all extents for this file
    struct ExtentInfo {
        unsigned int extent_num;
        uint8_t rc; // record count in this extent
    };
    std::vector<ExtentInfo> extents;

    for (unsigned int i = 0; i < CPM_DIR_ENTRIES; i++) {
        const uint8_t* entry = dir + i * CPM_DIR_ENTRY_SIZE;
        if (entry[0] == CPM_DELETED_ENTRY) continue;
        if (!same_file(entry, first_entry)) continue;

        unsigned int ext_lo = entry[12]; // Extent low byte
        unsigned int ext_hi = entry[14]; // Extent high byte (S2)
        unsigned int extent_num = ext_lo + ext_hi * 32;
        uint8_t rc = entry[15]; // Record Count

        extents.push_back({extent_num, rc});
    }

    if (extents.empty()) return 0;

    // Sort by extent number
    std::sort(extents.begin(), extents.end(),
              [](const ExtentInfo& a, const ExtentInfo& b) {
                  return a.extent_num < b.extent_num;
              });

    // Size = (all extents except last) * 16K + last_extent_RC * 128
    uint32_t size = 0;
    for (size_t i = 0; i + 1 < extents.size(); i++) {
        size += CPM_EXTENT_SIZE; // 16K per full extent
    }
    size += static_cast<uint32_t>(extents.back().rc) * CPM_RECORD_SIZE;

    return size;
}

std::vector<DiskFileEntry> disk_list_files(t_drive* drive, std::string& err) {
    std::vector<DiskFileEntry> result;
    err.clear();

    if (!drive || drive->tracks == 0) {
        err = "no disc in drive";
        return result;
    }

    uint8_t dir[CPM_DIR_ENTRIES * CPM_DIR_ENTRY_SIZE];
    if (!read_directory(drive, dir)) {
        err = "failed to read directory";
        return result;
    }

    // Track which files we've already listed (by user + name)
    std::set<std::string> seen;

    for (unsigned int i = 0; i < CPM_DIR_ENTRIES; i++) {
        const uint8_t* entry = dir + i * CPM_DIR_ENTRY_SIZE;
        if (entry[0] == CPM_DELETED_ENTRY) continue;
        if (entry[0] > 15) continue; // user numbers > 15 are reserved

        // Only process extent 0 (or lowest extent) to avoid duplicates
        unsigned int ext_lo = entry[12];
        unsigned int ext_hi = entry[14];
        unsigned int extent_num = ext_lo + ext_hi * 32;

        std::string display = format_cpm_name(entry);
        std::string key = std::to_string(entry[0]) + ":" + display;

        if (seen.count(key)) continue;

        // Check if there's a lower extent for this file
        bool has_lower = false;
        for (unsigned int j = 0; j < CPM_DIR_ENTRIES; j++) {
            if (j == i) continue;
            const uint8_t* other = dir + j * CPM_DIR_ENTRY_SIZE;
            if (other[0] == CPM_DELETED_ENTRY) continue;
            if (!same_file(entry, other)) continue;
            unsigned int other_ext = other[12] + static_cast<unsigned int>(other[14]) * 32;
            if (other_ext < extent_num) { has_lower = true; break; }
        }
        if (has_lower) continue;

        seen.insert(key);

        DiskFileEntry fe;
        fe.display_name = display;
        // Build padded 8.3 filename: "NAME    .EXT"
        {
            char name_buf[9], ext_buf[4];
            for (int k = 0; k < 8; k++) name_buf[k] = entry[1 + k] & 0x7F;
            name_buf[8] = 0;
            for (int k = 0; k < 3; k++) ext_buf[k] = entry[9 + k] & 0x7F;
            ext_buf[3] = 0;
            fe.filename = std::string(name_buf, 8) + "." + std::string(ext_buf, 3);
        }
        fe.user = entry[0];
        fe.read_only = (entry[9] & 0x80) != 0;
        fe.system = (entry[10] & 0x80) != 0;
        fe.size_bytes = compute_file_size(dir, entry);

        result.push_back(fe);
    }

    return result;
}

// Get all blocks allocated to a file, in order, across all extents
static std::vector<uint8_t> get_file_blocks(const uint8_t dir[], const uint8_t* first_entry) {
    struct ExtentData {
        unsigned int extent_num;
        std::vector<uint8_t> blocks;
    };
    std::vector<ExtentData> extents;

    for (unsigned int i = 0; i < CPM_DIR_ENTRIES; i++) {
        const uint8_t* entry = dir + i * CPM_DIR_ENTRY_SIZE;
        if (entry[0] == CPM_DELETED_ENTRY) continue;
        if (!same_file(entry, first_entry)) continue;

        unsigned int ext_lo = entry[12];
        unsigned int ext_hi = entry[14];
        unsigned int extent_num = ext_lo + ext_hi * 32;

        ExtentData ed;
        ed.extent_num = extent_num;
        // Block numbers are in bytes 16-31 (16 single-byte block numbers for DATA format)
        for (int b = 16; b < 32; b++) {
            if (entry[b] != 0) {
                ed.blocks.push_back(entry[b]);
            }
        }
        extents.push_back(ed);
    }

    std::sort(extents.begin(), extents.end(),
              [](const ExtentData& a, const ExtentData& b) {
                  return a.extent_num < b.extent_num;
              });

    std::vector<uint8_t> all_blocks;
    for (const auto& e : extents) {
        for (uint8_t b : e.blocks) {
            all_blocks.push_back(b);
        }
    }
    return all_blocks;
}

std::vector<uint8_t> disk_read_file(t_drive* drive, const std::string& filename,
                                     std::string& err) {
    std::vector<uint8_t> result;
    err.clear();

    if (!drive || drive->tracks == 0) {
        err = "no disc in drive";
        return result;
    }

    uint8_t dir[CPM_DIR_ENTRIES * CPM_DIR_ENTRY_SIZE];
    if (!read_directory(drive, dir)) {
        err = "failed to read directory";
        return result;
    }

    // Find the file
    uint8_t search_name[11];
    if (!parse_cpm_name(filename, search_name)) {
        err = "invalid filename";
        return result;
    }

    const uint8_t* found = nullptr;
    for (unsigned int i = 0; i < CPM_DIR_ENTRIES; i++) {
        const uint8_t* entry = dir + i * CPM_DIR_ENTRY_SIZE;
        if (entry[0] == CPM_DELETED_ENTRY) continue;
        if (entry[0] > 15) continue;
        // Compare name (bytes 1-11, masking high bits)
        bool match = true;
        for (int j = 0; j < 11; j++) {
            if ((entry[1 + j] & 0x7F) != search_name[j]) { match = false; break; }
        }
        if (match) { found = entry; break; }
    }

    if (!found) {
        err = "file not found: " + filename;
        return result;
    }

    uint32_t file_size = compute_file_size(dir, found);
    auto blocks = get_file_blocks(dir, found);

    // Read all blocks
    uint8_t block_buf[CPM_BLOCK_SIZE];
    uint32_t bytes_read = 0;
    for (uint8_t block_num : blocks) {
        if (!read_block(drive, block_num, block_buf)) {
            err = "failed to read block " + std::to_string(block_num);
            return {};
        }
        uint32_t to_copy = CPM_BLOCK_SIZE;
        if (bytes_read + to_copy > file_size) {
            to_copy = file_size - bytes_read;
        }
        result.insert(result.end(), block_buf, block_buf + to_copy);
        bytes_read += to_copy;
        if (bytes_read >= file_size) break;
    }

    return result;
}

AmsdosHeaderInfo disk_parse_amsdos_header(const std::vector<uint8_t>& data) {
    AmsdosHeaderInfo info;
    if (data.size() < 128) return info;

    // Verify checksum: sum of bytes 0-66
    uint16_t checksum = 0;
    for (int i = 0; i < 67; i++) {
        checksum += data[i];
    }
    uint16_t stored_checksum = static_cast<uint16_t>(data[67]) |
                                (static_cast<uint16_t>(data[68]) << 8);

    if (checksum != stored_checksum) return info;

    info.valid = true;
    info.type = static_cast<AmsdosFileType>(data[18]);
    info.load_addr = static_cast<uint16_t>(data[21]) |
                     (static_cast<uint16_t>(data[22]) << 8);
    info.file_length = static_cast<uint32_t>(data[64]) |
                       (static_cast<uint32_t>(data[65]) << 8) |
                       (static_cast<uint32_t>(data[66]) << 16);
    info.exec_addr = static_cast<uint16_t>(data[26]) |
                     (static_cast<uint16_t>(data[27]) << 8);

    return info;
}

std::vector<uint8_t> disk_make_amsdos_header(const std::string& cpc_filename,
                                              AmsdosFileType type,
                                              uint16_t load_addr,
                                              uint16_t exec_addr,
                                              uint32_t data_length) {
    std::vector<uint8_t> header(128, 0);

    // AMSDOS header format: byte 0 is user number, bytes 1-11 are filename
    // (8-byte name + 3-byte extension, space-padded, NO dot)
    uint8_t name11[11];
    parse_cpm_name(cpc_filename, name11);
    header[0] = 0; // User 0
    std::memcpy(&header[1], name11, 11);

    // Byte 18: file type
    header[18] = static_cast<uint8_t>(type);

    // Bytes 21-22: load address (LE)
    header[21] = static_cast<uint8_t>(load_addr & 0xFF);
    header[22] = static_cast<uint8_t>((load_addr >> 8) & 0xFF);

    // Bytes 24-25: file length (16-bit, LE) -- may be truncated for large files
    header[24] = static_cast<uint8_t>(data_length & 0xFF);
    header[25] = static_cast<uint8_t>((data_length >> 8) & 0xFF);

    // Bytes 26-27: exec address (LE)
    header[26] = static_cast<uint8_t>(exec_addr & 0xFF);
    header[27] = static_cast<uint8_t>((exec_addr >> 8) & 0xFF);

    // Bytes 64-66: real file length (24-bit, LE)
    header[64] = static_cast<uint8_t>(data_length & 0xFF);
    header[65] = static_cast<uint8_t>((data_length >> 8) & 0xFF);
    header[66] = static_cast<uint8_t>((data_length >> 16) & 0xFF);

    // Bytes 67-68: checksum of bytes 0-66 (LE)
    uint16_t checksum = 0;
    for (int i = 0; i < 67; i++) {
        checksum += header[i];
    }
    header[67] = static_cast<uint8_t>(checksum & 0xFF);
    header[68] = static_cast<uint8_t>((checksum >> 8) & 0xFF);

    return header;
}

// Find free blocks on the disc (not allocated by any directory entry)
static std::vector<uint8_t> find_free_blocks(const uint8_t dir[]) {
    std::set<uint8_t> used;
    // Blocks 0-1 are always used by directory
    used.insert(0);
    used.insert(1);

    for (unsigned int i = 0; i < CPM_DIR_ENTRIES; i++) {
        const uint8_t* entry = dir + i * CPM_DIR_ENTRY_SIZE;
        if (entry[0] == CPM_DELETED_ENTRY) continue;
        if (entry[0] > 15) continue;
        for (int b = 16; b < 32; b++) {
            if (entry[b] != 0) {
                used.insert(entry[b]);
            }
        }
    }

    std::vector<uint8_t> free_blocks;
    for (unsigned int b = 2; b < CPM_TOTAL_BLOCKS; b++) {
        if (used.find(static_cast<uint8_t>(b)) == used.end()) {
            free_blocks.push_back(static_cast<uint8_t>(b));
        }
    }
    return free_blocks;
}

// Find a free directory entry index
static int find_free_dir_entry(const uint8_t dir[]) {
    for (unsigned int i = 0; i < CPM_DIR_ENTRIES; i++) {
        if (dir[i * CPM_DIR_ENTRY_SIZE] == CPM_DELETED_ENTRY) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string disk_write_file(t_drive* drive, const std::string& cpc_filename,
                            const std::vector<uint8_t>& data, bool add_header,
                            uint16_t load_addr, uint16_t exec_addr,
                            AmsdosFileType type) {
    if (!drive || drive->tracks == 0) return "no disc in drive";

    uint8_t name11[11];
    if (!parse_cpm_name(cpc_filename, name11)) return "invalid filename";

    // Build the full data to write (optionally with AMSDOS header)
    std::vector<uint8_t> full_data;
    if (add_header) {
        auto hdr = disk_make_amsdos_header(cpc_filename, type, load_addr, exec_addr,
                                            static_cast<uint32_t>(data.size()));
        full_data.insert(full_data.end(), hdr.begin(), hdr.end());
    }
    full_data.insert(full_data.end(), data.begin(), data.end());

    uint8_t dir[CPM_DIR_ENTRIES * CPM_DIR_ENTRY_SIZE];
    if (!read_directory(drive, dir)) return "failed to read directory";

    // Check if file already exists
    for (unsigned int i = 0; i < CPM_DIR_ENTRIES; i++) {
        const uint8_t* entry = dir + i * CPM_DIR_ENTRY_SIZE;
        if (entry[0] == CPM_DELETED_ENTRY) continue;
        if (entry[0] > 15) continue;
        bool match = true;
        for (int j = 0; j < 11; j++) {
            if ((entry[1 + j] & 0x7F) != name11[j]) { match = false; break; }
        }
        if (match) return "file already exists: " + cpc_filename;
    }

    auto free_blocks = find_free_blocks(dir);

    // Calculate blocks needed
    uint32_t total_bytes = static_cast<uint32_t>(full_data.size());
    uint32_t blocks_needed = (total_bytes + CPM_BLOCK_SIZE - 1) / CPM_BLOCK_SIZE;

    if (blocks_needed > free_blocks.size()) {
        return "disc full (need " + std::to_string(blocks_needed) +
               " blocks, have " + std::to_string(free_blocks.size()) + " free)";
    }

    // Calculate extents needed (16 blocks per extent for DATA format with 1K blocks)
    uint32_t blocks_per_extent = 16; // 16 block pointers per directory entry
    uint32_t extents_needed = (blocks_needed + blocks_per_extent - 1) / blocks_per_extent;

    // Check we have enough directory entries
    int free_entries = 0;
    for (unsigned int i = 0; i < CPM_DIR_ENTRIES; i++) {
        if (dir[i * CPM_DIR_ENTRY_SIZE] == CPM_DELETED_ENTRY) free_entries++;
    }
    if (static_cast<uint32_t>(free_entries) < extents_needed) {
        return "directory full";
    }

    // Write data blocks
    uint32_t data_offset = 0;
    uint8_t block_buf[CPM_BLOCK_SIZE];

    for (uint32_t b = 0; b < blocks_needed; b++) {
        std::memset(block_buf, 0xE5, CPM_BLOCK_SIZE); // Fill with E5 (unused area marker)
        uint32_t to_copy = CPM_BLOCK_SIZE;
        if (data_offset + to_copy > total_bytes) {
            to_copy = total_bytes - data_offset;
        }
        std::memcpy(block_buf, full_data.data() + data_offset, to_copy);
        if (!write_block(drive, free_blocks[b], block_buf)) {
            return "failed to write block " + std::to_string(free_blocks[b]);
        }
        data_offset += to_copy;
    }

    // Create directory entries (one per extent)
    uint32_t blocks_assigned = 0;
    uint32_t bytes_remaining = total_bytes;

    for (uint32_t ext = 0; ext < extents_needed; ext++) {
        int entry_idx = find_free_dir_entry(dir);
        if (entry_idx < 0) return "directory full";

        uint8_t* entry = dir + entry_idx * CPM_DIR_ENTRY_SIZE;
        std::memset(entry, 0, CPM_DIR_ENTRY_SIZE);

        entry[0] = 0; // User 0
        std::memcpy(entry + 1, name11, 11); // Filename

        entry[12] = static_cast<uint8_t>(ext & 0x1F);  // Extent low (bits 0-4)
        entry[13] = 0; // S1 (reserved)
        entry[14] = static_cast<uint8_t>((ext >> 5) & 0x3F); // Extent high (S2)

        // Calculate records in this extent
        uint32_t extent_bytes = 0;
        uint32_t blocks_in_extent = 0;
        for (uint32_t b = 0; b < blocks_per_extent && blocks_assigned + b < blocks_needed; b++) {
            entry[16 + b] = free_blocks[blocks_assigned + b];
            blocks_in_extent++;
            uint32_t block_bytes = CPM_BLOCK_SIZE;
            if (block_bytes > bytes_remaining) {
                block_bytes = bytes_remaining;
            }
            extent_bytes += block_bytes;
            bytes_remaining -= block_bytes;
        }
        blocks_assigned += blocks_in_extent;

        // RC = number of 128-byte records in this extent
        uint32_t records = (extent_bytes + CPM_RECORD_SIZE - 1) / CPM_RECORD_SIZE;
        if (records > CPM_RECORDS_PER_EXTENT) records = CPM_RECORDS_PER_EXTENT;
        entry[15] = static_cast<uint8_t>(records);
    }

    // Write directory back
    if (!write_directory(drive, dir)) return "failed to write directory";

    drive->altered = true;
    return "";
}

std::string disk_delete_file(t_drive* drive, const std::string& filename) {
    if (!drive || drive->tracks == 0) return "no disc in drive";

    uint8_t name11[11];
    if (!parse_cpm_name(filename, name11)) return "invalid filename";

    uint8_t dir[CPM_DIR_ENTRIES * CPM_DIR_ENTRY_SIZE];
    if (!read_directory(drive, dir)) return "failed to read directory";

    bool found = false;
    for (unsigned int i = 0; i < CPM_DIR_ENTRIES; i++) {
        uint8_t* entry = dir + i * CPM_DIR_ENTRY_SIZE;
        if (entry[0] == CPM_DELETED_ENTRY) continue;
        if (entry[0] > 15) continue;
        bool match = true;
        for (int j = 0; j < 11; j++) {
            if ((entry[1 + j] & 0x7F) != name11[j]) { match = false; break; }
        }
        if (match) {
            entry[0] = CPM_DELETED_ENTRY; // Mark as deleted
            found = true;
        }
    }

    if (!found) return "file not found: " + filename;

    if (!write_directory(drive, dir)) return "failed to write directory";
    drive->altered = true;
    return "";
}

std::string disk_to_cpc_filename(const std::string& local_name) {
    // Extract just the filename part (no directory)
    std::string fname = local_name;
    auto slash = fname.rfind('/');
    if (slash != std::string::npos) fname = fname.substr(slash + 1);
    slash = fname.rfind('\\');
    if (slash != std::string::npos) fname = fname.substr(slash + 1);

    // Split into name and extension
    auto dot = fname.rfind('.');
    std::string name_part, ext_part;
    if (dot != std::string::npos) {
        name_part = fname.substr(0, dot);
        ext_part = fname.substr(dot + 1);
    } else {
        name_part = fname;
    }

    // Uppercase
    for (auto& c : name_part) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (auto& c : ext_part) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    // Truncate to 8.3
    if (name_part.size() > 8) name_part = name_part.substr(0, 8);
    if (ext_part.size() > 3) ext_part = ext_part.substr(0, 3);

    if (name_part.empty()) return "";

    if (ext_part.empty()) return name_part;
    return name_part + "." + ext_part;
}
