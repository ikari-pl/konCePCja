/* symbiface.cpp — the Symbiface II Device. See
 * docs/hardware/symbiface-device.md. The golden master is src/symbiface.cpp
 * (ours): the ATA sequencing, IDENTIFY block, DS12887 map and FIFO protocol
 * are mirrored register-for-register; the hw-layer differences are the
 * caller-owned mutable images, the host-fed clock and edge semantics. */

#include "symbiface.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace {

constexpr uint8_t kSrDrdy = 0x40;
constexpr uint8_t kSrDrq = 0x08;
constexpr uint8_t kSrErr = 0x01;

constexpr uint8_t kCmdReadSectors = 0x20;
constexpr uint8_t kCmdWriteSectors = 0x30;
constexpr uint8_t kCmdIdentify = 0xEC;
constexpr uint8_t kCmdIdleImmediate = 0xE1;
constexpr uint8_t kCmdInitParams = 0x91;

constexpr int kFifoSize = 64;

struct ide_drive {
  uint8_t error = 0, features = 0, sector_count = 0;
  uint8_t lba_low = 0, lba_mid = 0, lba_high = 0;
  uint8_t drive_head = 0, status = 0, command = 0;
  uint8_t sector_buf[512] = {0};
  uint16_t buf_pos = 0;
  uint8_t data_ready = 0, write_pending = 0;
};

struct sf2_state {
  ide_drive ide[2];
  uint8_t active_drive = 0;
  uint8_t rtc_index = 0;
  uint8_t rtc_time[10] = {0};  // host-fed clock registers 0..9 (spec §3)
  uint8_t cmos[50] = {0};      // battery-backed NVRAM — serialized
  uint8_t fifo[kFifoSize] = {0};
  uint8_t fifo_head = 0, fifo_tail = 0;
  uint8_t last_buttons = 0;
  uint8_t plugged = 0;
  uint8_t dirty = 0;         // an attached image diverged (spec §2)
  uint8_t out_latch = 0xFF;  // read byte held across one access
  bool access_prev = false;  // an owned I/O access was in flight last tick

  // Live wiring (never serialized): MUST stay the LAST members.
  uint8_t* img[2] = {nullptr, nullptr};
  uint32_t total_sectors[2] = {0, 0};
};

sf2_state* self_of(void* self) { return static_cast<sf2_state*>(self); }

ide_drive* active(sf2_state* f) { return &f->ide[f->active_drive]; }
bool present(const sf2_state* f, int d) { return f->img[d] != nullptr; }

uint32_t ide_lba(const ide_drive* d) {
  return (static_cast<uint32_t>(d->drive_head & 0x0F) << 24) |
         (static_cast<uint32_t>(d->lba_high) << 16) |
         (static_cast<uint32_t>(d->lba_mid) << 8) | d->lba_low;
}

void set_lba(ide_drive* d, uint32_t lba) {
  d->lba_low = lba & 0xFF;
  d->lba_mid = (lba >> 8) & 0xFF;
  d->lba_high = (lba >> 16) & 0xFF;
  d->drive_head =
      static_cast<uint8_t>((d->drive_head & 0xF0) | ((lba >> 24) & 0x0F));
}

// ATA strings are byte-swapped within each 16-bit word, space padded.
void ide_set_string(uint8_t* buf, int word_start, int word_count,
                    const char* str) {
  const int slen = static_cast<int>(std::strlen(str));
  for (int w = 0; w < word_count; w++) {
    const int si = w * 2;
    const uint8_t c0 = (si < slen) ? static_cast<uint8_t>(str[si]) : 0x20;
    const uint8_t c1 =
        (si + 1 < slen) ? static_cast<uint8_t>(str[si + 1]) : 0x20;
    buf[(word_start + w) * 2] = c1;  // little-endian word, high byte = c0
    buf[((word_start + w) * 2) + 1] = c0;
  }
}

void put_word(uint8_t* buf, int word, uint16_t val) {
  buf[word * 2] = static_cast<uint8_t>(val & 0xFF);
  buf[(word * 2) + 1] = static_cast<uint8_t>(val >> 8);
}

