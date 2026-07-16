// konCePCja — media manager: routes .dsk/.cdt/.voc/.sna/.cpr/.ipf/.raw
// (optionally inside .zip archives) into the machine's drive, tape, snapshot
// and cartridge slots, and keeps the host-side t_drive/t_track views (the
// disc tools' parse target) in sync with what the sub-cycle engine plays.

#include "slotshandler.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

#include "cartridge.h"
#include "errors.h"
#include "fileutils.h"
#include "flux_ingest.h"
#include "hw/crtc.h"
#include "hw/fdc.h"
#include "hw/gate_array.h"
#include "hw/memory.h"
#include "hw/ppi.h"
#include "hw/printer.h"
#include "hw/psg.h"
#include "hw_views.h"
#include "ipf.h"
#include "koncepcja.h"
#include "log.h"
#include "stringutils.h"
#include "subcycle/machine.h"
#include "subcycle_bridge.h"
#include "voc_import.h"
#include "z80_view.h"
#include "zip_archive.h"

extern t_CPC CPC;
extern t_drive driveA;
extern t_drive driveB;

extern byte* pbTapeImageEnd;
byte* pbTapeImageEnd = nullptr;
extern std::vector<byte> pbTapeImage;

namespace {
struct file_loader {
  DRIVE drive;
  std::string extension;
  int (*load_from_filename)(const std::string& filename);
  int (*load_from_file)(FILE* file);
};
}  // namespace

namespace {
file_loader files_loader_list[] = {
    {DRIVE::DSK_A, ".dsk",
     [](const std::string& filename) -> int {
       return dsk_load(filename, &driveA);
     },
     [](FILE* file) -> int { return dsk_load(file, &driveA); }},

    {DRIVE::DSK_B, ".dsk",
     [](const std::string& filename) -> int {
       return dsk_load(filename, &driveB);
     },
     [](FILE* file) -> int { return dsk_load(file, &driveB); }},

    {DRIVE::DSK_A, ".ipf",
     [](const std::string& filename) -> int {
       return ipf_load(filename, &driveA);
     },
     [](FILE* file) -> int { return ipf_load(file, &driveA); }},

    {DRIVE::DSK_B, ".ipf",
     [](const std::string& filename) -> int {
       return ipf_load(filename, &driveB);
     },
     [](FILE* file) -> int { return ipf_load(file, &driveB); }},

    {DRIVE::DSK_A, ".raw",
     [](const std::string& filename) -> int {
       return ipf_load(filename, &driveA);
     },
     [](FILE* file) -> int { return ipf_load(file, &driveA); }},

    {DRIVE::DSK_B, ".raw",
     [](const std::string& filename) -> int {
       return ipf_load(filename, &driveB);
     },
     [](FILE* file) -> int { return ipf_load(file, &driveB); }},

    // Native flux containers (drive-A only — the FDC's flux capture is
    // side-0/drive-A-only). They carry no legacy t_drive sector view; the
    // flux itself is transcoded to SCP by flux::to_scp in the mirror branch
    // below, so the loader is a validated no-op that just lets dispatch
    // reach that branch.
    {DRIVE::DSK_A, ".scp", [](const std::string&) -> int { return 0; },
     [](FILE*) -> int { return 0; }},
    {DRIVE::DSK_A, ".hfe", [](const std::string&) -> int { return 0; },
     [](FILE*) -> int { return 0; }},
    {DRIVE::DSK_A, ".a2r", [](const std::string&) -> int { return 0; },
     [](FILE*) -> int { return 0; }},

    {DRIVE::SNAPSHOT, ".sna", &snapshot_load, &snapshot_load},

    {DRIVE::TAPE, ".cdt", &tape_insert, &tape_insert},

    {DRIVE::TAPE, ".voc", &tape_insert, &tape_insert},

    {DRIVE::CARTRIDGE, ".cpr", &cartridge_load, &cartridge_load},
};
}  // namespace

extern t_disk_format disk_format[MAX_DISK_FORMAT];
t_disk_format disk_format[MAX_DISK_FORMAT] = {
    {"178K Data Format",
     40,
     1,
     9,
     2,
     0x52,
     0xe5,
     {{0xc1, 0xc6, 0xc2, 0xc7, 0xc3, 0xc8, 0xc4, 0xc9, 0xc5}}},
    {"169K Vendor Format",
     40,
     1,
     9,
     2,
     0x52,
     0xe5,
     {{0x41, 0x46, 0x42, 0x47, 0x43, 0x48, 0x44, 0x49, 0x45}}}};

// Parses the command-line file list and fills the matching CPC slots
// (drive A first, then drive B for a second disk, tape, snapshot,
// cartridge). Only the first file of each kind is kept.
// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other translation units/tests; internal linkage would break the link
void fillSlots(std::vector<std::string> slot_list, t_CPC& CPC) {
  struct SlotTarget {
    t_slot& slot;
    const char* extensions;  // dot-separated list, e.g. ".dsk.ipf.raw"
    const char* description;
    bool filled = false;
  };
  // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
  SlotTarget targets[] = {
      {CPC.driveA, ".dsk.ipf.raw.scp.hfe.a2r", "drive A disk"},
      {CPC.driveB, ".dsk.ipf.raw", "drive B disk"},
      {CPC.snapshot, ".sna", "CPC state snapshot"},
      {CPC.tape, ".cdt.voc", "tape"},
      {CPC.cartridge, ".cpr", "cartridge"},
  };

  for (const auto& arg : slot_list) {
    LOG_DEBUG("Handling arg " << arg);
    std::string const fullpath =
        stringutils::trim(arg, '"');       // remove quotes if arguments quoted
    if (fullpath.length() <= 5) continue;  // minimum for a valid filename
    std::string extension =
        stringutils::lower(fullpath.substr(fullpath.length() - 4));

    if (extension == ".zip") {  // classify by the first relevant entry inside
      zip::t_zip_info zip_info;
      zip_info.filename = fullpath;
      zip_info.extensions = ".dsk.sna.cdt.voc.cpr.ipf.raw.scp.hfe.a2r";
      if (zip::dir(&zip_info)) {
        continue;  // error or nothing relevant found
      }
      const std::string& filename = zip_info.filesOffsets[0].first;
      extension = stringutils::lower(filename.substr(filename.length() - 4));
    }

    for (auto& target : targets) {
      if (target.filled ||
          std::string_view(target.extensions).find(extension) ==
              std::string_view::npos) {
        continue;
      }
      LOG_VERBOSE("Loading " << target.description << " file: " << fullpath);
      target.slot.file = fullpath;
      target.slot.zip_index = 0;
      target.filled = true;
      break;
    }
  }
}

