/* zip_archive — minimal ZIP reading for slot-file loading: list an archive's
 * entries by extension and inflate one entry to a temporary FILE*. Authored
 * from the PKWARE APPNOTE (no minizip dependency; zlib does the inflate). */

#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "types.h"

namespace zip {

typedef struct {
  std::string filename;    // the .zip on disk
  std::string extensions;  // concatenated 4-char extensions: ".dsk.sna..."
  // (entry name, local-header offset) for every entry whose last four name
  // characters case-insensitively match one of `extensions`.
  std::vector<std::pair<std::string, dword>> filesOffsets;
  unsigned int dwOffset;  // local-header offset extract() reads from
} t_zip_info;

/* Scan the central directory and fill zi->filesOffsets (and dwOffset with the
 * last match). Returns 0, ERR_FILE_NOT_FOUND, ERR_FILE_BAD_ZIP, or
 * ERR_FILE_EMPTY_ZIP when nothing matched. */
int dir(t_zip_info* zi);

/* Decompress the entry whose LOCAL header sits at zi.dwOffset into a fresh
 * temporary file (rewound, caller closes). Stored and deflated entries are
 * supported. Returns 0 or ERR_FILE_UNZIP_FAILED. */
int extract(const t_zip_info& zi, FILE** pfileOut);

}  // namespace zip