void ide_do_identify(sf2_state* f) {
  ide_drive* d = active(f);
  const uint32_t total = f->total_sectors[f->active_drive];
  std::memset(d->sector_buf, 0, 512);
  uint8_t* b = d->sector_buf;
  uint32_t cyls = total / (16 * 63);
  cyls = std::min<uint32_t>(cyls, 16383);
  put_word(b, 0, 0x0040);  // non-removable, hard-sectored
  put_word(b, 1, static_cast<uint16_t>(cyls));
  put_word(b, 3, 16);
  put_word(b, 6, 63);
  ide_set_string(b, 10, 10, "KONCEPCJA001");
  ide_set_string(b, 23, 4, "1.00");
  ide_set_string(b, 27, 20, "konCePCja Virtual CF");
  put_word(b, 47, 1);
  put_word(b, 49, 0x0200);  // LBA supported
  put_word(b, 53, 0x0001);
  put_word(b, 54, static_cast<uint16_t>(cyls));
  put_word(b, 55, 16);
  put_word(b, 56, 63);
  const uint32_t chs = cyls * 16 * 63;
  put_word(b, 57, static_cast<uint16_t>(chs & 0xFFFF));
  put_word(b, 58, static_cast<uint16_t>(chs >> 16));
  put_word(b, 60, static_cast<uint16_t>(total & 0xFFFF));
  put_word(b, 61, static_cast<uint16_t>(total >> 16));
  d->buf_pos = 0;
  d->data_ready = 1;
  d->status = kSrDrdy | kSrDrq;
}

void ide_do_read(sf2_state* f) {
  ide_drive* d = active(f);
  const int n = f->active_drive;
  if (!present(f, n)) {
    d->status = kSrDrdy | kSrErr;
    d->error = 0x04;  // abort
    return;
  }
  const uint32_t lba = ide_lba(d);
  if (lba >= f->total_sectors[n]) {
    d->status = kSrDrdy | kSrErr;
    d->error = 0x10;  // IDNF
    return;
  }
  std::memcpy(d->sector_buf, f->img[n] + (static_cast<size_t>(lba) * 512), 512);
  d->buf_pos = 0;
  d->data_ready = 1;
  d->status = kSrDrdy | kSrDrq;
}

void ide_do_write_commit(sf2_state* f) {
  ide_drive* d = active(f);
  const int n = f->active_drive;
  if (!present(f, n)) {
    d->status = kSrDrdy | kSrErr;
    d->error = 0x04;
    return;
  }
  const uint32_t lba = ide_lba(d);
  if (lba >= f->total_sectors[n]) {
    d->status = kSrDrdy | kSrErr;
    d->error = 0x10;
    return;
  }
  std::memcpy(f->img[n] + (static_cast<size_t>(lba) * 512), d->sector_buf, 512);
  f->dirty = 1;
  d->write_pending = 0;
  d->sector_count--;  // multi-sector chaining, per the golden master
  if (d->sector_count > 0) {
    set_lba(d, lba + 1);
    d->buf_pos = 0;
    d->write_pending = 1;
    d->status = kSrDrdy | kSrDrq;
  } else {
    d->status = kSrDrdy;
  }
}

void ide_execute(sf2_state* f) {
  ide_drive* d = active(f);
  switch (d->command) {
    case kCmdIdentify:
      ide_do_identify(f);
      break;
    case kCmdReadSectors:
      ide_do_read(f);
      break;
    case kCmdWriteSectors:
      d->buf_pos = 0;
      d->write_pending = 1;
      d->data_ready = 0;
      std::memset(d->sector_buf, 0, 512);
      d->status = kSrDrdy | kSrDrq;
      break;
    case kCmdIdleImmediate:
    case kCmdInitParams:
      d->status = kSrDrdy;
      break;
    default:
      d->status = kSrDrdy | kSrErr;
      d->error = 0x04;  // abort
      break;
  }
}

uint8_t ide_read(sf2_state* f, uint8_t reg) {
  ide_drive* d = active(f);
  if (!present(f, f->active_drive)) return 0xFF;
  switch (reg & 0x07) {
    case 0:  // data register
      if (d->data_ready && d->buf_pos < 512) {
        const uint8_t val = d->sector_buf[d->buf_pos++];
        if (d->buf_pos >= 512) {
          d->data_ready = 0;
          if (d->command == kCmdReadSectors) {
            d->sector_count--;
            if (d->sector_count > 0) {
              set_lba(d, ide_lba(d) + 1);
              ide_do_read(f);
            } else {
              d->status = kSrDrdy;
            }
          } else {
            d->status = kSrDrdy;
          }
        }
        return val;
      }
      return 0xFF;
    case 1:
      return d->error;
    case 2:
      return d->sector_count;
    case 3:
      return d->lba_low;
    case 4:
      return d->lba_mid;
    case 5:
      return d->lba_high;
    case 6:
      return d->drive_head;
    case 7:
    default:
      return d->status;
  }
}