void loadSlots() {
  memset(&driveA, 0, sizeof(t_drive));  // clear disk drive A data structure
  file_load(CPC.driveA);
  memset(&driveB, 0, sizeof(t_drive));  // clear disk drive B data structure
  file_load(CPC.driveB);
  file_load(CPC.tape);
  file_load(CPC.snapshot);
  // Cartridge was loaded by emulator_init which called cartridge_load if needed
}

// Extract 'filename' from 'zipfile'. Filename must end with one of the
// extensions listed in 'ext'. FILE handle returned must be closed once finished
// with. nullptr is returned if file couldn't be extracted for any reason.
FILE* extractFile(const std::string& zipfile, const std::string& filename,
                  const std::string& ext) {
  zip::t_zip_info zip_info;
  zip_info.filename = zipfile;
  zip_info.extensions = ext;
  if (!zip::dir(&zip_info)) {  // parse the zip for relevant files
    for (const auto& fn : zip_info.filesOffsets) {
      if (!strcasecmp(filename.c_str(),
                      fn.first.c_str())) {  // do we have a match?
        FILE* file = nullptr;
        zip_info.dwOffset = fn.second;  // get the offset into the zip archive
        if (!zip::extract(zip_info, &file)) {
          return file;
        }
      }
    }
  }
  return nullptr;
}

// Parses a "label,tracks,sides,sectors,size,gap3,filler,id,id,..." format
// description (the [file] fmtNN config entries). An out-of-range field
// yields a format with an empty label (the "invalid" marker callers test).
t_disk_format parseDiskFormat(const std::string& format) {
  t_disk_format result;
  std::vector<std::string> tokens = stringutils::split(format, ',');
  if (tokens.size() < 7) {  // Minimum number of values required
    return result;
  }

  // Numeric field with an inclusive validity range; base auto-detected.
  auto field_in_range = [&tokens](size_t index, dword min, dword max,
                                  dword& out) {
    dword const value = strtoul(tokens[index].c_str(), nullptr, 0);
    if (value < min || value > max) return false;
    out = value;
    return true;
  };

  if (!field_in_range(1, 1, DSK_TRACKMAX, result.tracks) ||
      !field_in_range(2, 1, DSK_SIDEMAX, result.sides) ||
      !field_in_range(3, 1, DSK_SECTORMAX, result.sectors) ||
      !field_in_range(4, 1, 6, result.sector_size) ||
      !field_in_range(5, 1, 255, result.gap3_length)) {
    return result;
  }
  result.filler_byte =
      static_cast<byte>(strtoul(tokens[6].c_str(), nullptr, 0));

  size_t i = 7;
  for (dword side = 0; side < result.sides; side++) {
    for (dword sector = 0; sector < result.sectors; sector++) {
      // Historical default for missing IDs: the sector's index + 1.
      dword const id = (i < tokens.size())
                           ? strtoul(tokens[i++].c_str(), nullptr, 0)
                           : sector + 1;
      result.sector_ids[side][sector] = static_cast<byte>(id);
    }
  }
  // Fill the label only if the disk format is valid
  result.label = tokens[0];
  return result;
}

std::string serializeDiskFormat(const t_disk_format& format) {
  std::ostringstream oss;
  if (!format.label.empty()) {
    oss << format.label << ",";
    oss << format.tracks << ",";
    oss << format.sides << ",";
    oss << format.sectors << ",";
    oss << format.sector_size << ",";
    oss << format.gap3_length << ",";
    oss << static_cast<unsigned int>(format.filler_byte);
    for (int iSide = 0; iSide < static_cast<int>(format.sides); iSide++) {
      for (int iSector = 0; iSector < static_cast<int>(format.sectors);
           iSector++) {
        oss << ","
            << static_cast<unsigned int>(format.sector_ids[iSide][iSector]);
      }
    }
  }
  return oss.str();
}

int snapshot_save(const std::string& filename) {
  if (subcycle::Machine* m = subcycle_bridge_machine())
    return snapshot_save_machine(*m, filename);
  return ERR_SNA_WRITE;  // machine not up yet: nothing to save
}

void dsk_eject(t_drive* drive) {
  if (drive == &driveA) subcycle_bridge_eject_media(0);  // mirror to engine
  if (drive == &driveB) subcycle_bridge_eject_media(1);
  if (drive->eject_hook) drive->eject_hook(drive);  // additional cleanup

  for (auto& track_row : drive->track) {
    for (auto& track : track_row) {
      delete[] track.data;  // release memory allocated for this track
    }
  }
  dword const head_position =
      drive->current_track;                    // save the drive head position
  memset(drive, 0, sizeof(t_drive));           // clear drive info structure
  drive->current_track = head_position;
}

