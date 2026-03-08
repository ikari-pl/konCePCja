#include "m4board.h"
#include "disk.h"
#include "disk_file_editor.h"
#include "log.h"
#include <cstring>
#include <ctime>
#include <filesystem>
#include <algorithm>
#ifdef HAS_LIBCURL
#include <curl/curl.h>
#endif

// Forward declarations from slotshandler.cpp
int dsk_load(const std::string& filename, t_drive* drive);
void dsk_eject(t_drive* drive);

M4Board g_m4board;

// M4 command codes (from m4cmds.i)
static constexpr uint16_t C_OPEN       = 0x4301;
static constexpr uint16_t C_READ       = 0x4302;
static constexpr uint16_t C_WRITE      = 0x4303;
static constexpr uint16_t C_CLOSE      = 0x4304;
static constexpr uint16_t C_SEEK       = 0x4305;
static constexpr uint16_t C_READDIR    = 0x4306;
static constexpr uint16_t C_CD         = 0x4308;
static constexpr uint16_t C_ERASEFILE  = 0x430E;
static constexpr uint16_t C_RENAME     = 0x430F;
static constexpr uint16_t C_MAKEDIR    = 0x4310;
static constexpr uint16_t C_FSIZE      = 0x4311;
static constexpr uint16_t C_EOF        = 0x4307;
static constexpr uint16_t C_FREE       = 0x4309;
static constexpr uint16_t C_FTELL      = 0x430A;
static constexpr uint16_t C_GETPATH    = 0x4313;
static constexpr uint16_t C_HTTPGET    = 0x4320;
static constexpr uint16_t C_DIRSETARGS = 0x4325;
static constexpr uint16_t C_VERSION    = 0x4326;
static constexpr uint16_t C_READSECTOR = 0x430B;
static constexpr uint16_t C_READ2      = 0x4312;
static constexpr uint16_t C_FSTAT      = 0x4316;
static constexpr uint16_t C_WRITE2     = 0x431B;
static constexpr uint16_t C_WRITESECTOR = 0x430C;
static constexpr uint16_t C_FORMATTRACK = 0x430D;
static constexpr uint16_t C_SDREAD      = 0x4314;
static constexpr uint16_t C_SDWRITE     = 0x4315;
static constexpr uint16_t C_SETNETWORK  = 0x4321;
static constexpr uint16_t C_M4OFF       = 0x4322;
static constexpr uint16_t C_NETSTAT     = 0x4323;
static constexpr uint16_t C_TIME        = 0x4324;
static constexpr uint16_t C_HTTPGETMEM  = 0x4328;
static constexpr uint16_t C_ROMSUPDATE  = 0x432B;
static constexpr uint16_t C_NETSOCKET   = 0x4331;
static constexpr uint16_t C_NETCONNECT  = 0x4332;
static constexpr uint16_t C_NETCLOSE    = 0x4333;
static constexpr uint16_t C_NETSEND     = 0x4334;
static constexpr uint16_t C_NETRECV     = 0x4335;
static constexpr uint16_t C_NETHOSTIP   = 0x4336;
static constexpr uint16_t C_ROMWRITE    = 0x43FD;
static constexpr uint16_t C_CONFIG      = 0x43FE;

// Response status codes
static constexpr uint8_t M4_OK    = 0x00;
static constexpr uint8_t M4_ERROR = 0xFF;

// ── Path Safety ─────────────────────────────────

static std::string resolve_path(const std::string& rel_path)
{
   // Build full path from SD root + current dir + relative path
   std::string base = g_m4board.sd_root_path;
   if (base.empty()) return "";

   std::string full;
   if (!rel_path.empty() && rel_path[0] == '/') {
      full = base + rel_path;
   } else {
      full = base + g_m4board.current_dir;
      if (full.back() != '/') full += '/';
      full += rel_path;
   }

   // Resolve and verify it's within sd_root_path (component-aware check)
   try {
      auto canonical = std::filesystem::weakly_canonical(full);
      auto root_canonical = std::filesystem::weakly_canonical(base);
      auto rel = canonical.lexically_normal().lexically_relative(root_canonical.lexically_normal());
      if (rel.empty() || (!rel.empty() && *rel.begin() == "..")) {
         LOG_ERROR("M4: path traversal blocked: " << full);
         return "";
      }
      return canonical.string();
   } catch (const std::filesystem::filesystem_error& e) {
      LOG_ERROR("M4: " << e.what());
      return "";
   }
}

static std::string extract_string(const std::vector<uint8_t>& buf, size_t offset)
{
   std::string s;
   for (size_t i = offset; i < buf.size() && buf[i] != 0; i++) {
      s += static_cast<char>(buf[i]);
   }
   return s;
}

// ── Response Helpers ────────────────────────────
// Real M4 firmware response format: [status, len_lo, len_hi, data...]
// The ROM reads command-specific data from rom_response+3 (offset 3).
// For simple success: response[3] = 0. For errors: response[3] = 0xFF.

static void respond_ok()
{
   g_m4board.response[0] = M4_OK;
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;
   g_m4board.response[3] = 0;  // command-level success
   g_m4board.response_len = 4;
}

static void respond_error(const char* msg = nullptr)
{
   g_m4board.response[0] = M4_ERROR;
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;
   g_m4board.response[3] = 0xFF;  // command-level error marker
   if (msg) {
      size_t len = strlen(msg);
      size_t max = static_cast<size_t>(M4Board::RESPONSE_SIZE - 5);
      if (len > max) len = max;
      memcpy(g_m4board.response + 4, msg, len);
      g_m4board.response[4 + len] = 0;
      g_m4board.response_len = static_cast<int>(5 + len);
   } else {
      g_m4board.response_len = 4;
   }
}

// Error response with FatFs error code at response[4].
// The M4 ROM's fopen translates via ff_error_map[code] → CPC error.
//   0 = FR_OK, 4 = FR_NO_FILE, 5 = FR_NO_PATH, 8 = FR_EXIST, 255 = raw
static void respond_error_code(uint8_t code)
{
   g_m4board.response[0] = M4_ERROR;
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;
   g_m4board.response[3] = 0xFF;
   g_m4board.response[4] = code;
   g_m4board.response_len = 5;
}

// ── Container Helpers ───────────────────────────

static void container_exit()
{
   if (g_m4board.container_drive) {
      dsk_eject(g_m4board.container_drive);
      delete g_m4board.container_drive;
      g_m4board.container_drive = nullptr;
   }
   g_m4board.current_dir = g_m4board.container_parent_dir;
   g_m4board.container_parent_dir.clear();
   g_m4board.container_host_path.clear();
   g_m4board.container_type = M4Board::ContainerType::NONE;
}

static bool container_enter_dsk(const std::string& host_path, const std::string& parent_dir)
{
   auto* drive = new t_drive{};
   int rc = dsk_load(host_path, drive);
   if (rc != 0) {
      delete drive;
      return false;
   }
   g_m4board.container_drive = drive;
   g_m4board.container_type = M4Board::ContainerType::DSK;
   g_m4board.container_host_path = host_path;
   g_m4board.container_parent_dir = parent_dir;
   // current_dir shows the container filename as a virtual directory
   auto fname = std::filesystem::path(host_path).filename().string();
   g_m4board.current_dir = parent_dir + fname + "/";
   return true;
}

static bool in_container()
{
   return g_m4board.container_type != M4Board::ContainerType::NONE;
}

// ── Command Handlers ────────────────────────────

static void cmd_version()
{
   const char* ver = "M4 konCePCja v1.0";
   g_m4board.response[0] = M4_OK;
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;
   size_t len = strlen(ver);
   memcpy(g_m4board.response + 3, ver, len);
   g_m4board.response[3 + len] = 0;
   g_m4board.response_len = static_cast<int>(4 + len);
}

