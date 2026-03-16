/* konCePCja - Amstrad CPC Emulator
   M4 Board — Embedded HTTP server

   Serves a web interface compatible with the real M4 Board's HTTP API.
   The real M4 uses lwIP httpd with SSI; we serve a modern single-page
   app with JSON endpoints that matches the API shape expected by cpcxfer
   and the M4 Board Android app.

   Key API endpoints (matching real M4):
   - GET /sd/m4/dir.txt     — directory listing (line-per-entry)
   - GET /config.cgi?ls=X   — change directory + redirect
   - GET /config.cgi?cd=X   — CD on CPC
   - GET /config.cgi?run2=X — remote run file
   - GET /config.cgi?rm=X   — delete file
   - GET /sd/<path>          — download file from SD
   - POST /                  — upload file (multipart/form-data)
   - POST /upload.html       — upload file (form action target)
   - GET /status             — JSON status (extension)
   - POST /reset             — reset CPC (extension)
*/

#include "m4board_http.h"
#include "m4board.h"
#include "m4board_web_assets.h"
#include "log.h"
#include "autotype.h"
#include "koncepcja.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#endif

extern t_CPC CPC;
extern void emulator_reset();
extern AutoTypeQueue g_autotype_queue;

M4HttpServer g_m4_http;

// ── Platform socket helpers ──────────────────────────────

#ifdef _WIN32
using sock_t = SOCKET;
static constexpr sock_t BAD_SOCK = INVALID_SOCKET;
static void close_sock(sock_t s) { closesocket(s); }
static int sock_send(sock_t s, const void* buf, int len) {
   return ::send(s, static_cast<const char*>(buf), len, 0);
}
static int sock_recv(sock_t s, void* buf, int len) {
   return ::recv(s, static_cast<char*>(buf), len, 0);
}
#else
using sock_t = int;
static void close_sock(sock_t s) { ::close(s); }
static int sock_send(sock_t s, const void* buf, int len) {
   return static_cast<int>(::write(s, buf, static_cast<size_t>(len)));
}
static int sock_recv(sock_t s, void* buf, int len) {
   return static_cast<int>(::read(s, buf, static_cast<size_t>(len)));
}
#endif

// ── URL decoding ─────────────────────────────────────────

std::string M4HttpServer::url_decode(const std::string& str) {
   std::string result;
   result.reserve(str.size());
   for (size_t i = 0; i < str.size(); i++) {
      if (str[i] == '%' && i + 2 < str.size()) {
         int hi = 0, lo = 0;
         char c1 = str[i+1], c2 = str[i+2];
         if (c1 >= '0' && c1 <= '9') hi = c1 - '0';
         else if (c1 >= 'a' && c1 <= 'f') hi = c1 - 'a' + 10;
         else if (c1 >= 'A' && c1 <= 'F') hi = c1 - 'A' + 10;
         else { result += str[i]; continue; }
         if (c2 >= '0' && c2 <= '9') lo = c2 - '0';
         else if (c2 >= 'a' && c2 <= 'f') lo = c2 - 'a' + 10;
         else if (c2 >= 'A' && c2 <= 'F') lo = c2 - 'A' + 10;
         else { result += str[i]; continue; }
         result += static_cast<char>((hi << 4) | lo);
         i += 2;
      } else if (str[i] == '+') {
         result += ' ';
      } else {
         result += str[i];
      }
   }
   return result;
}

std::string M4HttpServer::get_query_param(const std::string& query, const std::string& key) {
   size_t pos = 0;
   while (pos < query.size()) {
      size_t eq = query.find('=', pos);
      size_t amp = query.find('&', pos);
      if (amp == std::string::npos) amp = query.size();
      if (eq != std::string::npos && eq < amp) {
         std::string k = query.substr(pos, eq - pos);
         std::string v = query.substr(eq + 1, amp - eq - 1);
         if (k == key) return url_decode(v);
      } else {
         // key with no value (e.g., ?ls)
         std::string k = query.substr(pos, amp - pos);
         if (k == key) return "";
      }
      pos = amp + 1;
   }
   return "";
}

std::string M4HttpServer::mime_type_for(const std::string& path) {
   auto ext_pos = path.rfind('.');
   if (ext_pos == std::string::npos) return "application/octet-stream";
   std::string ext = path.substr(ext_pos);
   // Lowercase the extension
   for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

   if (ext == ".html" || ext == ".htm" || ext == ".shtml") return "text/html; charset=utf-8";
   if (ext == ".css") return "text/css; charset=utf-8";
   if (ext == ".js") return "application/javascript; charset=utf-8";
   if (ext == ".json") return "application/json; charset=utf-8";
   if (ext == ".png") return "image/png";
   if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
   if (ext == ".gif") return "image/gif";
   if (ext == ".ico") return "image/x-icon";
   if (ext == ".svg") return "image/svg+xml";
   if (ext == ".txt") return "text/plain; charset=utf-8";
   if (ext == ".bin" || ext == ".rom") return "application/octet-stream";
   if (ext == ".dsk") return "application/octet-stream";
   if (ext == ".sna") return "application/octet-stream";
   if (ext == ".cdt") return "application/octet-stream";
   if (ext == ".cpr") return "application/octet-stream";
   if (ext == ".bas") return "text/plain; charset=utf-8";
   return "application/octet-stream";
}