namespace {

// One fread wrapper so every short read is handled the same way.
bool read_exact(FILE* pfile, byte* dst, size_t size) {
  return fread(dst, size, 1, pfile) == 1;
}

// Parses one "Track-Info" block (header + sector table + data) into `track`.
// Standard images give every sector the same size and every track the same
// data length (`track_size`); extended images store per-sector sizes in the
// sector-info table and per-track lengths in the disk header.
int dsk_load_track(FILE* pfile, t_track& track, dword track_size,
                   bool extended, dword track_no, dword side_no) {
  byte header[0x100];
  if (!read_exact(pfile, header, sizeof(header))) {
    LOG_ERROR("Couldn't read DSK track header for track " << track_no
                                                          << " side "
                                                          << side_no);
    return ERR_DSK_INVALID;
  }
  if (memcmp(header, "Track-Info", 10) != 0) {
    LOG_ERROR("Corrupted DSK track header for track " << track_no << " side "
                                                      << side_no);
    return ERR_DSK_INVALID;
  }

  const dword sectors = header[0x15];
  LOG_DEBUG("with " << sectors << " sectors");
  if (sectors > DSK_SECTORMAX) {
    LOG_ERROR("DSK track with " << sectors << " sectors, expected "
                                << DSK_SECTORMAX << " or less");
    return ERR_DSK_SECTORS;
  }

  track.sectors = sectors;
  track.size = track_size;
  track.data = new byte[track_size];  // sector data lives in one buffer

  const dword standard_size = 0x80 << header[0x14];
  const byte* info = header + 0x18;
  byte* sector_data = track.data;
  for (dword sector = 0; sector < sectors; sector++, info += 8) {
    t_sector& s = track.sector[sector];
    memcpy(s.CHRN, info, 4);       // C, H, R, N
    memcpy(s.flags, info + 4, 2);  // ST1 & ST2
    dword const real_size = extended ? (0x80 << info[3]) : standard_size;
    dword const stored_size =
        extended ? (info[6] + (info[7] << 8)) : standard_size;
    // NOLINTNEXTLINE(readability-suspicious-call-argument): argument order verified correct
    s.setSizes(real_size, stored_size);
    s.setData(sector_data);
    sector_data += stored_size;
    LOG_DEBUG("Sector " << sector << " size: " << stored_size
                        << " real size: " << real_size
                        << " CHRN: " << chrn_to_string(s.CHRN));
  }

  if (track_size > 0 && !read_exact(pfile, track.data, track_size)) {
    LOG_ERROR("Couldn't read track data for track " << track_no << " side "
                                                    << side_no);
    return ERR_DSK_INVALID;
  }
  return 0;
}

// Parses a DSK image (standard "MV - CPC" or "EXTENDED") into the drive's
// track structures. Returns 0 or an ERR_DSK_* code; the caller ejects on
// error.
int dsk_parse(FILE* pfile, t_drive* drive) {
  byte dsk_header[0x100];
  if (!read_exact(pfile, dsk_header, sizeof(dsk_header))) {
    LOG_ERROR("Couldn't read DSK header");
    return ERR_DSK_INVALID;
  }

  const bool extended = memcmp(dsk_header, "EXTENDED", 8) == 0;
  if (!extended && memcmp(dsk_header, "MV - CPC", 8) != 0) {
    LOG_ERROR("Unknown DSK type");
    return ERR_DSK_INVALID;
  }
  LOG_DEBUG("Loading " << (extended ? "extended" : "normal") << " disk");

  drive->tracks = dsk_header[0x30];
  drive->tracks =
      std::min<unsigned int>(drive->tracks, DSK_TRACKMAX);  // limit to maximum

  LOG_DEBUG("with " << drive->tracks << " tracks");

  drive->sides = extended ? (dsk_header[0x31] & 3) : dsk_header[0x31];
  if (drive->sides > DSK_SIDEMAX) {
    LOG_ERROR("DSK header has " << drive->sides << " sides, expected "
                                << DSK_SIDEMAX << " or less");
    return ERR_DSK_SIDES;
  }
  LOG_DEBUG("with " << drive->sides << " sides");
  drive->sides--;  // zero base number of sides

  if (extended) {
    drive->random_DEs = dsk_header[0x31] & 0x80;  // simulate random data errors
  }

  // Standard images: one track length for the whole disk. Extended images:
  // per-track lengths (in 256-byte units, header included) follow at 0x34.
  const dword standard_track_size =
      dsk_header[0x32] + (dsk_header[0x33] << 8);
  const byte* track_size_table = dsk_header + 0x34;

  for (dword track = 0; track < drive->tracks; track++) {
    for (dword side = 0; side <= drive->sides; side++) {
      dword const track_size =
          extended ? (*track_size_table++ << 8) : standard_track_size;
      LOG_DEBUG("Track " << track << ", side " << side << ", size "
                         << track_size);
      if (extended && track_size == 0) {
        LOG_DEBUG("empty track");
        memset(&drive->track[track][side], 0,
               sizeof(t_track));  // track not formatted
        continue;
      }
      int const rc = dsk_load_track(pfile, drive->track[track][side],
                                    track_size - 0x100, extended, track, side);
      if (rc != 0) return rc;
    }
  }
  drive->altered = false;  // disk is as yet unmodified
  return 0;
}

}  // namespace

int dsk_load(FILE* pfile, t_drive* drive) {
  LOG_DEBUG("Loading disk");
  int const rc = dsk_parse(pfile, drive);
  if (rc != 0) dsk_eject(drive);
  return rc;
}

int dsk_load(const std::string& filename, t_drive* drive) {
  LOG_DEBUG("Loading disk: " << filename);
  dsk_eject(drive);
  FILE* pfile = fopen(filename.c_str(), "rb");
  if (pfile == nullptr) {
    LOG_ERROR("File not found: " << filename);
    return ERR_FILE_NOT_FOUND;
  }
  int const rc = dsk_load(pfile, drive);
  fclose(pfile);
  return rc;  // dsk_load already ejected on error
}

