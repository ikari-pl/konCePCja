/* zip_archive — see zip_archive.h. Authored from the PKWARE APPNOTE
 * (sections 4.3.6/4.3.7/4.3.12/4.3.16): scan back to the End Of Central
 * Directory record, walk the central directory for extension matches, and
 * inflate (or copy, for stored entries) from the local header on extract. */

#include "zip_archive.h"

#ifdef _MSC_VER
#include "compat/msvc_compat.h"
#else
#include <strings.h>
#endif

#ifdef _WIN32
#include <windows.h>  // MAX_PATH for the named-temporary path in extract()
#endif

#include <zlib.h>

#include <cerrno>
#include <cstring>

#include "errors.h"
#include "log.h"
#include "memutils.h"

namespace {

// All ZIP integers are little-endian and unaligned — memcpy, never cast.
word le16(const byte* p) {
  word v;
  std::memcpy(&v, p, 2);
  return v;
}
dword le32(const byte* p) {
  dword v;
  std::memcpy(&v, p, 4);
  return v;
}

constexpr dword kEocdSig = 0x06054b50;     // PK\5\6 — end of central directory
constexpr dword kCentralSig = 0x02014b50;  // PK\1\2 — central directory entry
constexpr dword kLocalSig = 0x04034b50;    // PK\3\4 — local file header
constexpr size_t kEocdLen = 22;            // fixed part, before the comment
// EOCD sits within the last 22 + 65535 (max comment) bytes of the file.
constexpr long kEocdScanMax = 22 + 65535;

// The loaders' extension list is a run of 4-character entries (".dsk.sna");
// an entry matches when its last four name characters equal one of them.
bool name_matches(const byte* name, word name_len, const std::string& exts) {
  if (name_len < 4) return false;
  const char* tail = reinterpret_cast<const char*>(name) + (name_len - 4);
  for (size_t i = 0; i + 4 <= exts.size(); i += 4)
    if (strncasecmp(tail, exts.c_str() + i, 4) == 0) return true;
  return false;
}

}  // namespace