static void cmd_cd()
{
   std::string path = extract_string(g_m4board.cmd_buf, 3);

   // cd "/" — go to root, exit any container
   if (path == "/") {
      if (in_container()) container_exit();
      g_m4board.current_dir = "/";
      respond_ok();
      return;
   }

   // cd ".." from inside a container — exit the container
   if ((path == ".." || path == "../") && in_container()) {
      container_exit();
      respond_ok();
      return;
   }

   // Can't navigate further inside a container (no subdirectories in DSK)
   if (in_container()) {
      respond_error("Not a directory");
      return;
   }

   std::string resolved = resolve_path(path);
   if (resolved.empty()) {
      respond_error("Invalid path");
      return;
   }

   try {
      if (std::filesystem::is_directory(resolved)) {
         auto root_canonical = std::filesystem::weakly_canonical(g_m4board.sd_root_path);
         auto rel = std::filesystem::path(resolved).lexically_normal().lexically_relative(
            root_canonical.lexically_normal());
         std::string rel_str;
         if (rel == ".") {
            rel_str = "/";
         } else {
            rel_str = "/" + rel.generic_string();
            if (rel_str.back() != '/') rel_str += '/';
         }
         g_m4board.current_dir = rel_str;
         respond_ok();
      } else {
         // Check if it's a DSK file — enter as container
         std::string lower = path;
         for (auto& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
         if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".dsk") {
            if (container_enter_dsk(resolved, g_m4board.current_dir)) {
               respond_ok();
            } else {
               respond_error("Cannot open DSK");
            }
         } else {
            respond_error("Not a directory");
         }
      }
   } catch (const std::filesystem::filesystem_error& e) {
      LOG_ERROR("M4: " << e.what());
      respond_error(e.what());
   }
}

// Populate the directory entry cache (called by C_DIRSETARGS or on first C_READDIR)
static void dir_populate()
{
   g_m4board.dir_entries.clear();
   g_m4board.dir_index = 0;

   // Inside a DSK container — list CP/M directory entries
   if (in_container() && g_m4board.container_drive) {
      std::string err;
      auto files = disk_list_files(g_m4board.container_drive, err);
      if (!err.empty()) {
         LOG_ERROR("M4: container dir_populate: " << err);
         return;
      }
      for (const auto& f : files) {
         g_m4board.dir_entries.push_back({f.display_name, false, f.size_bytes});
      }
      return;
   }

   std::string resolved = resolve_path(g_m4board.current_dir);
   if (resolved.empty()) return;

   try {
      for (auto& entry : std::filesystem::directory_iterator(resolved)) {
         std::string name = entry.path().filename().string();
         if (name.empty() || name[0] == '.') continue; // skip dotfiles
         bool is_dir = entry.is_directory();
         uint32_t fsize = 0;
         if (!is_dir) {
            try { fsize = static_cast<uint32_t>(entry.file_size()); }
            catch (...) {}
         }
         g_m4board.dir_entries.push_back({name, is_dir, fsize});
      }
   } catch (const std::filesystem::filesystem_error& e) {
      LOG_ERROR("M4: " << e.what());
   }
}

// Convert a filename to AMSDOS 8.3 format: "FILENAME.EXT"
// Returns 12 characters (8 + '.' + 3), space-padded, uppercase
static void format_amsdos_83(const std::string& name, bool is_dir, char out[12])
{
   // Split on last '.' to separate name and extension
   std::string base, ext;
   size_t dot = name.rfind('.');
   if (dot != std::string::npos && dot > 0 && !is_dir) {
      base = name.substr(0, dot);
      ext = name.substr(dot + 1);
   } else {
      base = name;
   }

   // Truncate and pad filename to 8 chars
   for (int i = 0; i < 8; i++) {
      if (i < static_cast<int>(base.size()))
         out[i] = static_cast<char>(toupper(static_cast<unsigned char>(base[i])));
      else
         out[i] = ' ';
   }
   out[8] = '.';
   // Truncate and pad extension to 3 chars
   for (int i = 0; i < 3; i++) {
      if (i < static_cast<int>(ext.size()))
         out[9 + i] = static_cast<char>(toupper(static_cast<unsigned char>(ext[i])));
      else
         out[9 + i] = ' ';
   }
}

static void cmd_readdir()
{
   // If no entries cached yet (e.g. C_DIRSETARGS wasn't called), populate now
   if (g_m4board.dir_entries.empty() && g_m4board.dir_index == 0) {
      dir_populate();
   }

   // Check if directory listing is exhausted
   if (g_m4board.dir_index >= g_m4board.dir_entries.size()) {
      // Status 2 = end of directory
      g_m4board.response[0] = 2;
      g_m4board.response[1] = 0;
      g_m4board.response[2] = 0;
      g_m4board.response_len = 3;
      return;
   }

   auto& entry = g_m4board.dir_entries[g_m4board.dir_index++];

   // Check if LS mode (long filenames) — cmd_buf has extra data byte
   bool ls_mode = (g_m4board.cmd_buf.size() > 3);

   g_m4board.response[0] = 1;  // status: entry present
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;

   if (ls_mode) {
      // LS format: rom_response[3+] = null-terminated string
      // Directories prefixed with '>'
      int pos = 3;
      if (entry.is_dir) {
         g_m4board.response[pos++] = '>';
      }
      int max_name = M4Board::RESPONSE_SIZE - pos - 1;
      int len = std::min(static_cast<int>(entry.name.size()), max_name);
      memcpy(g_m4board.response + pos, entry.name.c_str(), len);
      pos += len;
      g_m4board.response[pos++] = 0;
      g_m4board.response_len = pos;
   } else {
      // CAT format: rom_response[3+] = AMSDOS 8.3 directory entry (20 bytes)
      // Bytes 0-7:   FILENAME (8 chars, space-padded, uppercase)
      // Byte 8:      '.'
      // Bytes 9-11:  EXT (3 chars, attributes in bit 7)
      // Bytes 12-16: ASCII size (5 chars, e.g. " 123K" or "<DIR>")
      // Byte 17:     null terminator
      // Bytes 18-19: binary file size in KB (16-bit LE)
      int pos = 3;
      char name83[12];
      format_amsdos_83(entry.name, entry.is_dir, name83);
      memcpy(g_m4board.response + pos, name83, 12);  // FILENAME.EXT
      pos += 12;

      // ASCII size field (5 chars): "1234B", " 100K", "  15M", "   2G", or "<DIR>"
      char sizebuf[12];
      if (entry.is_dir) {
         memcpy(sizebuf, "<DIR>", 5);
      } else if (entry.size < 1024) {
         snprintf(sizebuf, sizeof(sizebuf), "%4uB", entry.size);
      } else if (entry.size < 1024 * 1024) {
         snprintf(sizebuf, sizeof(sizebuf), "%4uK", (entry.size + 1023) / 1024);
      } else if (entry.size < 1024u * 1024 * 1024) {
         snprintf(sizebuf, sizeof(sizebuf), "%4uM", (entry.size + 1024*1024 - 1) / (1024*1024));
      } else {
         snprintf(sizebuf, sizeof(sizebuf), "%4uG", (entry.size + 1024u*1024*1024 - 1) / (1024u*1024*1024));
      }
      memcpy(g_m4board.response + pos, sizebuf, 5);
      pos += 5;
      g_m4board.response[pos++] = 0;  // null terminator

      // Binary file size in KB (16-bit LE)
      uint16_t kb = static_cast<uint16_t>(std::min(entry.size / 1024, uint32_t(0xFFFF)));
      g_m4board.response[pos++] = kb & 0xFF;
      g_m4board.response[pos++] = (kb >> 8) & 0xFF;

      g_m4board.response_len = pos;
   }
}

// Try to open a file, searching case-insensitively and with CPC extensions.
// CPC convention: RUN"TEST means search for TEST, TEST.BAS, TEST.BIN, etc.
static FILE* try_open_cpc(const std::string& dir_path, const std::string& filename)
{
   // Build list of names to try: exact, with extensions
   std::vector<std::string> candidates = { filename };
   // If no extension in the requested name, try common CPC extensions
   if (filename.find('.') == std::string::npos) {
      for (const char* ext : { ".BAS", ".BIN", ".", "" }) {
         candidates.push_back(filename + ext);
      }
   }

   // Scan directory for case-insensitive match against each candidate
   try {
      for (const auto& entry : std::filesystem::directory_iterator(dir_path)) {
         std::string entry_name = entry.path().filename().string();
         std::string entry_upper = entry_name;
         for (auto& c : entry_upper) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));

         for (const auto& cand : candidates) {
            std::string cand_upper = cand;
            for (auto& c : cand_upper) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
            if (entry_upper == cand_upper) {
               std::string full = dir_path + "/" + entry_name;
               FILE* fp = fopen(full.c_str(), "r+b");
               if (!fp) fp = fopen(full.c_str(), "rb");
               if (fp) {
                  LOG_DEBUG("M4: C_OPEN matched '" << entry_name << "' for '" << filename << "'");
                  return fp;
               }
            }
         }
      }
   } catch (const std::filesystem::filesystem_error&) {}
   return nullptr;
}