// NOLINTNEXTLINE(misc-use-internal-linkage): external API consumed by other translation units/tests; internal linkage would break the link
int dsk_to_bytes(t_drive* drive, std::vector<uint8_t>& out) {
  // If there are no tracks, don't serialize
  if (drive->tracks == 0) {
    LOG_ERROR("No tracks to save");
    return ERR_DSK_WRITE;
  }
  out.clear();
  auto append = [&](const void* data, size_t size) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    out.insert(out.end(), bytes, bytes + size);
  };

  t_DSK_header dh = {};
  memcpy(dh.id, "EXTENDED CPC DSK File\r\nDisk-Info\r\n", sizeof(dh.id));
  memcpy(dh.unused1, "konCePCja\r\n", 11);
  dh.tracks = drive->tracks;
  // correct side count, and indicate random DEs if necessary
  dh.sides = (drive->sides + 1) | (drive->random_DEs);
  dword pos = 0;
  for (dword track = 0; track < drive->tracks; track++) {
    for (dword side = 0; side <= drive->sides; side++) {
      if (drive->track[track][side].size) {  // track is formatted?
        dh.track_size[pos] =
            (drive->track[track][side].size + 0x100) >> 8;  // incl. header
      }
      pos++;
    }
  }
  append(&dh, sizeof(dh));

  for (dword track = 0; track < drive->tracks; track++) {
    for (dword side = 0; side <= drive->sides; side++) {
      const t_track& src = drive->track[track][side];
      if (!src.size) continue;  // unformatted track: no Track-Info block

      t_track_header th = {};
      memcpy(th.id, "Track-Info\r\n", sizeof(th.id));
      th.track = track;
      th.side = side;
      th.bps = 2;
      th.sectors = src.sectors;
      th.gap3 = 0x4e;
      th.filler = 0xe5;
      for (dword sector = 0; sector < th.sectors; sector++) {
        memcpy(&th.sector[sector][0], src.sector[sector].CHRN, 4);
        memcpy(&th.sector[sector][4], src.sector[sector].flags, 2);
        const dword stored = src.sector[sector].getTotalSize();
        th.sector[sector][6] = stored & 0xff;
        th.sector[sector][7] = (stored >> 8) & 0xff;
      }
      append(&th, sizeof(th));
      append(src.data, src.size);
    }
  }
  return 0;
}

int dsk_save(const std::string& filename, t_drive* drive) {
  std::vector<uint8_t> bytes;
  const int status = dsk_to_bytes(drive, bytes);
  if (status != 0) return status;

  FILE* pfile = fopen(filename.c_str(), "wb");
  if (pfile == nullptr) {
    LOG_ERROR("Error while opening the file '"
              << filename << "' for write: " << strerror(errno));
    return ERR_DSK_WRITE;
  }
  const bool wrote =
      fwrite(bytes.data(), 1, bytes.size(), pfile) == bytes.size();
  const bool flushed = fflush(pfile) == 0;
  const bool closed = fclose(pfile) == 0;
  if (!wrote || !flushed || !closed) {
    LOG_ERROR("Error while writing to the file '" << filename << "'");
    return ERR_DSK_WRITE;
  }
  drive->altered = false;  // Drive is not modified anymore
  return 0;
}

int dsk_format(t_drive* drive, int iFormat) {
  const t_disk_format& fmt = disk_format[iFormat];

  drive->tracks = std::min<dword>(fmt.tracks, DSK_TRACKMAX);
  drive->sides = fmt.sides;
  const dword sector_size = 0x80 << fmt.sector_size;
  const dword sectors = fmt.sectors;
  if (drive->sides > DSK_SIDEMAX) {
    dsk_eject(drive);
    return ERR_DSK_SIDES;
  }
  if (sectors > DSK_SECTORMAX) {
    dsk_eject(drive);
    return ERR_DSK_SECTORS;
  }
  drive->sides--;  // zero base number of sides

  const dword track_size = sector_size * sectors;
  for (dword track = 0; track < drive->tracks; track++) {
    for (dword side = 0; side <= drive->sides; side++) {
      t_track& dst = drive->track[track][side];
      dst.sectors = sectors;
      dst.size = track_size;
      dst.data = new byte[track_size];
      memset(dst.data, fmt.filler_byte, track_size);

      byte* sector_data = dst.data;
      for (dword sector = 0; sector < sectors; sector++) {
        const byte chrn[4] = {static_cast<byte>(track),
                              static_cast<byte>(side),
                              fmt.sector_ids[side][sector],
                              static_cast<byte>(fmt.sector_size)};
        memcpy(dst.sector[sector].CHRN, chrn, 4);
        dst.sector[sector].setSizes(sector_size, sector_size);
        dst.sector[sector].setData(sector_data);
        sector_data += sector_size;
      }
    }
  }
  drive->altered = true;  // flag disk as having been modified
  return 0;
}

void tape_eject() {
  pbTapeImage.clear();
  pbTapeImageEnd = nullptr;
  subcycle_bridge_eject_tape();  // the deck side (frame-boundary deferred)
}

// --- SNA <-> subcycle::Machine (Wave 1) -------------------------------------
// The legacy loader replayed chip state through z80_OUT_handler; the machine
// version replays it the same way through DMA-style io_write cycles, so any
// engine implementing the bus contract restores identically.