// ── HTTP request parsing ─────────────────────────────────

bool M4HttpServer::parse_request(const std::string& raw, HttpRequest& req) {
   // Find end of request line
   size_t line_end = raw.find("\r\n");
   if (line_end == std::string::npos) return false;

   std::string request_line = raw.substr(0, line_end);

   // Parse: METHOD /path HTTP/1.x
   size_t sp1 = request_line.find(' ');
   size_t sp2 = request_line.rfind(' ');
   if (sp1 == std::string::npos || sp1 == sp2) return false;

   req.method = request_line.substr(0, sp1);
   std::string full_path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);

   // Split path and query string
   size_t qpos = full_path.find('?');
   if (qpos != std::string::npos) {
      req.path = full_path.substr(0, qpos);
      req.query_string = full_path.substr(qpos + 1);
   } else {
      req.path = full_path;
   }

   // Parse headers
   size_t headers_start = line_end + 2;
   size_t headers_end = raw.find("\r\n\r\n", headers_start);
   if (headers_end == std::string::npos) return false;

   std::string headers_block = raw.substr(headers_start, headers_end - headers_start);
   // Parse Content-Length and Content-Type
   std::istringstream hstream(headers_block);
   std::string hline;
   while (std::getline(hstream, hline)) {
      if (!hline.empty() && hline.back() == '\r') hline.pop_back();
      // Case-insensitive header matching
      std::string lower_line = hline;
      for (auto& c : lower_line) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (lower_line.substr(0, 16) == "content-length: ") {
         try { req.content_length = std::stoi(hline.substr(16)); }
         catch (...) { req.content_length = 0; }
      }
      if (lower_line.substr(0, 14) == "content-type: ") {
         req.content_type = hline.substr(14);
         // Extract multipart boundary
         size_t bp = lower_line.find("boundary=");
         if (bp != std::string::npos) {
            req.boundary = req.content_type.substr(bp - 14 + 9);
            // Remove quotes if present
            if (!req.boundary.empty() && req.boundary.front() == '"') {
               req.boundary = req.boundary.substr(1);
               auto qp = req.boundary.find('"');
               if (qp != std::string::npos) req.boundary = req.boundary.substr(0, qp);
            }
         }
      }
   }

   // Body starts after \r\n\r\n
   size_t body_start = headers_end + 4;
   if (body_start < raw.size()) {
      req.body = raw.substr(body_start);
   }

   return true;
}

// ── M4-compatible directory listing ──────────────────────
// Format: first line is current directory, then name,type,size per line
// type: 0=directory, 1=file

std::string M4HttpServer::build_dir_txt(const std::string& web_path) {
   if (g_m4board.sd_root_path.empty()) return "/\n";

   // Determine which directory to list
   std::string dir_path = web_path;
   if (dir_path.empty()) dir_path = g_m4board.current_dir;

   try {
      auto root = std::filesystem::weakly_canonical(g_m4board.sd_root_path);
      std::filesystem::path full_path;
      if (dir_path.front() == '/') {
         full_path = root / dir_path.substr(1);
      } else {
         full_path = root / g_m4board.current_dir.substr(1) / dir_path;
      }
      auto canonical = std::filesystem::weakly_canonical(full_path);

      // Traversal check
      auto rel = canonical.lexically_normal().lexically_relative(root.lexically_normal());
      if (!rel.empty() && *rel.begin() == "..") return "/\n";

      // Build relative dir string for output
      std::string rel_dir;
      if (rel == ".") {
         rel_dir = "/";
      } else {
         rel_dir = "/" + rel.generic_string();
         if (rel_dir.back() != '/') rel_dir += '/';
      }

      std::string result = rel_dir + "\n";

      if (!std::filesystem::is_directory(canonical)) return rel_dir + "\n";

      for (const auto& entry : std::filesystem::directory_iterator(canonical)) {
         std::string name = entry.path().filename().string();
         if (name.empty() || name[0] == '.') continue;

         if (entry.is_directory()) {
            result += name + ",0,0\n";
         } else {
            uintmax_t size = 0;
            try { size = entry.file_size(); } catch (...) {}
            result += name + ",1," + std::to_string(size) + "\n";
         }
      }

      return result;
   } catch (const std::filesystem::filesystem_error& e) {
      LOG_ERROR("M4 HTTP: " << e.what());
      return "/\n";
   }
}

// ── Route handlers ───────────────────────────────────────

M4HttpServer::HttpResponse M4HttpServer::handle_index(const HttpRequest&) {
   HttpResponse resp;
   resp.body.assign(reinterpret_cast<const char*>(m4_web_index_html),
                    m4_web_index_html_len);
   resp.content_type = "text/html; charset=utf-8";
   return resp;
}

