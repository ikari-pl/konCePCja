/* rs232.cpp — the Amstrad Serial Interface card Device. See
 * docs/hardware/rs232-device.md. */

#include "rs232.h"

#include <cstring>
#include <new>

namespace {

// RR0 status bits (spec §3, legacy-oracle reading)
constexpr uint8_t RR0_RX_AVAILABLE = 0x01;
constexpr uint8_t RR0_TX_EMPTY = 0x04;
constexpr uint8_t RR0_TX_BUFFER_EMPTY = 0x08;
constexpr uint8_t RR0_CTS = 0x20;
// RR1 bits
constexpr uint8_t RR1_ALL_SENT = 0x01;
constexpr uint8_t RR1_FRAMING_ERROR = 0x08;
constexpr uint8_t RR1_OVERRUN = 0x10;

constexpr uint8_t RX_FIFO_SIZE = 3;

struct rs232_state {
  uint8_t plugged = 0;

  // DART channel A (channel B decodes but is unwired on the Amstrad SI:
  // writes stored, reads return 0)
  uint8_t reg_ptr_a = 0;  // WR0 register pointer, snaps back to 0
  uint8_t reg_ptr_b = 0;
  uint8_t wr_a[6] = {0};
  uint8_t wr_b[6] = {0};
  uint8_t rr1_err = 0;  // sticky error bits until Error Reset

  uint8_t rx_fifo[RX_FIFO_SIZE] = {0};
  uint8_t rx_count = 0;

  // TX engine: double-buffered — data-port write loads tx_buf, the shift
  // register frames it onto the wire (spec §5)
  uint8_t tx_buf = 0;
  uint8_t tx_buf_full = 0;
  uint16_t tx_shift = 0;     // frame bits, LSB transmitted first
  uint8_t tx_bits_left = 0;  // 0 = shift register idle
  uint32_t tx_cycle = 0;     // master cycles into the current bit
  uint8_t txd_level = 1;

  // RX engine: start-edge detect, mid-bit sampling (spec §5)
  uint8_t rx_phase = 0;  // 0 idle, 1 start verify, 2 data, 3 stop
  uint8_t rx_shift = 0;
  uint8_t rx_bits = 0;
  uint32_t rx_cycle = 0;
  uint8_t rxd_prev = 1;

  // Intel 8253: counter 0 = baud divisor (spec §4). Reload values are what
  // the line timing derives from; live down-counters are not modeled in V1
  // (mode-3 square wave feeds the DART, not the bus).
  uint16_t reload[3] = {0, 0, 0};
  uint16_t latch[3] = {0, 0, 0};
  uint8_t latched[3] = {0, 0, 0};
  uint8_t ctr_mode[3] = {0, 0, 0};
  uint8_t rl_mode[3] = {3, 3, 3};  // 1=LSB, 2=MSB, 3=LSB-then-MSB
  uint8_t wr_toggle[3] = {0, 0, 0};
  uint8_t rd_toggle[3] = {0, 0, 0};
  uint8_t mode_reg = 0;