int snapshot_load_machine(subcycle::Machine& m, FILE* pfile) {
  t_SNA_header sh{};
  if (fread(&sh, sizeof(sh), 1, pfile) != 1) return ERR_SNA_INVALID;
  if (memcmp(sh.id, "MV - SNA", 8) != 0) return ERR_SNA_INVALID;
  size_t const snap_kb = (sh.ram_size[0] + (sh.ram_size[1] * 256)) & ~0x3fu;
  if (snap_kb == 0) return ERR_SNA_SIZE;
  if (snap_kb * 1024 > m.ram_size()) {
    LOG_ERROR("snapshot: " << snap_kb << "K dump exceeds the machine's RAM");
    return ERR_SNA_SIZE;
  }
  std::vector<uint8_t> ram(snap_kb * 1024);
  if (fread(ram.data(), ram.size(), 1, pfile) != 1) return ERR_SNA_INVALID;

  m.reset();
  for (size_t i = 0; i < ram.size(); i++) m.ram_write(i, ram[i]);

  Z80Regs r = m.regs();
  r.af = static_cast<uint16_t>(sh.AF[0] | (sh.AF[1] << 8));
  r.bc = static_cast<uint16_t>(sh.BC[0] | (sh.BC[1] << 8));
  r.de = static_cast<uint16_t>(sh.DE[0] | (sh.DE[1] << 8));
  r.hl = static_cast<uint16_t>(sh.HL[0] | (sh.HL[1] << 8));
  r.af_ = static_cast<uint16_t>(sh.AFx[0] | (sh.AFx[1] << 8));
  r.bc_ = static_cast<uint16_t>(sh.BCx[0] | (sh.BCx[1] << 8));
  r.de_ = static_cast<uint16_t>(sh.DEx[0] | (sh.DEx[1] << 8));
  r.hl_ = static_cast<uint16_t>(sh.HLx[0] | (sh.HLx[1] << 8));
  r.ix = static_cast<uint16_t>(sh.IX[0] | (sh.IX[1] << 8));
  r.iy = static_cast<uint16_t>(sh.IY[0] | (sh.IY[1] << 8));
  r.sp = static_cast<uint16_t>(sh.SP[0] | (sh.SP[1] << 8));
  r.pc = static_cast<uint16_t>(sh.PC[0] | (sh.PC[1] << 8));
  r.i = sh.I;
  r.r = sh.R;
  r.im = (sh.IM <= 2) ? sh.IM : 1;
  r.iff1 = sh.IFF0 ? 1 : 0;
  r.iff2 = sh.IFF1 ? 1 : 0;
  r.halted = 0;
  m.set_regs(r);

  // Gate Array: pen-select + ink for every entry, then pen / ROM / RAM config.
  for (int n = 0; n < 17; n++) {
    m.io_write(0x7F00, static_cast<uint8_t>(n == 16 ? 0x10 : n));
    m.io_write(0x7F00,
               static_cast<uint8_t>((sh.ga_ink_values[n] & 0x1F) | 0x40));
  }
  m.io_write(0x7F00, sh.ga_pen & 0x3F);
  m.io_write(0x7F00, static_cast<uint8_t>((sh.ga_ROM_config & 0x3F) | 0x80));
  m.io_write(0x7F00, static_cast<uint8_t>((sh.ga_RAM_config & 0x3F) | 0xC0));
  // CRTC register file + selection.
  for (int n = 0; n < 18; n++) {
    m.io_write(0xBC00, static_cast<uint8_t>(n));
    m.io_write(0xBD00, sh.crtc_registers[n]);
  }
  m.io_write(0xBC00, sh.crtc_reg_select);
  // Upper ROM select.
  m.io_write(0xDF00, sh.upper_ROM);
  // PPI: mode word FIRST (a real 8255 mode-set clears the latches), then ports.
  m.io_write(0xF700, sh.ppi_control);
  m.io_write(0xF400, sh.ppi_A);
  m.io_write(0xF500, sh.ppi_B);
  m.io_write(0xF600, sh.ppi_C);
  // PSG register file + selection (direct, like the legacy SetAYRegister).
  for (int n = 0; n < 16; n++)
    m.psg_poke(static_cast<uint8_t>(n), sh.psg_registers[n]);
  m.psg_select(sh.psg_reg_select);

  if (sh.version > 2) {
    fdc_poke_mechanics(m.fdc(), sh.fdc_motor, sh.drvA_current_track,
                       sh.drvB_current_track);
    printer_poke_latch(m.printer(), sh.printer_data);
    psg_poke_env(m.psg(), sh.psg_env_step, sh.psg_env_direction);
    crtc_restore_v3(
        m.crtc(),
        static_cast<uint16_t>(sh.crtc_addr[0] + (sh.crtc_addr[1] * 256)),
        static_cast<uint16_t>(sh.crtc_scanline[0] +
                              (sh.crtc_scanline[1] * 256)),
        sh.crtc_char_count[0], sh.crtc_line_count, sh.crtc_raster_count,
        sh.crtc_hsw_count, sh.crtc_vsw_count, sh.crtc_flags[0]);
    ga_poke_counters(m.gate_array(), sh.ga_int_delay, sh.ga_sl_count,
                     sh.z80_int_pending);
  }

  if (sh.version > 1 && sh.cpc_model != CPC.model) {
    LOG_ERROR("snapshot: saved for model " << static_cast<int>(sh.cpc_model)
                                           << ", machine is model " << CPC.model
                                           << " — state loaded as-is");
  }
  subcycle_bridge_sync_regs_view();
  set_osd_message("Snapshot loaded (sub-cycle engine)");
  return 0;
}

