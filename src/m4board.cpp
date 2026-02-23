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
static constexpr uint16_t C_HTTPGET    = 0x4320;
static constexpr uint16_t C_VERSION    = 0x4326;

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

// ── Command Handlers ────────────────────────────

static void cmd_version()
{
   const char* ver = "M4 konCePCja v1.0";
   g_m4board.response[0] = M4_OK;
   size_t len = strlen(ver);
   memcpy(g_m4board.response + 1, ver, len);
   g_m4board.response[1 + len] = 0;
   g_m4board.response_len = static_cast<int>(2 + len);
}

static void cmd_cd()
{
   std::string path = extract_string(g_m4board.cmd_buf, 2);
   if (path == "/") {
      g_m4board.current_dir = "/";
      g_m4board.response[0] = M4_OK;
      g_m4board.response_len = 1;
      return;
   }

   std::string resolved = resolve_path(path);
   if (resolved.empty()) {
      g_m4board.response[0] = M4_ERROR;
      g_m4board.response_len = 1;
      return;
   }

   try {
      if (std::filesystem::is_directory(resolved)) {
         // Convert back to relative path within SD root
         auto root_canonical = std::filesystem::weakly_canonical(g_m4board.sd_root_path);
         std::string rel = resolved.substr(root_canonical.string().size());
         if (rel.empty()) rel = "/";
         if (rel.back() != '/') rel += '/';
         g_m4board.current_dir = rel;
         g_m4board.response[0] = M4_OK;
         g_m4board.response_len = 1;
      } else {
         g_m4board.response[0] = M4_ERROR;
         g_m4board.response_len = 1;
      }
   } catch (const std::filesystem::filesystem_error& e) {
      LOG_ERROR("M4: " << e.what());
      g_m4board.response[0] = M4_ERROR;
      g_m4board.response_len = 1;
   }
}

static void cmd_readdir()
{
   std::string resolved = resolve_path(g_m4board.current_dir);
   if (resolved.empty()) {
      g_m4board.response[0] = M4_ERROR;
      g_m4board.response_len = 1;
      return;
   }

   g_m4board.response[0] = M4_OK;
   int pos = 1;

   try {
      for (auto& entry : std::filesystem::directory_iterator(resolved)) {
         std::string name = entry.path().filename().string();
         bool is_dir = entry.is_directory();
         size_t fsize = is_dir ? 0 : entry.file_size();

         // Format: type(1) + size(4) + name + null
         if (pos + 5 + static_cast<int>(name.size()) + 1 >= M4Board::RESPONSE_SIZE) break;

         g_m4board.response[pos++] = is_dir ? 0x10 : 0x00;
         g_m4board.response[pos++] = fsize & 0xFF;
         g_m4board.response[pos++] = (fsize >> 8) & 0xFF;
         g_m4board.response[pos++] = (fsize >> 16) & 0xFF;
         g_m4board.response[pos++] = (fsize >> 24) & 0xFF;
         memcpy(g_m4board.response + pos, name.c_str(), name.size() + 1);
         pos += static_cast<int>(name.size()) + 1;
      }
   } catch (const std::filesystem::filesystem_error& e) {
      LOG_ERROR("M4: " << e.what());
      g_m4board.response[0] = M4_ERROR;
      g_m4board.response_len = 1;
      return;
   }

   // End marker
   if (pos < M4Board::RESPONSE_SIZE) {
      g_m4board.response[pos++] = 0xFF;
   }
   g_m4board.response_len = pos;
}

static void cmd_open()
{
   std::string path = extract_string(g_m4board.cmd_buf, 2);
   std::string resolved = resolve_path(path);
   if (resolved.empty()) {
      g_m4board.response[0] = M4_ERROR;
      g_m4board.response_len = 1;
      return;
   }

   // Find free handle
   int handle = -1;
   for (int i = 0; i < 4; i++) {
      if (!g_m4board.open_files[i]) { handle = i; break; }
   }
   if (handle < 0) {
      g_m4board.response[0] = M4_ERROR;
      g_m4board.response_len = 1;
      return;
   }

   g_m4board.open_files[handle] = fopen(resolved.c_str(), "r+b");
   if (!g_m4board.open_files[handle]) {
      g_m4board.open_files[handle] = fopen(resolved.c_str(), "rb");
   }
   if (!g_m4board.open_files[handle]) {
      g_m4board.response[0] = M4_ERROR;
      g_m4board.response_len = 1;
      return;
   }

   g_m4board.response[0] = M4_OK;
   g_m4board.response[1] = static_cast<uint8_t>(handle);
   g_m4board.response_len = 2;
}

