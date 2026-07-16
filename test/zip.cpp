#include "zip_archive.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "errors.h"

/*
 * These tests are not really unit tests as they use files that can be found
 * in test/zip/
 * The files content are the following:
 *  - test1.zip
 *     - README.txt
 *     - empty.dsk
 *     - hello.dsk
 */

TEST(Zip, DirOnFileWithNoMatchingEntry) {
  zip::t_zip_info file_infos;
  file_infos.filename = "test/zip/test1.zip";
  file_infos.extensions = ".zzz";

  int rc = zip::dir(&file_infos);

  // Returns ERR_FILE_EMPTY_ZIP although zip file is not really empty,
  // it just has no matching file
  ASSERT_EQ(ERR_FILE_EMPTY_ZIP, rc);
  ASSERT_EQ(0, file_infos.filesOffsets.size());
}

TEST(Zip, DirOnFileWithOneExtensionAndMultipleEntries) {
  zip::t_zip_info file_infos;
  file_infos.filename = "test/zip/test1.zip";
  file_infos.extensions = ".dsk";

  int rc = zip::dir(&file_infos);

  ASSERT_EQ(0, rc);
  ASSERT_EQ(2, file_infos.filesOffsets.size());
  ASSERT_EQ("disk/empty.dsk", file_infos.filesOffsets[0].first);
  ASSERT_EQ(91, file_infos.filesOffsets[0].second);
  ASSERT_EQ("disk/hello.dsk", file_infos.filesOffsets[1].first);
  ASSERT_EQ(1862, file_infos.filesOffsets[1].second);
}

TEST(Zip, DirOnFileWithMultipleExtensions) {
  zip::t_zip_info file_infos;
  file_infos.filename = "test/zip/test1.zip";
  file_infos.extensions = ".dsk.txt";

  int rc = zip::dir(&file_infos);

  ASSERT_EQ(0, rc);
  ASSERT_EQ(3, file_infos.filesOffsets.size());
  ASSERT_EQ("README.txt", file_infos.filesOffsets[0].first);
  ASSERT_EQ("disk/empty.dsk", file_infos.filesOffsets[1].first);
  ASSERT_EQ("disk/hello.dsk", file_infos.filesOffsets[2].first);
}

TEST(Zip, ExtractOnFileWithMultipleEntries) {
  FILE* file;
  // Retrieve offset
  zip::t_zip_info file_infos;
  file_infos.filename = "test/zip/test1.zip";
  file_infos.extensions = ".txt";
  int rc = zip::dir(&file_infos);
  ASSERT_EQ(0, rc);

  rc = zip::extract(file_infos, &file);

  ASSERT_EQ(0, rc);
  ASSERT_NE(nullptr, file);
  char buffer[256];
  size_t r = fread(buffer, 1, 256, file);
  buffer[r] = 0;
  ASSERT_STREQ("This file is a sample zip file used by konCePCja tests.\n",
               buffer);
  ASSERT_EQ(0, fclose(file));
}

// A zip whose End-Of-Central-Directory advertises a central directory larger
// than the 32K scan buffer must NOT overflow that buffer. It used to be read
// with an unchecked `fread(pbGPBuffer[32768], wCentralDirSize, ...)`, so any
// directory > 32K smashed the stack (canary abort / SIGABRT seen loading a
// .zip). The parser now reads it into a right-sized heap buffer; a directory of
// zeros has no matching entries → a clean ERR_FILE_EMPTY_ZIP. The point is that
// zip::dir *returns* rather than aborting.
TEST(Zip, DirDoesNotOverflowOnOversizedCentralDirectory) {
  namespace fs = std::filesystem;
  const fs::path path =
      fs::temp_directory_path() / "koncpc_zip_overflow_test.zip";

  const uint32_t kCentralDirSize = 40000;         // > 32768 (old stack buffer)
  std::vector<uint8_t> buf(kCentralDirSize, 0u);  // central directory region
  // 22-byte End Of Central Directory record at the tail (offset 0, size 40000).
  const uint8_t eocd[22] = {
      0x50, 0x4B, 0x05, 0x06,  // signature
      0x00, 0x00,              // number of this disk
      0x00, 0x00,              // disk with central dir
      0x01, 0x00,              // entries on this disk
      0x01, 0x00,              // total entries = 1
      0x40, 0x9C, 0x00, 0x00,  // central dir size: low word 0x9C40 = 40000
      0x00, 0x00, 0x00, 0x00,  // central dir offset = 0
      0x00, 0x00};             // comment length = 0
  buf.insert(buf.end(), eocd, eocd + sizeof(eocd));
  {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(buf.data()),
              static_cast<std::streamsize>(buf.size()));
  }

  zip::t_zip_info info;
  info.filename = path.string();
  info.extensions = ".dsk";

  const int rc = zip::dir(&info);  // must return, not abort
  EXPECT_EQ(ERR_FILE_EMPTY_ZIP, rc);
  EXPECT_TRUE(info.filesOffsets.empty());

  std::error_code ec;
  fs::remove(path, ec);
}

// The clean-room extractor also handles STORED (method 0) entries, which the
// old inflate-only path could not. Synthesize a one-entry stored zip in-test.
TEST(Zip, ExtractsAStoredEntry) {
  namespace fs = std::filesystem;
  const fs::path path = fs::temp_directory_path() / "koncpc_zip_stored.zip";
  const std::string payload = "stored payload";
  const std::string name = "file.dsk";
  std::vector<uint8_t> z;
  auto le16 = [&](uint16_t v) { z.push_back(v & 0xFF); z.push_back(v >> 8); };
  auto le32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) z.push_back((v >> (8 * i)) & 0xFF); };
  // Local header (PK\3\4): method 0, sizes = payload, then name + data.
  le32(0x04034b50); le16(20); le16(0); le16(0); le16(0); le16(0);
  le32(0);  // crc (unchecked by the loader)
  le32(payload.size()); le32(payload.size()); le16(name.size()); le16(0);
  z.insert(z.end(), name.begin(), name.end());
  z.insert(z.end(), payload.begin(), payload.end());
  const uint32_t cd_off = z.size();
  // Central directory entry (PK\1\2) pointing at local offset 0.
  le32(0x02014b50); le16(20); le16(20); le16(0); le16(0); le16(0); le16(0);
  le32(0); le32(payload.size()); le32(payload.size());
  le16(name.size()); le16(0); le16(0); le16(0); le16(0); le32(0); le32(0);
  z.insert(z.end(), name.begin(), name.end());
  const uint32_t cd_size = z.size() - cd_off;
  // EOCD (PK\5\6).
  le32(0x06054b50); le16(0); le16(0); le16(1); le16(1);
  le32(cd_size); le32(cd_off); le16(0);
  {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(z.data()),
              static_cast<std::streamsize>(z.size()));
  }

  zip::t_zip_info info;
  info.filename = path.string();
  info.extensions = ".dsk";
  ASSERT_EQ(0, zip::dir(&info));
  ASSERT_EQ(1u, info.filesOffsets.size());

  FILE* out = nullptr;
  ASSERT_EQ(0, zip::extract(info, &out));
  ASSERT_NE(nullptr, out);
  char buf[64] = {};
  const size_t got = fread(buf, 1, sizeof(buf), out);
  EXPECT_EQ(payload, std::string(buf, got));
  fclose(out);
  std::error_code ec;
  fs::remove(path, ec);
}
