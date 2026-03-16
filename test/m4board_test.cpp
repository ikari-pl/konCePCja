#include <gtest/gtest.h>
#include <algorithm>
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
static constexpr uint16_t C_CONFIG      = 0x43FE;

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

// ── DSK Container Tests ────────────────────────
// Helper: create a minimal valid DATA-format DSK with one file.
// The DSK has 1 track, 1 side, 9 sectors (C1-C9), 512 bytes each.
// Sectors C1-C2 = directory (block 0), sectors C3-C4 = directory (block 1),
// sectors C5-C6 = data (block 2) = first 1K of file data.
static void create_test_dsk(const std::filesystem::path& path,
                            const std::string& cpc_name, // e.g. "HELLO   .BAS"
                            const std::vector<uint8_t>& file_data)
{
   // Build a "MV - CPCEMU" normal DSK with 1 track, 1 side
   const int TRACKS = 1, SIDES = 1, SECTORS = 9, SECTOR_SIZE = 512;
   const int TRACK_DATA_SIZE = SECTORS * SECTOR_SIZE; // 4608

   // DSK header (256 bytes)
   uint8_t dsk_header[256] = {};
   memcpy(dsk_header, "MV - CPCEMU Disk-File\r\nDisk-Info\r\n", 34);
   dsk_header[0x30] = static_cast<uint8_t>(TRACKS);
   dsk_header[0x31] = static_cast<uint8_t>(SIDES);
   uint16_t track_total = static_cast<uint16_t>(0x100 + TRACK_DATA_SIZE);
   dsk_header[0x32] = track_total & 0xFF;
   dsk_header[0x33] = (track_total >> 8) & 0xFF;

   // Track header (256 bytes)
   uint8_t track_header[256] = {};
   memcpy(track_header, "Track-Info\r\n", 12);
   track_header[0x10] = 0; // track number
   track_header[0x11] = 0; // side number
   track_header[0x14] = 2; // sector size code (2 = 512 bytes)
   track_header[0x15] = static_cast<uint8_t>(SECTORS);
   track_header[0x16] = 0x4E; // GAP3
   track_header[0x17] = 0xE5; // filler
   // Sector info table: 8 bytes per sector
   for (int s = 0; s < SECTORS; s++) {
      uint8_t* si = track_header + 0x18 + s * 8;
      si[0] = 0;    // C (cylinder)
      si[1] = 0;    // H (head)
      si[2] = static_cast<uint8_t>(0xC1 + s); // R (sector ID)
      si[3] = 2;    // N (size code)
   }

   // Track data (9 sectors * 512 bytes = 4608 bytes)
   uint8_t track_data[SECTORS * SECTOR_SIZE];
   memset(track_data, 0xE5, sizeof(track_data)); // fill with empty marker

   // Build CP/M directory entry in sector C1 (first 32 bytes of block 0)
   // Entry: [user=0, name(8), ext(3), extent_lo, s1, s2, RC, block_alloc(16)]
   uint8_t* dir_entry = track_data; // sector C1 offset 0
   dir_entry[0] = 0; // user 0
   // Copy 8.3 name (must be 11 chars: "HELLO   BAS")
   if (cpc_name.size() >= 11) {
      // Remove dot: "HELLO   .BAS" → "HELLO   BAS"
      for (int i = 0; i < 8; i++) dir_entry[1 + i] = static_cast<uint8_t>(cpc_name[i]);
      // Skip the dot at position 8
      int ext_start = (cpc_name[8] == '.') ? 9 : 8;
      for (int i = 0; i < 3 && ext_start + i < static_cast<int>(cpc_name.size()); i++)
         dir_entry[9 + i] = static_cast<uint8_t>(cpc_name[ext_start + i]);
   }
   // Extent 0
   dir_entry[12] = 0; // extent low
   dir_entry[13] = 0; // S1
   dir_entry[14] = 0; // S2
   // RC = records used = ceil(file_data.size() / 128)
   int records = static_cast<int>((file_data.size() + 127) / 128);
   if (records > 128) records = 128;
   dir_entry[15] = static_cast<uint8_t>(records);
   // Block allocation: file data starts at block 2 (blocks 0-1 = directory)
   int blocks_needed = static_cast<int>((file_data.size() + 1023) / 1024);
   for (int b = 0; b < blocks_needed && b < 16; b++) {
      dir_entry[16 + b] = static_cast<uint8_t>(2 + b);
   }

   // Write file data into block 2 (sectors C5-C6)
   // Block 2 = sectors 4-5 (0-indexed) = sector IDs C5-C6
   size_t data_offset = 4 * SECTOR_SIZE; // sector C5 starts here
   size_t copy_len = std::min(file_data.size(), static_cast<size_t>(SECTOR_SIZE * 2));
   if (!file_data.empty()) {
      memcpy(track_data + data_offset, file_data.data(), copy_len);
   }

   // Write the DSK file
   std::ofstream f(path, std::ios::binary);
   f.write(reinterpret_cast<char*>(dsk_header), sizeof(dsk_header));
   f.write(reinterpret_cast<char*>(track_header), sizeof(track_header));
   f.write(reinterpret_cast<char*>(track_data), sizeof(track_data));
}

