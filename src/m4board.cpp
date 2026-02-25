#include "m4board.h"
#include "log.h"
#include <cstring>
#include <filesystem>
#include <algorithm>
#ifdef HAS_LIBCURL
#include <curl/curl.h>
#endif

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
static constexpr uint16_t C_ROMWRITE   = 0x43FD;
static constexpr uint16_t C_CONFIG     = 0x43FE;

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

   // Resolve and verify it's within sd_root_path
   try {
      auto canonical = std::filesystem::weakly_canonical(full);
      auto root_canonical = std::filesystem::weakly_canonical(base);
      std::string cs = canonical.string();
      std::string rs = root_canonical.string();
      if (cs.substr(0, rs.size()) != rs) {
         LOG_ERROR("M4: path traversal blocked: " << full);
         return "";
      }
      return cs;
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
   if (path == "/") {
      g_m4board.current_dir = "/";
      respond_ok();
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
         std::string rel = resolved.substr(root_canonical.string().size());
         if (rel.empty()) rel = "/";
         if (rel.back() != '/') rel += '/';
         g_m4board.current_dir = rel;
         respond_ok();
      } else {
         respond_error("Not a directory");
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

      // ASCII size field (5 chars)
      char sizebuf[6];
      if (entry.is_dir) {
         memcpy(sizebuf, "<DIR>", 5);
      } else {
         uint32_t kb = (entry.size + 1023) / 1024;
         snprintf(sizebuf, sizeof(sizebuf), "%5u", kb > 99999 ? 99999 : kb);
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

static void cmd_open()
{
   std::string path = extract_string(g_m4board.cmd_buf, 3);
   std::string resolved = resolve_path(path);
   if (resolved.empty()) {
      respond_error("Invalid path");
      return;
   }

   int handle = -1;
   for (int i = 0; i < 4; i++) {
      if (!g_m4board.open_files[i]) { handle = i; break; }
   }
   if (handle < 0) {
      respond_error("No free handles");
      return;
   }

   g_m4board.open_files[handle] = fopen(resolved.c_str(), "r+b");
   if (!g_m4board.open_files[handle]) {
      g_m4board.open_files[handle] = fopen(resolved.c_str(), "rb");
   }
   if (!g_m4board.open_files[handle]) {
      respond_error("Cannot open file");
      return;
   }

   // ROM reads: response[3] = fd, response[4] = error (0 = ok)
   g_m4board.response[0] = M4_OK;
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;
   g_m4board.response[3] = static_cast<uint8_t>(handle);
   g_m4board.response[4] = 0;  // success
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

   // ROM reads: response[3] = status, response[4-5] = bytes read, response[8+] = data
   int max_read = M4Board::RESPONSE_SIZE - 8;
   if (count > max_read) count = static_cast<uint16_t>(max_read);

   size_t nread = fread(g_m4board.response + 8, 1, count, g_m4board.open_files[handle]);
   bool at_eof = feof(g_m4board.open_files[handle]);

   g_m4board.response[0] = M4_OK;
   g_m4board.response[1] = 0;
   g_m4board.response[2] = 0;
   g_m4board.response[3] = at_eof ? 0x14 : 0;  // 0x14 = eof indicator
   g_m4board.response[4] = nread & 0xFF;
   g_m4board.response[5] = (nread >> 8) & 0xFF;
   g_m4board.response[6] = 0;
   g_m4board.response[7] = 0;
   g_m4board.response_len = static_cast<int>(8 + nread);
}

static void cmd_fsize()
{
   std::string path = extract_string(g_m4board.cmd_buf, 3);
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
   // Protocol: [size, cmd_lo, cmd_hi, fd, data...]
   // cmd_buf[3] = fd, cmd_buf[4..] = data to write
   if (g_m4board.cmd_buf.size() < 5) {
      respond_error();
      return;
   }
   int handle = g_m4board.cmd_buf[3];
   if (handle < 0 || handle >= 4 || !g_m4board.open_files[handle]) {
      respond_error("Bad handle");
      return;
   }

   size_t data_len = g_m4board.cmd_buf.size() - 4;
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
   LOG_DEBUG("M4: C_CONFIG offset=" << (int)config_offset << " len=" << max_len);
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
   g_m4board.cmd_buf.clear();
   g_m4board.cmd_pending = false;
   g_m4board.current_dir = "/";
   memset(g_m4board.response, 0, M4Board::RESPONSE_SIZE);
   g_m4board.response_len = 0;
   memset(g_m4board.config_buf, 0, M4Board::CONFIG_SIZE);
   g_m4board.dir_entries.clear();
   g_m4board.dir_index = 0;
}

void m4board_cleanup()
{
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

   memset(g_m4board.response, 0, M4Board::RESPONSE_SIZE);
   g_m4board.response_len = 0;

   switch (cmd) {
      case C_VERSION:    cmd_version(); break;
      case C_CD:         cmd_cd(); break;
      case C_READDIR:    cmd_readdir(); break;
      case C_OPEN:       cmd_open(); break;
      case C_CLOSE:      cmd_close(); break;
      case C_READ:       cmd_read(); break;
      case C_WRITE:      cmd_write(); break;
      case C_SEEK:       cmd_seek(); break;
      case C_EOF:        cmd_eof(); break;
      case C_FREE:       cmd_free(); break;
      case C_FSIZE:      cmd_fsize(); break;
      case C_FTELL:      cmd_ftell(); break;
      case C_ERASEFILE:  cmd_erasefile(); break;
      case C_RENAME:     cmd_rename(); break;
      case C_MAKEDIR:    cmd_makedir(); break;
      case C_GETPATH:    cmd_getpath(); break;
      case C_HTTPGET:    cmd_httpget(); break;
      case C_DIRSETARGS: cmd_dirsetargs(); break;
      case C_CONFIG:     cmd_config(); break;
      case C_ROMWRITE:   cmd_romwrite(); break;
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