// Open a file from inside a DSK container: extract to a temp file, return FILE*.
static FILE* container_open_file(const std::string& filename)
{
   if (!g_m4board.container_drive) return nullptr;

   std::string err;
   auto data = disk_read_file(g_m4board.container_drive, filename, err);
   if (!err.empty()) {
      // Case-insensitive retry with CPC extensions
      std::vector<std::string> candidates = { filename };
      if (filename.find('.') == std::string::npos) {
         for (const char* ext : { ".BAS", ".BIN", ".", "" })
            candidates.push_back(filename + ext);
      }
      // Get file list and match case-insensitively
      auto files = disk_list_files(g_m4board.container_drive, err);
      err.clear();
      for (const auto& cand : candidates) {
         std::string cand_upper = cand;
         for (auto& c : cand_upper) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
         for (const auto& f : files) {
            std::string disp_upper = f.display_name;
            for (auto& c : disp_upper) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
            if (disp_upper == cand_upper) {
               data = disk_read_file(g_m4board.container_drive, f.display_name, err);
               if (err.empty()) goto found;
            }
         }
      }
      return nullptr;
   }
found:
   // Write extracted data to a temp file
   FILE* fp = tmpfile();
   if (!fp) return nullptr;
   if (!data.empty()) {
      fwrite(data.data(), 1, data.size(), fp);
      fflush(fp);
      rewind(fp);
   }
   return fp;
}

static void cmd_open()
{
   // cmd_buf layout: [size, 0x01, 0x43, mode, filename...]
   // mode = FatFs access flags: FA_READ=0x01, FA_WRITE=0x02, FA_CREATE_ALWAYS=0x08
   // Read (RUN/LOAD):  mode = FA_READ (0x01)
   // Write (SAVE):     mode = FA_WRITE|FA_CREATE_ALWAYS (0x0A)
   uint8_t mode = (g_m4board.cmd_buf.size() > 3) ? g_m4board.cmd_buf[3] : 0x01;
   bool is_write = (mode & 0x02) != 0;   // FA_WRITE
   bool is_create = (mode & 0x08) != 0;  // FA_CREATE_ALWAYS
   std::string raw_name = extract_string(g_m4board.cmd_buf, 4);
   if (raw_name.empty()) {
      raw_name = extract_string(g_m4board.cmd_buf, 3);
   }
   LOG_DEBUG("M4: C_OPEN name='" << raw_name << "' mode=0x" << std::hex << (int)mode << std::dec);

   // Inside a container — read-only, extract file to temp
   if (in_container()) {
      if (is_write) {
         respond_error_code(7); // FR_DENIED — containers are read-only
         return;
      }
      int handle = 1; // FA_READ → handle 1
      if (g_m4board.open_files[handle]) {
         fclose(g_m4board.open_files[handle]);
         g_m4board.open_files[handle] = nullptr;
      }
      g_m4board.open_files[handle] = container_open_file(raw_name);
      if (!g_m4board.open_files[handle]) {
         respond_error_code(4); // FR_NO_FILE
         return;
      }
      g_m4board.last_filename = raw_name;
      LOG_DEBUG("M4: C_OPEN container → OK, handle=" << handle);
      g_m4board.response[0] = M4_OK;
      g_m4board.response[1] = 0;
      g_m4board.response[2] = 0;
      g_m4board.response[3] = static_cast<uint8_t>(handle);
      g_m4board.response[4] = 0;  // FR_OK
      g_m4board.response_len = 5;
      return;
   }

   std::string dir = resolve_path(g_m4board.current_dir);
   if (dir.empty()) {
      respond_error_code(5); // FR_NO_PATH
      return;
   }

   // The M4 ROM hardcodes fd=1 for CAS input, fd=2 for CAS output.
   // Use the ROM's convention: write → fd 2, read → fd 1, else first free.
   int handle = -1;
   if (is_write) {
      handle = 2;
   } else if (mode == 0x01) {  // FA_READ only
      handle = 1;
   }
   if (handle >= 0 && g_m4board.open_files[handle]) {
      // Close previous file at this slot
      fclose(g_m4board.open_files[handle]);
      g_m4board.open_files[handle] = nullptr;
   }
   if (handle < 0) {
      // General file access: find first free handle
      for (int i = 0; i < 4; i++) {
         if (!g_m4board.open_files[i]) { handle = i; break; }
      }
   }
   if (handle < 0) {
      respond_error_code(255); // no free handles
      return;
   }

   if (is_write && is_create) {
      // Write+create mode (SAVE): create or truncate in current directory
      std::string resolved = resolve_path(raw_name);
      if (resolved.empty()) {
         respond_error_code(6); // FR_INVALID_NAME
         return;
      }
      g_m4board.open_files[handle] = fopen(resolved.c_str(), "w+b");
      if (!g_m4board.open_files[handle]) {
         respond_error_code(7); // FR_DENIED
         return;
      }
   } else {
      // Read mode (RUN/LOAD): search for existing file
      std::string resolved = resolve_path(raw_name);
      if (!resolved.empty()) {
         g_m4board.open_files[handle] = fopen(resolved.c_str(), is_write ? "r+b" : "rb");
      }
      // Case-insensitive search with CPC extension probing
      if (!g_m4board.open_files[handle]) {
         g_m4board.open_files[handle] = try_open_cpc(dir, raw_name);
      }
      if (!g_m4board.open_files[handle]) {
         respond_error_code(4); // FR_NO_FILE
         return;
      }
   }

   g_m4board.last_filename = raw_name;
   LOG_DEBUG("M4: C_OPEN → OK, handle=" << handle);
   g_m4board.response[0] = M4_OK;
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;
   g_m4board.response[3] = static_cast<uint8_t>(handle);
   g_m4board.response[4] = 0;  // FR_OK
   g_m4board.response_len = 5;
}

static void cmd_close()
{
   if (g_m4board.cmd_buf.size() < 4) {
      respond_error();
      return;
   }
   int handle = g_m4board.cmd_buf[3];
   if (handle >= 0 && handle < 4 && g_m4board.open_files[handle]) {
      fclose(g_m4board.open_files[handle]);
      g_m4board.open_files[handle] = nullptr;
   }
   respond_ok();
}

// C_READ: Used by fread() in M4 ROM for bulk data loading (_cas_in_direct, etc.)
// Protocol: cmd[3]=fd, cmd[4-5]=count (16-bit LE)
// Response: response[3]=status (0=OK, non-zero=error), response[4+]=data bytes
// ROM copies data from rom_response+4 with LDIR, bc=requested chunk size.
static void cmd_read()
{
   if (g_m4board.cmd_buf.size() < 6) {
      respond_error();
      return;
   }
   int handle = g_m4board.cmd_buf[3];
   uint16_t count = g_m4board.cmd_buf[4] | (g_m4board.cmd_buf[5] << 8);

   if (handle < 0 || handle >= 4 || !g_m4board.open_files[handle]) {
      respond_error("Bad handle");
      return;
   }

   int max_read = M4Board::RESPONSE_SIZE - 4;
   if (count > max_read) count = static_cast<uint16_t>(max_read);

   size_t nread = fread(g_m4board.response + 4, 1, count, g_m4board.open_files[handle]);

   g_m4board.response[0] = M4_OK;
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;
   g_m4board.response[3] = 0;  // status: 0 = OK
   g_m4board.response_len = static_cast<int>(4 + nread);
}

// C_READ2: Used by _cas_in_char() for byte-by-byte BASIC file reading.
// Same as C_READ but without AMSDOS header checking.
// Protocol: cmd[3]=fd, cmd[4-5]=count (sent as 0x0800 = 2048 bytes)
// Response: response[3]=status (0=OK, 0x14=EOF), response[4-5]=bytes_read (16-bit LE),
//           response[8+]=data bytes.
// ROM copies data from rom_response+8 with LDIR, bc=response[4-5].
static void cmd_read2()
{
   if (g_m4board.cmd_buf.size() < 6) {
      respond_error();
      return;
   }
   int handle = g_m4board.cmd_buf[3];
   uint16_t count = g_m4board.cmd_buf[4] | (g_m4board.cmd_buf[5] << 8);

   if (handle < 0 || handle >= 4 || !g_m4board.open_files[handle]) {
      respond_error("Bad handle");
      return;
   }

   int max_read = M4Board::RESPONSE_SIZE - 8;
   if (count > max_read) count = static_cast<uint16_t>(max_read);

   size_t nread = fread(g_m4board.response + 8, 1, count, g_m4board.open_files[handle]);
   bool at_eof = feof(g_m4board.open_files[handle]);

   g_m4board.response[0] = M4_OK;
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;
   g_m4board.response[3] = at_eof ? 0x14 : 0;  // 0x14 = EOF indicator
   g_m4board.response[4] = nread & 0xFF;
   g_m4board.response[5] = (nread >> 8) & 0xFF;
   g_m4board.response[6] = 0;
   g_m4board.response[7] = 0;
   g_m4board.response_len = static_cast<int>(8 + nread);
}