TEST_F(M4BoardTest, CdIntoDsk) {
   std::vector<uint8_t> content = {'H', 'E', 'L', 'L', 'O'};
   create_test_dsk(temp_dir / "game.dsk", "HELLO   .BAS", content);

   // cd into the DSK
   send_command_str(C_CD, "game.dsk");
   EXPECT_EQ(g_m4board.response[0], 0x00) << "cd into DSK should succeed";
   EXPECT_EQ(g_m4board.container_type, M4Board::ContainerType::DSK);
   EXPECT_NE(g_m4board.container_drive, nullptr);
   EXPECT_TRUE(g_m4board.current_dir.find("game.dsk") != std::string::npos);
}

TEST_F(M4BoardTest, CdDotDotExitsContainer) {
   std::vector<uint8_t> content = {'H', 'I'};
   create_test_dsk(temp_dir / "test.dsk", "DATA    .BIN", content);

   std::string orig_dir = g_m4board.current_dir;
   send_command_str(C_CD, "test.dsk");
   ASSERT_EQ(g_m4board.container_type, M4Board::ContainerType::DSK);

   // cd ".." should exit
   send_command_str(C_CD, "..");
   EXPECT_EQ(g_m4board.response[0], 0x00);
   EXPECT_EQ(g_m4board.container_type, M4Board::ContainerType::NONE);
   EXPECT_EQ(g_m4board.container_drive, nullptr);
   EXPECT_EQ(g_m4board.current_dir, orig_dir);
}

TEST_F(M4BoardTest, CdSlashExitsContainer) {
   std::vector<uint8_t> content = {'X'};
   create_test_dsk(temp_dir / "test.dsk", "FILE    .   ", content);

   send_command_str(C_CD, "test.dsk");
   ASSERT_EQ(g_m4board.container_type, M4Board::ContainerType::DSK);

   // cd "/" should exit container and go to root
   send_command_str(C_CD, "/");
   EXPECT_EQ(g_m4board.response[0], 0x00);
   EXPECT_EQ(g_m4board.container_type, M4Board::ContainerType::NONE);
   EXPECT_EQ(g_m4board.current_dir, "/");
}

TEST_F(M4BoardTest, DirInsideDsk) {
   std::vector<uint8_t> content = {'D', 'A', 'T', 'A'};
   create_test_dsk(temp_dir / "files.dsk", "HELLO   .BAS", content);

   send_command_str(C_CD, "files.dsk");
   ASSERT_EQ(g_m4board.container_type, M4Board::ContainerType::DSK);

   // DIRSETARGS + READDIR should list the file
   send_command(C_DIRSETARGS);
   // Read first entry (LS mode)
   std::vector<uint8_t> ls_data = {0x01};
   send_command(C_READDIR, ls_data);
   EXPECT_EQ(g_m4board.response[0], 1) << "Should have an entry";
   std::string name(reinterpret_cast<char*>(g_m4board.response + 3));
   EXPECT_EQ(name, "HELLO.BAS");

   // Second READDIR should signal end
   send_command(C_READDIR, ls_data);
   EXPECT_EQ(g_m4board.response[0], 2) << "Should be end of directory";
}

