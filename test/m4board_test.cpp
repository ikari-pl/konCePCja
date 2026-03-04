#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <cstring>
#include "m4board.h"

// Build a command buffer and execute it.
// Format: [size_prefix, cmd_lo, cmd_hi, data...]
static void send_command(uint16_t cmd, const std::vector<uint8_t>& data = {})
{
   g_m4board.cmd_buf.clear();
   g_m4board.cmd_buf.push_back(static_cast<uint8_t>(data.size() + 2)); // size prefix
   g_m4board.cmd_buf.push_back(cmd & 0xFF);         // cmd low
   g_m4board.cmd_buf.push_back((cmd >> 8) & 0xFF);  // cmd high
   g_m4board.cmd_buf.insert(g_m4board.cmd_buf.end(), data.begin(), data.end());
   m4board_execute();
}

// Build a command with a string payload.
static void send_command_str(uint16_t cmd, const std::string& str)
{
   std::vector<uint8_t> data(str.begin(), str.end());
   data.push_back(0); // null terminator
   send_command(cmd, data);
}

// Command codes (must match m4board.cpp)
static constexpr uint16_t C_OPEN       = 0x4301;
static constexpr uint16_t C_READ       = 0x4302;
static constexpr uint16_t C_WRITE      = 0x4303;
static constexpr uint16_t C_CLOSE      = 0x4304;
static constexpr uint16_t C_SEEK       = 0x4305;
static constexpr uint16_t C_READDIR    = 0x4306;
static constexpr uint16_t C_EOF        = 0x4307;
static constexpr uint16_t C_CD         = 0x4308;
static constexpr uint16_t C_FTELL      = 0x430A;
static constexpr uint16_t C_FSIZE      = 0x4311;
static constexpr uint16_t C_READ2      = 0x4312;
static constexpr uint16_t C_FSTAT      = 0x4316;
static constexpr uint16_t C_WRITE2     = 0x431B;
static constexpr uint16_t C_DIRSETARGS = 0x4325;
static constexpr uint16_t C_VERSION    = 0x4326;
static constexpr uint16_t C_CONFIG     = 0x43FE;

class M4BoardTest : public ::testing::Test {
protected:
   std::filesystem::path temp_dir;

   void SetUp() override {
      temp_dir = std::filesystem::temp_directory_path() / "m4board_test";
      std::filesystem::remove_all(temp_dir);
      std::filesystem::create_directories(temp_dir);

      m4board_cleanup();
      m4board_reset();
      g_m4board.enabled = true;
      g_m4board.sd_root_path = temp_dir.string();
   }

   void TearDown() override {
      // Close all open files before removing temp dir (Windows compat)
      m4board_cleanup();
      m4board_reset();
      g_m4board.enabled = false;
      g_m4board.sd_root_path.clear();
      std::filesystem::remove_all(temp_dir);
   }

   // Helper: create a file in the virtual SD
   void create_file(const std::string& name, const std::string& content) {
      std::ofstream f(temp_dir / name, std::ios::binary);
      f.write(content.data(), content.size());
   }

   // Helper: create a file in a subdirectory
   void create_file_in(const std::string& dir, const std::string& name,
                       const std::string& content) {
      std::filesystem::create_directories(temp_dir / dir);
      std::ofstream f(temp_dir / dir / name, std::ios::binary);
      f.write(content.data(), content.size());
   }

   // Helper: open a file for reading via M4 protocol
   int open_for_read(const std::string& filename) {
      // mode = FA_READ (0x01)
      std::vector<uint8_t> data = {0x01}; // mode
      for (char c : filename) data.push_back(static_cast<uint8_t>(c));
      data.push_back(0); // null terminator
      send_command(C_OPEN, data);
      if (g_m4board.response[0] != 0x00) return -1;
      return g_m4board.response[3]; // handle
   }