static void cmd_close()
{
   if (g_m4board.cmd_buf.size() < 3) {
      g_m4board.response[0] = M4_ERROR;
      g_m4board.response_len = 1;
      return;
   }
   int handle = g_m4board.cmd_buf[2];
   if (handle >= 0 && handle < 4 && g_m4board.open_files[handle]) {
      fclose(g_m4board.open_files[handle]);
      g_m4board.open_files[handle] = nullptr;
   }
   g_m4board.response[0] = M4_OK;
   g_m4board.response_len = 1;
}

static void cmd_read()
{
   if (g_m4board.cmd_buf.size() < 5) {
      g_m4board.response[0] = M4_ERROR;
      g_m4board.response_len = 1;
      return;
   }
   int handle = g_m4board.cmd_buf[2];
   uint16_t count = g_m4board.cmd_buf[3] | (g_m4board.cmd_buf[4] << 8);

   if (handle < 0 || handle >= 4 || !g_m4board.open_files[handle]) {
      g_m4board.response[0] = M4_ERROR;
      g_m4board.response_len = 1;
      return;
   }

   int max_read = M4Board::RESPONSE_SIZE - 3;
   if (count > max_read) count = static_cast<uint16_t>(max_read);

   size_t read = fread(g_m4board.response + 3, 1, count, g_m4board.open_files[handle]);
   g_m4board.response[0] = M4_OK;
   g_m4board.response[1] = read & 0xFF;
   g_m4board.response[2] = (read >> 8) & 0xFF;
   g_m4board.response_len = static_cast<int>(3 + read);
}

static void cmd_fsize()
{
   std::string path = extract_string(g_m4board.cmd_buf, 2);
   std::string resolved = resolve_path(path);
   if (resolved.empty()) {
      g_m4board.response[0] = M4_ERROR;
      g_m4board.response_len = 1;
      return;
   }

   try {
      auto fsize = std::filesystem::file_size(resolved);
      g_m4board.response[0] = M4_OK;
      g_m4board.response[1] = fsize & 0xFF;
      g_m4board.response[2] = (fsize >> 8) & 0xFF;
      g_m4board.response[3] = (fsize >> 16) & 0xFF;
      g_m4board.response[4] = (fsize >> 24) & 0xFF;
      g_m4board.response_len = 5;
   } catch (const std::filesystem::filesystem_error& e) {
      LOG_ERROR("M4: " << e.what());
      g_m4board.response[0] = M4_ERROR;
      g_m4board.response_len = 1;
   }
}

static void cmd_erasefile()
{
   std::string path = extract_string(g_m4board.cmd_buf, 2);
   std::string resolved = resolve_path(path);
   if (resolved.empty()) {
      g_m4board.response[0] = M4_ERROR;
      g_m4board.response_len = 1;
      return;
   }

   try {
      if (std::filesystem::remove(resolved)) {
         g_m4board.response[0] = M4_OK;
      } else {
         g_m4board.response[0] = M4_ERROR;
      }
   } catch (const std::filesystem::filesystem_error& e) {
      LOG_ERROR("M4: " << e.what());
      g_m4board.response[0] = M4_ERROR;
   }
   g_m4board.response_len = 1;
}

static void cmd_makedir()
{
   std::string path = extract_string(g_m4board.cmd_buf, 2);
   std::string resolved = resolve_path(path);
   if (resolved.empty()) {
      g_m4board.response[0] = M4_ERROR;
      g_m4board.response_len = 1;
      return;
   }

   try {
      std::filesystem::create_directories(resolved);
      g_m4board.response[0] = M4_OK;
   } catch (const std::filesystem::filesystem_error& e) {
      LOG_ERROR("M4: " << e.what());
      g_m4board.response[0] = M4_ERROR;
   }
   g_m4board.response_len = 1;
}

static void cmd_write()
{
   // Protocol: data[0] = fd, data[1..] = file data
   // cmd_buf[2] = fd, cmd_buf[3..] = data to write
   if (g_m4board.cmd_buf.size() < 4) {
      g_m4board.response[0] = M4_ERROR;
      g_m4board.response_len = 1;
      return;
   }
   int handle = g_m4board.cmd_buf[2];
   if (handle < 0 || handle >= 4 || !g_m4board.open_files[handle]) {
      g_m4board.response[0] = M4_ERROR;
      g_m4board.response_len = 1;
      return;
   }

   size_t data_len = g_m4board.cmd_buf.size() - 3;
   size_t written = fwrite(g_m4board.cmd_buf.data() + 3, 1, data_len,
                           g_m4board.open_files[handle]);
   fflush(g_m4board.open_files[handle]);

   if (written == data_len) {
      g_m4board.response[0] = M4_OK;
   } else {
      g_m4board.response[0] = M4_ERROR;
   }
   g_m4board.response_len = 1;
}