M4HttpServer::HttpResponse M4HttpServer::handle_files_api(const HttpRequest& req) {
   HttpResponse resp;
   std::string path = get_query_param(req.query_string, "path");
   if (path.empty()) path = g_m4board.current_dir;

   resp.body = build_dir_txt(path);
   resp.content_type = "text/plain; charset=utf-8";
   return resp;
}

M4HttpServer::HttpResponse M4HttpServer::handle_download(const HttpRequest& req) {
   HttpResponse resp;
   std::string path = get_query_param(req.query_string, "path");
   if (path.empty()) {
      resp.status = 400;
      resp.status_text = "Bad Request";
      resp.body = "Missing path parameter";
      return resp;
   }

   if (g_m4board.sd_root_path.empty()) {
      resp.status = 503;
      resp.status_text = "Service Unavailable";
      resp.body = "No SD path configured";
      return resp;
   }

   try {
      auto root = std::filesystem::weakly_canonical(g_m4board.sd_root_path);
      auto full = std::filesystem::weakly_canonical(root / path.substr(path[0] == '/' ? 1 : 0));
      auto rel = full.lexically_normal().lexically_relative(root.lexically_normal());
      if (!rel.empty() && *rel.begin() == "..") {
         resp.status = 403; resp.status_text = "Forbidden";
         resp.body = "Path traversal blocked";
         return resp;
      }

      if (!std::filesystem::exists(full) || std::filesystem::is_directory(full)) {
         resp.status = 404; resp.status_text = "Not Found";
         resp.body = "File not found";
         return resp;
      }

      resp.send_file = true;
      resp.file_path = full.string();
      resp.content_type = mime_type_for(full.string());
   } catch (const std::filesystem::filesystem_error& e) {
      resp.status = 500; resp.status_text = "Internal Server Error";
      resp.body = e.what();
   }
   return resp;
}

M4HttpServer::HttpResponse M4HttpServer::handle_upload(const HttpRequest& req) {
   HttpResponse resp;

   if (g_m4board.sd_root_path.empty()) {
      resp.status = 503; resp.status_text = "Service Unavailable";
      resp.body = "No SD path configured";
      return resp;
   }

   if (req.boundary.empty() || req.body.empty()) {
      resp.status = 400; resp.status_text = "Bad Request";
      resp.body = "Expected multipart/form-data with boundary";
      return resp;
   }

   // Parse multipart body — extract filename and content
   std::string delim = "--" + req.boundary;
   size_t part_start = req.body.find(delim);
   if (part_start == std::string::npos) {
      resp.status = 400; resp.status_text = "Bad Request";
      resp.body = "No multipart boundary found in body";
      return resp;
   }
   part_start += delim.size() + 2; // skip \r\n after boundary

   // Find Content-Disposition header for filename
   size_t disp_start = req.body.find("filename=\"", part_start);
   if (disp_start == std::string::npos) {
      resp.status = 400; resp.status_text = "Bad Request";
      resp.body = "No filename in upload";
      return resp;
   }
   disp_start += 10; // skip 'filename="'
   size_t disp_end = req.body.find('"', disp_start);
   std::string filename = req.body.substr(disp_start, disp_end - disp_start);

   // The real M4 uses the filename as a full path (e.g., "/games/test.bas")
   // Sanitize: strip leading slashes from the filename for safety
   while (!filename.empty() && filename[0] == '/') filename = filename.substr(1);
   if (filename.empty()) {
      resp.status = 400; resp.status_text = "Bad Request";
      resp.body = "Empty filename";
      return resp;
   }

   // Block path traversal
   if (filename.find("..") != std::string::npos) {
      resp.status = 403; resp.status_text = "Forbidden";
      resp.body = "Path traversal blocked";
      return resp;
   }

   // Find body content: after \r\n\r\n in this part
   size_t content_start = req.body.find("\r\n\r\n", part_start);
   if (content_start == std::string::npos) {
      resp.status = 400; resp.status_text = "Bad Request";
      resp.body = "Malformed multipart body";
      return resp;
   }
   content_start += 4;

   // Find end boundary
   std::string end_delim = "\r\n" + delim;
   size_t content_end = req.body.find(end_delim, content_start);
   if (content_end == std::string::npos) {
      content_end = req.body.size(); // Take everything
   }

   std::string file_data = req.body.substr(content_start, content_end - content_start);

   // Write to SD directory
   try {
      auto root = std::filesystem::weakly_canonical(g_m4board.sd_root_path);
      auto dest = root / filename;
      auto dest_canonical = std::filesystem::weakly_canonical(dest.parent_path());
      auto rel = dest_canonical.lexically_normal().lexically_relative(root.lexically_normal());
      if (!rel.empty() && *rel.begin() == "..") {
         resp.status = 403; resp.status_text = "Forbidden";
         resp.body = "Path traversal blocked";
         return resp;
      }

      // Create parent directories if needed
      std::filesystem::create_directories(dest.parent_path());

      FILE* f = fopen(dest.string().c_str(), "wb");
      if (!f) {
         resp.status = 500; resp.status_text = "Internal Server Error";
         resp.body = "Cannot create file";
         return resp;
      }
      size_t written = fwrite(file_data.data(), 1, file_data.size(), f);
      if (written != file_data.size()) {
         LOG_ERROR("M4 HTTP: short write uploading " << filename
                   << " (" << written << "/" << file_data.size() << ")");
      }
      if (fclose(f) != 0) {
         LOG_ERROR("M4 HTTP: fclose failed uploading " << filename);
      }

      LOG_INFO("M4 HTTP: uploaded " << filename << " (" << file_data.size() << " bytes)");

      resp.body = "OK " + filename + " (" + std::to_string(file_data.size()) + " bytes)\n";
      resp.content_type = "text/plain";
   } catch (const std::filesystem::filesystem_error& e) {
      resp.status = 500; resp.status_text = "Internal Server Error";
      resp.body = e.what();
   }
   return resp;
}