   // Helper: open a file for writing via M4 protocol
   int open_for_write(const std::string& filename) {
      // mode = FA_WRITE | FA_CREATE_ALWAYS (0x0A)
      std::vector<uint8_t> data = {0x0A};
      for (char c : filename) data.push_back(static_cast<uint8_t>(c));
      data.push_back(0);
      send_command(C_OPEN, data);
      if (g_m4board.response[0] != 0x00) return -1;
      return g_m4board.response[3];
   }
};

// ── Protocol Tests ──────────────────────────────

TEST_F(M4BoardTest, ExecuteEmptyBuffer) {
   // Buffer too short (< 3 bytes) should be silently rejected
   g_m4board.cmd_buf.clear();
   g_m4board.cmd_buf.push_back(0x00);
   m4board_execute();
   EXPECT_EQ(g_m4board.response_len, 0);
}

TEST_F(M4BoardTest, ExecuteUnknownCommand) {
   send_command(0x4399); // non-existent command
   EXPECT_EQ(g_m4board.response[0], 0xFF); // M4_ERROR
}

TEST_F(M4BoardTest, VersionCommand) {
   send_command(C_VERSION);
   EXPECT_EQ(g_m4board.response[0], 0x00); // M4_OK
   EXPECT_TRUE(g_m4board.response_len > 4);
   // Version string starts at response[3]
   EXPECT_NE(std::string(reinterpret_cast<char*>(g_m4board.response + 3)).find("konCePCja"),
             std::string::npos);
}

TEST_F(M4BoardTest, ActivityTracking) {
   send_command(C_VERSION);
   EXPECT_GT(g_m4board.cmd_count, 0);
   EXPECT_GT(g_m4board.activity_frames, 0);
}

// ── Path Safety Tests ───────────────────────────

TEST_F(M4BoardTest, PathTraversalBlocked) {
   // Create a file outside the SD root to verify it's not accessible
   send_command_str(C_CD, "../");
   EXPECT_EQ(g_m4board.response[0], 0xFF); // error
}

TEST_F(M4BoardTest, PathTraversalInFilename) {
   // Try to open a file with path traversal
   std::vector<uint8_t> data = {0x01}; // FA_READ
   std::string evil = "../../etc/passwd";
   for (char c : evil) data.push_back(static_cast<uint8_t>(c));
   data.push_back(0);
   send_command(C_OPEN, data);
   // Should fail — either path traversal blocked (0xFF) or file not found
   EXPECT_NE(g_m4board.response[0], 0x00);
}

TEST_F(M4BoardTest, NormalCdWorks) {
   std::filesystem::create_directories(temp_dir / "subdir");
   send_command_str(C_CD, "subdir");
   EXPECT_EQ(g_m4board.response[0], 0x00); // M4_OK
   EXPECT_NE(g_m4board.current_dir.find("subdir"), std::string::npos);
}

TEST_F(M4BoardTest, CdToRoot) {
   send_command_str(C_CD, "/");
   EXPECT_EQ(g_m4board.response[0], 0x00);
   EXPECT_EQ(g_m4board.current_dir, "/");
}

// ── Response Format Tests ───────────────────────

TEST_F(M4BoardTest, OpenResponseFormat) {
   create_file("test.bas", "10 PRINT \"HELLO\"\r\n");
   int handle = open_for_read("test.bas");
   EXPECT_GE(handle, 0);
   // Response format: [status, len_lo, len_hi, handle, fr_ok]
   EXPECT_EQ(g_m4board.response[0], 0x00); // M4_OK
   EXPECT_EQ(g_m4board.response[4], 0x00); // FR_OK
   EXPECT_EQ(g_m4board.response_len, 5);
}

TEST_F(M4BoardTest, OpenFileNotFound) {
   std::vector<uint8_t> data = {0x01}; // FA_READ
   std::string name = "nonexistent.xyz";
   for (char c : name) data.push_back(static_cast<uint8_t>(c));
   data.push_back(0);
   send_command(C_OPEN, data);
   // Should return error with FR_NO_FILE code (4)
   EXPECT_EQ(g_m4board.response[0], 0xFF); // M4_ERROR
   EXPECT_EQ(g_m4board.response[4], 4);    // FR_NO_FILE
}