// C_READSECTOR: BIOS-level disc sector read redirected through M4 virtual FS.
// Protocol: cmd[3]=track, cmd[4]=sector, cmd[5]=drive
// Response: response[3]=status (0=OK), response[4+]=512 bytes sector data.
// On the real M4, this reads from .dsk images. For our virtual FS (loose files),
// we map track/sector to a linear file offset via AMSDOS convention:
// offset = (track * 9 + (sector - 0xC1)) * 512
// AMSDOS sectors are numbered &C1-&C9 (9 sectors per track).
static void cmd_readsector()
{
   if (g_m4board.cmd_buf.size() < 6) {
      respond_error();
      return;
   }
   uint8_t track  = g_m4board.cmd_buf[3];
   uint8_t sector = g_m4board.cmd_buf[4];
   // cmd_buf[5] = drive (unused for virtual FS)

   // Inside a DSK container — read directly from the parsed t_drive
   if (in_container() && g_m4board.container_drive) {
      t_drive* drv = g_m4board.container_drive;
      if (track >= drv->tracks) {
         g_m4board.response[0] = M4_OK;
         g_m4board.response[3] = 1; // error
         memset(g_m4board.response + 4, 0xE5, 512);
         g_m4board.response_len = 4 + 512;
         return;
      }
      t_track& trk = drv->track[track][0];
      // Find sector by ID
      uint8_t* data = nullptr;
      for (unsigned int s = 0; s < trk.sectors; s++) {
         if (trk.sector[s].CHRN[2] == sector) {
            data = trk.sector[s].getDataForRead();
            break;
         }
      }
      if (data) {
         memcpy(g_m4board.response + 4, data, 512);
      } else {
         memset(g_m4board.response + 4, 0xE5, 512);
      }
      g_m4board.response[0] = M4_OK;
      g_m4board.response[1] = 0;
      g_m4board.response[2] = 0;
      g_m4board.response[3] = 0;
      g_m4board.response_len = 4 + 512;
      return;
   }

   // We need an open file to read from. Use handle 1 (the read handle).
   FILE* f = g_m4board.open_files[1];
   if (!f) {
      // No file open — return error
      g_m4board.response[0] = M4_OK;
      g_m4board.response[1] = 0;
      g_m4board.response[2] = 0;
      g_m4board.response[3] = 1;  // error status
      memset(g_m4board.response + 4, 0xE5, 512);
      g_m4board.response_len = 4 + 512;
      return;
   }

   // AMSDOS sectors &C1-&C9 → linear sector 0-8
   int linear_sector = (sector >= 0xC1) ? (sector - 0xC1) : sector;
   long offset = (track * 9 + linear_sector) * 512L;

   fseek(f, offset, SEEK_SET);
   size_t nread = fread(g_m4board.response + 4, 1, 512, f);
   // Pad remainder with 0xE5 (empty disc filler)
   if (nread < 512)
      memset(g_m4board.response + 4 + nread, 0xE5, 512 - nread);

   g_m4board.response[0] = M4_OK;
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;
   g_m4board.response[3] = 0;  // status: OK
   g_m4board.response_len = 4 + 512;
}

static void cmd_fsize()
{
   std::string path = extract_string(g_m4board.cmd_buf, 3);

   // Inside a container — look up file size from CP/M directory
   if (in_container() && g_m4board.container_drive) {
      std::string err;
      auto files = disk_list_files(g_m4board.container_drive, err);
      // Case-insensitive search
      std::string upper = path;
      for (auto& c : upper) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
      for (const auto& f : files) {
         std::string fu = f.display_name;
         for (auto& c : fu) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
         if (fu == upper) {
            uint32_t sz = f.size_bytes;
            g_m4board.response[0] = M4_OK;
            g_m4board.response[1] = 0;
            g_m4board.response[2] = 0;
            g_m4board.response[3] = sz & 0xFF;
            g_m4board.response[4] = (sz >> 8) & 0xFF;
            g_m4board.response[5] = (sz >> 16) & 0xFF;
            g_m4board.response[6] = (sz >> 24) & 0xFF;
            g_m4board.response_len = 7;
            return;
         }
      }
      respond_error("File not found");
      return;
   }

   std::string resolved = resolve_path(path);
   if (resolved.empty()) {
      respond_error("Invalid path");
      return;
   }

   try {
      auto fsize = std::filesystem::file_size(resolved);
      g_m4board.response[0] = M4_OK;
      g_m4board.response[1] = 0;
      g_m4board.response[2] = 0;
      g_m4board.response[3] = fsize & 0xFF;
      g_m4board.response[4] = (fsize >> 8) & 0xFF;
      g_m4board.response[5] = (fsize >> 16) & 0xFF;
      g_m4board.response[6] = (fsize >> 24) & 0xFF;
      g_m4board.response_len = 7;
   } catch (const std::filesystem::filesystem_error& e) {
      LOG_ERROR("M4: " << e.what());
      respond_error(e.what());
   }
}

static void cmd_erasefile()
{
   if (in_container()) { respond_error("Read-only container"); return; }
   std::string path = extract_string(g_m4board.cmd_buf, 3);
   std::string resolved = resolve_path(path);
   if (resolved.empty()) {
      respond_error("Invalid path");
      return;
   }

   try {
      if (std::filesystem::remove(resolved)) {
         respond_ok();
      } else {
         respond_error("File not found");
      }
   } catch (const std::filesystem::filesystem_error& e) {
      LOG_ERROR("M4: " << e.what());
      respond_error(e.what());
   }
}

static void cmd_makedir()
{
   if (in_container()) { respond_error("Read-only container"); return; }
   std::string path = extract_string(g_m4board.cmd_buf, 3);
   std::string resolved = resolve_path(path);
   if (resolved.empty()) {
      respond_error("Invalid path");
      return;
   }

   try {
      std::filesystem::create_directories(resolved);
      respond_ok();
   } catch (const std::filesystem::filesystem_error& e) {
      LOG_ERROR("M4: " << e.what());
      respond_error(e.what());
   }
}

static void cmd_write()
{
   if (in_container()) { respond_error("Read-only container"); return; }
   // Protocol: [size, cmd_lo, cmd_hi, fd, data...]
   // cmd_buf[3] = fd, cmd_buf[4..] = data to write
   if (g_m4board.cmd_buf.size() < 5) {
      LOG_DEBUG("M4: C_WRITE too short, buf_size=" << g_m4board.cmd_buf.size());
      respond_error();
      return;
   }
   int handle = g_m4board.cmd_buf[3];
   if (handle < 0 || handle >= 4 || !g_m4board.open_files[handle]) {
      LOG_DEBUG("M4: C_WRITE bad handle=" << handle
               << " file=" << (g_m4board.open_files[handle & 3] ? "open" : "null"));
      respond_error("Bad handle");
      return;
   }

   size_t data_len = g_m4board.cmd_buf.size() - 4;
   LOG_DEBUG("M4: C_WRITE fd=" << handle << " data_len=" << data_len
            << " buf_size=" << g_m4board.cmd_buf.size());
   size_t written = fwrite(g_m4board.cmd_buf.data() + 4, 1, data_len,
                           g_m4board.open_files[handle]);
   fflush(g_m4board.open_files[handle]);

   if (written == data_len) {
      respond_ok();
   } else {
      respond_error("Write failed");
   }
}