M4HttpServer::HttpResponse M4HttpServer::handle_config_cgi(const HttpRequest& req) {
   HttpResponse resp;

   // config.cgi handles multiple actions via query params (matching real M4)
   // ?ls=<path>  — list directory (returns dir.txt content)
   // ?cd=<path>  — change CPC working directory
   // ?cd2=<path> — alias for cd (used by some tools)
   // ?run=<path> — remote run (alias)
   // ?run2=<path> — remote run file on CPC
   // ?rm=<path>  — delete file
   // ?mkdir=<name> — create directory

   // ls: list directory, change the web browsing dir
   std::string ls_path = get_query_param(req.query_string, "ls");
   if (req.query_string.find("ls") != std::string::npos) {
      resp.body = build_dir_txt(ls_path.empty() ? "/" : ls_path);
      resp.content_type = "text/plain; charset=utf-8";
      return resp;
   }

   // cd: change the CPC's M4 working directory
   std::string cd_path = get_query_param(req.query_string, "cd");
   if (cd_path.empty()) cd_path = get_query_param(req.query_string, "cd2");
   if (!cd_path.empty()) {
      // Sanitize and set on the M4 board
      if (cd_path.back() != '/') cd_path += '/';
      g_m4board.current_dir = cd_path;

      // Redirect back to files page
      resp.status = 302;
      resp.status_text = "Found";
      resp.body = "Redirecting...";
      resp.content_type = "text/html";
      // The redirect is done via Location header (handled in format_response)
      return resp;
   }

   // run / run2: remote execute file on CPC
   std::string run_path = get_query_param(req.query_string, "run2");
   if (run_path.empty()) run_path = get_query_param(req.query_string, "run");
   if (!run_path.empty()) {
      // Extract filename from path for RUN command
      auto fname = std::filesystem::path(run_path).filename().string();
      if (!fname.empty()) {
         // First, ensure we're on the SD
         std::string cmd = "|sd\n";
         // Navigate to the directory containing the file
         auto dir = std::filesystem::path(run_path).parent_path().generic_string();
         if (!dir.empty() && dir != "/" && dir != ".") {
            cmd += "|cd,\"" + dir + "\"\n";
         }
         cmd += "run\"" + fname + "\"\n";
         g_autotype_queue.enqueue(cmd);
         LOG_INFO("M4 HTTP: remote run " << run_path);
      }
      resp.body = "OK running " + run_path;
      resp.content_type = "text/plain";
      return resp;
   }

   // rm: delete file
   std::string rm_path = get_query_param(req.query_string, "rm");
   if (!rm_path.empty()) {
      if (g_m4board.sd_root_path.empty()) {
         resp.status = 503; resp.status_text = "Service Unavailable";
         resp.body = "No SD path configured";
         return resp;
      }
      try {
         auto root = std::filesystem::weakly_canonical(g_m4board.sd_root_path);
         auto full = std::filesystem::weakly_canonical(root / rm_path.substr(rm_path[0] == '/' ? 1 : 0));
         auto rel = full.lexically_normal().lexically_relative(root.lexically_normal());
         if (!rel.empty() && *rel.begin() == "..") {
            resp.status = 403; resp.status_text = "Forbidden";
            resp.body = "Path traversal blocked";
            return resp;
         }
         if (std::filesystem::remove(full)) {
            LOG_INFO("M4 HTTP: deleted " << rm_path);
            resp.body = "OK deleted " + rm_path;
         } else {
            resp.status = 404; resp.status_text = "Not Found";
            resp.body = "File not found";
         }
      } catch (const std::filesystem::filesystem_error& e) {
         resp.status = 500; resp.status_text = "Internal Server Error";
         resp.body = e.what();
      }
      resp.content_type = "text/plain";
      return resp;
   }

   // mkdir
   std::string mkdir_name = get_query_param(req.query_string, "mkdir");
   if (!mkdir_name.empty()) {
      if (g_m4board.sd_root_path.empty()) {
         resp.status = 503; resp.status_text = "Service Unavailable";
         resp.body = "No SD path configured";
         return resp;
      }
      try {
         auto root = std::filesystem::weakly_canonical(g_m4board.sd_root_path);
         auto full = root / mkdir_name.substr(mkdir_name[0] == '/' ? 1 : 0);
         auto parent = std::filesystem::weakly_canonical(full.parent_path());
         auto rel = parent.lexically_normal().lexically_relative(root.lexically_normal());
         if (!rel.empty() && *rel.begin() == "..") {
            resp.status = 403; resp.status_text = "Forbidden";
            resp.body = "Path traversal blocked";
            return resp;
         }
         std::filesystem::create_directories(full);
         resp.body = "OK created " + mkdir_name;
      } catch (const std::filesystem::filesystem_error& e) {
         resp.status = 500; resp.status_text = "Internal Server Error";
         resp.body = e.what();
      }
      resp.content_type = "text/plain";
      return resp;
   }

   resp.status = 400; resp.status_text = "Bad Request";
   resp.body = "Unknown config.cgi action";
   resp.content_type = "text/plain";
   return resp;
}