void ide_write(sf2_state* f, uint8_t reg, uint8_t val) {
  if ((reg & 0x07) == 6) f->active_drive = (val >> 4) & 1;
  ide_drive* d = active(f);
  if (!present(f, f->active_drive) && (reg & 0x07) != 6) return;
  switch (reg & 0x07) {
    case 0:
      if (d->write_pending && d->buf_pos < 512) {
        d->sector_buf[d->buf_pos++] = val;
        if (d->buf_pos >= 512) ide_do_write_commit(f);
      }
      break;
    case 1:
      d->features = val;
      break;
    case 2:
      d->sector_count = val;
      break;
    case 3:
      d->lba_low = val;
      break;
    case 4:
      d->lba_mid = val;
      break;
    case 5:
      d->lba_high = val;
      break;
    case 6:
      d->drive_head = val;
      break;
    case 7:
    default:
      d->command = val;
      d->error = 0;
      ide_execute(f);
      break;
  }
}

uint8_t rtc_read(sf2_state* f) {
  const uint8_t reg = f->rtc_index;
  if (reg < 10) return f->rtc_time[reg];  // the host-fed clock (spec §3)
  switch (reg) {
    case 10:
      return 0x26;  // A: UIP=0, DV=010, RS=0110
    case 11:
      return 0x02;  // B: 24h, BCD
    case 12:
      return 0x00;  // C: no interrupts
    case 13:
      return 0x80;  // D: VRT (valid RAM)
    default:
      return (reg < 64) ? f->cmos[reg - 14] : 0xFF;
  }
}

void fifo_push(sf2_state* f, uint8_t val) {
  const uint8_t next = static_cast<uint8_t>((f->fifo_head + 1) % kFifoSize);
  if (next == f->fifo_tail) return;  // full: drop the new entry
  f->fifo[f->fifo_head] = val;
  f->fifo_head = next;
}

uint8_t fifo_pop(sf2_state* f) {
  if (f->fifo_head == f->fifo_tail) return 0x00;  // empty: mode 00
  const uint8_t val = f->fifo[f->fifo_tail];
  f->fifo_tail = static_cast<uint8_t>((f->fifo_tail + 1) % kFifoSize);
  return val;
}

// One data-register / FIFO / RTC access, edge-latched. Returns the byte the
// board drives for this access; 0xFF-driving cases still drive (the board
// answers every decoded read).
bool owned_read(sf2_state* f, uint16_t addr, uint8_t* out) {
  const uint8_t lo = addr & 0xFF;
  if (lo >= 0x08 && lo <= 0x0F) {
    *out = ide_read(f, static_cast<uint8_t>(lo - 0x08));
    return true;
  }
  if (lo == 0x06) {  // alternate status: no side effects
    *out = present(f, f->active_drive) ? active(f)->status : 0xFF;
    return true;
  }
  if (lo == 0x10 || lo == 0x18) {
    *out = fifo_pop(f);
    return true;
  }
  if (lo == 0x14) {
    *out = rtc_read(f);
    return true;
  }
  return false;
}

void sf2_board_reset(sf2_state* f) {
  // The golden master's symbiface_reset: register files and FIFO clear;
  // CMOS, the clock and the attachments persist.
  for (int n = 0; n < 2; ++n) {
    ide_drive* d = &f->ide[n];
    d->status = present(f, n) ? kSrDrdy : 0;
    d->error = 0;
    d->buf_pos = 0;
    d->data_ready = 0;
    d->write_pending = 0;
  }
  f->active_drive = 0;
  f->rtc_index = 0;
  f->fifo_head = f->fifo_tail = 0;
  f->last_buttons = 0;
}

void sf2_tick(void* self, const Bus* __restrict in, Bus* __restrict out) {
  sf2_state* f = self_of(self);
  if (!f->plugged) return;  // empty expansion slot

  const bool sel =
      in->cpu.iorq && !in->cpu.m1 && (in->cpu.addr & 0xFF00) == 0xFD00;
  const bool access = sel && (in->cpu.rd || in->cpu.wr);
  const bool edge = access && !f->access_prev;
  f->access_prev = access;
  if (!sel) return;

  const uint8_t lo = in->cpu.addr & 0xFF;
  if (in->cpu.rd) {
    if (edge) {
      uint8_t val = 0xFF;
      if (owned_read(f, in->cpu.addr, &val))
        f->out_latch = val;
      else
        return;  // not our port: stay off the bus
    }
    // Drive the latched byte for the whole access (one side effect each).
    if (lo == 0x06 || (lo >= 0x08 && lo <= 0x0F) || lo == 0x10 || lo == 0x14 ||
        lo == 0x18)
      out->cpu.data = f->out_latch;
  } else if (in->cpu.wr && edge) {
    if (lo >= 0x08 && lo <= 0x0F)
      ide_write(f, static_cast<uint8_t>(lo - 0x08), in->cpu.data);
    else if (lo == 0x06 && (in->cpu.data & 0x04))
      sf2_board_reset(f);  // device control SRST
    else if (lo == 0x14 && f->rtc_index >= 14 && f->rtc_index < 64)
      f->cmos[f->rtc_index - 14] = in->cpu.data;  // time regs are read-only
    else if (lo == 0x15)
      f->rtc_index = in->cpu.data & 0x3F;
  }
}