namespace zip {

int dir(t_zip_info* zi) {
  FILE* f = fopen(zi->filename.c_str(), "rb");
  if (f == nullptr) {
    LOG_ERROR("File not found or not readable: " << zi->filename);
    return ERR_FILE_NOT_FOUND;
  }
  auto closer = [&]() { fclose(f); };
  memutils::scope_exit<decltype(closer)> const cs(closer);

  if (fseek(f, 0, SEEK_END) != 0) return ERR_FILE_BAD_ZIP;
  const long file_len = ftell(f);
  if (file_len < static_cast<long>(kEocdLen)) {
    LOG_ERROR("Couldn't read zip file (too short): " << zi->filename);
    return ERR_FILE_BAD_ZIP;
  }

  // Read the whole EOCD candidate region and scan backwards for the
  // signature (a trailing archive comment pushes it away from the end).
  const long tail_len = file_len < kEocdScanMax ? file_len : kEocdScanMax;
  std::vector<byte> tail(static_cast<size_t>(tail_len));
  if (fseek(f, file_len - tail_len, SEEK_SET) != 0 ||
      fread(tail.data(), tail.size(), 1, f) != 1) {
    LOG_ERROR("Couldn't read zip file: " << zi->filename);
    return ERR_FILE_BAD_ZIP;
  }
  long eocd = -1;
  for (long i = tail_len - static_cast<long>(kEocdLen); i >= 0; --i) {
    if (le32(tail.data() + i) == kEocdSig) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) {
    LOG_ERROR(
        "Couldn't read zip file (no central directory): " << zi->filename);
    return ERR_FILE_BAD_ZIP;
  }
  const word entries = le16(tail.data() + eocd + 10);
  const dword cd_size = le32(tail.data() + eocd + 12);
  const dword cd_offset = le32(tail.data() + eocd + 16);
  if (cd_size == 0 || entries == 0) {
    LOG_ERROR(
        "Couldn't read zip file (no central directory): " << zi->filename);
    return ERR_FILE_BAD_ZIP;
  }
  if (static_cast<long>(cd_size) > file_len) {  // corrupt: larger than file
    LOG_ERROR("Couldn't read zip file (bad directory size): " << zi->filename);
    return ERR_FILE_BAD_ZIP;
  }

  std::vector<byte> cd(cd_size);
  if (fseek(f, static_cast<long>(cd_offset), SEEK_SET) != 0 ||
      fread(cd.data(), cd.size(), 1, f) != 1) {
    LOG_ERROR("Couldn't read zip file: " << zi->filename);
    return ERR_FILE_BAD_ZIP;
  }

  const byte* p = cd.data();
  const byte* end = cd.data() + cd.size();
  for (word n = 0; n < entries; ++n) {
    if (p + 46 > end || le32(p) != kCentralSig) break;  // truncated/corrupt
    const word name_len = le16(p + 28);
    const word extra_len = le16(p + 30);
    const word comment_len = le16(p + 32);
    const dword local_off = le32(p + 42);
    if (p + 46 + name_len > end) break;  // name runs past the directory
    if (name_matches(p + 46, name_len, zi->extensions)) {
      zi->filesOffsets.emplace_back(
          std::string(reinterpret_cast<const char*>(p + 46), name_len),
          local_off);
      zi->dwOffset = local_off;
    }
    p += 46 + name_len + extra_len + comment_len;
  }

  if (zi->filesOffsets.empty()) {
    LOG_ERROR("Empty zip file: " << zi->filename);
    return ERR_FILE_EMPTY_ZIP;
  }
  return 0;
}

int extract(const t_zip_info& zi, FILE** pfileOut) {
#ifdef WINDOWS
  // Windows tmpfile() wants the root directory; use a named temporary.
  char tmpFilePath[MAX_PATH];
  snprintf(tmpFilePath, sizeof(tmpFilePath), ".\\koncpc_tmp_XXXXXX");
  if (_mktemp_s(tmpFilePath, strlen(tmpFilePath) + 1) != 0) {
    LOG_ERROR("Couldn't unzip file: Couldn't generate temporary file name: "
              << strerror(errno));
    return ERR_FILE_UNZIP_FAILED;
  }
  *pfileOut = fopen(tmpFilePath, "w+b");
#else
  *pfileOut = tmpfile();
#endif
  if (*pfileOut == nullptr) {
    LOG_ERROR("Couldn't unzip file: Couldn't create temporary file: "
              << strerror(errno));
    return ERR_FILE_UNZIP_FAILED;
  }
  auto fail = [&]() {
    fclose(*pfileOut);
    *pfileOut = nullptr;
    return ERR_FILE_UNZIP_FAILED;
  };

  FILE* in = fopen(zi.filename.c_str(), "rb");
  if (in == nullptr) {
    LOG_ERROR("Couldn't open zip file for reading: " << zi.filename);
    return fail();
  }
  auto closer = [&]() { fclose(in); };
  memutils::scope_exit<decltype(closer)> const cs(closer);

  byte hdr[30];
  if (fseek(in, static_cast<long>(zi.dwOffset), SEEK_SET) != 0 ||
      fread(hdr, sizeof(hdr), 1, in) != 1 || le32(hdr) != kLocalSig) {
    LOG_ERROR("Couldn't read zip file: " << zi.filename);
    return fail();
  }
  const word method = le16(hdr + 8);       // 0 = stored, 8 = deflate
  const dword comp_size = le32(hdr + 18);  // 0 with a data descriptor (bit 3)
  const long data_off =
      static_cast<long>(zi.dwOffset) + 30 + le16(hdr + 26) + le16(hdr + 28);
  if (fseek(in, data_off, SEEK_SET) != 0) {
    LOG_ERROR("Couldn't read zip file: " << zi.filename);
    return fail();
  }

  std::vector<byte> ibuf(16384), obuf(16384);

  if (method == 0) {  // stored: a straight copy of comp_size bytes
    dword left = comp_size;
    while (left > 0) {
      const size_t want = left < ibuf.size() ? left : ibuf.size();
      if (fread(ibuf.data(), want, 1, in) != 1) {
        LOG_ERROR("Couldn't unzip file (truncated): " << zi.filename);
        return fail();
      }
      if (fwrite(ibuf.data(), want, 1, *pfileOut) != 1) {
        LOG_ERROR("Couldn't unzip file: Couldn't write to output file");
        return fail();
      }
      left -= static_cast<dword>(want);
    }
  } else if (method == 8) {  // deflate: raw stream (no zlib header)
    z_stream z{};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return fail();
    auto zend = [&]() { inflateEnd(&z); };
    memutils::scope_exit<decltype(zend)> const zs(zend);

    // comp_size drives the reads; 0 (data-descriptor entries) reads to EOF —
    // inflate stops exactly at Z_STREAM_END either way.
    dword left = comp_size;
    int status = Z_OK;
    while (status == Z_OK) {
      size_t want = ibuf.size();
      if (comp_size != 0) {
        if (left == 0) break;  // compressed data exhausted before STREAM_END
        want = left < ibuf.size() ? left : ibuf.size();
      }
      const size_t got = fread(ibuf.data(), 1, want, in);
      if (got == 0) break;  // EOF/error before the stream ended
      if (comp_size != 0) left -= static_cast<dword>(got);
      z.next_in = ibuf.data();
      z.avail_in = static_cast<uInt>(got);
      while (z.avail_in != 0 && status == Z_OK) {
        z.next_out = obuf.data();
        z.avail_out = static_cast<uInt>(obuf.size());
        status = inflate(&z, Z_NO_FLUSH);
        const size_t produced = obuf.size() - z.avail_out;
        if (produced != 0 && fwrite(obuf.data(), produced, 1, *pfileOut) != 1) {
          LOG_ERROR("Couldn't unzip file: Couldn't write to output file");
          return fail();
        }
      }
    }
    if (status != Z_STREAM_END) {
      LOG_ERROR("Couldn't unzip file: " << zi.filename << " (" << status
                                        << ")");
      return fail();
    }
  } else {
    LOG_ERROR("Couldn't unzip file: unsupported compression method " << method
                                                                     << ")");
    return fail();
  }

  fseek(*pfileOut, 0, SEEK_SET);
  return 0;
}

}  // namespace zip