M4HttpServer::HttpResponse M4HttpServer::handle_status(const HttpRequest&) {
   HttpResponse resp;
   int open_files = 0;
   for (int i = 0; i < 4; i++) {
      if (g_m4board.open_files[i]) open_files++;
   }

   std::ostringstream json;
   json << "{\n"
        << "  \"enabled\": " << (g_m4board.enabled ? "true" : "false") << ",\n"
        << "  \"sd_path\": \"" << g_m4board.sd_root_path << "\",\n"
        << "  \"current_dir\": \"" << g_m4board.current_dir << "\",\n"
        << "  \"open_files\": " << open_files << ",\n"
        << "  \"cmd_count\": " << g_m4board.cmd_count << ",\n"
        << "  \"network\": " << (g_m4board.network_enabled ? "true" : "false") << ",\n"
        << "  \"paused\": " << (CPC.paused ? "true" : "false") << ",\n"
        << "  \"http_port\": " << actual_port.load() << ",\n"
        << "  \"bind_ip\": \"" << bind_ip_ << "\",\n"
        << "  \"version\": \"koncepcja-m4\"\n"
        << "}\n";

   resp.body = json.str();
   resp.content_type = "application/json; charset=utf-8";
   return resp;
}

M4HttpServer::HttpResponse M4HttpServer::handle_sd_file(const HttpRequest& req) {
   // /sd/<path> — serve file directly from SD card directory
   HttpResponse resp;

   if (g_m4board.sd_root_path.empty()) {
      resp.status = 503; resp.status_text = "Service Unavailable";
      resp.body = "No SD path configured";
      return resp;
   }

   // Strip /sd/ prefix
   std::string rel_path = req.path.substr(4); // skip "/sd/"
   if (rel_path.empty()) {
      // /sd/ alone — return directory listing
      resp.body = build_dir_txt("/");
      resp.content_type = "text/plain; charset=utf-8";
      return resp;
   }

   // Special case: /sd/m4/dir.txt — the AJAX directory listing endpoint
   if (rel_path == "m4/dir.txt") {
      resp.body = build_dir_txt(g_m4board.current_dir);
      resp.content_type = "text/plain; charset=utf-8";
      return resp;
   }

   try {
      auto root = std::filesystem::weakly_canonical(g_m4board.sd_root_path);
      auto full = std::filesystem::weakly_canonical(root / rel_path);
      auto rel = full.lexically_normal().lexically_relative(root.lexically_normal());
      if (!rel.empty() && *rel.begin() == "..") {
         resp.status = 403; resp.status_text = "Forbidden";
         resp.body = "Path traversal blocked";
         return resp;
      }

      if (!std::filesystem::exists(full)) {
         resp.status = 404; resp.status_text = "Not Found";
         resp.body = "File not found";
         return resp;
      }

      if (std::filesystem::is_directory(full)) {
         // Return directory listing for this path
         auto dir_rel = "/" + rel.generic_string();
         if (dir_rel.back() != '/') dir_rel += '/';
         resp.body = build_dir_txt(dir_rel);
         resp.content_type = "text/plain; charset=utf-8";
         return resp;
      }

      resp.send_file = true;
      resp.file_path = full.string();
      resp.content_type = mime_type_for(full.string());
   } catch (const std::filesystem::filesystem_error& e) {
      resp.status = 500; resp.status_text = "Internal Server Error";
      resp.body = e.what();
   }
   return resp;
}

M4HttpServer::HttpResponse M4HttpServer::handle_static(const HttpRequest& req) {
   HttpResponse resp;

   // Serve embedded assets by path
   if (req.path == "/stylesheet.css") {
      resp.body.assign(reinterpret_cast<const char*>(m4_web_stylesheet_css),
                       m4_web_stylesheet_css_len);
      resp.content_type = "text/css; charset=utf-8";
      return resp;
   }

   resp.status = 404; resp.status_text = "Not Found";
   resp.body = "Not found";
   return resp;
}

// ── Request routing ──────────────────────────────────────