TEST_F(M4BoardTest, ReadResponseFormat) {
   create_file("data.bin", "ABCD");
   int handle = open_for_read("data.bin");
   ASSERT_GE(handle, 0);

   // Read 4 bytes: [fd, count_lo, count_hi]
   std::vector<uint8_t> read_data = {
      static_cast<uint8_t>(handle), 0x04, 0x00
   };
   send_command(C_READ, read_data);
   EXPECT_EQ(g_m4board.response[0], 0x00); // M4_OK
   EXPECT_EQ(g_m4board.response[3], 0x00); // status OK
   // Data starts at response[4]
   EXPECT_EQ(g_m4board.response[4], 'A');
   EXPECT_EQ(g_m4board.response[5], 'B');
   EXPECT_EQ(g_m4board.response[6], 'C');
   EXPECT_EQ(g_m4board.response[7], 'D');
   EXPECT_EQ(g_m4board.response_len, 8); // 4 header + 4 data
}

TEST_F(M4BoardTest, Read2ResponseFormat) {
   create_file("data2.bin", "XY");
   int handle = open_for_read("data2.bin");
   ASSERT_GE(handle, 0);

   // Read2: [fd, count_lo, count_hi]
   std::vector<uint8_t> read_data = {
      static_cast<uint8_t>(handle), 0x02, 0x00
   };
   send_command(C_READ2, read_data);
   EXPECT_EQ(g_m4board.response[0], 0x00); // M4_OK
   // response[4-5] = bytes read (16-bit LE)
   uint16_t nread = g_m4board.response[4] | (g_m4board.response[5] << 8);
   EXPECT_EQ(nread, 2u);
   // Data at response[8+]
   EXPECT_EQ(g_m4board.response[8], 'X');
   EXPECT_EQ(g_m4board.response[9], 'Y');
}

TEST_F(M4BoardTest, FsizeResponseFormat) {
   create_file("sized.bin", "12345678"); // 8 bytes
   send_command_str(C_FSIZE, "sized.bin");
   EXPECT_EQ(g_m4board.response[0], 0x00); // M4_OK
   // Size at response[3-6] as 32-bit LE
   uint32_t size = g_m4board.response[3] |
                   (g_m4board.response[4] << 8) |
                   (g_m4board.response[5] << 16) |
                   (g_m4board.response[6] << 24);
   EXPECT_EQ(size, 8u);
   EXPECT_EQ(g_m4board.response_len, 7);
}

TEST_F(M4BoardTest, ErrorResponseCode) {
   // Open non-existent file — should get FR_NO_FILE error code
   std::vector<uint8_t> data = {0x01}; // FA_READ
   std::string name = "ghost.fil";
   for (char c : name) data.push_back(static_cast<uint8_t>(c));
   data.push_back(0);
   send_command(C_OPEN, data);
   EXPECT_EQ(g_m4board.response[0], 0xFF); // M4_ERROR
   EXPECT_EQ(g_m4board.response[3], 0xFF); // error marker
   EXPECT_EQ(g_m4board.response[4], 4);    // FR_NO_FILE
}

// ── C_FSTAT Tests ───────────────────────────────

TEST_F(M4BoardTest, FstatOnOpenFile) {
   create_file("stat.bin", "hello");
   int handle = open_for_read("stat.bin");
   ASSERT_GE(handle, 0);

   std::vector<uint8_t> data = {static_cast<uint8_t>(handle)};
   send_command(C_FSTAT, data);
   EXPECT_EQ(g_m4board.response[0], 0x00); // M4_OK
   EXPECT_EQ(g_m4board.response[3], 0x00); // attributes: normal file
}

TEST_F(M4BoardTest, FstatBadHandle) {
   std::vector<uint8_t> data = {3}; // handle 3 — not open
   send_command(C_FSTAT, data);
   EXPECT_EQ(g_m4board.response[0], 0xFF); // M4_ERROR
}