TEST_F(M4BoardTest, OpenFileInsideDsk) {
   std::vector<uint8_t> content = {'C', 'P', 'C', '!', '!'};
   create_test_dsk(temp_dir / "run.dsk", "GAME    .BIN", content);

   send_command_str(C_CD, "run.dsk");
   ASSERT_EQ(g_m4board.container_type, M4Board::ContainerType::DSK);

   // Open the file
   int handle = open_for_read("GAME.BIN");
   ASSERT_GE(handle, 0) << "Should open file from container";

   // Read the data
   std::vector<uint8_t> read_cmd = {
      static_cast<uint8_t>(handle),
      0x00, 0x06  // count = 0x0600 (1536 bytes, more than our 5)
   };
   send_command(C_READ, read_cmd);
   EXPECT_EQ(g_m4board.response[0], 0x00);
   // Verify the first 5 bytes match what we wrote
   EXPECT_EQ(g_m4board.response[4], 'C');
   EXPECT_EQ(g_m4board.response[5], 'P');
   EXPECT_EQ(g_m4board.response[6], 'C');
   EXPECT_EQ(g_m4board.response[7], '!');
   EXPECT_EQ(g_m4board.response[8], '!');
}

TEST_F(M4BoardTest, WriteBlockedInContainer) {
   std::vector<uint8_t> content = {'X'};
   create_test_dsk(temp_dir / "ro.dsk", "FILE    .BAS", content);

   send_command_str(C_CD, "ro.dsk");
   ASSERT_EQ(g_m4board.container_type, M4Board::ContainerType::DSK);

   // Attempt to open for write — should fail
   int handle = open_for_write("NEWFILE.BAS");
   EXPECT_EQ(handle, -1) << "Write should be denied in container";
}

TEST_F(M4BoardTest, FsizeInsideDsk) {
   std::vector<uint8_t> content(256, 0xAA);
   create_test_dsk(temp_dir / "sz.dsk", "BIG     .DAT", content);

   send_command_str(C_CD, "sz.dsk");
   ASSERT_EQ(g_m4board.container_type, M4Board::ContainerType::DSK);

   // FSIZE should return the file size
   send_command_str(C_FSIZE, "BIG.DAT");
   EXPECT_EQ(g_m4board.response[0], 0x00);
   uint32_t size = g_m4board.response[3] |
                   (g_m4board.response[4] << 8) |
                   (g_m4board.response[5] << 16) |
                   (g_m4board.response[6] << 24);
   // CP/M reports size in records * 128 — 256 bytes = 2 records = 256 bytes
   EXPECT_EQ(size, 256u);
}

TEST_F(M4BoardTest, CdIntoNonexistentDsk) {
   send_command_str(C_CD, "ghost.dsk");
   EXPECT_EQ(g_m4board.response[0], 0xFF) << "Should fail for missing file";
   EXPECT_EQ(g_m4board.container_type, M4Board::ContainerType::NONE);
}

TEST_F(M4BoardTest, CdIntoCorruptDsk) {
   // Create a file with .dsk extension but garbage content
   create_file("bad.dsk", "This is not a valid DSK file at all");
   send_command_str(C_CD, "bad.dsk");
   EXPECT_EQ(g_m4board.response[0], 0xFF) << "Should fail for corrupt DSK";
   EXPECT_EQ(g_m4board.container_type, M4Board::ContainerType::NONE);
}