  uint8_t access_prev = 0;  // I/O strobe edge detect
  uint8_t out_latch = 0;    // byte driven for the whole read strobe
  void (*host_tx)(uint8_t, void*) = nullptr;
  void* host_tx_ctx = nullptr;
};

rs232_state* self_of(void* self) { return static_cast<rs232_state*>(self); }

// bit_time = divisor × 8 × 16 master cycles (2 MHz PIT clock, DART ×16 —
// spec §4). Divisor 0 behaves as 1.
uint32_t bit_time(const rs232_state* s) {
  uint16_t div = s->reload[0];
  if (div == 0) div = 1;
  return static_cast<uint32_t>(div) * 128u;
}

uint8_t rr0_of(const rs232_state* s) {
  uint8_t v = RR0_CTS;
  if (s->rx_count > 0) v |= RR0_RX_AVAILABLE;
  if (!s->tx_buf_full) v |= RR0_TX_EMPTY | RR0_TX_BUFFER_EMPTY;
  return v;
}

uint8_t rr1_of(const rs232_state* s) {
  uint8_t v = s->rr1_err;
  if (!s->tx_buf_full && s->tx_bits_left == 0) v |= RR1_ALL_SENT;
  return v;
}

void fifo_push(rs232_state* s, uint8_t byte) {
  if (s->rx_count >= RX_FIFO_SIZE) {
    s->rr1_err |= RR1_OVERRUN;  // FIFO full: newest byte lost (spec §3)
    return;
  }
  s->rx_fifo[s->rx_count++] = byte;
}

uint8_t fifo_pop(rs232_state* s) {
  if (s->rx_count == 0) return 0;
  uint8_t const byte = s->rx_fifo[0];
  s->rx_count--;
  for (uint8_t i = 0; i < s->rx_count; i++) s->rx_fifo[i] = s->rx_fifo[i + 1];
  return byte;
}

void chan_a_reset(rs232_state* s) {
  s->reg_ptr_a = 0;
  std::memset(s->wr_a, 0, sizeof(s->wr_a));
  s->rr1_err = 0;
  s->rx_count = 0;
  s->tx_buf_full = 0;
  s->tx_bits_left = 0;
  s->tx_cycle = 0;
  s->txd_level = 1;
  s->rx_phase = 0;
}

// DART control write: WR0 selects a register / issues a command; the next
// write lands there and the pointer snaps back (spec §3).
void dart_ctrl_write(rs232_state* s, bool chan_a, uint8_t val) {
  uint8_t* ptr = chan_a ? &s->reg_ptr_a : &s->reg_ptr_b;
  uint8_t* wr = chan_a ? s->wr_a : s->wr_b;
  if (*ptr == 0) {
    wr[0] = val;
    *ptr = val & 0x07;
    const uint8_t cmd = (val >> 3) & 0x07;
    if (chan_a) {
      if (cmd == 3) chan_a_reset(s);  // channel reset
      if (cmd == 6) s->rr1_err = 0;   // error reset
    }
  } else {
    if (*ptr < 6) wr[*ptr] = val;
    *ptr = 0;
  }
}

uint8_t dart_ctrl_read(rs232_state* s, bool chan_a) {
  if (!chan_a) return 0;  // channel B unwired
  const uint8_t which = s->reg_ptr_a;
  s->reg_ptr_a = 0;
  if (which == 1) return rr1_of(s);
  if (which == 2) return 0;  // RR2 is channel B's vector
  return rr0_of(s);
}

// 8253 write: counters take reload bytes per the programmed access mode;
// the control word ($FBDF) sets mode + access or latches a count.
void pit_write(rs232_state* s, uint8_t offset, uint8_t val) {
  if (offset == 3) {
    const uint8_t sel = (val >> 6) & 0x03;
    if (sel == 3) return;  // read-back (8254 only): ignored
    const uint8_t rl = (val >> 4) & 0x03;
    if (rl == 0) {  // counter latch command
      s->latch[sel] = s->reload[sel];
      s->latched[sel] = 1;
      return;
    }
    s->mode_reg = val;
    s->rl_mode[sel] = rl;
    s->ctr_mode[sel] = (val >> 1) & 0x07;
    s->wr_toggle[sel] = 0;
    s->rd_toggle[sel] = 0;
    return;
  }
  uint16_t* reload = &s->reload[offset];
  switch (s->rl_mode[offset]) {
    case 1:  // LSB only
      *reload = (*reload & 0xFF00u) | val;
      break;
    case 2:  // MSB only
      *reload = static_cast<uint16_t>((*reload & 0x00FFu) | (val << 8));
      break;
    default:  // LSB then MSB
      if (s->wr_toggle[offset] == 0) {
        *reload = (*reload & 0xFF00u) | val;
        s->wr_toggle[offset] = 1;
      } else {
        *reload = static_cast<uint16_t>((*reload & 0x00FFu) | (val << 8));
        s->wr_toggle[offset] = 0;
      }
      break;
  }
}

// The IORQ strobe spans several master cycles: the data byte is computed
// non-destructively every selected cycle, the toggles advance on the access
// edge only.
uint8_t pit_read_peek(const rs232_state* s, uint8_t offset) {
  if (offset == 3) return 0;  // mode register is write-only
  const uint16_t value =
      s->latched[offset] ? s->latch[offset] : s->reload[offset];
  const bool msb = s->rl_mode[offset] == 2 ||
                   (s->rl_mode[offset] == 3 && s->rd_toggle[offset] != 0);
  return static_cast<uint8_t>(msb ? value >> 8 : value & 0xFF);
}

void pit_read_advance(rs232_state* s, uint8_t offset) {
  if (offset == 3) return;
  switch (s->rl_mode[offset]) {
    case 1:
    case 2:
      s->latched[offset] = 0;
      break;
    default:
      if (s->rd_toggle[offset] == 0) {
        s->rd_toggle[offset] = 1;
      } else {
        s->rd_toggle[offset] = 0;
        s->latched[offset] = 0;
      }
      break;
  }
}

void tx_advance(rs232_state* s) {
  if (s->tx_bits_left == 0) {
    if (!s->tx_buf_full) {
      s->txd_level = 1;  // idle mark
      return;
    }
    // Load the shift register: start(0) + 8 data LSB-first + stop(1).
    s->tx_shift = static_cast<uint16_t>((s->tx_buf << 1) | 0x200u);
    s->tx_bits_left = 10;
    s->tx_cycle = 0;
    s->tx_buf_full = 0;  // double buffering: buffer frees immediately
  }
  s->txd_level = s->tx_shift & 1u;
  s->tx_cycle++;
  if (s->tx_cycle >= bit_time(s)) {
    s->tx_cycle = 0;
    s->tx_shift >>= 1;
    s->tx_bits_left--;
    if (s->tx_bits_left == 0) s->txd_level = 1;
  }
}

void rx_advance(rs232_state* s, uint8_t rxd) {
  const uint32_t bt = bit_time(s);
  switch (s->rx_phase) {
    case 0:  // idle: hunt for the start edge
      if (s->rxd_prev && !rxd) {
        s->rx_phase = 1;
        s->rx_cycle = 0;
      }
      break;
    case 1:  // verify the start bit at mid-bit (noise rejection)
      s->rx_cycle++;
      if (s->rx_cycle >= bt / 2) {
        if (!rxd) {
          s->rx_phase = 2;
          s->rx_cycle = 0;
          s->rx_shift = 0;
          s->rx_bits = 0;
        } else {
          s->rx_phase = 0;
        }
      }
      break;
    case 2:  // sample 8 data bits, each one bit-time after the last
      s->rx_cycle++;
      if (s->rx_cycle >= bt) {
        s->rx_cycle = 0;
        s->rx_shift =
            static_cast<uint8_t>((s->rx_shift >> 1) | (rxd ? 0x80u : 0u));
        s->rx_bits++;
        if (s->rx_bits == 8) s->rx_phase = 3;
      }
      break;
    default:  // stop bit
      s->rx_cycle++;
      if (s->rx_cycle >= bt) {
        if (rxd)
          fifo_push(s, s->rx_shift);
        else
          s->rr1_err |= RR1_FRAMING_ERROR;  // stop bit low: framing error
        s->rx_phase = 0;
        s->rx_cycle = 0;
      }
      break;
  }
  s->rxd_prev = rxd;
}

void rs232_tick(void* self, const Bus* __restrict in, Bus* __restrict out) {
  rs232_state* s = self_of(self);
  if (!s->plugged) return;  // wire rests at mark via bus_resting()

  // The wire evolves every master cycle.
  tx_advance(s);
  rx_advance(s, in->serial.rxd ? 1 : 0);
  out->serial.txd = s->txd_level != 0;

  // I/O decode: $FAxx / $FBxx with low byte $DC–$DF (spec §2). One access
  // per strobe (edge-detected, like every port device on this board).
  const bool io = in->cpu.iorq && !in->cpu.m1 && (in->cpu.rd || in->cpu.wr);
  const uint8_t hi = static_cast<uint8_t>(in->cpu.addr >> 8);
  const uint8_t lo = static_cast<uint8_t>(in->cpu.addr & 0xFF);
  const bool sel = io && (hi == 0xFA || hi == 0xFB) && lo >= 0xDC && lo <= 0xDF;
  const bool edge = sel && !s->access_prev;
  s->access_prev = sel;

  if (!sel) return;
  const uint8_t offset = lo - 0xDC;
  const bool dart = hi == 0xFA;

  if (in->cpu.rd) {
    // Mutate on the access edge only; the latched byte is driven for the
    // whole strobe (a control read moves the register pointer — the value
    // must not change mid-strobe).
    if (edge) {
      uint8_t v;
      if (dart) {
        switch (offset) {
          case 0:
            v = fifo_pop(s);
            break;
          case 1:
            v = 0;  // channel B data: unwired
            break;
          default:
            v = dart_ctrl_read(s, offset == 2);
            break;
        }
      } else {
        v = pit_read_peek(s, offset);
        pit_read_advance(s, offset);
      }
      s->out_latch = v;
    }
    out->cpu.data = s->out_latch;
  } else if (edge) {  // write
    if (dart) {
      switch (offset) {
        case 0:
          s->tx_buf = in->cpu.data;
          s->tx_buf_full = 1;
          if (s->host_tx) s->host_tx(in->cpu.data, s->host_tx_ctx);
          break;
        case 1:
          break;  // channel B data: unwired
        default:
          dart_ctrl_write(s, offset == 2, in->cpu.data);
          break;
      }
    } else {
      pit_write(s, offset, in->cpu.data);
    }
  }
}

void rs232_dev_reset(void* self) {
  rs232_state* s = self_of(self);
  const uint8_t plugged = s->plugged;  // a reset is not an unplug
  *s = rs232_state{};
  s->plugged = plugged;
}

size_t rs232_dev_state_size(const void* /*unused*/) {
  return sizeof(rs232_state) + 1;
}
void rs232_save(const void* self, void* buf) {
  uint8_t* b = static_cast<uint8_t*>(buf);
  b[0] = 1;
  std::memcpy(b + 1, self, sizeof(rs232_state));
}
void rs232_load(void* self, const void* buf) {
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  if (b[0] == 1) std::memcpy(self, b + 1, sizeof(rs232_state));
}

}  // namespace