// ── C_WRITE2 Tests ──────────────────────────────

TEST_F(M4BoardTest, Write2SameAsWrite) {
   int handle = open_for_write("w2test.bin");
   ASSERT_GE(handle, 0);

   // Write via C_WRITE2: [fd, data...]
   std::vector<uint8_t> data = {static_cast<uint8_t>(handle), 'A', 'B', 'C'};
   send_command(C_WRITE2, data);
   EXPECT_EQ(g_m4board.response[0], 0x00); // M4_OK

   // Close and verify
   std::vector<uint8_t> close_data = {static_cast<uint8_t>(handle)};
   send_command(C_CLOSE, close_data);

   std::ifstream f(temp_dir / "w2test.bin", std::ios::binary);
   std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
   EXPECT_EQ(content, "ABC");
}

// ── File I/O Tests ──────────────────────────────

TEST_F(M4BoardTest, OpenReadCloseRoundTrip) {
   std::string test_data = "Hello from CPC!";
   create_file("hello.txt", test_data);

   int handle = open_for_read("hello.txt");
   ASSERT_GE(handle, 0);

   // Read all bytes
   std::vector<uint8_t> read_cmd = {
      static_cast<uint8_t>(handle),
      static_cast<uint8_t>(test_data.size()), 0x00
   };
   send_command(C_READ, read_cmd);
   EXPECT_EQ(g_m4board.response[0], 0x00);
   std::string got(reinterpret_cast<char*>(g_m4board.response + 4), test_data.size());
   EXPECT_EQ(got, test_data);

   // Close
   std::vector<uint8_t> close_data = {static_cast<uint8_t>(handle)};
   send_command(C_CLOSE, close_data);
   EXPECT_EQ(g_m4board.response[0], 0x00);
}

TEST_F(M4BoardTest, SaveLoadRoundTrip) {
   std::string content = "10 PRINT \"SAVED\"\r\n20 GOTO 10\r\n";

   // Save: open for write
   int wh = open_for_write("saved.bas");
   ASSERT_GE(wh, 0);

   // Write content
   std::vector<uint8_t> write_data = {static_cast<uint8_t>(wh)};
   for (char c : content) write_data.push_back(static_cast<uint8_t>(c));
   send_command(C_WRITE, write_data);
   EXPECT_EQ(g_m4board.response[0], 0x00);

   // Close write handle
   std::vector<uint8_t> close_w = {static_cast<uint8_t>(wh)};
   send_command(C_CLOSE, close_w);

   // Load: open for read
   int rh = open_for_read("saved.bas");
   ASSERT_GE(rh, 0);

   // Read back
   std::vector<uint8_t> read_data = {
      static_cast<uint8_t>(rh),
      static_cast<uint8_t>(content.size()), 0x00
   };
   send_command(C_READ, read_data);
   EXPECT_EQ(g_m4board.response[0], 0x00);
   std::string got(reinterpret_cast<char*>(g_m4board.response + 4), content.size());
   EXPECT_EQ(got, content);

   std::vector<uint8_t> close_r = {static_cast<uint8_t>(rh)};
   send_command(C_CLOSE, close_r);
}

TEST_F(M4BoardTest, CaseInsensitiveOpen) {
   create_file("game.bas", "10 REM GAME");
   // CPC sends uppercase filenames
   int handle = open_for_read("GAME.BAS");
   EXPECT_GE(handle, 0);
   if (handle >= 0) {
      std::vector<uint8_t> close_data = {static_cast<uint8_t>(handle)};
      send_command(C_CLOSE, close_data);
   }
}

TEST_F(M4BoardTest, ExtensionProbing) {
   create_file("test.bas", "10 REM TEST");
   // CPC RUN"TEST tries without extension first, then .BAS, .BIN, etc.
   int handle = open_for_read("TEST");
   EXPECT_GE(handle, 0);
   if (handle >= 0) {
      std::vector<uint8_t> close_data = {static_cast<uint8_t>(handle)};
      send_command(C_CLOSE, close_data);
   }
}