TEST_F(M4BoardTest, ResetExitsContainer) {
   std::vector<uint8_t> content = {'X'};
   create_test_dsk(temp_dir / "rst.dsk", "FILE    .BAS", content);

   send_command_str(C_CD, "rst.dsk");
   ASSERT_EQ(g_m4board.container_type, M4Board::ContainerType::DSK);

   m4board_cleanup();
   m4board_reset();
   // Re-set test state
   g_m4board.enabled = true;
   g_m4board.sd_root_path = temp_dir.string();
   EXPECT_EQ(g_m4board.container_type, M4Board::ContainerType::NONE);
   EXPECT_EQ(g_m4board.container_drive, nullptr);
}

TEST_F(M4BoardTest, ReadSectorInsideDsk) {
   // Create DSK with known data in sector C5 (block 2)
   std::vector<uint8_t> content(512, 0x42); // 512 bytes of 0x42
   create_test_dsk(temp_dir / "sec.dsk", "SECT    .BIN", content);

   send_command_str(C_CD, "sec.dsk");
   ASSERT_EQ(g_m4board.container_type, M4Board::ContainerType::DSK);

   // READSECTOR: track=0, sector=C5 (block 2, first sector)
   std::vector<uint8_t> cmd = {0, 0xC5, 0}; // track, sector, drive
   send_command(0x430B, cmd); // C_READSECTOR
   EXPECT_EQ(g_m4board.response[0], 0x00);
   EXPECT_EQ(g_m4board.response[3], 0x00); // status OK
   // Sector C5 should contain our 0x42 data
   EXPECT_EQ(g_m4board.response[4], 0x42);
   EXPECT_EQ(g_m4board.response[4 + 511], 0x42);
}

// ── Tests for newly implemented commands ──────────

TEST_F(M4BoardTest, WriteSectorToOpenFile) {
   // Create a small "disk image" file
   std::string path = (temp_dir / "disk.img").string();
   FILE* f = fopen(path.c_str(), "wb");
   ASSERT_NE(f, nullptr);
   std::vector<uint8_t> blank(9 * 512, 0xE5); // 1 track, 9 sectors
   fwrite(blank.data(), 1, blank.size(), f);
   fclose(f);

   // Open for writing (handle 2)
   std::vector<uint8_t> open_data = {0x0A}; // FA_WRITE | FA_CREATE_ALWAYS
   std::string fname = "disk.img";
   open_data.insert(open_data.end(), fname.begin(), fname.end());
   open_data.push_back(0);
   send_command(C_OPEN, open_data);
   ASSERT_EQ(g_m4board.response[4], 0x00); // FR_OK (response[3] = fd)

   // Write sector: track=0, sector=0xC1, drive=0, 512 bytes of 0xAA
   std::vector<uint8_t> ws_data = {0, 0xC1, 0};
   ws_data.insert(ws_data.end(), 512, 0xAA);
   send_command(C_WRITESECTOR, ws_data);
   EXPECT_EQ(g_m4board.response[3], 0x00); // OK

   // Read back via READSECTOR to verify
   // First close and reopen for reading
   send_command(C_CLOSE, {2});
   std::vector<uint8_t> open_read = {0x01}; // FA_READ
   open_read.insert(open_read.end(), fname.begin(), fname.end());
   open_read.push_back(0);
   send_command(C_OPEN, open_read);

   std::vector<uint8_t> rs_data = {0, 0xC1, 0}; // track, sector, drive
   send_command(0x430B, rs_data); // C_READSECTOR
   EXPECT_EQ(g_m4board.response[3], 0x00);
   EXPECT_EQ(g_m4board.response[4], 0xAA);
}

TEST_F(M4BoardTest, WriteSectorBlockedInContainer) {
   create_test_dsk(temp_dir / "ws.dsk", "FILE    .BIN", {0x01});
   send_command_str(C_CD, "ws.dsk");
   ASSERT_EQ(g_m4board.container_type, M4Board::ContainerType::DSK);

   std::vector<uint8_t> ws_data = {0, 0xC1, 0};
   ws_data.insert(ws_data.end(), 512, 0x00);
   send_command(C_WRITESECTOR, ws_data);
   EXPECT_EQ(g_m4board.response[0], 0xFF); // error
}

