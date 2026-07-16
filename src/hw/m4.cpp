/* m4.cpp — the M4 Board Device. See docs/hardware/m4-device.md.
 *
 * The silicon half of the golden master src/m4board.cpp: a pure conduit. It
 * accumulates command bytes, latches a completed frame to a mailbox and reports
 * busy (the async STM32), and overlays the host-written response/config buffers
 * on reads of the M4 ROM's &E800/&F400 windows — the SmartWatch snoop-and-
 * override pattern. All filesystem/network work is the host's, between frames.
 */

#include "m4.h"

#include <cstring>
#include <new>

namespace {

// Response window at ROM offset 0x2800 -> &E800; config at 0x3400 -> &F400.
constexpr uint16_t kRespBase = 0xE800;
constexpr uint16_t kConfigBase = 0xF400;

struct m4_state {
  uint8_t plugged = 0;
  uint8_t slot = 6;           // upper-ROM slot the M4 ROM answers in
  uint8_t upper_rom_off = 0;  // snooped GA config bit 3 (ROM-enable)
  uint8_t rom_select = 0;     // snooped &DFxx ROM number
  bool io_prev = false;       // previous cycle was an owned I/O write

  // Command protocol.
  uint16_t acc_len = 0;
  uint8_t acc[M4_FRAME_MAX] = {0};
  uint8_t busy = 0;
  M4Pending pending = {};
  uint8_t pending_valid = 0;
  uint16_t last_cmd = 0;

  // Host-written read overlays (device state, serialized). Only the first
  // response_len / config_len bytes overlay the ROM — the rest of each window
  // is the ROM's own content (the golden master memcpy's exactly len bytes).
  uint8_t response[M4_RESPONSE_SIZE] = {0};
  uint16_t response_len = 0;
  uint8_t config[M4_CONFIG_SIZE] = {0};
  uint16_t config_len = 0;

  // Live wiring (never serialized): MUST stay last.
  const uint8_t* rom = nullptr;
  size_t rom_len = 0;
};

m4_state* self_of(void* self) { return static_cast<m4_state*>(self); }

// Latch the accumulated frame for the host (spec §3, step 1). A frame shorter
// than 3 bytes is discarded (m4board_execute, :2591); if the mailbox is still
// full the execute is held — the CPC cannot outrun the coprocessor.
void latch_execute(m4_state* m) {
  if (m->pending_valid) return;  // host hasn't drained the last one yet
  if (m->acc_len >= 3) {
    m->pending.cmd = static_cast<uint16_t>(m->acc[1] | (m->acc[2] << 8));
    m->pending.len = m->acc_len;
    std::memcpy(m->pending.frame, m->acc, m->acc_len);
    m->pending_valid = 1;
    m->busy = 1;
    m->last_cmd = m->pending.cmd;
  }
  m->acc_len = 0;  // the accumulator drains on every execute (:2783)
}

// The M4 ROM is paged into &C000-&FFFF right now.
bool rom_selected(const m4_state* m) {
  return m->rom != nullptr && !m->upper_rom_off && m->rom_select == m->slot;
}

void m4_tick(void* self, const Bus* __restrict in, Bus* __restrict out) {
  m4_state* m = self_of(self);
  if (!m->plugged) return;

  // Snoop the ROM enable + ROM select the memory Device latches, so the Device
  // knows when its ROM (and its overlay windows) are visible (spec §2).
  const bool io_wr = in->cpu.iorq && !in->cpu.m1 && in->cpu.wr;
  if (io_wr) {
    if ((in->cpu.addr & 0xC000) == 0x4000 && (in->cpu.data >> 6) == 2)
      m->upper_rom_off = (in->cpu.data >> 3) & 1;  // GA mode/ROM config
    if ((in->cpu.addr & 0x2000) == 0)
      m->rom_select = in->cpu.data;  // &DFxx upper-ROM select
  }

  // Command ports, edge-latched (one byte / one execute per access, spec §1).
  const bool cmd_wr = io_wr && (in->cpu.addr & 0xFF00) == 0xFE00 &&
                      (in->cpu.addr & 0xFF) == 0x00;
  const bool exec_wr = io_wr && (in->cpu.addr & 0xFF00) == 0xFC00;
  const bool owned = cmd_wr || exec_wr;
  const bool edge = owned && !m->io_prev;
  m->io_prev = owned;
  if (edge) {
    if (cmd_wr) {
      if (m->acc_len < M4_FRAME_MAX) m->acc[m->acc_len++] = in->cpu.data;
    } else {
      latch_execute(m);
    }
  }

  // Read overlays on the M4 ROM's windows (spec §2): drive the response/config
  // byte under romdis, keeping the caller ROM image immutable.
  if (in->cpu.mreq && in->cpu.rd && !in->cpu.rfsh && rom_selected(m)) {
    const uint16_t a = in->cpu.addr;
    if (m->busy && a == kRespBase) {
      // Busy sentinel (beads-315e): while the coprocessor is working, the
      // status byte at &E800 reads 0xFF ("not ready"). The M4 ROM polls here
      // until it flips; m4_complete_response then clears busy and writes the
      // real status (0x00 OK / 0xFF error) into the window. Takes precedence
      // over any stale prior response still sitting under response_len.
      out->cpu.romdis = true;
      out->cpu.data = 0xFF;
    } else if (a >= kRespBase && a - kRespBase < m->response_len) {
      out->cpu.romdis = true;
      out->cpu.data = m->response[a - kRespBase];
    } else if (a >= kConfigBase && a - kConfigBase < m->config_len) {
      out->cpu.romdis = true;
      out->cpu.data = m->config[a - kConfigBase];
    }
  }
}

void m4_dev_reset(void* self) {
  m4_state* m = self_of(self);
  // m4board_reset (:2556): drain the protocol; slot/plugged/ROM persist.
  m->acc_len = 0;
  m->busy = 0;
  m->pending_valid = 0;
  m->pending = M4Pending{};
  m->last_cmd = 0;
  std::memset(m->response, 0, sizeof(m->response));
  m->response_len = 0;
  std::memset(m->config, 0, sizeof(m->config));
  m->config_len = 0;
  m->io_prev = false;
}

// Serialize everything BEFORE the live ROM wiring (spec §4).
constexpr size_t kSaveBytes = offsetof(m4_state, rom);

size_t m4_dev_state_size(const void* /*unused*/) { return kSaveBytes + 1; }
void m4_save(const void* self, void* buf) {
  uint8_t* b = static_cast<uint8_t*>(buf);
  b[0] = 1;
  std::memcpy(b + 1, self, kSaveBytes);
}
void m4_load(void* self, const void* buf) {
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  if (b[0] == 1) std::memcpy(self, b + 1, kSaveBytes);
}

}  // namespace

