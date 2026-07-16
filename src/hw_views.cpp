/* hw_views.cpp — definitions for the host-side view buffers (see hw_views.h)
 * plus the TZX block scanner backing the tape UI's block table. */

#include "hw_views.h"

#include <vector>

#include "hw/tape.h"
#include "imgui_state.h"
#include "koncepcja.h"
#include "log.h"

// ---- view instances --------------------------------------------------------

asic_t asic;

// Audio-scope ring the DevTools PSG window draws. (Filling it from the
// sub-cycle PSG's mixer is an open peek-cutover item — the legacy filler
// died with psg.cpp.)
PsgScopeCapture g_psg_scope;

namespace {
byte s_register_page[16 * 1024];
}
byte* pbRegisterPage = s_register_page;

byte bTapeLevel = TAPE_LEVEL_LOW;

// ---- DSK helpers -----------------------------------------------------------

std::string chrn_to_string(unsigned char* chrn) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%d-%d-%d-%d", chrn[0], chrn[1], chrn[2],
           chrn[3]);
  return {buf};
}

void t_sector::setSizes(unsigned int size, unsigned int total_size) {
  size_ = size;
  total_size_ = total_size;
  weak_read_version_ = 0;
  // Multiple stored versions mark a weak/random sector.
  weak_versions_ = (size_ > 0 && size_ <= total_size_) ? total_size_ / size_
                                                       : 1;
  LOG_DEBUG("weak_versions_ = " << weak_versions_ << " for "
                                << chrn_to_string(CHRN));
}

// ---- TZX block scanner -----------------------------------------------------

extern std::vector<byte> pbTapeImage;
extern byte* pbTapeImageEnd;

void tape_scan_blocks() {
  imgui_state.tape_block_offsets.clear();
  imgui_state.tape_current_block = 0;
  if (pbTapeImage.empty()) return;

  byte* base = pbTapeImage.data();
  if (pbTapeImageEnd == nullptr || pbTapeImageEnd <= base) return;
  const uint32_t len = static_cast<uint32_t>(pbTapeImageEnd - base);

  // Walk the block stream with the deck Device's own sizing, so the Nth entry
  // here is the Nth block the deck seeks to.
  uint32_t pos = 0;
  while (pos < len) {
    imgui_state.tape_block_offsets.push_back(base + pos);
    const uint32_t sz = tape_cdt_block_len(base, len, pos);
    if (sz == 0 || sz > len - pos) break;  // truncated/malformed: stop
    pos += sz;
  }
}