int snapshot_save_machine(subcycle::Machine& m, const std::string& filename) {
  t_SNA_header sh{};
  memcpy(sh.id, "MV - SNA", sizeof(sh.id));
  sh.version = 3;
  const Z80Regs r = m.regs();
  sh.AF[0] = r.af & 0xFF;
  sh.AF[1] = r.af >> 8;
  sh.BC[0] = r.bc & 0xFF;
  sh.BC[1] = r.bc >> 8;
  sh.DE[0] = r.de & 0xFF;
  sh.DE[1] = r.de >> 8;
  sh.HL[0] = r.hl & 0xFF;
  sh.HL[1] = r.hl >> 8;
  sh.AFx[0] = r.af_ & 0xFF;
  sh.AFx[1] = r.af_ >> 8;
  sh.BCx[0] = r.bc_ & 0xFF;
  sh.BCx[1] = r.bc_ >> 8;
  sh.DEx[0] = r.de_ & 0xFF;
  sh.DEx[1] = r.de_ >> 8;
  sh.HLx[0] = r.hl_ & 0xFF;
  sh.HLx[1] = r.hl_ >> 8;
  sh.IX[0] = r.ix & 0xFF;
  sh.IX[1] = r.ix >> 8;
  sh.IY[0] = r.iy & 0xFF;
  sh.IY[1] = r.iy >> 8;
  sh.SP[0] = r.sp & 0xFF;
  sh.SP[1] = r.sp >> 8;
  sh.PC[0] = r.pc & 0xFF;
  sh.PC[1] = r.pc >> 8;
  sh.I = r.i;
  sh.R = r.r;
  sh.IM = r.im;
  sh.IFF0 = r.iff1;
  sh.IFF1 = r.iff2;
  GateArrayRegs ga{};
  ga_peek(m.gate_array(), &ga);
  memcpy(sh.ga_ink_values, ga.ink, 17);
  sh.ga_pen = ga.pen;
  sh.ga_ROM_config = ga.rom_config;
  sh.ga_RAM_config = ga.ram_config;  // mode rides in rom_config bits 0-1
  CrtcRegs cr{};
  crtc_peek(m.crtc(), &cr);
  memcpy(sh.crtc_registers, cr.reg, 18);
  sh.crtc_reg_select = cr.reg_select;
  MemRegs mr{};
  mem_peek(m.memory(), &mr);
  sh.upper_ROM = mr.rom_select;
  PpiRegs pp{};
  ppi_peek(m.ppi(), &pp);
  sh.ppi_A = pp.portA;
  sh.ppi_B = pp.portB;
  sh.ppi_C = pp.portC;
  sh.ppi_control = pp.control;
  PsgRegs ps{};
  psg_peek(m.psg(), &ps);
  memcpy(sh.psg_registers, ps.reg, 16);
  sh.psg_reg_select = ps.sel;
  sh.cpc_model = static_cast<byte>(CPC.model);
  FdcRegs fr{};
  fdc_peek(m.fdc(), &fr);
  sh.fdc_motor = fr.motor;
  sh.drvA_current_track = fr.track[0];
  sh.drvB_current_track = fr.track[1];
  PrinterRegs prt{};
  printer_peek(m.printer(), &prt);
  sh.printer_data = prt.latch;
  // SNA v3 encodes the CURRENT LEVEL halved into 0..15 (legacy engine writes
  // AmplitudeEnv >> 1), not the segment step — psg_poke_env decodes the same.
  sh.psg_env_step = ps.env_level >> 1;
  sh.psg_env_direction = ps.env_direction;
  sh.crtc_addr[0] = cr.ma & 0xFF;
  sh.crtc_addr[1] = cr.ma >> 8;
  sh.crtc_scanline[0] = cr.scanline & 0xFF;
  sh.crtc_scanline[1] = cr.scanline >> 8;
  sh.crtc_char_count[0] = cr.hcc & 0xFF;
  sh.crtc_line_count = cr.vcc;
  sh.crtc_raster_count = cr.ra;
  sh.crtc_hsw_count = cr.hsw;
  sh.crtc_vsw_count = cr.vsw;
  {
    dword dwFlags = 0;
    if (cr.vsync) dwFlags |= 1;
    if (cr.hsync) dwFlags |= 2;
    if (cr.vta) dwFlags |= 0x80;
    sh.crtc_flags[0] = dwFlags & 0xFF;
    sh.crtc_flags[1] = (dwFlags >> 8) & 0xFF;
  }
  sh.ga_int_delay = ga.hs_count;
  sh.ga_sl_count = ga.sl_count;
  sh.z80_int_pending = ga.irq;
  const size_t ram_bytes = m.ram_size();
  sh.ram_size[0] = (ram_bytes / 1024) & 0xFF;
  sh.ram_size[1] = ((ram_bytes / 1024) >> 8) & 0xFF;

  FILE* f = fopen(filename.c_str(), "wb");
  if (f == nullptr) return ERR_SNA_WRITE;
  if (fwrite(&sh, sizeof(sh), 1, f) != 1) {
    fclose(f);
    return ERR_SNA_WRITE;
  }
  std::vector<uint8_t> ram(ram_bytes);
  for (size_t i = 0; i < ram.size(); i++) ram[i] = m.ram_read(i);
  const int ok = fwrite(ram.data(), ram.size(), 1, f) == 1 ? 0 : ERR_SNA_WRITE;
  if (fclose(f) != 0) return ERR_SNA_WRITE;
  set_osd_message(ok == 0 ? "Snapshot saved (sub-cycle engine)"
                          : "Snapshot save FAILED");
  return ok;
}

int snapshot_load(FILE* pfile) {
  if (subcycle::Machine* m = subcycle_bridge_machine())
    return snapshot_load_machine(*m, pfile);
  return ERR_SNA_INVALID;  // machine not up yet: nothing to restore into
}

int snapshot_load(const std::string& filename) {
  FILE* pfile = fopen(filename.c_str(), "rb");
  if (pfile == nullptr) {
    LOG_ERROR("Error loading snapshot: file not found: '" << filename << "'");
    return ERR_FILE_NOT_FOUND;
  }
  int const rc = snapshot_load(pfile);
  fclose(pfile);
  return rc;
}

int tape_insert(FILE* pfile) {
  tape_eject();
  byte header[10];
  if (fread(header, sizeof(header), 1, pfile) != 1) {
    LOG_ERROR("Error loading tape: couldn't read header");
    return ERR_TAP_INVALID;
  }
  // Rewind so the format-specific loader can recheck the header
  fseek(pfile, 0, SEEK_SET);
  if (memcmp(header, "ZXTape!\032", 8) == 0) {  // CDT file?
    LOG_DEBUG("tape_insert CDT file");
    return tape_insert_cdt(pfile);
  }
  if (memcmp(header, "Creative", 8) == 0) {  // VOC file ?
    LOG_DEBUG("tape_insert VOC file");
    return tape_insert_voc(pfile);
  }
  LOG_ERROR("Error loading tape: Unrecognized file type");
  return ERR_TAP_INVALID;
}