static void cmd_seek()
{
   // Protocol: [size, cmd_lo, cmd_hi, fd, offset(4 bytes LE)]
   // cmd_buf[3] = fd, cmd_buf[4..7] = offset
   if (g_m4board.cmd_buf.size() < 8) {
      respond_error();
      return;
   }
   int handle = g_m4board.cmd_buf[3];
   if (handle < 0 || handle >= 4 || !g_m4board.open_files[handle]) {
      respond_error("Bad handle");
      return;
   }

   uint32_t offset = static_cast<uint32_t>(g_m4board.cmd_buf[4]) |
                     (static_cast<uint32_t>(g_m4board.cmd_buf[5]) << 8) |
                     (static_cast<uint32_t>(g_m4board.cmd_buf[6]) << 16) |
                     (static_cast<uint32_t>(g_m4board.cmd_buf[7]) << 24);

   if (fseek(g_m4board.open_files[handle], static_cast<long>(offset), SEEK_SET) == 0) {
      respond_ok();
   } else {
      respond_error("Seek failed");
   }
}

static void cmd_rename()
{
   if (in_container()) { respond_error("Read-only container"); return; }
   // Protocol: [size, cmd_lo, cmd_hi, "newname\0oldname\0"]
   // cmd_buf[3..] = "newname\0oldname\0"
   std::string newname = extract_string(g_m4board.cmd_buf, 3);
   // Find the second string after the first null terminator
   size_t old_offset = 3 + newname.size() + 1;
   if (old_offset >= g_m4board.cmd_buf.size()) {
      respond_error("Missing old name");
      return;
   }
   std::string oldname = extract_string(g_m4board.cmd_buf, old_offset);

   std::string resolved_old = resolve_path(oldname);
   std::string resolved_new = resolve_path(newname);
   if (resolved_old.empty() || resolved_new.empty()) {
      respond_error("Invalid path");
      return;
   }

   try {
      std::filesystem::rename(resolved_old, resolved_new);
      respond_ok();
   } catch (const std::exception& e) {
      respond_error(e.what());
   }
}

// ── HTTP GET ────────────────────────────────────

#ifdef HAS_LIBCURL

static size_t curl_write_file(void* ptr, size_t size, size_t nmemb, void* userdata)
{
   FILE* fp = static_cast<FILE*>(userdata);
   return fwrite(ptr, size, nmemb, fp);
}

static void cmd_httpget()
{
   // Protocol: [size, cmd_lo, cmd_hi, "url:port/file"]
   // URL format: [@ prefix]host[:port]/path[>outfile]
   std::string raw_url = extract_string(g_m4board.cmd_buf, 3);
   if (raw_url.empty()) {
      respond_error("No URL given");
      return;
   }

   // Strip leading @ (silent mode — doesn't affect emulation)
   std::string url = raw_url;
   if (!url.empty() && url[0] == '@') url = url.substr(1);

   // Strip http:// prefix if present (as real firmware does)
   if (url.substr(0, 7) == "http://") url = url.substr(7);

   // Check for >filename redirect suffix
   std::string out_filename;
   auto redir = url.rfind('>');
   if (redir != std::string::npos) {
      out_filename = url.substr(redir + 1);
      url = url.substr(0, redir);
   }

   // If no redirect filename, extract from URL path
   if (out_filename.empty()) {
      auto last_slash = url.rfind('/');
      if (last_slash != std::string::npos) {
         out_filename = url.substr(last_slash + 1);
      }
   }

   if (out_filename.empty()) {
      respond_error("No filename in URL");
      return;
   }

   // Build the full HTTP URL
   std::string full_url = "http://" + url;

   // Resolve output path within the virtual SD
   std::string dest = resolve_path(out_filename);
   if (dest.empty()) {
      respond_error("Invalid output path");
      return;
   }

   // Perform the HTTP GET download
   FILE* fp = fopen(dest.c_str(), "wb");
   if (!fp) {
      respond_error("Cannot create file");
      return;
   }

   CURL* curl = curl_easy_init();
   if (!curl) {
      fclose(fp);
      std::filesystem::remove(dest);
      respond_error("HTTP init failed");
      return;
   }

   curl_easy_setopt(curl, CURLOPT_URL, full_url.c_str());
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_file);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
   curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
   curl_easy_setopt(curl, CURLOPT_USERAGENT, "M4Board/2.0");
   curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

   CURLcode res = curl_easy_perform(curl);
   long http_code = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
   curl_easy_cleanup(curl);
   fclose(fp);

   if (res != CURLE_OK) {
      std::filesystem::remove(dest);
      respond_error(curl_easy_strerror(res));
      LOG_ERROR("M4 HTTPGET: " << curl_easy_strerror(res) << " (URL: " << full_url << ")");
      return;
   }

   // Success response: message at offset 3
   std::string ok_msg = "Downloaded " + out_filename + "\r\n";
   g_m4board.response[0] = M4_OK;
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;
   size_t max_ok = static_cast<size_t>(M4Board::RESPONSE_SIZE - 4);
   if (ok_msg.size() > max_ok) ok_msg.resize(max_ok);
   memcpy(g_m4board.response + 3, ok_msg.c_str(), ok_msg.size() + 1);
   g_m4board.response_len = static_cast<int>(4 + ok_msg.size());
   LOG_INFO("M4 HTTPGET: downloaded " << full_url << " -> " << dest);
}

#else // !HAS_LIBCURL

static void cmd_httpget()
{
   respond_error("HTTP not available (no libcurl)");
   LOG_ERROR("M4 HTTPGET: libcurl not available at build time");
}

#endif // HAS_LIBCURL

// ── HTTP GET to memory ──────────────────────────

#ifdef HAS_LIBCURL

struct MemBuf {
   std::vector<uint8_t> data;
   size_t max_size;
};

static size_t curl_write_mem(void* ptr, size_t size, size_t nmemb, void* userdata)
{
   auto* buf = static_cast<MemBuf*>(userdata);
   size_t total = size * nmemb;
   size_t remain = buf->max_size - buf->data.size();
   size_t to_copy = std::min(total, remain);
   if (to_copy > 0) {
      auto* bytes = static_cast<uint8_t*>(ptr);
      buf->data.insert(buf->data.end(), bytes, bytes + to_copy);
   }
   return total; // pretend we consumed all (to avoid curl error)
}

static void cmd_httpgetmem()
{
   // Protocol: [size, cmd_lo, cmd_hi, size_hi, size_lo, url\0]
   // Downloads to CPC memory instead of SD card
   if (g_m4board.cmd_buf.size() < 6) {
      respond_error("Bad args");
      return;
   }
   uint16_t max_dl = (static_cast<uint16_t>(g_m4board.cmd_buf[3]) << 8) |
                      static_cast<uint16_t>(g_m4board.cmd_buf[4]);
   std::string raw_url = extract_string(g_m4board.cmd_buf, 5);
   if (raw_url.empty()) {
      respond_error("No URL given");
      return;
   }

   std::string url = raw_url;
   if (!url.empty() && url[0] == '@') url = url.substr(1);
   if (url.substr(0, 7) == "http://") url = url.substr(7);
   std::string full_url = "http://" + url;

   // Cap download to response buffer space (offset 5 onward)
   size_t max_size = std::min(static_cast<size_t>(max_dl),
      static_cast<size_t>(M4Board::RESPONSE_SIZE - 5));

   MemBuf membuf;
   membuf.max_size = max_size;

   CURL* curl = curl_easy_init();
   if (!curl) {
      respond_error("HTTP init failed");
      return;
   }

   curl_easy_setopt(curl, CURLOPT_URL, full_url.c_str());
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_mem);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &membuf);
   curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
   curl_easy_setopt(curl, CURLOPT_USERAGENT, "M4Board/2.0");
   curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

   CURLcode res = curl_easy_perform(curl);
   curl_easy_cleanup(curl);

   if (res != CURLE_OK) {
      respond_error(curl_easy_strerror(res));
      return;
   }

   // Response: [status=0] [0] [0] [dl_size_hi] [dl_size_lo] [data...]
   uint16_t dl_size = static_cast<uint16_t>(membuf.data.size());
   g_m4board.response[0] = M4_OK;
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;
   g_m4board.response[3] = static_cast<uint8_t>(dl_size >> 8);
   g_m4board.response[4] = static_cast<uint8_t>(dl_size & 0xFF);
   if (!membuf.data.empty())
      memcpy(g_m4board.response + 5, membuf.data.data(), membuf.data.size());
   g_m4board.response_len = static_cast<int>(5 + membuf.data.size());
   LOG_INFO("M4 HTTPGETMEM: downloaded " << dl_size << " bytes from " << full_url);
}

#else // !HAS_LIBCURL

static void cmd_httpgetmem()
{
   respond_error("HTTP not available (no libcurl)");
}

#endif // HAS_LIBCURL

