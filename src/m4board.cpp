#include "m4board.h"
#include "log.h"
#include <cstring>
#include <filesystem>
#include <algorithm>

M4Board g_m4board;

// M4 command codes (from m4cmds.i)
static constexpr uint16_t C_OPEN       = 0x4301;
static constexpr uint16_t C_READ       = 0x4302;
[[maybe_unused]] static constexpr uint16_t C_WRITE      = 0x4303;
static constexpr uint16_t C_CLOSE      = 0x4304;
[[maybe_unused]] static constexpr uint16_t C_SEEK       = 0x4305;
static constexpr uint16_t C_READDIR    = 0x4306;
static constexpr uint16_t C_CD         = 0x4308;
static constexpr uint16_t C_ERASEFILE  = 0x430E;
[[maybe_unused]] static constexpr uint16_t C_RENAME     = 0x430F;
static constexpr uint16_t C_MAKEDIR    = 0x4310;
static constexpr uint16_t C_FSIZE      = 0x4311;
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
      case C_VERSION:  cmd_version(); break;
      case C_CD:       cmd_cd(); break;
      case C_READDIR:  cmd_readdir(); break;
      case C_OPEN:     cmd_open(); break;
      case C_CLOSE:    cmd_close(); break;
      case C_READ:     cmd_read(); break;
      case C_FSIZE:    cmd_fsize(); break;
      case C_ERASEFILE: cmd_erasefile(); break;
      case C_MAKEDIR:  cmd_makedir(); break;
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
   // Write response at offset &0800 within the ROM (maps to &E800 in CPC address space)
   int offset = 0x0800;
   int len = std::min(g_m4board.response_len, M4Board::RESPONSE_SIZE);
   memcpy(rom_base + offset, g_m4board.response, len);
}