int tape_insert(const std::string& filename) {
  LOG_DEBUG("tape_insert " << filename);
  FILE* pfile;
  if ((pfile = fopen(filename.c_str(), "rb")) == nullptr) {
    LOG_ERROR("Error loading tape: File not found: '" << filename << "'");
    return ERR_FILE_NOT_FOUND;
  }

  int const iRetCode = tape_insert(pfile);
  fclose(pfile);

  return iRetCode;
}

int tape_insert_cdt(FILE* pfile) {
  // Host-side CDT/TZX ingest: keep the RAW block stream (the file body after
  // the 10-byte "ZXTape!" header) in pbTapeImage. The deck Device receives
  // the same bytes (file_load hands it the whole file), so the host block
  // table built by tape_scan_blocks and the deck's block ordinals are walks
  // over identical data — the seek UI depends on that.
  byte header[10];
  if (fread(header, sizeof(header), 1, pfile) != 1) {
    LOG_ERROR("Couldn't read CDT header");
    return ERR_TAP_INVALID;
  }
  if (memcmp(header, "ZXTape!\032", 8) != 0) {
    LOG_ERROR("Invalid CDT header");
    return ERR_TAP_INVALID;
  }
  if (header[0x08] != 1) {  // major version must be 1
    LOG_ERROR("Invalid CDT major version");
    return ERR_TAP_INVALID;
  }
  const long lFileSize = file_size(fileno(pfile)) - 10;
  if (lFileSize <= 0) {
    LOG_ERROR("Invalid CDT file size");
    return ERR_TAP_INVALID;
  }
  pbTapeImage.resize(lFileSize);
  if (fread(pbTapeImage.data(), lFileSize, 1, pfile) != 1) {
    LOG_ERROR("Couldn't read CDT file");
    tape_eject();
    return ERR_TAP_INVALID;
  }
  pbTapeImageEnd = pbTapeImage.data() + lFileSize;
  // Validate: the deck's own block sizing must land exactly on end-of-file.
  {
    const uint32_t len = static_cast<uint32_t>(lFileSize);
    uint32_t pos = 0;
    while (pos < len) {
      const uint32_t sz = tape_cdt_block_len(pbTapeImage.data(), len, pos);
      if (sz == 0 || sz > len - pos) {
        LOG_ERROR("CDT block 0x" << std::hex
                                 << static_cast<int>(pbTapeImage[pos])
                                 << std::dec
                                 << " extends past end of file");
        tape_eject();
        return ERR_TAP_INVALID;
      }
      pos += sz;
    }
  }
  return 0;
}

int tape_insert_voc(FILE* pfile) {
  // .voc rides the clean VOC->TZX converter (voc_import.cpp); the host keeps
  // the converted block stream so the tape UI table matches what the deck
  // plays (file_load runs the same conversion for the deck).
  tape_eject();
  const long lFileSize = file_size(fileno(pfile));
  if (lFileSize <= 0) {
    LOG_ERROR("Reading VOC file: empty file");
    return ERR_TAP_BAD_VOC;
  }
  std::vector<uint8_t> voc(static_cast<size_t>(lFileSize));
  if (fread(voc.data(), voc.size(), 1, pfile) != 1) {
    LOG_ERROR("Reading VOC file: read error");
    return ERR_TAP_BAD_VOC;
  }
  std::vector<uint8_t> tzx = voc_to_tzx(voc.data(), voc.size());
  if (tzx.size() <= 10) {
    LOG_ERROR("Reading VOC file: invalid VOC file");
    return ERR_TAP_BAD_VOC;
  }
  pbTapeImage.assign(tzx.begin() + 10, tzx.end());  // strip the TZX header
  pbTapeImageEnd = pbTapeImage.data() + pbTapeImage.size();
  return 0;
}

void cartridge_load() {
  if (CPC.model >= 3) {
    if (file_load(CPC.cartridge)) {
      fprintf(stderr, "Load of cartridge failed. Aborting.\n");
      cleanExit(-1);
    }
  }
}

int cartridge_load(const std::string& filepath) {
  if (CPC.model >= 3) {
    return cpr_load(filepath);
  }
  LOG_ERROR(
      "Loading cartridge: not supported on the chosen CPC model: CPC.model="
      << CPC.model);
  return ERR_FILE_UNSUPPORTED;
}

int cartridge_load(FILE* file) {
  if (CPC.model >= 3) {
    return cpr_load(file);
  }
  LOG_ERROR(
      "Loading cartridge: not supported on the chosen CPC model: CPC.model="
      << CPC.model);
  return ERR_FILE_UNSUPPORTED;
}

namespace {
std::string drive_extensions(const DRIVE drive) {
  switch (drive) {
    case DRIVE::DSK_A:
      // Flux containers are drive-A-only (side-0/drive-A flux capture).
      return ".dsk.ipf.raw.scp.hfe.a2r";
    case DRIVE::DSK_B:
      return ".dsk.ipf.raw";
    case DRIVE::TAPE:
      return ".cdt.voc";
    case DRIVE::SNAPSHOT:
      return ".sna";
    case DRIVE::CARTRIDGE:
      return ".cpr";
  }
  LOG_ERROR("Unsupported drive type: " << static_cast<int>(drive))
  return "";
}
}  // namespace

// Read a whole file into memory — for mirroring media into the sub-cycle
// engine, which wants the raw image bytes rather than parsed track structures.
namespace {
std::vector<uint8_t> read_file_bytes(FILE* file) {
  std::vector<uint8_t> bytes;
  if (file == nullptr) return bytes;
  uint8_t buf[65536];
  size_t got;
  while ((got = fread(buf, 1, sizeof(buf), file)) > 0)
    bytes.insert(bytes.end(), buf, buf + got);
  fclose(file);
  return bytes;
}
}  // namespace

