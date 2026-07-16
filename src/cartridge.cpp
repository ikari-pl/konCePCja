// konCePCja — loading of Plus-range cartridge files (.cpr).

#include "cartridge.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include "errors.h"
#include "log.h"
#include "types.h"

constexpr dword CARTRIDGE_NB_PAGES = 32;
constexpr dword CARTRIDGE_PAGE_SIZE = 16 * 1024;
constexpr dword CARTRIDGE_MAX_SIZE = CARTRIDGE_NB_PAGES * CARTRIDGE_PAGE_SIZE;

// The parsed cartridge image (32 sequential 16 KiB pages) and a per-page
// pointer table into it. Consumed by the board bring-up (Plus boot) and
// the ROM banking code.
extern std::unique_ptr<byte[]> pbCartridgeImage;
std::unique_ptr<byte[]> pbCartridgeImage = nullptr;
extern byte* pbCartridgePages[CARTRIDGE_NB_PAGES];
byte* pbCartridgePages[CARTRIDGE_NB_PAGES] = {nullptr};

extern byte* pbROMlo;

namespace {

// Allocates a zeroed image and rebuilds the page table.
void cartridge_init() {
  pbCartridgeImage.reset(new byte[CARTRIDGE_MAX_SIZE]());  // value-initialized
  for (dword i = 0; i < CARTRIDGE_NB_PAGES; ++i) {
    pbCartridgePages[i] = &pbCartridgeImage[i * CARTRIDGE_PAGE_SIZE];
  }
}

dword read_le32(const byte* p) {
  return static_cast<dword>(p[0]) | (static_cast<dword>(p[1]) << 8) |
         (static_cast<dword>(p[2]) << 16) | (static_cast<dword>(p[3]) << 24);
}

// Reads the remaining stream content into a byte vector.
std::vector<byte> slurp(std::FILE* pfile) {
  std::vector<byte> data;
  byte buffer[16384];
  size_t read;
  while ((read = std::fread(buffer, 1, sizeof(buffer), pfile)) > 0) {
    data.insert(data.end(), buffer, buffer + read);
  }
  return data;
}

}  // namespace

void cpr_eject() {
  pbCartridgeImage = nullptr;
  for (auto& page : pbCartridgePages) {
    page = nullptr;
  }
}

int cpr_load(const std::string& filename) {
  LOG_DEBUG("cpr_load " << filename);
  std::FILE* pfile = std::fopen(filename.c_str(), "rb");
  if (pfile == nullptr) {
    LOG_DEBUG("File not found: " << filename);
    return ERR_FILE_NOT_FOUND;
  }
  int const iRetCode = cpr_load(pfile);
  std::fclose(pfile);
  return iRetCode;
}

int cpr_load(std::FILE* pfile) {
  constexpr dword CPR_HEADER_SIZE = 12;
  constexpr dword CPR_CHUNK_HEADER_SIZE = 8;

  cpr_eject();

  const std::vector<byte> data = slurp(pfile);

  // RIFF container header: "RIFF" <le32 size> "AMS!"
  if (data.size() < CPR_HEADER_SIZE) {
    LOG_DEBUG("Cartridge file less than " << CPR_HEADER_SIZE
                                          << " bytes long !");
    return ERR_CPR_INVALID;
  }
  if (std::memcmp(data.data(), "RIFF", 4) != 0) {
    LOG_DEBUG("Cartridge file is not a RIFF file");
    return ERR_CPR_INVALID;
  }
  if (std::memcmp(data.data() + 8, "AMS!", 4) != 0) {
    LOG_DEBUG("Cartridge file is not a CPR file");
    return ERR_CPR_INVALID;
  }
  const dword totalSize = read_le32(data.data() + 4);
  LOG_DEBUG("CPR size: " << totalSize);

  cartridge_init();

  // Walk the chunks. `cursor` mirrors the historical file position;
  // `offset` keeps the historical loop bound (header size + declared chunk
  // sizes, unpadded) so any CPR that loaded before still loads bit-for-bit.
  size_t cursor = CPR_HEADER_SIZE;
  dword offset = CPR_HEADER_SIZE;
  dword cartridgeOffset = 0;
  while (offset < totalSize) {
    if (cursor + CPR_CHUNK_HEADER_SIZE > data.size()) {
      LOG_DEBUG("Failed reading chunk header");
      return ERR_CPR_INVALID;
    }
    const byte* chunkHeader = data.data() + cursor;
    cursor += CPR_CHUNK_HEADER_SIZE;
    offset += CPR_CHUNK_HEADER_SIZE;

    const dword chunkSize = read_le32(chunkHeader + 4);
    LOG_DEBUG("Chunk '" << std::string(chunkHeader, chunkHeader + 4)
                        << "' at offset " << offset << " of size "
                        << chunkSize);

    // A page chunk is normally 16 KiB. Smaller chunks leave the rest of the
    // page zeroed; larger chunks are truncated to the page and the excess
    // (plus the RIFF pad byte for odd sizes) is skipped.
    dword chunkKept = std::min(chunkSize, CARTRIDGE_PAGE_SIZE);
    if (chunkKept % 2 != 0) {
      ++chunkKept;  // odd chunk: the RIFF pad byte rides along
    }
    if (chunkKept == 0) continue;  // empty chunks exist in the wild

    if (cartridgeOffset + CARTRIDGE_PAGE_SIZE > CARTRIDGE_MAX_SIZE) {
      LOG_DEBUG("Maximum cartridge size exceeded ! (" << CARTRIDGE_MAX_SIZE
                                                      << " bytes)");
      return ERR_CPR_INVALID;
    }
    if (cursor + chunkKept > data.size()) {
      LOG_DEBUG("Failed reading chunk content");
      return ERR_CPR_INVALID;
    }
    std::memcpy(&pbCartridgeImage[cartridgeOffset], data.data() + cursor,
                chunkKept);
    cursor += chunkKept;
    if (chunkKept >= CARTRIDGE_PAGE_SIZE && chunkKept < chunkSize) {
      LOG_DEBUG("This chunk is bigger than the max allowed size !!!");
      cursor += ((static_cast<size_t>(chunkSize) + 1) & ~size_t{1}) -
                chunkKept;  // skip the excess + pad
      if (cursor > data.size()) {
        LOG_DEBUG("Failed skipping excessive chunk content");
        return ERR_CPR_INVALID;
      }
    }
    cartridgeOffset += CARTRIDGE_PAGE_SIZE;
    offset += chunkSize;
  }
  LOG_DEBUG("Final offset: " << offset);
  LOG_DEBUG("Final cartridge offset: " << cartridgeOffset);
  pbROMlo = &pbCartridgeImage[0];
  return 0;
}
