/* hw_views.h — host-side VIEW BUFFERS over the sub-cycle machine (Gate C
 * Wave 1, replacement-ledger).
 *
 * These structs are what the host UI/IPC/debug surfaces read: the bridge (and
 * the media loaders) publish Device truth into them at frame/refresh
 * boundaries. Nothing here executes — the emulation lives in src/hw and
 * src/subcycle. The DSK-geometry types double as the parse target for the
 * host's DSK loader/editor tooling (disc tools operate on the host image; the
 * FDC Device gets the raw bytes). */

#pragma once

#include <cstdint>
#include <string>

#include "types.h"

// ---- DSK geometry / drive view -------------------------------------------

enum : std::uint16_t {
  DSK_BPTMAX = 8192,
  DSK_TRACKMAX = 102,  // max amount that fits in a DSK header
  DSK_SIDEMAX = 2,
  DSK_SECTORMAX = 29  // max amount that fits in a track header
};

typedef struct {
  char id[34];
  char unused1[14];
  unsigned char tracks;
  unsigned char sides;
  unsigned char unused2[2];
  unsigned char track_size[DSK_TRACKMAX * DSK_SIDEMAX];
} t_DSK_header;

typedef struct {
  char id[12];
  char unused1[4];
  unsigned char track;
  unsigned char side;
  unsigned char unused2[2];
  unsigned char bps;
  unsigned char sectors;
  unsigned char gap3;
  unsigned char filler;
  unsigned char sector[DSK_SECTORMAX][8];
} t_track_header;

class t_sector {
 public:
  unsigned char CHRN[4];   // sector ID as recorded
  unsigned char flags[4];  // ST1/ST2 error bits from the image

  void setData(unsigned char* data) { data_ = data; }

  unsigned char* getDataForWrite() { return data_; }

  // Weak/random sectors carry several data versions; reads rotate through
  // them so repeated reads see the on-tape variability.
  unsigned char* getDataForRead() {
    weak_read_version_ = (weak_read_version_ + 1) % weak_versions_;
    return &data_[weak_read_version_ * size_];
  }

  void setSizes(unsigned int size, unsigned int total_size);

  unsigned int getTotalSize() const { return total_size_; }

 private:
  unsigned int size_;               // one version's size in bytes
  unsigned char* data_;             // sector data (all versions, contiguous)
  unsigned int total_size_;         // all versions together
  unsigned int weak_versions_;      // number of stored versions (1 = normal)
  unsigned int weak_read_version_;  // which version the next read returns
};

typedef struct {
  unsigned int sectors;  // sector count for this track
  unsigned int size;     // track size in bytes
  unsigned char* data;   // track data
  t_sector sector[DSK_SECTORMAX];
} t_track;

struct t_drive {
  unsigned int tracks;
  unsigned int current_track;  // head position (bridge-mirrored from the FDC)
  unsigned int sides;
  unsigned int current_side;
  unsigned int current_sector;
  bool altered;  // host image modified (needs write-back)
  unsigned int write_protected;
  unsigned int random_DEs;              // data-error sectors return random data
  unsigned int flipped;                 // access the reverse side
  long ipf_id;                          // CAPS image id when loaded from IPF
  void (*eject_hook)(struct t_drive*);  // eject callback (IPF CAPS unlock)
  t_track track[DSK_TRACKMAX][DSK_SIDEMAX];
};

struct t_disk_format {
  std::string label;
  unsigned int tracks{0};
  unsigned int sides{0};
  unsigned int sectors{0};
  unsigned int sector_size{0};  // N value
  unsigned int gap3_length{0};
  unsigned char filler_byte{0};
  unsigned char sector_ids[2][16]{{}};  // indices: side, sector
};

// "C-H-R-N" formatting for logs and the disc tools.
std::string chrn_to_string(unsigned char* chrn);

// ---- FDC status view constants (koncepcja.h t_FDC fields) -----------------

enum : std::uint8_t { FDC_TO_CPU = 0, CPU_TO_FDC = 1 };

enum : std::uint8_t { CMD_PHASE = 0, EXEC_PHASE = 1, RESULT_PHASE = 2 };

enum : std::uint16_t {
  SKIP_flag = 1,          // skip sectors with DDAM/DAM
  SEEKDRVA_flag = 2,      // drive A seek finished
  SEEKDRVB_flag = 4,      // drive B seek finished
  RNDDE_flag = 8,         // simulate random DE sectors
  OVERRUN_flag = 16,      // data transfer timed out
  SCAN_flag = 32,         // a scan command is active
  SCANFAILED_flag = 64,   // scan mismatch
  STATUSDRVA_flag = 128,  // drive A status changed
  STATUSDRVB_flag = 256   // drive B status changed
};

// ---- Plus ASIC view --------------------------------------------------------

enum : std::uint8_t { NB_DMA_CHANNELS = 3 };

struct dma_channel {
  unsigned int source_address;
  unsigned int loop_address;
  byte prescaler;
  bool enabled;
  bool interrupt;
  int pause_ticks;
  byte tick_cycles;
  int loops;
};

struct dma_t {
  dma_channel ch[NB_DMA_CHANNELS];
};

struct asic_t {
  bool locked;
  int lockSeqPos;

  bool extend_border;
  unsigned int hscroll;
  unsigned int vscroll;
  byte sprites[16][16][16];  // [id][x][y]; palette index + 16, 0 = transparent
  int16_t sprites_x[16];
  int16_t sprites_y[16];
  short int sprites_mag_x[16];
  short int sprites_mag_y[16];

  bool raster_interrupt;
  byte interrupt_vector;

  dma_t dma;
};

// The view instances (filled by the bridge / the loaders).
extern asic_t asic;
// Snapshot of the ASIC register page (&4000-&7FFF while mapped); refreshed
// with the asic view. asic_debug reads the palette out of it.
extern byte* pbRegisterPage;

// ---- Tape line view --------------------------------------------------------

enum : std::uint8_t { TAPE_LEVEL_LOW = 0, TAPE_LEVEL_HIGH = 0x80 };

// Cassette read-data level, mirrored each frame from the deck Device for the
// status-bar scope (RAW view).
extern byte bTapeLevel;

// Walk the loaded TZX/CDT image (pbTapeImage) and rebuild
// imgui_state.tape_block_offsets; resets tape_current_block to 0. Block
// ordinals match the deck Device's own walk (hw/tape.cpp block_len) — the
// seek UI relies on that.
void tape_scan_blocks();