TEST_F(M4BoardTest, FormatTrackReturnsError) {
   send_command(C_FORMATTRACK, {});
   EXPECT_EQ(g_m4board.response[0], 0xFF); // not supported
}

TEST_F(M4BoardTest, SdReadReturnsError) {
   // Protocol: lba0-3, num_sectors
   send_command(C_SDREAD, {0, 0, 0, 0, 1});
   EXPECT_EQ(g_m4board.response[3], 1); // error flag
}

TEST_F(M4BoardTest, SdWriteReturnsError) {
   send_command(C_SDWRITE, {0, 0, 0, 0, 1});
   EXPECT_EQ(g_m4board.response[3], 1); // error flag
}

TEST_F(M4BoardTest, SetNetworkAcknowledges) {
   send_command_str(C_SETNETWORK, "ssid:password");
   EXPECT_EQ(g_m4board.response[0], 0x00); // OK
}

TEST_F(M4BoardTest, NetstatReportsHostNetwork) {
   send_command(C_NETSTAT, {});
   EXPECT_EQ(g_m4board.response[0], 0x00); // OK

   // Response layout: [3: string...] [3+len: \0] [3+len+1: status_byte]
   // Find the null terminator
   size_t i = 3;
   while (i < static_cast<size_t>(g_m4board.response_len) && g_m4board.response[i] != 0) i++;
   ASSERT_LT(i, static_cast<size_t>(g_m4board.response_len))
      << "Response string should be null-terminated";
   EXPECT_EQ(g_m4board.response[i], 0); // null terminator

   // Status byte is after the null
   ASSERT_LT(i + 1, static_cast<size_t>(M4Board::RESPONSE_SIZE))
      << "Status byte must be within response buffer";
   uint8_t status = g_m4board.response[i + 1];
   EXPECT_TRUE(status == 0 || status == 5)
      << "Expected status 0 (disconnected) or 5 (connected), got " << (int)status;

   // If connected, the string should contain "IP:"
   if (status == 5) {
      std::string msg(reinterpret_cast<char*>(g_m4board.response + 3));
      EXPECT_NE(msg.find("IP:"), std::string::npos)
         << "Connected status should include IP address, got: " << msg;
   }
}

TEST_F(M4BoardTest, TimeReturnsFormattedString) {
   send_command(C_TIME, {});
   EXPECT_EQ(g_m4board.response[0], 0x00); // OK
   // Should contain ":" (time) and "-" (date)
   std::string time_str(reinterpret_cast<char*>(g_m4board.response + 3));
   EXPECT_NE(time_str.find(':'), std::string::npos);
   EXPECT_NE(time_str.find('-'), std::string::npos);
   // Format: "hh:mm:ss yyyy-mm-dd" = 19 chars
   EXPECT_GE(time_str.size(), 19u);
}

TEST_F(M4BoardTest, M4OffDisablesBoard) {
   ASSERT_TRUE(g_m4board.enabled);
   send_command(C_M4OFF, {});
   EXPECT_EQ(g_m4board.response[0], 0x00); // OK
   EXPECT_FALSE(g_m4board.enabled);
   // Re-enable for TearDown
   g_m4board.enabled = true;
}

TEST_F(M4BoardTest, WifiDisableBlocksNetstat) {
   g_m4board.network_enabled = false;
   send_command(C_NETSTAT, {});
   EXPECT_EQ(g_m4board.response[0], 0x00);
   // Should report "WiFi disabled" and status byte 0 (disconnected)
   const char* msg = reinterpret_cast<const char*>(g_m4board.response + 3);
   EXPECT_NE(std::string(msg).find("disabled"), std::string::npos);
   // Find null terminator and check status byte after it
   size_t len = strlen(msg);
   EXPECT_EQ(g_m4board.response[3 + len], 0);       // null terminator
   EXPECT_EQ(g_m4board.response[3 + len + 1], 0);    // status: disconnected
   g_m4board.network_enabled = true;
}

