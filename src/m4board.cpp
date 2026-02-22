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
   } catch (...) {
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
   } catch (...) {
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
   } catch (...) {
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
   } catch (...) {
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
   } catch (...) {
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
   } catch (...) {
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