namespace {
std::vector<uint8_t> read_file_bytes(const std::string& path) {
  return read_file_bytes(fopen(path.c_str(), "rb"));
}
}  // namespace

// Still some duplication there... but it cannot really be helped
int file_load(t_slot& slot) {
  if (slot.file.empty()) {
    // Special casing because this is not an error if called from loadSlots
    LOG_VERBOSE("Ignoring empty filename passed to file_load.")
    return ERR_FILE_NOT_FOUND;
  }
  if (slot.file.length() < 4) {
    LOG_ERROR("File path is too short: '" << slot.file << "'");
    return ERR_FILE_NOT_FOUND;
  }
  int pos = slot.file.length() - 4;
  std::string extension = stringutils::lower(slot.file.substr(pos));

  // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
  FILE *file = nullptr;
  std::string zip_inner;  // inner filename when loading out of a zip
  if (extension == ".zip") {
    zip::t_zip_info zip_info;
    zip_info.filename = slot.file;
    zip_info.extensions = drive_extensions(slot.drive);
    if (zip::dir(&zip_info)) {
      // error or nothing relevant found
      LOG_ERROR("Error opening or parsing zip file " << slot.file);
      return ERR_FILE_UNZIP_FAILED;
    }

    slot.zip_index = slot.zip_index % zip_info.filesOffsets.size();
    std::string const filename = zip_info.filesOffsets[slot.zip_index].first;
    pos = filename.length() - 4;
    extension = stringutils::lower(
        filename.substr(pos));  // grab the extension in lowercases
    LOG_DEBUG("Extracting " << slot.file << ", " << filename << ", "
                            << extension);
    file = extractFile(slot.file, filename, extension);
    zip_inner = filename;
    if (zip_info.filesOffsets.size() > 1) {
      // Give 5s to the user to read the message.
      set_osd_message(
          "Loaded '" + filename + "' - Press Shift+F5 for next file", 5000);
    }
  }

  // Raw image bytes for mirroring into the sub-cycle engine (the parsed
  // t_drive structures consumed the loader's copy).
  auto slot_bytes = [&]() -> std::vector<uint8_t> {
    return zip_inner.empty()
               ? read_file_bytes(slot.file)
               : read_file_bytes(extractFile(slot.file, zip_inner, extension));
  };

  for (const auto& loader : files_loader_list) {
    if (slot.drive != loader.drive || extension != loader.extension) continue;
    // NOLINTNEXTLINE(misc-const-correctness): clang-tidy FP — variable is mutated (out-param/compound-assign/loop/reference)
    int ret = file ? loader.load_from_file(file)
                         : loader.load_from_filename(slot.file);
    if (file) fclose(file);

    // Flux containers only use the loader above to fill the best-effort legacy
    // sector view (disc-tools/DSK-export). The real medium is the flux
    // transcoded below, so a sector-view failure must NOT abort the load — e.g.
    // ipf_load returns an error for a CAPS-encoder IPF, yet a mainstream SPS
    // IPF must still reach flux::to_scp (the clean-room decoder).
    const bool is_flux = extension == ".ipf" || extension == ".raw" ||
                         extension == ".scp" || extension == ".hfe" ||
                         extension == ".a2r";
    if (!subcycle_bridge_active()) return ret;
    if (ret != 0 && !is_flux) return ret;

    if (slot.drive == DRIVE::TAPE) {
      std::vector<uint8_t> bytes = slot_bytes();
      // .voc: the deck speaks TZX — convert (clean-room, voc_import.cpp).
      if (extension == ".voc") bytes = voc_to_tzx(bytes.data(), bytes.size());
      if (!bytes.empty()) subcycle_bridge_insert_tape(std::move(bytes));
    } else if (extension == ".dsk" &&
               (slot.drive == DRIVE::DSK_A || slot.drive == DRIVE::DSK_B)) {
      // Mirror the new disc into the sub-cycle engine (the Z80 thread
      // applies it at the next frame boundary). Re-read the image bytes:
      // the track-structure loader consumed its copy.
      const int unit = (slot.drive == DRIVE::DSK_B) ? 1 : 0;
      std::vector<uint8_t> bytes = slot_bytes();
      if (bytes.empty()) {
        LOG_ERROR("subcycle engine: could not re-read "
                  << slot.file << " for drive " << (unit ? 'B' : 'A'));
      } else {
        subcycle_bridge_insert_media(std::move(bytes), false, unit);
      }
    } else if (is_flux) {
      // The sub-cycle FDC eats flux: run the raw file bytes through the unified
      // content-sniffing dispatcher (flux::to_scp) into an in-memory SCP
      // capture. For .ipf/.raw the loader above ran ipf_load into driveA (the
      // sector view disc-tools/DSK-export need), and to_scp falls back to that
      // mirror only for CAPS-encoder IPFs. Flux is drive-A-only (fdc.cpp
      // sel_media), so drive B keeps the legacy path.
      if (slot.drive == DRIVE::DSK_A) {
        std::vector<uint8_t> raw = slot_bytes();
        std::vector<uint8_t> scp =
            flux::to_scp(raw.data(), raw.size(), extension);
        if (scp.empty()) {
          LOG_ERROR("subcycle engine: could not transcode "
                    << slot.file << " into flux");
          // The flux medium IS the disk for the sub-cycle FDC — no flux, no
          // load. Surface the sector-view error if we had one, else generic.
          return ret != 0 ? ret : ERR_DSK_INVALID;
        }
        subcycle_bridge_insert_media(std::move(scp), true, 0);
        return 0;  // flux transcode is the authority: a successful decode is a
                   // successful load even when the sector-view loader
                   // (ipf_load) failed or was stubbed out in a clean build.
      }
      LOG_INFO(
          "subcycle engine: flux is drive-A-only — IPF/RAW in "
          "drive B stays on the legacy path");
    }
    return ret;
  }
  LOG_ERROR("File format unsupported for " << slot.file);
  return ERR_FILE_UNSUPPORTED;
}