static void cmd_seek()
{
   // Protocol: data[0] = fd, data[1..4] = 32-bit LE offset
   // cmd_buf[2] = fd, cmd_buf[3..6] = offset
   if (g_m4board.cmd_buf.size() < 7) {
      g_m4board.response[0] = M4_ERROR;
      g_m4board.response_len = 1;
      return;
   }
   int handle = g_m4board.cmd_buf[2];
   if (handle < 0 || handle >= 4 || !g_m4board.open_files[handle]) {
      g_m4board.response[0] = M4_ERROR;
      g_m4board.response_len = 1;
      return;
   }

   uint32_t offset = static_cast<uint32_t>(g_m4board.cmd_buf[3]) |
                     (static_cast<uint32_t>(g_m4board.cmd_buf[4]) << 8) |
                     (static_cast<uint32_t>(g_m4board.cmd_buf[5]) << 16) |
                     (static_cast<uint32_t>(g_m4board.cmd_buf[6]) << 24);

   if (fseek(g_m4board.open_files[handle], static_cast<long>(offset), SEEK_SET) == 0) {
      g_m4board.response[0] = M4_OK;
   } else {
      g_m4board.response[0] = M4_ERROR;
   }
   g_m4board.response_len = 1;
}

static void cmd_rename()
{
   // Protocol: data = "newname\0oldname\0"
   // cmd_buf[2..] = "newname\0oldname\0"
   std::string newname = extract_string(g_m4board.cmd_buf, 2);
   // Find the second string after the first null terminator
   size_t old_offset = 2 + newname.size() + 1;
   if (old_offset >= g_m4board.cmd_buf.size()) {
      g_m4board.response[0] = M4_ERROR;
      g_m4board.response_len = 1;
      return;
   }
   std::string oldname = extract_string(g_m4board.cmd_buf, old_offset);

   std::string resolved_old = resolve_path(oldname);
   std::string resolved_new = resolve_path(newname);
   if (resolved_old.empty() || resolved_new.empty()) {
      g_m4board.response[0] = M4_ERROR;
      g_m4board.response_len = 1;
      return;
   }

   try {
      std::filesystem::rename(resolved_old, resolved_new);
      g_m4board.response[0] = M4_OK;
      g_m4board.response_len = 1;
   } catch (const std::exception& e) {
      g_m4board.response[0] = M4_ERROR;
      std::string msg = e.what();
      size_t max_msg = static_cast<size_t>(M4Board::RESPONSE_SIZE - 2);
      if (msg.size() > max_msg) msg.resize(max_msg);
      memcpy(g_m4board.response + 1, msg.c_str(), msg.size() + 1);
      g_m4board.response_len = static_cast<int>(2 + msg.size());
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
   // Protocol: data[0] = "url:port/file"
   // URL format: [@ prefix]host[:port]/path[>outfile]
   std::string raw_url = extract_string(g_m4board.cmd_buf, 2);
   if (raw_url.empty()) {
      g_m4board.response[0] = M4_ERROR;
      const char* msg = "No URL given";
      memcpy(g_m4board.response + 1, msg, strlen(msg) + 1);
      g_m4board.response_len = static_cast<int>(2 + strlen(msg));
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
      g_m4board.response[0] = M4_ERROR;
      const char* msg = "No filename in URL";
      memcpy(g_m4board.response + 1, msg, strlen(msg) + 1);
      g_m4board.response_len = static_cast<int>(2 + strlen(msg));
      return;
   }

   // Build the full HTTP URL
   std::string full_url = "http://" + url;

   // Resolve output path within the virtual SD
   std::string dest = resolve_path(out_filename);
   if (dest.empty()) {
      g_m4board.response[0] = M4_ERROR;
      const char* msg = "Invalid output path";
      memcpy(g_m4board.response + 1, msg, strlen(msg) + 1);
      g_m4board.response_len = static_cast<int>(2 + strlen(msg));
      return;
   }

   // Perform the HTTP GET download
   FILE* fp = fopen(dest.c_str(), "wb");
   if (!fp) {
      g_m4board.response[0] = M4_ERROR;
      const char* msg = "Cannot create file";
      memcpy(g_m4board.response + 1, msg, strlen(msg) + 1);
      g_m4board.response_len = static_cast<int>(2 + strlen(msg));
      return;
   }

   CURL* curl = curl_easy_init();
   if (!curl) {
      fclose(fp);
      std::filesystem::remove(dest);
      g_m4board.response[0] = M4_ERROR;
      const char* msg = "HTTP init failed";
      memcpy(g_m4board.response + 1, msg, strlen(msg) + 1);
      g_m4board.response_len = static_cast<int>(2 + strlen(msg));
      return;
   }

   curl_easy_setopt(curl, CURLOPT_URL, full_url.c_str());
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_file);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
   curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
   curl_easy_setopt(curl, CURLOPT_USERAGENT, "M4Board/2.0");
   // Check for Content-Disposition filename (the real M4 uses attachment filenames)
   curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

   CURLcode res = curl_easy_perform(curl);
   long http_code = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
   curl_easy_cleanup(curl);
   fclose(fp);

   if (res != CURLE_OK) {
      std::filesystem::remove(dest);
      g_m4board.response[0] = M4_ERROR;
      std::string msg = curl_easy_strerror(res);
      size_t max_msg = static_cast<size_t>(M4Board::RESPONSE_SIZE - 2);
      if (msg.size() > max_msg) msg.resize(max_msg);
      memcpy(g_m4board.response + 1, msg.c_str(), msg.size() + 1);
      g_m4board.response_len = static_cast<int>(2 + msg.size());
      LOG_ERROR("M4 HTTPGET: " << msg << " (URL: " << full_url << ")");
      return;
   }

   // Success response: "Downloaded <filename>\r\n"
   std::string ok_msg = "Downloaded " + out_filename + "\r\n";
   size_t max_ok = static_cast<size_t>(M4Board::RESPONSE_SIZE - 1);
   if (ok_msg.size() > max_ok) ok_msg.resize(max_ok);
   memcpy(g_m4board.response, ok_msg.c_str(), ok_msg.size() + 1);
   g_m4board.response_len = static_cast<int>(ok_msg.size() + 1);
   LOG_INFO("M4 HTTPGET: downloaded " << full_url << " -> " << dest);
}