extern "C" {

size_t m4_state_size(void) { return sizeof(m4_state); }

Device m4_init(void* storage) {
  // NOLINTNEXTLINE(misc-const-correctness): pointer is stored in Device::self
  // (void*), cannot be const
  m4_state* m = new (storage) m4_state();
  return Device{m,       "m4",   m4_tick, m4_dev_reset, m4_dev_state_size,
                m4_save, m4_load};
}

void m4_peek(const Device* dev, M4Regs* out) {
  const m4_state* m = static_cast<const m4_state*>(dev->self);
  out->plugged = m->plugged;
  out->slot = m->slot;
  out->busy = m->busy;
  out->cmd_count = m->acc_len;
  out->response_len = m->response_len;
  out->last_cmd = m->last_cmd;
}

void m4_attach_rom(const Device* dev, const uint8_t* rom16k, size_t len) {
  m4_state* m = static_cast<m4_state*>(dev->self);
  m->rom = rom16k;
  m->rom_len = len;
}

void m4_set_slot(const Device* dev, int slot) {
  static_cast<m4_state*>(dev->self)->slot = static_cast<uint8_t>(slot & 0xFF);
}

void m4_set_plugged(const Device* dev, int on) {
  static_cast<m4_state*>(dev->self)->plugged = on ? 1 : 0;
}

int m4_pending_command(const Device* dev, M4Pending* out) {
  m4_state* m = static_cast<m4_state*>(dev->self);
  if (!m->pending_valid) return 0;
  if (out != nullptr) *out = m->pending;
  m->pending_valid = 0;
  return 1;
}

void m4_complete_response(const Device* dev, const uint8_t* buf, uint16_t len) {
  m4_state* m = static_cast<m4_state*>(dev->self);
  const uint16_t cap = static_cast<uint16_t>(M4_RESPONSE_SIZE);
  const uint16_t n = len < cap ? len : cap;
  std::memset(m->response, 0, sizeof(m->response));
  if (buf != nullptr && n > 0) std::memcpy(m->response, buf, n);
  m->response_len = n;
  m->busy = 0;  // the coprocessor answered; the CPC may read the window
}

void m4_write_config(const Device* dev, const uint8_t* buf, uint16_t len) {
  m4_state* m = static_cast<m4_state*>(dev->self);
  const uint16_t cap = static_cast<uint16_t>(M4_CONFIG_SIZE);
  const uint16_t n = len < cap ? len : cap;
  if (buf != nullptr && n > 0) std::memcpy(m->config, buf, n);
  m->config_len = n;
}

}  // extern "C"