// ── Write Sector ────────────────────────────────

static void cmd_writesector()
{
   if (in_container()) { respond_error("Read-only container"); return; }
   // Protocol: [size, cmd_lo, cmd_hi, track, sector, drive, 512 bytes]
   if (g_m4board.cmd_buf.size() < 6 + 512) {
      respond_error();
      return;
   }
   uint8_t track  = g_m4board.cmd_buf[3];
   uint8_t sector = g_m4board.cmd_buf[4];
   // cmd_buf[5] = drive (unused for virtual FS)

   FILE* f = g_m4board.open_files[2]; // write handle
   if (!f) {
      respond_error("No file open for write");
      return;
   }

   int linear_sector = (sector >= 0xC1) ? (sector - 0xC1) : sector;
   long offset = (track * 9 + linear_sector) * 512L;

   fseek(f, offset, SEEK_SET);
   size_t written = fwrite(g_m4board.cmd_buf.data() + 6, 1, 512, f);
   fflush(f);

   if (written == 512) {
      respond_ok();
   } else {
      respond_error("Write sector failed");
   }
}

static void cmd_formattrack()
{
   // Not implemented even on real M4 hardware
   respond_error("Format not supported");
}

// ── Raw SD Card Access ──────────────────────────
// The real M4 provides raw block-level access to the SD card.
// We map LBA sectors to a flat file ("sdcard.img") in the SD root, if present.

static void cmd_sdread()
{
   // Protocol: [size, cmd_lo, cmd_hi, lba0, lba1, lba2, lba3, num_sectors]
   if (g_m4board.cmd_buf.size() < 8) {
      respond_error();
      return;
   }
   uint32_t lba = static_cast<uint32_t>(g_m4board.cmd_buf[3]) |
                  (static_cast<uint32_t>(g_m4board.cmd_buf[4]) << 8) |
                  (static_cast<uint32_t>(g_m4board.cmd_buf[5]) << 16) |
                  (static_cast<uint32_t>(g_m4board.cmd_buf[6]) << 24);
   uint8_t num = g_m4board.cmd_buf[7];

   // Raw SD not supported in emulation — return error
   (void)lba;
   (void)num;
   g_m4board.response[0] = M4_OK;
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;
   g_m4board.response[3] = 1; // error flag
   g_m4board.response_len = 4;
   LOG_DEBUG("M4: C_SDREAD lba=" << lba << " n=" << (int)num << " (not supported)");
}

static void cmd_sdwrite()
{
   // Protocol: [size, cmd_lo, cmd_hi, lba0, lba1, lba2, lba3, num_sectors, data...]
   // Raw SD not supported in emulation
   g_m4board.response[0] = M4_OK;
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;
   g_m4board.response[3] = 1; // error flag
   g_m4board.response_len = 4;
   LOG_DEBUG("M4: C_SDWRITE (not supported)");
}

// ── Network & Socket Operations ─────────────────
// The real M4 has an ESP8266 WiFi module. In emulation we return
// "not connected" / "no network" responses that match the firmware's
// expected response format, so the ROM handles it gracefully.

static void cmd_setnetwork()
{
   // Protocol: [size, cmd_lo, cmd_hi, setup_string\0]
   // The real M4 configures WiFi SSID/password here.
   // In emulation, just acknowledge.
   respond_ok();
   LOG_DEBUG("M4: C_SETNETWORK (ignored in emulation)");
}

static void cmd_netstat()
{
   // Response: [status_string] [status_byte]
   // Status byte: 0=disconnected, 1=connecting, 2=wrong_password,
   //              3=no_ap, 4=connect_fail, 5=connected
   // We report "not connected" — the ROM shows this as network status.
   g_m4board.response[0] = M4_OK;
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;
   const char* msg = "Emulated (no WiFi)\r\n";
   size_t len = strlen(msg);
   memcpy(g_m4board.response + 3, msg, len);
   g_m4board.response[3 + len] = 0; // status byte: 0 = disconnected
   g_m4board.response_len = static_cast<int>(4 + len);
}