extern "C" {

size_t rs232_state_size(void) { return sizeof(rs232_state); }

Device rs232_init(void* storage) {
  // NOLINTNEXTLINE(misc-const-correctness): pointer is stored in Device::self
  // (void*), cannot be const
  rs232_state* s = new (storage) rs232_state();
  return Device{
      s,          "rs232",   rs232_tick, rs232_dev_reset, rs232_dev_state_size,
      rs232_save, rs232_load};
}

void rs232_peek(const Device* dev, Rs232Regs* out) {
  const rs232_state* s = static_cast<const rs232_state*>(dev->self);
  std::memcpy(out->wr, s->wr_a, sizeof(out->wr));
  out->rr0 = rr0_of(s);
  out->rr1 = rr1_of(s);
  out->divisor = s->reload[0];
  out->fifo_depth = s->rx_count;
  out->tx_busy = s->tx_bits_left != 0 || s->tx_buf_full;
  out->txd = s->txd_level;
  out->rxd = s->rxd_prev;
  out->plugged = s->plugged;
}

void rs232_set_plugged(const Device* dev, int on) {
  static_cast<rs232_state*>(dev->self)->plugged = on ? 1 : 0;
}

int rs232_quiet(const Device* dev) {
  const rs232_state* s = static_cast<const rs232_state*>(dev->self);
  // Idle on both wires: no TX byte shifting (tx_bits_left) or double-buffered
  // (tx_buf_full), and RX not hunting/receiving (rx_phase 0). A held RX FIFO
  // needs no per-cycle work — an iorq read drains it and wakes the device.
  return (s->tx_bits_left == 0 && !s->tx_buf_full && s->rx_phase == 0) ? 1 : 0;
}

void rs232_host_rx(const Device* dev, uint8_t byte) {
  fifo_push(self_of(dev->self), byte);
}

void rs232_set_host_tx(const Device* dev, void (*fn)(uint8_t, void*),
                       void* ctx) {
  rs232_state* s = static_cast<rs232_state*>(dev->self);
  s->host_tx = fn;
  s->host_tx_ctx = ctx;
}

}  // extern "C"