void sf2_dev_reset(void* self) { sf2_board_reset(self_of(self)); }

// Serialize everything BEFORE the live wiring (spec §5).
constexpr size_t kSaveBytes = offsetof(sf2_state, img);

size_t sf2_dev_state_size(const void* /*unused*/) { return kSaveBytes + 1; }
void sf2_save(const void* self, void* buf) {
  uint8_t* b = static_cast<uint8_t*>(buf);
  b[0] = 1;
  std::memcpy(b + 1, self, kSaveBytes);
}
void sf2_load(void* self, const void* buf) {
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  if (b[0] == 1) std::memcpy(self, b + 1, kSaveBytes);
}

}  // namespace

extern "C" {

size_t sf2_state_size(void) { return sizeof(sf2_state); }

Device sf2_init(void* storage) {
  // NOLINTNEXTLINE(misc-const-correctness): pointer is stored in Device::self (void*), cannot be const
  sf2_state *f = new (storage) sf2_state();
  return Device{
      f,        "symbiface", sf2_tick, sf2_dev_reset, sf2_dev_state_size,
      sf2_save, sf2_load};
}

void sf2_peek(const Device* dev, Sf2Regs* out) {
  sf2_state const* f = static_cast<sf2_state*>(dev->self);
  out->plugged = f->plugged;
  out->active_drive = f->active_drive;
  out->ide_status = f->ide[f->active_drive].status;
  out->rtc_index = f->rtc_index;
  out->fifo_used = static_cast<uint8_t>(
      (f->fifo_head + kFifoSize - f->fifo_tail) % kFifoSize);
}

void sf2_ide_attach(const Device* dev, int drive, uint8_t* img, size_t len) {
  sf2_state* f = static_cast<sf2_state*>(dev->self);
  const int n = drive ? 1 : 0;
  f->img[n] = img;
  f->total_sectors[n] = static_cast<uint32_t>(len / 512);
  f->ide[n].status = kSrDrdy;
  f->ide[n].error = 0;
}

void sf2_ide_detach(const Device* dev, int drive) {
  sf2_state* f = static_cast<sf2_state*>(dev->self);
  const int n = drive ? 1 : 0;
  f->img[n] = nullptr;
  f->total_sectors[n] = 0;
  f->ide[n].status = 0;
}

int sf2_media_dirty(const Device* dev) {
  return static_cast<const sf2_state*>(dev->self)->dirty ? 1 : 0;
}

void sf2_media_mark_clean(const Device* dev) {
  static_cast<sf2_state*>(dev->self)->dirty = 0;
}

void sf2_rtc_set_time(const Device* dev, const uint8_t regs10[10]) {
  std::memcpy(static_cast<sf2_state*>(dev->self)->rtc_time, regs10, 10);
}

void sf2_mouse_feed(const Device* dev, int dx, int dy, uint8_t buttons) {
  sf2_state* f = static_cast<sf2_state*>(dev->self);
  // Chunk large deltas into ±31/32 packets (spec §4). SF2 Y is inverted
  // relative to the host's screen-down-positive convention.
  int x = dx;
  while (x != 0) {
    // NOLINTNEXTLINE(readability-avoid-nested-conditional-operator): nested conditional kept intentionally; no clang-tidy auto-fix
    int const step = x > 31 ? 31 : (x < -32 ? -32 : x);
    fifo_push(f, static_cast<uint8_t>(0x40 | (step & 0x3F)));
    x -= step;
  }
  int y = -dy;
  while (y != 0) {
    // NOLINTNEXTLINE(readability-avoid-nested-conditional-operator): nested conditional kept intentionally; no clang-tidy auto-fix
    int const step = y > 31 ? 31 : (y < -32 ? -32 : y);
    fifo_push(f, static_cast<uint8_t>(0x80 | (step & 0x3F)));
    y -= step;
  }
  uint8_t btn = 0;
  if (buttons & 1) btn |= 0x01;  // left
  if (buttons & 4) btn |= 0x02;  // right (host bit2)
  if (buttons & 2) btn |= 0x04;  // middle (host bit1)
  if (btn != f->last_buttons) {
    fifo_push(f, static_cast<uint8_t>(0xC0 | (btn & 0x1F)));
    f->last_buttons = btn;
  }
}

void sf2_mouse_push_packet(const Device* dev, uint8_t pkt) {
  fifo_push(static_cast<sf2_state*>(dev->self), pkt);
}

void sf2_set_plugged(const Device* dev, int on) {
  static_cast<sf2_state*>(dev->self)->plugged = on ? 1 : 0;
}

}  // extern "C"