TEST_F(M4BoardTest, WifiDisableBlocksSocket) {
   g_m4board.network_enabled = false;
   send_command(C_NETSOCKET, {2, 1, 0});
   EXPECT_EQ(g_m4board.response[0], 0xFF); // error
   g_m4board.network_enabled = true;
}

TEST_F(M4BoardTest, WifiReenableRestoresNetwork) {
   g_m4board.network_enabled = false;
   send_command(C_NETSOCKET, {2, 1, 0});
   EXPECT_EQ(g_m4board.response[0], 0xFF); // error while disabled
   g_m4board.network_enabled = true;
   send_command(C_NETSOCKET, {2, 1, 0});
   EXPECT_EQ(g_m4board.response[0], 0x00); // OK when re-enabled
   uint8_t slot = g_m4board.response[3];
   if (slot != 0xFF) send_command(C_NETCLOSE, {slot});
}

TEST_F(M4BoardTest, NetSocketCreatesRealSocket) {
   // domain=2 (AF_INET), type=1 (SOCK_STREAM), protocol=0
   send_command(C_NETSOCKET, {2, 1, 0});
   EXPECT_EQ(g_m4board.response[0], 0x00); // OK
   uint8_t slot = g_m4board.response[3];
   EXPECT_NE(slot, 0xFF) << "Should create a real socket";
   EXPECT_LT(slot, M4Board::MAX_SOCKETS);
   EXPECT_NE(g_m4board.sockets[slot], M4Board::INVALID_SOCK);

   // Clean up
   send_command(C_NETCLOSE, {slot});
   EXPECT_EQ(g_m4board.sockets[slot], M4Board::INVALID_SOCK);
}

TEST_F(M4BoardTest, NetSocketExhaustsSlots) {
   // Fill all 4 slots
   for (int i = 0; i < M4Board::MAX_SOCKETS; i++) {
      send_command(C_NETSOCKET, {2, 1, 0});
      EXPECT_NE(g_m4board.response[3], 0xFF);
   }
   // 5th should fail
   send_command(C_NETSOCKET, {2, 1, 0});
   EXPECT_EQ(g_m4board.response[3], 0xFF);

   // Clean up all
   for (int i = 0; i < M4Board::MAX_SOCKETS; i++)
      send_command(C_NETCLOSE, {static_cast<uint8_t>(i)});
}

TEST_F(M4BoardTest, NetConnectNonBlocking) {
   // Non-blocking connect to localhost:1 returns OK (EINPROGRESS) or error
   // — either is valid, the key thing is we don't block the thread.
   send_command(C_NETSOCKET, {2, 1, 0});
   uint8_t slot = g_m4board.response[3];
   ASSERT_NE(slot, 0xFF);

   // Connect to 127.0.0.1 port 1 — with non-blocking socket, this
   // returns 0x00 (EINPROGRESS) or 0xFF (immediate refusal)
   send_command(C_NETCONNECT, {slot, 127, 0, 0, 1, 0, 1});
   uint8_t result = g_m4board.response[3];
   EXPECT_TRUE(result == 0x00 || result == 0xFF)
      << "Expected OK (in progress) or error, got " << (int)result;

   send_command(C_NETCLOSE, {slot});
}

TEST_F(M4BoardTest, NetCloseInvalidSlot) {
   // Closing a non-existent slot should not crash
   send_command(C_NETCLOSE, {99});
   EXPECT_EQ(g_m4board.response[0], 0x00); // OK (no-op)
}