M4HttpServer::HttpResponse M4HttpServer::handle_request(const HttpRequest& req) {
   LOG_DEBUG("M4 HTTP: " << req.method << " " << req.path
             << (req.query_string.empty() ? "" : "?") << req.query_string);

   // POST / or POST /upload.html — file upload
   if (req.method == "POST" && (req.path == "/" || req.path == "/upload.html")) {
      return handle_upload(req);
   }

   // POST /reset — reset CPC (deferred to main thread)
   if (req.method == "POST" && req.path == "/reset") {
      pending_reset.store(true);
      HttpResponse resp;
      resp.body = "OK reset queued\n";
      resp.content_type = "text/plain";
      return resp;
   }

   // POST /pause — toggle pause (deferred to main thread)
   if (req.method == "POST" && req.path == "/pause") {
      pending_pause_toggle.store(true);
      HttpResponse resp;
      resp.body = "OK pause toggle queued\n";
      resp.content_type = "text/plain";
      return resp;
   }

   // GET routes
   if (req.method == "GET" || req.method == "HEAD") {
      if (req.path == "/" || req.path == "/index.html" || req.path == "/index.shtml"
          || req.path == "/files.shtml") {
         return handle_index(req);
      }
      if (req.path == "/files") return handle_files_api(req);
      if (req.path == "/download") return handle_download(req);
      if (req.path == "/config.cgi") return handle_config_cgi(req);
      if (req.path == "/status") return handle_status(req);
      if (req.path == "/roms" || req.path == "/roms.shtml") {
         // ROM management page — not implemented yet, redirect to index
         HttpResponse resp;
         resp.status = 302; resp.status_text = "Found";
         resp.body = "ROM management not yet implemented";
         return resp;
      }
      if (req.path == "/upload.html") return handle_index(req);

      // /sd/* — serve files from SD card
      if (req.path.size() >= 4 && req.path.substr(0, 4) == "/sd/") {
         return handle_sd_file(req);
      }

      // Static assets (CSS, JS, images)
      return handle_static(req);
   }

   HttpResponse resp;
   resp.status = 405; resp.status_text = "Method Not Allowed";
   resp.body = "Method not allowed";
   resp.content_type = "text/plain";
   return resp;
}

// ── Response formatting ──────────────────────────────────

std::string M4HttpServer::format_response(const HttpResponse& resp) {
   std::ostringstream out;
   out << "HTTP/1.1 " << resp.status << " " << resp.status_text << "\r\n";
   out << "Server: koncepcja-m4\r\n";
   out << "Connection: close\r\n";
   out << "Access-Control-Allow-Origin: *\r\n"; // CORS for cpcxfer etc.

   if (resp.status == 302) {
      // For redirects, we need Location header — redirect to /
      out << "Location: /\r\n";
   }

   if (!resp.send_file) {
      out << "Content-Type: " << resp.content_type << "\r\n";
      if (!resp.content_encoding.empty()) {
         out << "Content-Encoding: " << resp.content_encoding << "\r\n";
      }
      out << "Content-Length: " << resp.body.size() << "\r\n";
      out << "\r\n";
      out << resp.body;
   } else {
      out << "Content-Type: " << resp.content_type << "\r\n";
      // File size for Content-Length
      try {
         auto size = std::filesystem::file_size(resp.file_path);
         out << "Content-Length: " << size << "\r\n";
      } catch (...) {}
      out << "\r\n";
      // Body will be sent by the caller reading the file
   }
   return out.str();
}

// ── Port forwarding ──────────────────────────────────────

void M4HttpServer::set_port_mapping(uint16_t cpc_port, uint16_t host_port, bool user_override) {
   std::lock_guard<std::mutex> lock(port_mutex);
   for (auto& m : port_mappings_) {
      if (m.cpc_port == cpc_port) {
         m.host_port = host_port;
         m.user_override = user_override;
         return;
      }
   }
   port_mappings_.push_back({cpc_port, host_port, user_override, false, ""});
}

void M4HttpServer::remove_port_mapping(uint16_t cpc_port) {
   std::lock_guard<std::mutex> lock(port_mutex);
   port_mappings_.erase(
      std::remove_if(port_mappings_.begin(), port_mappings_.end(),
         [cpc_port](const M4PortMapping& m) { return m.cpc_port == cpc_port; }),
      port_mappings_.end()
   );
}

uint16_t M4HttpServer::resolve_host_port(uint16_t cpc_port) {
   std::lock_guard<std::mutex> lock(port_mutex);
   for (const auto& m : port_mappings_) {
      if (m.cpc_port == cpc_port) return m.host_port;
   }
   return cpc_port; // default: same port
}

// ── Lifecycle ────────────────────────────────────────────

// ── Deferred action drain (called from main loop) ────────

void M4HttpServer::drain_pending() {
   if (pending_reset.exchange(false)) {
      emulator_reset();
      LOG_INFO("M4 HTTP: reset executed");
   }
   if (pending_pause_toggle.exchange(false)) {
      CPC.paused = !CPC.paused;
      LOG_INFO("M4 HTTP: " << (CPC.paused ? "paused" : "resumed"));
   }
}

M4HttpServer::~M4HttpServer() {
   stop();
}

