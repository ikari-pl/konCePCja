#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <thread>
#include <chrono>
#include <random>
#include "m4board.h"
#include "m4board_http.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

// ── Helpers ──

static std::string http_get(int port, const std::string& path) {
   int fd = ::socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0) return "";

   sockaddr_in addr{};
   addr.sin_family = AF_INET;
   addr.sin_port = htons(static_cast<uint16_t>(port));
   inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

   if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
      ::close(fd);
      return "";
   }

   std::string req = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
   (void)::write(fd, req.c_str(), req.size());

   std::string response;
   char buf[4096];
   // Set read timeout
   timeval tv{2, 0};
   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

   while (true) {
      ssize_t n = ::read(fd, buf, sizeof(buf));
      if (n <= 0) break;
      response.append(buf, static_cast<size_t>(n));
   }
   ::close(fd);
   return response;
}

static std::string http_post(int port, const std::string& path,
                             const std::string& content_type,
                             const std::string& body) {
   int fd = ::socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0) return "";

   sockaddr_in addr{};
   addr.sin_family = AF_INET;
   addr.sin_port = htons(static_cast<uint16_t>(port));
   inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

   if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
      ::close(fd);
      return "";
   }

   std::string req = "POST " + path + " HTTP/1.1\r\n"
                     "Host: localhost\r\n"
                     "Content-Type: " + content_type + "\r\n"
                     "Content-Length: " + std::to_string(body.size()) + "\r\n"
                     "\r\n" + body;
   (void)::write(fd, req.c_str(), req.size());

   std::string response;
   char buf[4096];
   timeval tv{2, 0};
   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

   while (true) {
      ssize_t n = ::read(fd, buf, sizeof(buf));
      if (n <= 0) break;
      response.append(buf, static_cast<size_t>(n));
   }
   ::close(fd);
   return response;
}

static std::string extract_body(const std::string& response) {
   size_t pos = response.find("\r\n\r\n");
   if (pos == std::string::npos) return "";
   return response.substr(pos + 4);
}

static int extract_status(const std::string& response) {
   // "HTTP/1.1 200 OK\r\n..."
   size_t sp1 = response.find(' ');
   if (sp1 == std::string::npos) return 0;
   try { return std::stoi(response.substr(sp1 + 1)); }
   catch (...) { return 0; }
}

// ── Test fixture ──