TEST_F(M4BoardTest, NetSendWithoutConnect) {
   send_command(C_NETSOCKET, {2, 1, 0});
   uint8_t slot = g_m4board.response[3];
   ASSERT_NE(slot, 0xFF);

   // Send on unconnected socket — should fail
   send_command(C_NETSEND, {slot, 5, 0, 'H', 'e', 'l', 'l', 'o'});
   EXPECT_EQ(g_m4board.response[3], 0xFF); // error

   send_command(C_NETCLOSE, {slot});
}

TEST_F(M4BoardTest, NetRecvNoData) {
   send_command(C_NETSOCKET, {2, 1, 0});
   uint8_t slot = g_m4board.response[3];
   ASSERT_NE(slot, 0xFF);

   // Recv on unconnected socket — returns 0 bytes (non-blocking)
   send_command(C_NETRECV, {slot, 0xFF, 0x00});
   // Either error or 0 bytes is acceptable for unconnected socket
   EXPECT_EQ(g_m4board.response[4], 0); // actual_lo = 0
   EXPECT_EQ(g_m4board.response[5], 0); // actual_hi = 0

   send_command(C_NETCLOSE, {slot});
}

TEST_F(M4BoardTest, NetHostIpResolvesHostname) {
   // "localhost" should always resolve to 127.0.0.1
   send_command_str(C_NETHOSTIP, "localhost");
   // response[3]=0 means resolved, response[3]=1 means lookup in progress (failed)
   if (g_m4board.response[3] == 0) {
      // Resolved: check IP bytes at [4..7]
      EXPECT_EQ(g_m4board.response[4], 127);
      EXPECT_EQ(g_m4board.response[5], 0);
      EXPECT_EQ(g_m4board.response[6], 0);
      EXPECT_EQ(g_m4board.response[7], 1);
      EXPECT_EQ(g_m4board.response_len, 8);
   }
   // If DNS is unavailable, response[3]=1 is also acceptable
}

TEST_F(M4BoardTest, RomsUpdateOk) {
   send_command(C_ROMSUPDATE, {});
   EXPECT_EQ(g_m4board.response[0], 0x00); // OK
}

TEST_F(M4BoardTest, ActivityTracksNewCommands) {
   g_m4board.activity_frames = 0;
   send_command(C_TIME, {});
   EXPECT_GT(g_m4board.activity_frames, 0);
   EXPECT_EQ(g_m4board.last_op, M4Board::LastOp::CMD);

   send_command(C_SDREAD, {0, 0, 0, 0, 1});
   EXPECT_EQ(g_m4board.last_op, M4Board::LastOp::READ);

   std::vector<uint8_t> ws_data = {0, 0xC1, 0};
   ws_data.insert(ws_data.end(), 512, 0x00);
   // Need a file open for write handle — just test SDWRITE tracking
   send_command(C_SDWRITE, {0, 0, 0, 0, 1});
   EXPECT_EQ(g_m4board.last_op, M4Board::LastOp::WRITE);
}

// ── New M4 commands (v1.0.9+ / v2.0.5+) ──

static constexpr uint16_t C_NMI         = 0x431D;
static constexpr uint16_t C_UPGRADE     = 0x4327;
static constexpr uint16_t C_COPYBUF     = 0x4329;
static constexpr uint16_t C_COPYFILE    = 0x432A;
static constexpr uint16_t C_NETRSSI     = 0x4337;
static constexpr uint16_t C_NETBIND     = 0x4338;
static constexpr uint16_t C_NETLISTEN   = 0x4339;
static constexpr uint16_t C_NETACCEPT   = 0x433A;
static constexpr uint16_t C_GETNETWORK  = 0x433B;
static constexpr uint16_t C_WIFIPOW     = 0x433C;
static constexpr uint16_t C_ROMLOW      = 0x433D;
static constexpr uint16_t C_ROMCP       = 0x43FC;

TEST_F(M4BoardTest, NmiReturnsOk)
{
   send_command(C_NMI);
   EXPECT_EQ(g_m4board.response[0], 0x00); // M4_OK
}