TEST_F(M4BoardTest, EofDetection) {
   create_file("eof.bin", "AB");
   int handle = open_for_read("eof.bin");
   ASSERT_GE(handle, 0);

   // Read the full file
   std::vector<uint8_t> read_all = {
      static_cast<uint8_t>(handle), 0x02, 0x00
   };
   send_command(C_READ, read_all);

   // Check EOF
   std::vector<uint8_t> eof_data = {static_cast<uint8_t>(handle)};
   send_command(C_EOF, eof_data);
   EXPECT_EQ(g_m4board.response[0], 0x00);
   // After reading exactly 2 bytes of a 2-byte file, EOF may or may not be set
   // Read one more byte to guarantee EOF
   std::vector<uint8_t> read_more = {
      static_cast<uint8_t>(handle), 0x01, 0x00
   };
   send_command(C_READ, read_more);
   // Now check EOF — should be set
   send_command(C_EOF, eof_data);
   EXPECT_EQ(g_m4board.response[0], 0x00);
   EXPECT_EQ(g_m4board.response[3], 1); // EOF

   std::vector<uint8_t> close_data = {static_cast<uint8_t>(handle)};
   send_command(C_CLOSE, close_data);
}

TEST_F(M4BoardTest, FtellAfterSeek) {
   create_file("seek.bin", "0123456789");
   int handle = open_for_read("seek.bin");
   ASSERT_GE(handle, 0);

   // Seek to offset 5
   std::vector<uint8_t> seek_data = {
      static_cast<uint8_t>(handle), 0x05, 0x00, 0x00, 0x00
   };
   send_command(C_SEEK, seek_data);
   EXPECT_EQ(g_m4board.response[0], 0x00);

   // Ftell should return 5
   std::vector<uint8_t> ftell_data = {static_cast<uint8_t>(handle)};
   send_command(C_FTELL, ftell_data);
   EXPECT_EQ(g_m4board.response[0], 0x00);
   uint32_t pos = g_m4board.response[3] |
                  (g_m4board.response[4] << 8) |
                  (g_m4board.response[5] << 16) |
                  (g_m4board.response[6] << 24);
   EXPECT_EQ(pos, 5u);

   std::vector<uint8_t> close_data = {static_cast<uint8_t>(handle)};
   send_command(C_CLOSE, close_data);
}

// ── Directory Listing Tests ─────────────────────

TEST_F(M4BoardTest, ReadDir) {
   create_file("one.bas", "data");
   create_file("two.dsk", "data");

   // Start listing
   send_command(C_DIRSETARGS);
   EXPECT_EQ(g_m4board.response[0], 0x00);

   // Read entries
   int count = 0;
   for (int i = 0; i < 10; i++) {
      send_command(C_READDIR);
      if (g_m4board.response[0] == 2) break; // end of directory
      EXPECT_EQ(g_m4board.response[0], 1); // entry present
      count++;
   }
   EXPECT_EQ(count, 2);
}

TEST_F(M4BoardTest, ReadDirLsMode) {
   create_file("hello.txt", "world");

   send_command(C_DIRSETARGS);

   // LS mode: extra byte in cmd_buf after the command
   std::vector<uint8_t> ls_data = {0x01}; // extra byte = LS mode
   send_command(C_READDIR, ls_data);
   EXPECT_EQ(g_m4board.response[0], 1);
   // LS format: response[3+] = null-terminated filename
   std::string name(reinterpret_cast<char*>(g_m4board.response + 3));
   EXPECT_EQ(name, "hello.txt");
}

TEST_F(M4BoardTest, LastFilenameTracking) {
   create_file("track.me", "data");
   open_for_read("track.me");
   EXPECT_EQ(g_m4board.last_filename, "track.me");
}