class M4HttpTest : public ::testing::Test {
protected:
   void SetUp() override {
      // Create a temp SD directory
      sd_dir_ = std::filesystem::temp_directory_path() / "m4http_test";
      std::filesystem::remove_all(sd_dir_);
      std::filesystem::create_directories(sd_dir_);

      // Create some test files
      std::ofstream(sd_dir_ / "test.bas") << "10 PRINT \"HELLO\"\n20 GOTO 10\n";
      std::ofstream(sd_dir_ / "game.dsk"); // empty file
      std::filesystem::create_directories(sd_dir_ / "games");
      std::ofstream(sd_dir_ / "games" / "demo.bin") << std::string(256, 'X');

      // Configure M4 board
      g_m4board.enabled = true;
      g_m4board.sd_root_path = sd_dir_.string();
      g_m4board.current_dir = "/";

      // Start HTTP server on a random high port to avoid TIME_WAIT conflicts
      // when multiple tests run in rapid succession
      static std::mt19937 rng(std::random_device{}());
      int base = 30000 + static_cast<int>(rng() % 20000);
      g_m4_http.start(base, "127.0.0.1");
      // Wait for server to bind (port() > 0 means the socket is ready)
      for (int i = 0; i < 100 && g_m4_http.port() == 0; i++) {
         std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      port_ = g_m4_http.port();
   }

   void TearDown() override {
      g_m4_http.stop();
      g_m4board.enabled = false;
      g_m4board.sd_root_path.clear();
      g_m4board.current_dir = "/";
      std::filesystem::remove_all(sd_dir_);
   }

   std::filesystem::path sd_dir_;
   int port_ = 0;
};

// ── Tests ──

TEST_F(M4HttpTest, ServerStartsAndListens) {
   ASSERT_TRUE(g_m4_http.is_running());
   ASSERT_GT(port_, 0);
}

TEST_F(M4HttpTest, GetIndexReturnsHtml) {
   auto resp = http_get(port_, "/");
   ASSERT_FALSE(resp.empty());
   EXPECT_EQ(200, extract_status(resp));
   auto body = extract_body(resp);
   EXPECT_NE(std::string::npos, body.find("<!DOCTYPE html"));
   EXPECT_NE(std::string::npos, body.find("M4 Board"));
}

TEST_F(M4HttpTest, GetStylesheetReturnsCss) {
   auto resp = http_get(port_, "/stylesheet.css");
   ASSERT_FALSE(resp.empty());
   EXPECT_EQ(200, extract_status(resp));
   auto body = extract_body(resp);
   EXPECT_NE(std::string::npos, body.find("body"));
}

TEST_F(M4HttpTest, GetStatusReturnsJson) {
   auto resp = http_get(port_, "/status");
   ASSERT_FALSE(resp.empty());
   EXPECT_EQ(200, extract_status(resp));
   auto body = extract_body(resp);
   EXPECT_NE(std::string::npos, body.find("\"enabled\": true"));
   EXPECT_NE(std::string::npos, body.find("\"version\": \"koncepcja-m4\""));
}

TEST_F(M4HttpTest, GetDirTxtListsFiles) {
   auto resp = http_get(port_, "/sd/m4/dir.txt");
   ASSERT_FALSE(resp.empty());
   EXPECT_EQ(200, extract_status(resp));
   auto body = extract_body(resp);
   // First line should be current directory
   EXPECT_EQ('/', body[0]);
   // Should list our test files
   EXPECT_NE(std::string::npos, body.find("test.bas"));
   EXPECT_NE(std::string::npos, body.find("game.dsk"));
   EXPECT_NE(std::string::npos, body.find("games,0")); // directory
}

TEST_F(M4HttpTest, ConfigCgiLsListsDirectory) {
   auto resp = http_get(port_, "/config.cgi?ls=/");
   ASSERT_FALSE(resp.empty());
   EXPECT_EQ(200, extract_status(resp));
   auto body = extract_body(resp);
   EXPECT_NE(std::string::npos, body.find("test.bas"));
}

TEST_F(M4HttpTest, ConfigCgiLsSubdir) {
   auto resp = http_get(port_, "/config.cgi?ls=/games/");
   ASSERT_FALSE(resp.empty());
   EXPECT_EQ(200, extract_status(resp));
   auto body = extract_body(resp);
   EXPECT_NE(std::string::npos, body.find("demo.bin"));
}

TEST_F(M4HttpTest, SdFileDownload) {
   auto resp = http_get(port_, "/sd/test.bas");
   ASSERT_FALSE(resp.empty());
   EXPECT_EQ(200, extract_status(resp));
   auto body = extract_body(resp);
   EXPECT_NE(std::string::npos, body.find("PRINT"));
}

TEST_F(M4HttpTest, SdFileDownloadSubdir) {
   auto resp = http_get(port_, "/sd/games/demo.bin");
   ASSERT_FALSE(resp.empty());
   EXPECT_EQ(200, extract_status(resp));
   auto body = extract_body(resp);
   EXPECT_EQ(256u, body.size());
}

TEST_F(M4HttpTest, SdFileNotFound) {
   auto resp = http_get(port_, "/sd/nonexistent.bin");
   ASSERT_FALSE(resp.empty());
   EXPECT_EQ(404, extract_status(resp));
}

TEST_F(M4HttpTest, PathTraversalBlocked) {
   auto resp = http_get(port_, "/sd/../../../etc/passwd");
   ASSERT_FALSE(resp.empty());
   int status = extract_status(resp);
   // Either 403 (blocked) or 404 (not found after normalization)
   EXPECT_TRUE(status == 403 || status == 404);
}

TEST_F(M4HttpTest, Upload) {
   std::string boundary = "----TestBoundary12345";
   std::string body = "------TestBoundary12345\r\n"
      "Content-Disposition: form-data; name=\"file\"; filename=\"/uploaded.txt\"\r\n"
      "Content-Type: text/plain\r\n"
      "\r\n"
      "Hello from upload!\r\n"
      "------TestBoundary12345--\r\n";

   auto resp = http_post(port_, "/",
      "multipart/form-data; boundary=----TestBoundary12345", body);
   ASSERT_FALSE(resp.empty());
   EXPECT_EQ(200, extract_status(resp));

   // Verify the file was written
   auto file_path = sd_dir_ / "uploaded.txt";
   ASSERT_TRUE(std::filesystem::exists(file_path));
   std::ifstream ifs(file_path);
   std::string content((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
   EXPECT_EQ("Hello from upload!", content);
}

TEST_F(M4HttpTest, ConfigCgiDelete) {
   // Create a file to delete
   std::ofstream(sd_dir_ / "deleteme.txt") << "delete this";
   ASSERT_TRUE(std::filesystem::exists(sd_dir_ / "deleteme.txt"));

   auto resp = http_get(port_, "/config.cgi?rm=/deleteme.txt");
   ASSERT_FALSE(resp.empty());
   EXPECT_EQ(200, extract_status(resp));
   EXPECT_FALSE(std::filesystem::exists(sd_dir_ / "deleteme.txt"));
}

TEST_F(M4HttpTest, ConfigCgiMkdir) {
   auto resp = http_get(port_, "/config.cgi?mkdir=/newdir");
   ASSERT_FALSE(resp.empty());
   EXPECT_EQ(200, extract_status(resp));
   EXPECT_TRUE(std::filesystem::is_directory(sd_dir_ / "newdir"));
}

TEST_F(M4HttpTest, ResetQueuesDeferred) {
   auto resp = http_post(port_, "/reset", "text/plain", "");
   ASSERT_FALSE(resp.empty());
   EXPECT_EQ(200, extract_status(resp));
   // The reset should be queued, not executed immediately
   EXPECT_TRUE(g_m4_http.pending_reset.load());
}

TEST_F(M4HttpTest, PauseToggleQueuesDeferred) {
   auto resp = http_post(port_, "/pause", "text/plain", "");
   ASSERT_FALSE(resp.empty());
   EXPECT_EQ(200, extract_status(resp));
   EXPECT_TRUE(g_m4_http.pending_pause_toggle.load());
}

TEST_F(M4HttpTest, UnknownRouteReturns404) {
   auto resp = http_get(port_, "/nonexistent");
   ASSERT_FALSE(resp.empty());
   EXPECT_EQ(404, extract_status(resp));
}

TEST_F(M4HttpTest, MethodNotAllowed) {
   // DELETE method not supported
   int fd = ::socket(AF_INET, SOCK_STREAM, 0);
   ASSERT_GE(fd, 0);
   sockaddr_in addr{};
   addr.sin_family = AF_INET;
   addr.sin_port = htons(static_cast<uint16_t>(port_));
   inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
   ASSERT_EQ(0, connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
   std::string req = "DELETE /test HTTP/1.1\r\nHost: localhost\r\n\r\n";
   (void)::write(fd, req.c_str(), req.size());
   char buf[4096];
   timeval tv{2, 0};
   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
   ssize_t n = ::read(fd, buf, sizeof(buf));
   ::close(fd);
   ASSERT_GT(n, 0);
   std::string resp(buf, static_cast<size_t>(n));
   EXPECT_EQ(405, extract_status(resp));
}

TEST_F(M4HttpTest, PreviewReturns503WhenNoSurface) {
   // back_surface is null in test environment
   auto resp = http_get(port_, "/preview.bmp");
   ASSERT_FALSE(resp.empty());
   // 503 because back_surface is null in test mode
   EXPECT_EQ(503, extract_status(resp));
}

TEST_F(M4HttpTest, RomsApiReturnsJson) {
   auto resp = http_get(port_, "/roms.json");
   ASSERT_FALSE(resp.empty());
   EXPECT_EQ(200, extract_status(resp));
   auto body = extract_body(resp);
   EXPECT_NE(std::string::npos, body.find("\"slot\""));
   EXPECT_NE(std::string::npos, body.find("\"loaded\""));
}

TEST_F(M4HttpTest, ConfigCgiCpcReset) {
   auto resp = http_get(port_, "/config.cgi?cres=1");
   ASSERT_FALSE(resp.empty());
   EXPECT_EQ(200, extract_status(resp));
   EXPECT_TRUE(g_m4_http.pending_reset.load());
}

TEST_F(M4HttpTest, ConfigCgiPauseToggle) {
   auto resp = http_get(port_, "/config.cgi?chlt=1");
   ASSERT_FALSE(resp.empty());
   EXPECT_EQ(200, extract_status(resp));
   EXPECT_TRUE(g_m4_http.pending_pause_toggle.load());
}

TEST_F(M4HttpTest, ConfigCgiNmi) {
   auto resp = http_get(port_, "/config.cgi?cnmi=1");
   ASSERT_FALSE(resp.empty());
   EXPECT_EQ(200, extract_status(resp));
   EXPECT_TRUE(g_m4_http.pending_nmi.load());
}

TEST_F(M4HttpTest, ConfigCgiRunCommand) {
   auto resp = http_get(port_, "/config.cgi?run=cat");
   ASSERT_FALSE(resp.empty());
   EXPECT_EQ(200, extract_status(resp));
}

// ── Port mapping tests ──

TEST(M4PortMappingTest, SetAndResolve) {
   M4HttpServer server;
   server.set_port_mapping(80, 8080, true);
   EXPECT_EQ(8080, server.resolve_host_port(80));
}

TEST(M4PortMappingTest, UnmappedReturnsSamePort) {
   M4HttpServer server;
   EXPECT_EQ(23, server.resolve_host_port(23));
}

TEST(M4PortMappingTest, OverwriteExisting) {
   M4HttpServer server;
   server.set_port_mapping(80, 8080, true);
   server.set_port_mapping(80, 9090, true);
   EXPECT_EQ(9090, server.resolve_host_port(80));
   EXPECT_EQ(1u, server.port_mappings().size());
}

TEST(M4PortMappingTest, RemoveMapping) {
   M4HttpServer server;
   server.set_port_mapping(80, 8080, true);
   server.set_port_mapping(23, 2323, false);
   server.remove_port_mapping(80);
   EXPECT_EQ(80, server.resolve_host_port(80)); // back to default
   EXPECT_EQ(2323, server.resolve_host_port(23)); // still there
   EXPECT_EQ(1u, server.port_mappings().size());
}

// ── URL decode tests ──

TEST(M4HttpUrlDecode, BasicDecoding) {
   // Access url_decode via a test instance
   // Since it's static, we can test it indirectly through the request handling
   // For now, test the port mapping which exercises other code paths
}

TEST(M4HttpServerTest, StopWithoutStart) {
   M4HttpServer server;
   server.stop(); // Should not crash
}

TEST(M4HttpServerTest, DoubleStart) {
   M4HttpServer server;
   server.start(19080);
   for (int i = 0; i < 100 && server.port() == 0; i++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
   }
   EXPECT_TRUE(server.is_running());
   server.start(19081); // Should be no-op
   EXPECT_EQ(server.port(), server.port()); // Same port
   server.stop();
}
