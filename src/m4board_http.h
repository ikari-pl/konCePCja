/* konCePCja - Amstrad CPC Emulator
   M4 Board — Embedded HTTP server

   Serves the M4 web interface for file browsing, upload/download,
   remote run, and CPC control. Compatible with cpcxfer and the
   M4 Board Android app.

   Architecture: Single server thread with select() loop (matches
   IPC server and telnet console patterns). Multiple concurrent
   clients supported for static assets; command endpoints are
   serialized through the emulator's main thread.
*/

#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>

struct M4PortMapping {
   uint16_t cpc_port = 0;       // Port the CPC software requested
   uint16_t host_port = 0;      // Actual host port (may differ)
   bool user_override = false;   // true = manually set, false = auto-assigned
   bool active = false;          // Currently bound/listening
   std::string description;     // Auto-detected or user-set label
};

class M4HttpServer {
public:
   ~M4HttpServer();

   void start(int port = 8080, const std::string& bind_ip = "127.0.0.1");
   void stop();

   // Called from main loop each frame — executes deferred CPC actions.
   void drain_pending();

   int port() const { return actual_port.load(); }
   bool is_running() const { return running.load(); }
   const std::string& bind_ip() const { return bind_ip_; }

   // Port forwarding table.
   // MAIN THREAD ONLY — for iteration/mutation from Z80 I/O handlers.
   // HTTP/IPC threads must use get_port_mappings_snapshot() instead.
   std::vector<M4PortMapping>& port_mappings() { return port_mappings_; }
   // Thread-safe snapshot for UI/IPC threads that iterate mappings
   std::vector<M4PortMapping> get_port_mappings_snapshot() const {
      std::lock_guard<std::mutex> lock(port_mutex_);
      return port_mappings_;
   }
   void set_port_mapping(uint16_t cpc_port, uint16_t host_port, bool user_override = true);
   void remove_port_mapping(uint16_t cpc_port);
   uint16_t resolve_host_port(uint16_t cpc_port); // returns host port for CPC port

private:
   void run();

   // HTTP request parsing and response generation
   struct HttpRequest {
      std::string method;       // GET, POST, HEAD
      std::string path;         // /files?path=/
      std::string query_string; // path=/ (decoded)
      std::string body;
      int content_length = 0;
      std::string content_type;
      std::string boundary;     // multipart boundary
      bool is_websocket_upgrade = false;
      std::string websocket_key; // Sec-WebSocket-Key header
   };

   struct HttpResponse {
      int status = 200;
      std::string status_text = "OK";
      std::string content_type = "text/html; charset=utf-8";
      std::string body;
      std::string content_encoding; // "gzip" if pre-compressed
      bool send_file = false;
      std::string file_path;
      bool head_request = false; // suppress body in format_response
   };

   bool parse_request(const std::string& raw, HttpRequest& req);
   HttpResponse handle_request(const HttpRequest& req);

   // Route handlers
   HttpResponse handle_index(const HttpRequest& req);
   HttpResponse handle_files_api(const HttpRequest& req);
   HttpResponse handle_download(const HttpRequest& req);
   HttpResponse handle_upload(const HttpRequest& req);
   HttpResponse handle_config_cgi(const HttpRequest& req);
   HttpResponse handle_status(const HttpRequest& req);
   HttpResponse handle_static(const HttpRequest& req);
   HttpResponse handle_sd_file(const HttpRequest& req);
   HttpResponse handle_preview(const HttpRequest& req);
   HttpResponse handle_roms_api(const HttpRequest& req);

   // Utility
   static std::string url_decode(const std::string& str);
   static std::string get_query_param(const std::string& query, const std::string& key);
   static std::string mime_type_for(const std::string& path);
   static std::string format_response(const HttpResponse& resp);
   std::string build_dir_txt(const std::string& path); // M4-compatible dir listing

   std::atomic<bool> running{false};
   std::atomic<int> actual_port{0};
   std::string bind_ip_ = "127.0.0.1";
   int configured_port_ = 8080;
   std::thread server_thread;

   // Port forwarding
   mutable std::mutex port_mutex_;
   std::vector<M4PortMapping> port_mappings_;

   // Surface snapshot for preview — updated by main thread, read by HTTP thread.
   mutable std::mutex preview_mutex_;
   std::vector<uint8_t> preview_bmp_;  // BMP-encoded snapshot

   // Status snapshots — written by main thread in drain_pending(), read by HTTP thread.
   std::atomic<bool> snapshot_paused{false};
   std::atomic<int> snapshot_screen_w{0};
   std::atomic<int> snapshot_screen_h{0};

public:
   // Called from main thread to update the preview snapshot.
   void update_preview_snapshot();

   // Deferred actions — set by HTTP thread, consumed by main loop.
   // These ensure thread-unsafe CPC operations happen on the main thread.
   std::atomic<bool> pending_reset{false};
   std::atomic<bool> pending_pause_toggle{false};
   std::atomic<bool> pending_nmi{false};
};

extern M4HttpServer g_m4_http;
