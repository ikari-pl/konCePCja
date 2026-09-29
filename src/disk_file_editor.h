#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct t_drive;

// Every AMSDOS header is one 128-byte record ahead of the payload.
inline constexpr size_t kAmsdosHeaderSize = 128;

// AMSDOS file types
enum class AmsdosFileType : uint8_t {
  BASIC = 0,
  PROTECTED = 1,
  BINARY = 2,
  UNKNOWN = 0xFF
};

// Represents a file entry in the CP/M directory
struct DiskFileEntry {
  std::string filename;      // "NAME    .EXT" (8.3 padded)
  std::string display_name;  // "NAME.EXT" (trimmed, human-readable)
  uint32_t size_bytes = 0;   // File size in bytes
  bool read_only = false;
  bool system = false;
  uint8_t user = 0;
};

// AMSDOS header info
struct AmsdosHeaderInfo {
  bool valid = false;  // true if checksum matches
  AmsdosFileType type = AmsdosFileType::UNKNOWN;
  uint16_t load_addr = 0;
  uint16_t exec_addr = 0;
  uint32_t file_length = 0;  // Logical file length from header
};

// List files on a drive (DATA format only).
// Returns vector of file entries. err is set on error.
std::vector<DiskFileEntry> disk_list_files(t_drive* drive, std::string& err);

// Read raw file content from disc (including AMSDOS header if present).
// Returns the raw bytes. err is set on error.
std::vector<uint8_t> disk_read_file(t_drive* drive, const std::string& filename,
                                    std::string& err);

// Parse AMSDOS header from raw file data (first 128 bytes).
AmsdosHeaderInfo disk_parse_amsdos_header(const std::vector<uint8_t>& data);

// Write a file to disc. data should NOT include AMSDOS header -- one will be
// generated if add_header is true. cpc_filename must be uppercase 8.3 format.
// Returns empty string on success, error message on failure.
std::string disk_write_file(t_drive* drive, const std::string& cpc_filename,
                            const std::vector<uint8_t>& data, bool add_header,
                            uint16_t load_addr = 0, uint16_t exec_addr = 0,
                            AmsdosFileType type = AmsdosFileType::BINARY);

// How `disk put` stores a host file (beads-otjf).
//   BASIC  - 128-byte AMSDOS header, type 0: a tokenised BASIC program.
//   BINARY - 128-byte AMSDOS header, type 2 (load/exec address 0).
//   ASCII  - no header, as BASIC's SAVE"name",A writes it: the firmware
//            reads a headerless file as text, so LOAD/RUN" tokenise it.
//            Bare LF line ends become CR LF and a ^Z (0x1A) end marker is
//            appended, or the padding after the last line reads as text.
//   AUTO   - pick from the host file (disk_resolve_put_mode).
enum class DiskPutMode : uint8_t { AUTO, BASIC, BINARY, ASCII };

// "auto", "basic", "binary" or "ascii" (any case). False for anything else.
bool disk_parse_put_mode(const std::string& word, DiskPutMode& out);

// Resolves AUTO from the host path (or, when its extension says nothing, the
// CPC name) and contents; any other mode is returned unchanged. .txt/.asc
// are ASCII. A .bas holding any 0x00 byte is tokenised (BASIC); one without is
// a listing (ASCII). Anything else is BINARY.
DiskPutMode disk_resolve_put_mode(DiskPutMode mode,
                                  const std::string& local_path,
                                  const std::vector<uint8_t>& data,
                                  const std::string& cpc_filename = "");

// Writes data as `mode` (AUTO is resolved first). In AUTO, a file that
// already carries a valid AMSDOS header is written unchanged. ASCII text ends
// at the first ^Z. Same return contract as disk_write_file.
std::string disk_put_file(t_drive* drive, const std::string& cpc_filename,
                          const std::string& local_path,
                          const std::vector<uint8_t>& data, DiskPutMode mode);

// Delete a file from disc.
// Returns empty string on success, error message on failure.
std::string disk_delete_file(t_drive* drive, const std::string& filename);

// Build an AMSDOS 128-byte header for the given parameters.
std::vector<uint8_t> disk_make_amsdos_header(const std::string& cpc_filename,
                                             AmsdosFileType type,
                                             uint16_t load_addr,
                                             uint16_t exec_addr,
                                             uint32_t data_length);

// Convert a local filename to CPC 8.3 format (uppercase, padded).
// Returns empty string if the filename cannot be converted.
std::string disk_to_cpc_filename(const std::string& local_name);