void M4HttpServer::start(int port, const std::string& bind_ip) {
   if (running.load()) return;
   configured_port_ = port;
   bind_ip_ = bind_ip;
   running.store(true);
   server_thread = std::thread(&M4HttpServer::run, this);
}

void M4HttpServer::stop() {
   running.store(false);
   if (server_thread.joinable()) server_thread.join();
   actual_port.store(0);
}

// ── Server thread ────────────────────────────────────────

#ifdef _WIN32

void M4HttpServer::run() {
   WSADATA wsa;
   if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;

   // Validate bind IP — on Windows, loopback aliases need admin.
   // Probe with an ephemeral port; fall back to 127.0.0.1 on WSAEADDRNOTAVAIL.
   if (bind_ip_ != "127.0.0.1" && bind_ip_ != "0.0.0.0") {
      SOCKET probe = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (probe != INVALID_SOCKET) {
         sockaddr_in probe_addr{};
         probe_addr.sin_family = AF_INET;
         probe_addr.sin_port = 0;
         inet_pton(AF_INET, bind_ip_.c_str(), &probe_addr.sin_addr);
         if (bind(probe, reinterpret_cast<sockaddr*>(&probe_addr), sizeof(probe_addr)) == SOCKET_ERROR
             && WSAGetLastError() == WSAEADDRNOTAVAIL) {
            LOG_ERROR("M4 HTTP: " << bind_ip_
                      << " not assignable (Windows needs admin for loopback aliases)."
                      << " Falling back to 127.0.0.1");
            bind_ip_ = "127.0.0.1";
         }
         closesocket(probe);
      }
   }

   sock_t server_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
   if (server_fd == BAD_SOCK) { WSACleanup(); return; }

   int opt = 1;
   setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
              reinterpret_cast<const char*>(&opt), sizeof(opt));

   sockaddr_in addr{};
   addr.sin_family = AF_INET;
   inet_pton(AF_INET, bind_ip_.c_str(), &addr.sin_addr);

   int bound_port = 0;
   for (int p = configured_port_; p < configured_port_ + 10; p++) {
      addr.sin_port = htons(static_cast<uint16_t>(p));
      if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != SOCKET_ERROR) {
         bound_port = p;
         break;
      }
   }
   if (bound_port == 0) {
      LOG_ERROR("M4 HTTP: could not bind to ports " << configured_port_
                << "-" << (configured_port_ + 9));
      closesocket(server_fd);
      WSACleanup();
      return;
   }

   if (listen(server_fd, 4) == SOCKET_ERROR) {
      closesocket(server_fd);
      WSACleanup();
      return;
   }

   actual_port.store(bound_port);
   LOG_INFO("M4 HTTP server: listening on " << bind_ip_ << ":" << bound_port);

   while (running.load()) {
      fd_set readfds;
      FD_ZERO(&readfds);
      FD_SET(server_fd, &readfds);
      timeval tv{0, 200000}; // 200ms

      int ready = select(0, &readfds, nullptr, nullptr, &tv);
      if (ready <= 0) continue;

      sockaddr_in peer{};
      int plen = sizeof(peer);
      sock_t client = accept(server_fd, reinterpret_cast<sockaddr*>(&peer), &plen);
      if (client == BAD_SOCK) continue;

      // Read request (up to 256KB for uploads)
      static constexpr int MAX_REQ = 256 * 1024;
      std::string raw;
      raw.reserve(4096);

      // Set a short timeout for reading
      DWORD timeout = 5000;
      setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout), sizeof(timeout));

      char buf[4096];
      bool headers_complete = false;
      int content_remaining = 0;

      while (true) {
         int n = sock_recv(client, buf, sizeof(buf));
         if (n <= 0) break;
         raw.append(buf, static_cast<size_t>(n));

         // Check if we have complete headers
         if (!headers_complete) {
            size_t hdr_end = raw.find("\r\n\r\n");
            if (hdr_end != std::string::npos) {
               headers_complete = true;
               // Parse Content-Length to know how much body to expect
               std::string lower = raw;
               for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
               size_t cl_pos = lower.find("content-length: ");
               if (cl_pos != std::string::npos) {
                  try { content_remaining = std::stoi(raw.substr(cl_pos + 16)); }
                  catch (...) { content_remaining = 0; }
               }
               int body_so_far = static_cast<int>(raw.size() - (hdr_end + 4));
               content_remaining -= body_so_far;
            }
         } else {
            content_remaining -= n;
         }

         if (headers_complete && content_remaining <= 0) break;
         if (raw.size() > MAX_REQ) break;
      }

      // Parse and handle
      HttpRequest req;
      if (parse_request(raw, req)) {
         HttpResponse resp = handle_request(req);
         std::string response_str = format_response(resp);

         sock_send(client, response_str.data(), static_cast<int>(response_str.size()));

         // If file download, stream the file
         if (resp.send_file) {
            FILE* f = fopen(resp.file_path.c_str(), "rb");
            if (f) {
               char fbuf[8192];
               size_t nr;
               while ((nr = fread(fbuf, 1, sizeof(fbuf), f)) > 0) {
                  sock_send(client, fbuf, static_cast<int>(nr));
               }
               fclose(f);
            }
         }
      }

      close_sock(client);
   }

   closesocket(server_fd);
   WSACleanup();
}