static void cmd_time()
{
   // Response: "hh:mm:ss yyyy-mm-dd" at offset 3
   time_t now = time(nullptr);
   struct tm* t = localtime(&now);
   char buf[32];
   snprintf(buf, sizeof(buf), "%02d:%02d:%02d %04d-%02d-%02d",
            t->tm_hour, t->tm_min, t->tm_sec,
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
   g_m4board.response[0] = M4_OK;
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;
   size_t len = strlen(buf);
   memcpy(g_m4board.response + 3, buf, len + 1);
   g_m4board.response_len = static_cast<int>(4 + len);
}

static void cmd_m4off()
{
   // Disable M4 ROM and trigger a reset.
   // On real hardware this disables the M4 until next power cycle.
   g_m4board.enabled = false;
   respond_ok();
   LOG_INFO("M4: C_M4OFF — M4 Board disabled");
}

static void cmd_netsocket()
{
   // Protocol: [size, cmd_lo, cmd_hi, domain, type, protocol]
   // Response: [socket_num or 0xFF=error]
   // No real network available — return error
   g_m4board.response[0] = M4_OK;
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;
   g_m4board.response[3] = 0xFF; // error: no socket available
   g_m4board.response_len = 4;
}

static void cmd_netconnect()
{
   // Protocol: [size, cmd_lo, cmd_hi, socket, ip0, ip1, ip2, ip3, port_hi, port_lo]
   // Response: [0=OK, 0xFF=error]
   g_m4board.response[0] = M4_OK;
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;
   g_m4board.response[3] = 0xFF; // error: connection failed
   g_m4board.response_len = 4;
}

static void cmd_netclose()
{
   // Protocol: [size, cmd_lo, cmd_hi, socket]
   respond_ok();
}

static void cmd_netsend()
{
   // Protocol: [size, cmd_lo, cmd_hi, socket, size_lo, size_hi, data...]
   // Response: [0=OK, 0xFF=error]
   g_m4board.response[0] = M4_OK;
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;
   g_m4board.response[3] = 0xFF; // error: not connected
   g_m4board.response_len = 4;
}

static void cmd_netrecv()
{
   // Protocol: [size, cmd_lo, cmd_hi, socket, size_lo, size_hi]
   // Response: [0] [actual_lo] [actual_hi] [data...]
   g_m4board.response[0] = M4_OK;
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;
   g_m4board.response[3] = 0;  // status OK (just no data)
   g_m4board.response[4] = 0;  // actual_lo = 0
   g_m4board.response[5] = 0;  // actual_hi = 0
   g_m4board.response_len = 6;
}

static void cmd_nethostip()
{
   // Protocol: [size, cmd_lo, cmd_hi, hostname\0]
   // Response: [1=lookup in progress] (then poll via C_NETRECV for result)
   // We return "lookup in progress" but the result will never arrive — matching
   // the behavior of a real M4 with no WiFi connection.
   g_m4board.response[0] = M4_OK;
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;
   g_m4board.response[3] = 1; // lookup in progress
   g_m4board.response_len = 4;
}

// ── ROM Management ──────────────────────────────

static void cmd_romsupdate()
{
   // On real hardware, applies ROM configuration changes from the config buffer.
   // In emulation, ROM slots are managed by the emulator directly.
   respond_ok();
   LOG_DEBUG("M4: C_ROMSUPDATE (no-op in emulation)");
}

// ── Remaining handlers ──────────────────────────

static void cmd_fstat()
{
   // Protocol: [size, cmd_lo, cmd_hi, fd]
   // Returns file attributes byte at response[3]:
   //   0x00 = normal file, 0x10 = directory
   if (g_m4board.cmd_buf.size() < 4) {
      respond_error();
      return;
   }
   int handle = g_m4board.cmd_buf[3];
   if (handle < 0 || handle >= 4 || !g_m4board.open_files[handle]) {
      respond_error("Bad handle");
      return;
   }
   g_m4board.response[0] = M4_OK;
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;
   g_m4board.response[3] = 0;  // attributes: normal file
   g_m4board.response_len = 4;
}

static void cmd_write2()
{
   if (in_container()) { respond_error("Read-only container"); return; }
   // C_WRITE2 is the buffered write counterpart to C_READ2.
   // The M4 ROM uses this for _cas_out_char buffered writes.
   // Same protocol as C_WRITE.
   cmd_write();
}

// ── New command handlers ────────────────────────

static void cmd_config()
{
   // Protocol: [size, 0xFE, 0x43, config_offset, data...]
   // The M4 ROM init sends C_CONFIG to populate its runtime data area
   // (jump vectors, ROM slot number, AMSDOS version, etc.) at ROM offset 0x3400+.
   if (g_m4board.cmd_buf.size() < 4) {
      respond_error();
      return;
   }
   uint8_t config_offset = g_m4board.cmd_buf[3];
   size_t data_len = g_m4board.cmd_buf.size() - 4;

   // Store in local config buffer
   size_t max_len = std::min(data_len,
      static_cast<size_t>(M4Board::CONFIG_SIZE - config_offset));
   if (max_len > 0) {
      memcpy(g_m4board.config_buf + config_offset, &g_m4board.cmd_buf[4], max_len);
   }

   // Write to ROM data area at offset 0x3400 + config_offset
   // This populates jump_vec, rom_num, amsdos_ver, etc. that set_SDdrive reads
   extern byte *memmap_ROM[];
   byte* rom = memmap_ROM[g_m4board.rom_slot];
   if (rom && config_offset + max_len <= 0xC00) {  // stay within 16K ROM bounds
      memcpy(rom + 0x3400 + config_offset, &g_m4board.cmd_buf[4], max_len);
   }

   respond_ok();
   LOG_DEBUG("M4: C_CONFIG offset=" << (int)config_offset << " len=" << max_len
            << " slot=" << g_m4board.rom_slot);
}

static void cmd_romwrite()
{
   // C_ROMWRITE stores keyboard layout data — not needed for emulation
   respond_ok();
}

static void cmd_eof()
{
   // Protocol: [size, cmd_lo, cmd_hi, fd]
   if (g_m4board.cmd_buf.size() < 4) {
      respond_error();
      return;
   }
   int handle = g_m4board.cmd_buf[3];
   if (handle < 0 || handle >= 4 || !g_m4board.open_files[handle]) {
      respond_error("Bad handle");
      return;
   }
   g_m4board.response[0] = M4_OK;
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;
   g_m4board.response[3] = feof(g_m4board.open_files[handle]) ? 1 : 0;
   g_m4board.response_len = 4;
}

static void cmd_ftell()
{
   // Protocol: [size, cmd_lo, cmd_hi, fd]
   if (g_m4board.cmd_buf.size() < 4) {
      respond_error();
      return;
   }
   int handle = g_m4board.cmd_buf[3];
   if (handle < 0 || handle >= 4 || !g_m4board.open_files[handle]) {
      respond_error("Bad handle");
      return;
   }
   long pos = ftell(g_m4board.open_files[handle]);
   if (pos < 0) {
      respond_error("ftell failed");
      return;
   }
   g_m4board.response[0] = M4_OK;
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;
   g_m4board.response[3] = static_cast<uint8_t>(pos & 0xFF);
   g_m4board.response[4] = static_cast<uint8_t>((pos >> 8) & 0xFF);
   g_m4board.response[5] = static_cast<uint8_t>((pos >> 16) & 0xFF);
   g_m4board.response[6] = static_cast<uint8_t>((pos >> 24) & 0xFF);
   g_m4board.response_len = 7;
}

static void cmd_getpath()
{
   // Return current working directory within virtual SD
   g_m4board.response[0] = M4_OK;
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;
   const std::string& dir = g_m4board.current_dir;
   size_t max_len = static_cast<size_t>(M4Board::RESPONSE_SIZE - 4);
   size_t len = std::min(dir.size(), max_len);
   memcpy(g_m4board.response + 3, dir.c_str(), len);
   g_m4board.response[3 + len] = 0;
   g_m4board.response_len = static_cast<int>(4 + len);
}

static void cmd_dirsetargs()
{
   // C_DIRSETARGS is sent before C_READDIR to start a new listing.
   // Populate the directory cache now so C_READDIR can iterate one-at-a-time.
   dir_populate();
   respond_ok();
}

static void cmd_free()
{
   // Return free disk space as a string at rom_response[3+]
   g_m4board.response[0] = M4_OK;
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;

   std::string resolved = resolve_path("/");
   uint64_t free_kb = 0;
   if (!resolved.empty()) {
      try {
         auto si = std::filesystem::space(resolved);
         free_kb = si.available / 1024;
      } catch (const std::filesystem::filesystem_error&) {}
   }

   char buf[32];
   if (free_kb >= 1048576)
      snprintf(buf, sizeof(buf), "%lluG free", (unsigned long long)(free_kb / 1048576));
   else if (free_kb >= 1024)
      snprintf(buf, sizeof(buf), "%lluM free", (unsigned long long)(free_kb / 1024));
   else
      snprintf(buf, sizeof(buf), "%lluK free", (unsigned long long)free_kb);

   size_t len = strlen(buf);
   memcpy(g_m4board.response + 3, buf, len + 1);
   g_m4board.response_len = static_cast<int>(4 + len);
}

// ── Public API ──────────────────────────────────

void m4board_reset()
{
   if (in_container()) container_exit();
   g_m4board.cmd_buf.clear();
   g_m4board.cmd_pending = false;
   g_m4board.current_dir = "/";
   memset(g_m4board.response, 0, M4Board::RESPONSE_SIZE);
   g_m4board.response_len = 0;
   memset(g_m4board.config_buf, 0, M4Board::CONFIG_SIZE);
   g_m4board.dir_entries.clear();
   g_m4board.dir_index = 0;
   g_m4board.activity_frames = 0;
   g_m4board.last_op = M4Board::LastOp::NONE;
   g_m4board.last_filename.clear();
   g_m4board.cmd_count = 0;
}

void m4board_cleanup()
{
   if (in_container()) container_exit();
   for (int i = 0; i < 4; i++) {
      if (g_m4board.open_files[i]) {
         fclose(g_m4board.open_files[i]);
         g_m4board.open_files[i] = nullptr;
      }
   }
}

void m4board_data_out(byte val)
{
   g_m4board.cmd_buf.push_back(val);
}

void m4board_execute()
{
   // Protocol: [size_prefix, cmd_lo, cmd_hi, data...]
   // The M4 ROM sends a size byte at position 0, then the 16-bit command at 1-2.
   // Direct I/O sends 0x00 as the size prefix.
   if (g_m4board.cmd_buf.size() < 3) {
      g_m4board.cmd_buf.clear();
      return;
   }

   uint16_t cmd = g_m4board.cmd_buf[1] | (static_cast<uint16_t>(g_m4board.cmd_buf[2]) << 8);

   LOG_DEBUG("M4: exec cmd=0x" << std::hex << cmd << std::dec);

   memset(g_m4board.response, 0, M4Board::RESPONSE_SIZE);
   g_m4board.response_len = 0;

   // Activity tracking
   g_m4board.cmd_count++;
   g_m4board.activity_frames = 30;  // ~0.6s at 50fps
   g_m4board.last_op = M4Board::LastOp::CMD;

   switch (cmd) {
      case C_VERSION:    cmd_version(); break;
      case C_CD:         cmd_cd(); break;
      case C_READDIR:    cmd_readdir(); g_m4board.last_op = M4Board::LastOp::DIR; break;
      case C_OPEN:       cmd_open(); break;
      case C_CLOSE:      cmd_close(); break;
      case C_READ:       cmd_read(); g_m4board.last_op = M4Board::LastOp::READ; break;
      case C_READ2:      cmd_read2(); g_m4board.last_op = M4Board::LastOp::READ; break;
      case C_READSECTOR:  cmd_readsector(); g_m4board.last_op = M4Board::LastOp::READ; break;
      case C_WRITESECTOR: cmd_writesector(); g_m4board.last_op = M4Board::LastOp::WRITE; break;
      case C_FORMATTRACK: cmd_formattrack(); break;
      case C_WRITE:      cmd_write(); g_m4board.last_op = M4Board::LastOp::WRITE; break;
      case C_WRITE2:     cmd_write2(); g_m4board.last_op = M4Board::LastOp::WRITE; break;
      case C_SEEK:       cmd_seek(); break;
      case C_EOF:        cmd_eof(); break;
      case C_FREE:       cmd_free(); break;
      case C_FSIZE:      cmd_fsize(); break;
      case C_FSTAT:      cmd_fstat(); break;
      case C_FTELL:      cmd_ftell(); break;
      case C_ERASEFILE:  cmd_erasefile(); break;
      case C_RENAME:     cmd_rename(); break;
      case C_MAKEDIR:    cmd_makedir(); break;
      case C_GETPATH:    cmd_getpath(); break;
      case C_HTTPGET:     cmd_httpget(); break;
      case C_HTTPGETMEM:  cmd_httpgetmem(); break;
      case C_DIRSETARGS:  cmd_dirsetargs(); g_m4board.last_op = M4Board::LastOp::DIR; break;
      case C_SDREAD:      cmd_sdread(); g_m4board.last_op = M4Board::LastOp::READ; break;
      case C_SDWRITE:     cmd_sdwrite(); g_m4board.last_op = M4Board::LastOp::WRITE; break;
      case C_SETNETWORK:  cmd_setnetwork(); break;
      case C_M4OFF:       cmd_m4off(); break;
      case C_NETSTAT:     cmd_netstat(); break;
      case C_TIME:        cmd_time(); break;
      case C_NETSOCKET:   cmd_netsocket(); break;
      case C_NETCONNECT:  cmd_netconnect(); break;
      case C_NETCLOSE:    cmd_netclose(); break;
      case C_NETSEND:     cmd_netsend(); break;
      case C_NETRECV:     cmd_netrecv(); break;
      case C_NETHOSTIP:   cmd_nethostip(); break;
      case C_ROMSUPDATE:  cmd_romsupdate(); break;
      case C_CONFIG:      cmd_config(); break;
      case C_ROMWRITE:    cmd_romwrite(); break;
      default:
         LOG_DEBUG("M4: unknown command 0x" << std::hex << cmd);
         respond_error();
         break;
   }

   g_m4board.cmd_buf.clear();
}

void m4board_write_response(byte* rom_base)
{
   if (!rom_base || g_m4board.response_len == 0) return;
   // Write response at offset &2800 within the ROM (maps to &E800 in CPC address space)
   // The M4 ROM link table at &FF02 points to &E800 = ROM base + &2800
   int offset = 0x2800;
   int len = std::min(g_m4board.response_len, M4Board::RESPONSE_SIZE);
   memcpy(rom_base + offset, g_m4board.response, len);
}

void m4board_load_rom(byte** rom_map, const std::string& rom_path, const std::string& resources_path)
{
   if (!g_m4board.enabled) return;

   int slot = g_m4board.rom_slot;
   if (slot < 0 || slot >= 256) return;

   // Override any existing ROM in this slot (like real hardware on the expansion bus)
   if (rom_map[slot] != nullptr) {
      LOG_INFO("M4: overriding ROM in slot " << slot << " (expansion bus priority)");
      delete[] rom_map[slot];
      rom_map[slot] = nullptr;
   }

   // Search for the M4 ROM in standard locations
   static const char* rom_names[] = { "m4board.rom", "M4ROM.BIN", nullptr };
   std::string found_path;

   for (int i = 0; rom_names[i]; i++) {
      // Check rom_path (where other CPC ROMs live)
      std::string candidate = rom_path + "/" + rom_names[i];
      if (std::filesystem::exists(candidate)) {
         found_path = candidate;
         break;
      }
      // Check resources/roms/ (absolute path from app directory)
      candidate = resources_path + "/roms/" + rom_names[i];
      if (std::filesystem::exists(candidate)) {
         found_path = candidate;
         break;
      }
   }

   if (found_path.empty()) {
      LOG_ERROR("M4: ROM file not found (searched for m4board.rom / M4ROM.BIN in " << rom_path << ")");
      return;
   }

   FILE* fp = fopen(found_path.c_str(), "rb");
   if (!fp) {
      LOG_ERROR("M4: cannot open ROM file: " << found_path);
      return;
   }

   byte* rom_data = new byte[16384];
   memset(rom_data, 0xFF, 16384);
   size_t read = fread(rom_data, 1, 16384, fp);
   fclose(fp);

   if (read < 128 || !(rom_data[0] <= 0x02)) {
      LOG_ERROR("M4: invalid ROM file: " << found_path);
      delete[] rom_data;
      return;
   }

   // On real hardware, the M4 firmware (ESP8266) pre-fills runtime data in the ROM
   // image before the CPC boots. The ROM's init sends C_CONFIG commands to populate
   // the data area at offset 0x3400+ (jump vectors, ROM slot number, AMSDOS version).
   // Then set_SDdrive (offset 0x798) patches the CAS firmware jumpblock (&BC77+)
   // so CAT/LOAD/SAVE go through the M4. This works now because cmd_config() fills
   // the ROM data area — no need to disable set_SDdrive.

   // Patch the init return (offset 0x268: AND A / SCF / RET) to jump to a boot
   // message routine. We use offset 0x3800 (CPC &F800) — NOT the response area
   // at 0x2800 (&E800), because the ROM's own init code writes 0xFF there.
   rom_data[0x0268] = 0xC3;  // JP &F800
   rom_data[0x0269] = 0x00;
   rom_data[0x026A] = 0xF8;

   // Boot message: two-stage approach because code in upper ROM space (&C000+)
   // becomes inaccessible when TXT OUTPUT pages out the ROM to write screen memory.
   // Stage 1 (ROM &F800): saves DE, copies stage 2 + string to RAM &8000, jumps there.
   // Stage 2 (RAM &8000): prints string via CALL &BB5A, restores DE, SCF/RET.
   // DE must be preserved — the firmware uses it to track free memory after ROM init.
   static const uint8_t stage1[] = {
      0xD5,                  // PUSH DE (save firmware memory pointer)
      0x21, 0x0F, 0xF8,     // LD HL, &F80F  (stage2 source in ROM)
      0x11, 0x00, 0x80,     // LD DE, &8000   (dest in RAM)
      0x01, 0x27, 0x00,     // LD BC, 39      (stage2 + string size)
      0xED, 0xB0,           // LDIR
      0xC3, 0x00, 0x80,     // JP &8000
   };
   // Stage 2: print boot message from RAM, restore DE, signal success.
   static const uint8_t stage2[] = {
      0x21, 0x12, 0x80,     // LD HL, &8012  (string in RAM at &8000+18)
      0x7E,                  // loop: LD A, (HL)
      0xB7,                  // OR A
      0x28, 0x08,            // JR Z, +8 → done
      0xE5,                  // PUSH HL
      0xCD, 0x5A, 0xBB,     // CALL &BB5A (TXT OUTPUT)
      0xE1,                  // POP HL
      0x23,                  // INC HL
      0x18, 0xF4,            // JR loop
      0xD1,                  // done: POP DE (restore firmware memory pointer)
      0x37,                  // SCF (ROM init success)
      0xC9,                  // RET
   };
   static const char boot_msg[] = "\r\nEmulated M4 v2.0\r\n";

   memcpy(rom_data + 0x3800, stage1, sizeof(stage1));                      // 15 bytes
   memcpy(rom_data + 0x380F, stage2, sizeof(stage2));                      // 18 bytes
   memcpy(rom_data + 0x380F + sizeof(stage2), boot_msg, sizeof(boot_msg)); // 21 bytes

   rom_map[slot] = rom_data;
   g_m4board.rom_auto_loaded = true;
   LOG_INFO("M4: auto-loaded ROM from " << found_path << " into slot " << slot);
}

void m4board_unload_rom(byte** rom_map)
{
   if (!g_m4board.rom_auto_loaded) return;

   int slot = g_m4board.rom_slot;
   if (slot >= 0 && slot < 256 && rom_map[slot] != nullptr) {
      delete[] rom_map[slot];
      rom_map[slot] = nullptr;
      LOG_INFO("M4: unloaded ROM from slot " << slot);
   }
   g_m4board.rom_auto_loaded = false;
}

// ── I/O dispatch registration ──────────────────

#include "io_dispatch.h"

static bool m4board_out_handler_fe(reg_pair port, byte val)
{
   if (port.b.l != 0x00) return false;
   m4board_data_out(val);
   return true;
}

static bool m4board_out_handler_fc(reg_pair /*port*/, byte /*val*/)
{
   m4board_execute();
   // Write response into M4 ROM overlay
   extern byte *memmap_ROM[];
   if (memmap_ROM[g_m4board.rom_slot]) {
      m4board_write_response(memmap_ROM[g_m4board.rom_slot]);
   }
   return true;
}

void m4board_register_io()
{
   io_register_out(0xFE, m4board_out_handler_fe, &g_m4board.enabled, "M4 Board Data");
   io_register_out(0xFC, m4board_out_handler_fc, &g_m4board.enabled, "M4 Board Kick");
}