#else // !HAS_LIBCURL

static void cmd_httpget()
{
   g_m4board.response[0] = M4_ERROR;
   const char* msg = "HTTP not available (no libcurl)";
   memcpy(g_m4board.response + 1, msg, strlen(msg) + 1);
   g_m4board.response_len = static_cast<int>(2 + strlen(msg));
   LOG_ERROR("M4 HTTPGET: libcurl not available at build time");
}

#endif // HAS_LIBCURL

// ── Public API ──────────────────────────────────

void m4board_reset()
{
   g_m4board.cmd_buf.clear();
   g_m4board.cmd_pending = false;
   g_m4board.current_dir = "/";
   memset(g_m4board.response, 0, M4Board::RESPONSE_SIZE);
   g_m4board.response_len = 0;
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
   if (g_m4board.cmd_buf.size() < 2) {
      g_m4board.cmd_buf.clear();
      return;
   }

   uint16_t cmd = g_m4board.cmd_buf[0] | (static_cast<uint16_t>(g_m4board.cmd_buf[1]) << 8);

   memset(g_m4board.response, 0, M4Board::RESPONSE_SIZE);
   g_m4board.response_len = 0;

   switch (cmd) {
      case C_VERSION:   cmd_version(); break;
      case C_CD:        cmd_cd(); break;
      case C_READDIR:   cmd_readdir(); break;
      case C_OPEN:      cmd_open(); break;
      case C_CLOSE:     cmd_close(); break;
      case C_READ:      cmd_read(); break;
      case C_WRITE:     cmd_write(); break;
      case C_SEEK:      cmd_seek(); break;
      case C_FSIZE:     cmd_fsize(); break;
      case C_ERASEFILE: cmd_erasefile(); break;
      case C_RENAME:    cmd_rename(); break;
      case C_MAKEDIR:   cmd_makedir(); break;
      case C_HTTPGET:   cmd_httpget(); break;
      default:
         LOG_DEBUG("M4: unknown command 0x" << std::hex << cmd);
         g_m4board.response[0] = M4_ERROR;
         g_m4board.response_len = 1;
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
   // image before the CPC boots. The ROM's init then patches the CPC firmware jumpblock
   // (&BC77+) to redirect certain firmware calls through M4 event handlers. Without
   // real M4 hardware writing those handlers, the patched vectors crash the CPC.
   // Fix: NOP out the firmware patcher at ROM offset 0x798 (CPC &C798) and its
   // 464-variant at 0x7EA. RSX commands still work (registered via KL LOG EXT).
   rom_data[0x0798] = 0xC9;  // RET — skip firmware jumpblock patching (664/6128)
   rom_data[0x07EA] = 0xC9;  // RET — skip firmware jumpblock patching (464 variant)

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