#else // POSIX

void M4HttpServer::run() {
   // Validate bind IP before creating the server socket.
   // Platform behaviour for loopback aliases (e.g. 127.0.0.2):
   //   macOS  — any 127.x.x.x works without root (lo0 accepts the full /8)
   //   Linux  — only 127.0.0.1 exists by default; others need root:
   //            "ip addr add 127.0.0.2/8 dev lo"
   //   Windows — loopback aliases need admin
   // If the address isn't assignable, fall back to 127.0.0.1 with a warning.
   if (bind_ip_ != "127.0.0.1" && bind_ip_ != "0.0.0.0") {
      int probe = ::socket(AF_INET, SOCK_STREAM, 0);
      if (probe >= 0) {
         sockaddr_in probe_addr{};
         probe_addr.sin_family = AF_INET;
         probe_addr.sin_port = 0; // ephemeral port — we just want to test the IP
         inet_pton(AF_INET, bind_ip_.c_str(), &probe_addr.sin_addr);
         if (bind(probe, reinterpret_cast<sockaddr*>(&probe_addr), sizeof(probe_addr)) != 0
             && errno == EADDRNOTAVAIL) {
            LOG_ERROR("M4 HTTP: " << bind_ip_
                      << " not assignable on this system"
                      << " (Linux needs: sudo ip addr add " << bind_ip_ << "/8 dev lo)."
                      << " Falling back to 127.0.0.1");
            bind_ip_ = "127.0.0.1";
         }
         ::close(probe);
      }
   }

   sock_t server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
   if (server_fd < 0) return;

   int opt = 1;
   setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

   sockaddr_in addr{};
   addr.sin_family = AF_INET;
   inet_pton(AF_INET, bind_ip_.c_str(), &addr.sin_addr);

   int bound_port = 0;
   for (int p = configured_port_; p < configured_port_ + 10; p++) {
      addr.sin_port = htons(static_cast<uint16_t>(p));
      if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
         bound_port = p;
         break;
      }
   }
   if (bound_port == 0) {
      LOG_ERROR("M4 HTTP: could not bind to ports " << configured_port_
                << "-" << (configured_port_ + 9));
      ::close(server_fd);
      return;
   }

   if (listen(server_fd, 4) < 0) {
      ::close(server_fd);
      return;
   }

   actual_port.store(bound_port);
   LOG_INFO("M4 HTTP server: listening on " << bind_ip_ << ":" << bound_port);

   while (running.load()) {
      fd_set readfds;
      FD_ZERO(&readfds);
      FD_SET(server_fd, &readfds);
      timeval tv{0, 200000}; // 200ms

      int ready = select(server_fd + 1, &readfds, nullptr, nullptr, &tv);
      if (ready <= 0) continue;

      sockaddr_in peer{};
      socklen_t plen = sizeof(peer);
      sock_t client = accept(server_fd, reinterpret_cast<sockaddr*>(&peer), &plen);
      if (client < 0) continue;

      // Set a read timeout
      timeval recv_tv{5, 0}; // 5 seconds
      setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &recv_tv, sizeof(recv_tv));

      // Read request (up to 256KB for uploads)
      static constexpr int MAX_REQ = 256 * 1024;
      std::string raw;
      raw.reserve(4096);
      char buf[4096];
      bool headers_complete = false;
      int content_remaining = 0;

      while (true) {
         int n = sock_recv(client, buf, sizeof(buf));
         if (n <= 0) break;
         raw.append(buf, static_cast<size_t>(n));

         if (!headers_complete) {
            size_t hdr_end = raw.find("\r\n\r\n");
            if (hdr_end != std::string::npos) {
               headers_complete = true;
               std::string lower = raw;
               for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
               size_t cl_pos = lower.find("content-length: ");
               if (cl_pos != std::string::npos) {
                  try { content_remaining = std::stoi(raw.substr(cl_pos + 16)); }
                  catch (...) { content_remaining = 0; }
               }
               int body_so_far = static_cast<int>(raw.size() - (hdr_end + 4));
               content_remaining -= body_so_far;
            }
         } else {
            content_remaining -= n;
         }

         if (headers_complete && content_remaining <= 0) break;
         if (static_cast<int>(raw.size()) > MAX_REQ) break;
      }

      HttpRequest req;
      if (parse_request(raw, req)) {
         HttpResponse resp = handle_request(req);
         std::string response_str = format_response(resp);

         sock_send(client, response_str.data(), static_cast<int>(response_str.size()));

         if (resp.send_file) {
            FILE* f = fopen(resp.file_path.c_str(), "rb");
            if (f) {
               char fbuf[8192];
               size_t nr;
               while ((nr = fread(fbuf, 1, sizeof(fbuf), f)) > 0) {
                  sock_send(client, fbuf, static_cast<int>(nr));
               }
               fclose(f);
            }
         }
      }

      close_sock(client);
   }

   ::close(server_fd);
}

#endif // _WIN32