TEST_F(M4BoardTest, WifiPowerToggle)
{
   g_m4board.network_enabled = true;
   send_command(C_WIFIPOW, {0}); // OFF
   EXPECT_FALSE(g_m4board.network_enabled);
   send_command(C_WIFIPOW, {1}); // ON
   EXPECT_TRUE(g_m4board.network_enabled);
}

TEST_F(M4BoardTest, UpgradeNoOp)
{
   send_command(C_UPGRADE);
   EXPECT_EQ(g_m4board.response[0], 0x00);
}

TEST_F(M4BoardTest, NetRssiReturnsMaxSignal)
{
   g_m4board.network_enabled = true;
   send_command(C_NETRSSI);
   EXPECT_EQ(g_m4board.response[0], 0x00);
   EXPECT_EQ(g_m4board.response[3], 0); // 0 = max signal
}

TEST_F(M4BoardTest, GetNetworkReturnsStruct)
{
   g_m4board.network_enabled = true;
   send_command(C_GETNETWORK);
   EXPECT_EQ(g_m4board.response[0], 0x00);
   EXPECT_GE(g_m4board.response_len, 3 + 140);
   // Check device name starts with "koncepcja"
   const char* name = reinterpret_cast<const char*>(g_m4board.response + 3);
   EXPECT_EQ(std::string(name).substr(0, 9), "koncepcja");
   // Check IP is 127.0.0.1
   EXPECT_EQ(g_m4board.response[3 + 112], 127);
   EXPECT_EQ(g_m4board.response[3 + 115], 1);
}

TEST_F(M4BoardTest, CopyFileSuccess)
{
   // Create source file
   {
      std::ofstream f(temp_dir / "source.txt");
      f << "copy me";
   }
   // Build command: "source.txt\0dest.txt\0"
   std::vector<uint8_t> data;
   for (char c : std::string("source.txt")) data.push_back(static_cast<uint8_t>(c));
   data.push_back(0);
   for (char c : std::string("dest.txt")) data.push_back(static_cast<uint8_t>(c));
   data.push_back(0);
   send_command(C_COPYFILE, data);
   EXPECT_EQ(g_m4board.response[0], 0x00);
   EXPECT_EQ(g_m4board.response[3], 0); // success
   // Verify dest file exists with same content
   std::ifstream ifs(temp_dir / "dest.txt");
   ASSERT_TRUE(ifs.good());
   std::string content((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
   EXPECT_EQ(content, "copy me");
}

TEST_F(M4BoardTest, CopyFileMissingSource)
{
   std::vector<uint8_t> data;
   for (char c : std::string("nonexistent.txt")) data.push_back(static_cast<uint8_t>(c));
   data.push_back(0);
   for (char c : std::string("dest.txt")) data.push_back(static_cast<uint8_t>(c));
   data.push_back(0);
   send_command(C_COPYFILE, data);
   EXPECT_EQ(g_m4board.response[0], 0xFF); // error
}

TEST_F(M4BoardTest, CopybufEmptyBuffer)
{
   // COPYBUF with no prior HTTPGETMEM — should return empty/zero
   send_command(C_COPYBUF, {0, 0, 0, 16}); // offset=0, size=16
   EXPECT_EQ(g_m4board.response[0], 0x00);
}

TEST_F(M4BoardTest, RomlowNoOp)
{
   send_command(C_ROMLOW, {0}); // mode 0 = system lower ROM
   EXPECT_EQ(g_m4board.response[0], 0x00);
   send_command(C_ROMLOW, {1}); // mode 1 = ROM board
   EXPECT_EQ(g_m4board.response[0], 0x00);
   send_command(C_ROMLOW, {2}); // mode 2 = hack menu
   EXPECT_EQ(g_m4board.response[0], 0x00);
}

TEST_F(M4BoardTest, RomcpWithoutRomReturnsError)
{
   // No ROM loaded — should return error
   send_command(C_ROMCP, {0, 0, 0, 0, 0x10, 0, 0, 0x10});
   EXPECT_EQ(g_m4board.response[0], 0xFF); // error
}
