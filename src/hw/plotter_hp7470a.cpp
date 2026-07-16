/* plotter_hp7470a.cpp — the HP 7470A plotter Device. See
 * docs/hardware/plotter-device.md and plotter_hp7470a.h.
 *
 * The HP-GL parser is transplanted logic-for-logic from the legacy
 * src/plotter.cpp HpglPlotter (the parity oracle, acid test 1): the same
 * byte filter (control chars other than CR/LF dropped — ESC sequences fall
 * apart into ignored fragments exactly as they do on the oracle), the same
 * comma-only parameter splitting with prefix float parse, the same handler
 * bodies. Around it: UART engines adapted from rs232.cpp (roles reversed —
 * the plotter receives on serial.txd and transmits on serial.rxd), the
 * 255-byte input buffer, XON/XOFF, and the query responder carried
 * byte-exactly from the legacy PlotterBackend. */

#include "plotter_hp7470a.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

// HP 7470A hard-clip limits (plotter units, A4 landscape) — the legacy
// oracle's constants.
constexpr float PLT_MAX_X = 10365.0f;
constexpr float PLT_MAX_Y = 7962.0f;
constexpr float PLT_UNITS_PER_MM = 40.0f;

constexpr uint32_t PLOTTER_MAX_SEGS = 16384;
constexpr uint16_t INBUF_SIZE = 255;
constexpr uint8_t RESP_SIZE = 32;
constexpr uint16_t CMDBUF_SIZE = 256;
constexpr int MAX_PARAMS = 64;

// Input buffer service period, in byte times (10 bit times each). Two byte
// times per parsed byte — the mechanism (pen motion outpaced by the wire)
// that makes the buffer, ENQ answers, and XON/XOFF real. Deterministic.
constexpr uint32_t DRAIN_BYTE_TIMES = 2;

constexpr uint8_t XON = 0x11;
constexpr uint8_t XOFF = 0x13;
constexpr uint8_t ENQ = 0x05;
constexpr uint16_t XOFF_BELOW_FREE = 32;  // send XOFF when free < 32
constexpr uint16_t XON_ABOVE_FREE = 128;  // send XON when free > 128

struct plotter_state {
  uint8_t plugged = 0;
  uint16_t divisor = 1;  // DIP-fixed; bit time = divisor * 128 cycles

  // UART RX (samples serial.txd — the card's transmit line)
  uint8_t rx_phase = 0;  // 0 idle, 1 start verify, 2 data, 3 stop
  uint8_t rx_shift = 0;
  uint8_t rx_bits = 0;
  uint32_t rx_cycle = 0;
  uint8_t line_prev = 1;

  // UART TX (drives serial.rxd — responses + flow control)
  uint16_t tx_shift = 0;
  uint8_t tx_bits_left = 0;
  uint32_t tx_cycle = 0;
  uint8_t tx_level = 1;

  // 255-byte input buffer (ring)
  uint8_t inbuf[INBUF_SIZE] = {0};
  uint16_t in_head = 0;
  uint16_t in_count = 0;
  uint32_t drain_cnt = 0;  // master cycles until the next byte is serviced
  uint8_t flow_stopped = 0;
  uint8_t flow_pending = 0;  // XON/XOFF byte awaiting the TX line, 0 = none

  // Response ring (OS;/OD;/OI;/ENQ replies)
  uint8_t resp[RESP_SIZE] = {0};
  uint8_t resp_head = 0;
  uint8_t resp_count = 0;

  // Query detector (the legacy PlotterBackend accumulation)
  char qbuf[64] = {0};
  uint8_t qlen = 0;

  // HP-GL parser (the legacy HpglPlotter, flattened)
  char cmd[CMDBUF_SIZE] = {0};
  uint16_t cmd_len = 0;
  char label[CMDBUF_SIZE] = {0};
  uint16_t label_len = 0;
  uint8_t in_label = 0;
  char label_term = 0x03;

  float pen_x = 0, pen_y = 0;
  uint8_t pen_down = 0;
  int8_t selected_pen = 0;
  int8_t line_type = -1;
  float p1_x = 0, p1_y = 0, p2_x = PLT_MAX_X, p2_y = PLT_MAX_Y;
  uint8_t scaling_active = 0;
  float sc_xmin = 0, sc_xmax = 1, sc_ymin = 0, sc_ymax = 1;
  float win_x1 = 0, win_y1 = 0, win_x2 = PLT_MAX_X, win_y2 = PLT_MAX_Y;
  float char_width = 0.187f, char_height = 0.269f;
  float char_dir_run = 1, char_dir_rise = 0;
  float char_slant = 0;

  // The page
  uint32_t page_rev = 0;
  uint8_t page_overflow = 0;
  uint32_t seg_count = 0;
  PlotSeg segs[PLOTTER_MAX_SEGS];
};

plotter_state* self_of(void* self) { return static_cast<plotter_state*>(self); }

uint32_t bit_time(const plotter_state* s) {
  uint16_t div = s->divisor;
  if (div == 0) div = 1;
  return static_cast<uint32_t>(div) * 128u;
}

// ── the page ─────────────────────────────────────────────────────────────

void seg_push(plotter_state* s, const PlotSeg& seg) {
  if (s->seg_count >= PLOTTER_MAX_SEGS) {
    s->page_overflow = 1;  // silent stop at the cap; peek surfaces it
    return;
  }
  s->segs[s->seg_count++] = seg;
  s->page_rev++;
}

// ── coordinate transform (legacy tx/ty) ──────────────────────────────────

float txf(const plotter_state* s, float x) {
  if (!s->scaling_active) return x;
  float const frac = (x - s->sc_xmin) / (s->sc_xmax - s->sc_xmin);
  return s->p1_x + (frac * (s->p2_x - s->p1_x));
}

float tyf(const plotter_state* s, float y) {
  if (!s->scaling_active) return y;
  float const frac = (y - s->sc_ymin) / (s->sc_ymax - s->sc_ymin);
  return s->p1_y + (frac * (s->p2_y - s->p1_y));
}

void move_to(plotter_state* s, float x, float y) {
  if (s->pen_down && s->selected_pen > 0) {
    PlotSeg seg{};
    seg.type = 0;  // Line
    seg.pen = static_cast<uint8_t>(s->selected_pen);
    seg.x1 = s->pen_x;
    seg.y1 = s->pen_y;
    seg.x2 = x;
    seg.y2 = y;
    seg.line_type = s->line_type;
    seg_push(s, seg);
  }
  s->pen_x = x;
  s->pen_y = y;
}

// ── command handlers (legacy bodies, flattened) ──────────────────────────

void cmd_IN(plotter_state* s) {
  s->pen_down = 0;
  s->selected_pen = 0;
  s->pen_x = 0;
  s->pen_y = 0;
  s->line_type = -1;
  s->p1_x = 0;
  s->p1_y = 0;
  s->p2_x = PLT_MAX_X;
  s->p2_y = PLT_MAX_Y;
  s->scaling_active = 0;
  s->win_x1 = 0;
  s->win_y1 = 0;
  s->win_x2 = PLT_MAX_X;
  s->win_y2 = PLT_MAX_Y;
  s->char_width = 0.187f;
  s->char_height = 0.269f;
  s->char_dir_run = 1;
  s->char_dir_rise = 0;
  s->char_slant = 0;
  s->label_term = 0x03;
}

void cmd_DF(plotter_state* s) {
  s->line_type = -1;
  s->scaling_active = 0;
  s->char_width = 0.187f;
  s->char_height = 0.269f;
  s->char_dir_run = 1;
  s->char_dir_rise = 0;
  s->char_slant = 0;
  s->label_term = 0x03;
}

void cmd_SP(plotter_state* s, const float* p, int n) {
  s->pen_down = 0;  // pen lifts during change
  s->selected_pen = (n == 0) ? 0 : static_cast<int8_t>(p[0]);
  if (s->selected_pen < 0 || s->selected_pen > 2) s->selected_pen = 0;
}

void cmd_PU(plotter_state* s, const float* p, int n) {
  s->pen_down = 0;
  for (int i = 0; i + 1 < n; i += 2) {
    s->pen_x = txf(s, p[i]);
    s->pen_y = tyf(s, p[i + 1]);
  }
}

void cmd_PD(plotter_state* s, const float* p, int n) {
  s->pen_down = 1;
  for (int i = 0; i + 1 < n; i += 2) move_to(s, txf(s, p[i]), tyf(s, p[i + 1]));
}

void cmd_PA(plotter_state* s, const float* p, int n) {
  for (int i = 0; i + 1 < n; i += 2) move_to(s, txf(s, p[i]), tyf(s, p[i + 1]));
}

void cmd_PR(plotter_state* s, const float* p, int n) {
  for (int i = 0; i + 1 < n; i += 2) {
    float const dx = s->scaling_active ? (p[i] / (s->sc_xmax - s->sc_xmin)) *
                                             (s->p2_x - s->p1_x)
                                       : p[i];
    float const dy =
        s->scaling_active
            ? (p[i + 1] / (s->sc_ymax - s->sc_ymin)) * (s->p2_y - s->p1_y)
            : p[i + 1];
    move_to(s, s->pen_x + dx, s->pen_y + dy);
  }
}

void cmd_CI(plotter_state* s, const float* p, int n) {
  if (n == 0) return;
  float radius = p[0];
  if (s->scaling_active)
    radius = (radius / (s->sc_xmax - s->sc_xmin)) * (s->p2_x - s->p1_x);
  if (s->selected_pen > 0) {
    PlotSeg seg{};
    seg.type = 1;  // Circle
    seg.pen = static_cast<uint8_t>(s->selected_pen);
    seg.x1 = s->pen_x;
    seg.y1 = s->pen_y;
    seg.radius = std::abs(radius);
    seg.line_type = s->line_type;
    seg_push(s, seg);
  }
}

void cmd_AA(plotter_state* s, const float* p, int n) {
  if (n < 3) return;
  float const cx = txf(s, p[0]);
  float const cy = tyf(s, p[1]);
  float const sweep = p[2];
  float const dx = s->pen_x - cx;
  float const dy = s->pen_y - cy;
  float const radius = std::sqrt((dx * dx) + (dy * dy));
  float const start = std::atan2(dy, dx) * 180.0f / static_cast<float>(M_PI);
  if (s->selected_pen > 0 && radius > 0) {
    PlotSeg seg{};
    seg.type = 2;  // Arc
    seg.pen = static_cast<uint8_t>(s->selected_pen);
    seg.x1 = cx;
    seg.y1 = cy;
    seg.radius = radius;
    seg.start_angle = start;
    seg.sweep_angle = sweep;
    seg.line_type = s->line_type;
    seg_push(s, seg);
  }
  // Move pen to arc endpoint
  float const end_angle = (start + sweep) * static_cast<float>(M_PI) / 180.0f;
  s->pen_x = cx + (radius * std::cos(end_angle));
  s->pen_y = cy + (radius * std::sin(end_angle));
}

void cmd_AR(plotter_state* s, const float* p, int n) {
  if (n < 3) return;
  float abs_p[4] = {s->pen_x + p[0], s->pen_y + p[1], p[2], 0};
  int abs_n = 3;
  if (n > 3) {
    abs_p[3] = p[3];
    abs_n = 4;
  }
  cmd_AA(s, abs_p, abs_n);
}

void cmd_EA(plotter_state* s, const float* p, int n) {
  if (n < 2) return;
  float const x2 = txf(s, p[0]);
  float const y2 = tyf(s, p[1]);
  if (s->selected_pen > 0) {
    float const x1 = s->pen_x;
    float const y1 = s->pen_y;
    auto add_line = [&](float ax, float ay, float bx, float by) {
      PlotSeg seg{};
      seg.type = 0;  // Line
      seg.pen = static_cast<uint8_t>(s->selected_pen);
      seg.x1 = ax;
      seg.y1 = ay;
      seg.x2 = bx;
      seg.y2 = by;
      seg.line_type = s->line_type;
      seg_push(s, seg);
    };
    add_line(x1, y1, x2, y1);
    add_line(x2, y1, x2, y2);
    add_line(x2, y2, x1, y2);
    add_line(x1, y2, x1, y1);
  }
}

void cmd_ER(plotter_state* s, const float* p, int n) {
  if (n < 2) return;
  float abs_p[2] = {s->pen_x + p[0], s->pen_y + p[1]};
  cmd_EA(s, abs_p, 2);
}

void cmd_LT(plotter_state* s, const float* p, int n) {
  s->line_type = (n == 0) ? -1 : static_cast<int8_t>(p[0]);
}

void cmd_LB(plotter_state* s, const char* text, uint16_t len) {
  if (len == 0 || s->selected_pen == 0) return;
  PlotSeg seg{};
  seg.type = 3;  // Label
  seg.pen = static_cast<uint8_t>(s->selected_pen);
  seg.x1 = s->pen_x;
  seg.y1 = s->pen_y;
  seg.line_type = s->line_type;
  uint16_t const copy = len < sizeof(seg.text) - 1
                            ? len
                            : static_cast<uint16_t>(sizeof(seg.text) - 1);
  std::memcpy(seg.text, text, copy);
  seg.text[copy] = 0;
  seg_push(s, seg);
  // Advance pen position by text width (approximate) — full length, as the
  // oracle does, even when the stored text is truncated.
  float const char_w_units = s->char_width * PLT_UNITS_PER_MM * 10.0f;
  s->pen_x += char_w_units * static_cast<float>(len) * s->char_dir_run;
  s->pen_y += char_w_units * static_cast<float>(len) * s->char_dir_rise;
}

void cmd_DT(plotter_state* s, const char* params, uint16_t len) {
  if (len != 0)
    s->label_term = params[0];
  else
    s->label_term = 0x03;
}

void cmd_SI(plotter_state* s, const float* p, int n) {
  if (n >= 2) {
    s->char_width = p[0];
    s->char_height = p[1];
  }
}

void cmd_DI(plotter_state* s, const float* p, int n) {
  if (n >= 2) {
    s->char_dir_run = p[0];
    s->char_dir_rise = p[1];
  }
}

void cmd_SC(plotter_state* s, const float* p, int n) {
  if (n >= 4) {
    float const xmin = p[0];
    float const xmax = p[1];
    float const ymin = p[2];
    float const ymax = p[3];
    // Reject degenerate ranges (division by zero downstream)
    if (xmax == xmin || ymax == ymin) return;
    s->sc_xmin = xmin;
    s->sc_xmax = xmax;
    s->sc_ymin = ymin;
    s->sc_ymax = ymax;
    s->scaling_active = 1;
  } else {
    s->scaling_active = 0;
  }
}

void cmd_IP(plotter_state* s, const float* p, int n) {
  if (n >= 4) {
    s->p1_x = p[0];
    s->p1_y = p[1];
    s->p2_x = p[2];
    s->p2_y = p[3];
  } else {
    s->p1_x = 0;
    s->p1_y = 0;
    s->p2_x = PLT_MAX_X;
    s->p2_y = PLT_MAX_Y;
  }
}

void cmd_IW(plotter_state* s, const float* p, int n) {
  if (n >= 4) {
    s->win_x1 = p[0];
    s->win_y1 = p[1];
    s->win_x2 = p[2];
    s->win_y2 = p[3];
  } else {
    s->win_x1 = 0;
    s->win_y1 = 0;
    s->win_x2 = PLT_MAX_X;
    s->win_y2 = PLT_MAX_Y;
  }
}

// ── parameter parsing (legacy parse_params: comma-split, leading-trim,
//    prefix float parse, invalid tokens skipped) ─────────────────────────

int parse_params(const char* params, uint16_t len, float* out) {
  int n = 0;
  if (len == 0) return 0;
  uint16_t pos = 0;
  while (pos < len) {
    uint16_t end = pos;
    while (end < len && params[end] != ',') end++;
    // Trim leading whitespace, as the oracle's find_first_not_of(" \t")
    uint16_t start = pos;
    while (start < end && (params[start] == ' ' || params[start] == '\t'))
      start++;
    if (start < end && n < MAX_PARAMS) {
      char token[64];
      uint16_t tlen = static_cast<uint16_t>(end - start);
      if (tlen >= sizeof(token)) tlen = sizeof(token) - 1;
      std::memcpy(token, params + start, tlen);
      token[tlen] = 0;
      errno = 0;
      char* endp = nullptr;
      float const v = std::strtof(token, &endp);
      if (endp != token && errno != ERANGE) out[n++] = v;
    }
    pos = static_cast<uint16_t>(end + 1);
  }
  return n;
}

// ── command dispatch (legacy process_command) ────────────────────────────

void process_command(plotter_state* s, const char* cmd, uint16_t len) {
  if (len < 2) return;

  char const m0 =
      static_cast<char>(toupper(static_cast<unsigned char>(cmd[0])));
  char const m1 =
      static_cast<char>(toupper(static_cast<unsigned char>(cmd[1])));
  const char* params = cmd + 2;
  uint16_t const plen = static_cast<uint16_t>(len - 2);
  float p[MAX_PARAMS];

  if (m0 == 'I' && m1 == 'N') {
    cmd_IN(s);
    return;
  }
  if (m0 == 'D' && m1 == 'F') {
    cmd_DF(s);
    return;
  }
  if (m0 == 'L' && m1 == 'B') {
    // Reached only via a ';'-terminated LB (the byte feed normally switches
    // to label mode at the mnemonic) — kept for oracle fidelity.
    s->in_label = 1;
    uint16_t const copy = plen < CMDBUF_SIZE ? plen : CMDBUF_SIZE - 1;
    std::memcpy(s->label, params, copy);
    s->label_len = copy;
    return;
  }
  if (m0 == 'D' && m1 == 'T') {
    cmd_DT(s, params, plen);
    return;
  }

  int const n = parse_params(params, plen, p);

  if (m0 == 'S' && m1 == 'P')
    cmd_SP(s, p, n);
  else if (m0 == 'P' && m1 == 'U')
    cmd_PU(s, p, n);
  else if (m0 == 'P' && m1 == 'D')
    cmd_PD(s, p, n);
  else if (m0 == 'P' && m1 == 'A')
    cmd_PA(s, p, n);
  else if (m0 == 'P' && m1 == 'R')
    cmd_PR(s, p, n);
  else if (m0 == 'C' && m1 == 'I')
    cmd_CI(s, p, n);
  else if (m0 == 'A' && m1 == 'A')
    cmd_AA(s, p, n);
  else if (m0 == 'A' && m1 == 'R')
    cmd_AR(s, p, n);
  else if (m0 == 'E' && m1 == 'A')
    cmd_EA(s, p, n);
  else if (m0 == 'E' && m1 == 'R')
    cmd_ER(s, p, n);
  else if (m0 == 'L' && m1 == 'T')
    cmd_LT(s, p, n);
  else if (m0 == 'S' && m1 == 'I')
    cmd_SI(s, p, n);
  else if (m0 == 'D' && m1 == 'I')
    cmd_DI(s, p, n);
  else if (m0 == 'S' && m1 == 'C')
    cmd_SC(s, p, n);
  else if (m0 == 'I' && m1 == 'P')
    cmd_IP(s, p, n);
  else if (m0 == 'I' && m1 == 'W')
    cmd_IW(s, p, n);
  // Everything else (VS, PT, query commands, ESC fragments) is ignored,
  // as on the oracle.
}

// ── byte feed (legacy feed_byte) ─────────────────────────────────────────

void parser_feed(plotter_state* s, uint8_t byte) {
  char const c = static_cast<char>(byte);

  // Inside a label: accumulate until the terminator (control chars allowed)
  if (s->in_label) {
    if (c == s->label_term) {
      cmd_LB(s, s->label, s->label_len);
      s->label_len = 0;
      s->in_label = 0;
    } else if (s->label_len < CMDBUF_SIZE - 1) {
      s->label[s->label_len++] = c;
    }
    return;
  }

  // ';' terminates a command. Control chars other than CR/LF are dropped —
  // ESC never enters the buffer, so ESC. sequences shed their ESC and the
  // printable remainder parses as unknown mnemonics (ignored). CR/LF are
  // treated as whitespace (skipped). All exactly the oracle's behavior.
  if (c == ';') {
    if (s->cmd_len != 0) {
      process_command(s, s->cmd, s->cmd_len);
      s->cmd_len = 0;
    }
  } else if (c >= 0x20 || c == '\r' || c == '\n') {
    if (c != '\r' && c != '\n') {
      if (s->cmd_len < CMDBUF_SIZE - 1) s->cmd[s->cmd_len++] = c;
      // LB has no ';' terminator — switch to label mode at the mnemonic.
      if (s->cmd_len == 2 &&
          toupper(static_cast<unsigned char>(s->cmd[0])) == 'L' &&
          toupper(static_cast<unsigned char>(s->cmd[1])) == 'B') {
        s->in_label = 1;
        s->label_len = 0;
        s->cmd_len = 0;
      }
    }
  }
}

// ── responses (legacy PlotterBackend, byte-exact) ────────────────────────

void resp_push(plotter_state* s, const char* str) {
  for (const char* c = str; *c; c++) {
    if (s->resp_count >= RESP_SIZE) return;  // ring full: drop (32 is ample)
    s->resp[(s->resp_head + s->resp_count) % RESP_SIZE] =
        static_cast<uint8_t>(*c);
    s->resp_count++;
  }
}

void query_process(plotter_state* s) {
  // Uppercase and strip whitespace for matching, as the oracle does.
  char cmd[64];
  uint8_t len = 0;
  for (uint8_t i = 0; i < s->qlen; i++) {
    char const c = s->qbuf[i];
    if (c >= 'a' && c <= 'z')
      cmd[len++] = static_cast<char>(c - 32);
    else if (static_cast<uint8_t>(c) > ' ')
      cmd[len++] = c;
  }
  if (len < 2 || cmd[0] != 'O') return;
  if (cmd[1] == 'S')
    resp_push(s, "16\r");  // Ready
  else if (cmd[1] == 'D')
    resp_push(s, "0,0,10300,7650\r");  // plottable area
  else if (cmd[1] == 'I')
    resp_push(s, "7470A\r");  // model
}

void query_feed(plotter_state* s, uint8_t byte) {
  char const c = static_cast<char>(byte);
  if (s->qlen < sizeof(s->qbuf)) s->qbuf[s->qlen++] = c;
  if (c == ';' || c == ':' || c == '\r' || c == '\n') {
    query_process(s);
    s->qlen = 0;
  }
  if (s->qlen >= sizeof(s->qbuf)) s->qlen = 0;  // runaway partial command
}

// ── input buffer + flow control ──────────────────────────────────────────

uint16_t in_free(const plotter_state* s) {
  return static_cast<uint16_t>(INBUF_SIZE - s->in_count);
}

void on_receive_byte(plotter_state* s, uint8_t byte) {
  if (byte == ENQ) {
    // Buffer-space query, answered immediately (bypasses the buffer):
    // decimal free space capped at 128 + CR — "128\r" on an idle plotter,
    // the oracle's constant.
    char buf[8];
    unsigned free_now = in_free(s);
    free_now = std::min<unsigned int>(free_now, 128);
    std::snprintf(buf, sizeof(buf), "%u\r", free_now);
    resp_push(s, buf);
    return;
  }
  if (s->in_count < INBUF_SIZE) {
    s->inbuf[(s->in_head + s->in_count) % INBUF_SIZE] = byte;
    s->in_count++;
    if (s->drain_cnt == 0)
      s->drain_cnt = 10u * bit_time(s) * DRAIN_BYTE_TIMES;
  }
  // (a byte arriving at a full buffer is lost — the 7470A's overrun)
  if (!s->flow_stopped && in_free(s) < XOFF_BELOW_FREE) {
    s->flow_stopped = 1;
    s->flow_pending = XOFF;
  }
}

void service_input(plotter_state* s) {
  if (s->in_count == 0) return;
  if (s->drain_cnt > 1) {
    s->drain_cnt--;
    return;
  }
  s->drain_cnt = s->in_count > 1 ? 10u * bit_time(s) * DRAIN_BYTE_TIMES : 0;
  uint8_t const byte = s->inbuf[s->in_head];
  s->in_head = static_cast<uint16_t>((s->in_head + 1) % INBUF_SIZE);
  s->in_count--;
  const uint8_t masked = byte & 0x7F;  // HP-GL is 7-bit ASCII
  parser_feed(s, masked);
  query_feed(s, masked);
  if (s->flow_stopped && in_free(s) > XON_ABOVE_FREE) {
    s->flow_stopped = 0;
    s->flow_pending = XON;
  }
}

// ── UART engines (rs232.cpp bit engines, roles reversed) ─────────────────

void tx_advance(plotter_state* s) {
  if (s->tx_bits_left == 0) {
    uint8_t byte = 0;
    bool have = false;
    if (s->flow_pending != 0) {
      byte = s->flow_pending;
      s->flow_pending = 0;
      have = true;
    } else if (s->resp_count != 0) {
      byte = s->resp[s->resp_head];
      s->resp_head = static_cast<uint8_t>((s->resp_head + 1) % RESP_SIZE);
      s->resp_count--;
      have = true;
    }
    if (!have) {
      s->tx_level = 1;  // idle mark
      return;
    }
    // start(0) + 8 data LSB-first + stop(1)
    s->tx_shift = static_cast<uint16_t>((byte << 1) | 0x200u);
    s->tx_bits_left = 10;
    s->tx_cycle = 0;
  }
  s->tx_level = s->tx_shift & 1u;
  s->tx_cycle++;
  if (s->tx_cycle >= bit_time(s)) {
    s->tx_cycle = 0;
    s->tx_shift >>= 1;
    s->tx_bits_left--;
    if (s->tx_bits_left == 0) s->tx_level = 1;
  }
}

void rx_advance(plotter_state* s, uint8_t line) {
  const uint32_t bt = bit_time(s);
  switch (s->rx_phase) {
    case 0:  // idle: hunt for the start edge
      if (s->line_prev && !line) {
        s->rx_phase = 1;
        s->rx_cycle = 0;
      }
      break;
    case 1:  // verify the start bit at mid-bit
      s->rx_cycle++;
      if (s->rx_cycle >= bt / 2) {
        if (!line) {
          s->rx_phase = 2;
          s->rx_cycle = 0;
          s->rx_shift = 0;
          s->rx_bits = 0;
        } else {
          s->rx_phase = 0;
        }
      }
      break;
    case 2:  // sample 8 data bits
      s->rx_cycle++;
      if (s->rx_cycle >= bt) {
        s->rx_cycle = 0;
        s->rx_shift =
            static_cast<uint8_t>((s->rx_shift >> 1) | (line ? 0x80u : 0u));
        s->rx_bits++;
        if (s->rx_bits == 8) s->rx_phase = 3;
      }
      break;
    default:  // stop bit
      s->rx_cycle++;
      if (s->rx_cycle >= bt) {
        if (line) on_receive_byte(s, s->rx_shift);
        // (stop bit low = framing error: byte discarded)
        s->rx_phase = 0;
        s->rx_cycle = 0;
      }
      break;
  }
  s->line_prev = line;
}

// ── the Device ───────────────────────────────────────────────────────────

void plt_tick(void* self, const Bus* __restrict in, Bus* __restrict out) {
  plotter_state* s = self_of(self);
  if (!s->plugged) return;  // wire rests at mark via bus_resting()

  tx_advance(s);
  rx_advance(s, in->serial.txd ? 1 : 0);
  service_input(s);
  out->serial.rxd = s->tx_level != 0;
}

void plt_dev_reset(void* self) {
  // Power cycle: parser, UART, buffer and flow state reset; the PAGE stays
  // (the sheet is still on the platen), as do plug state and the DIP rate.
  plotter_state* s = self_of(self);
  s->rx_phase = 0;
  s->rx_shift = 0;
  s->rx_bits = 0;
  s->rx_cycle = 0;
  s->line_prev = 1;
  s->tx_shift = 0;
  s->tx_bits_left = 0;
  s->tx_cycle = 0;
  s->tx_level = 1;
  s->in_head = 0;
  s->in_count = 0;
  s->drain_cnt = 0;
  s->flow_stopped = 0;
  s->flow_pending = 0;
  s->resp_head = 0;
  s->resp_count = 0;
  s->qlen = 0;
  s->cmd_len = 0;
  s->label_len = 0;
  s->in_label = 0;
  s->label_term = 0x03;
  s->pen_x = 0;
  s->pen_y = 0;
  s->pen_down = 0;
  s->selected_pen = 0;
  s->line_type = -1;
  s->p1_x = 0;
  s->p1_y = 0;
  s->p2_x = PLT_MAX_X;
  s->p2_y = PLT_MAX_Y;
  s->scaling_active = 0;
  s->win_x1 = 0;
  s->win_y1 = 0;
  s->win_x2 = PLT_MAX_X;
  s->win_y2 = PLT_MAX_Y;
  s->char_width = 0.187f;
  s->char_height = 0.269f;
  s->char_dir_run = 1;
  s->char_dir_rise = 0;
  s->char_slant = 0;
}

size_t plt_dev_state_size(const void* /*unused*/) {
  return sizeof(plotter_state) + 1;
}
void plt_save(const void* self, void* buf) {
  uint8_t* b = static_cast<uint8_t*>(buf);
  b[0] = 1;
  std::memcpy(b + 1, self, sizeof(plotter_state));
}
void plt_load(void* self, const void* buf) {
  const uint8_t* b = static_cast<const uint8_t*>(buf);
  if (b[0] == 1) std::memcpy(self, b + 1, sizeof(plotter_state));
}

}  // namespace

extern "C" {

size_t plotter_hp7470a_state_size(void) { return sizeof(plotter_state); }

Device plotter_hp7470a_init(void* storage) {
  // NOLINTNEXTLINE(misc-const-correctness): pointer is stored in Device::self (void*), cannot be const
  plotter_state *s = new (storage) plotter_state();
  return Device{s,
                "plotter-hp7470a",
                plt_tick,
                plt_dev_reset,
                plt_dev_state_size,
                plt_save,
                plt_load};
}

int plotter_hp7470a_quiet(const Device* dev) {
  const plotter_state* s = static_cast<const plotter_state*>(dev->self);
  // Idle: no TX frame (responses/flow), RX not mid-frame, input ring drained
  // (drain_cnt only runs while in_count > 0), no queued response, no pending
  // flow byte. The pen/parser state is host-observable only at frame boundary,
  // so it needs no per-cycle presence once the input ring is empty.
  return (s->tx_bits_left == 0 && s->rx_phase == 0 && s->in_count == 0 &&
          s->resp_count == 0 && s->flow_pending == 0)
             ? 1
             : 0;
}

void plotter_hp7470a_peek(const Device* dev, PlotterRegs* out) {
  const plotter_state* s = static_cast<const plotter_state*>(dev->self);
  out->pen_x = s->pen_x;
  out->pen_y = s->pen_y;
  out->pen_down = s->pen_down;
  out->selected_pen = s->selected_pen;
  out->buffer_fill = s->in_count;
  out->page_rev = s->page_rev;
  out->page_overflow = s->page_overflow;
  out->flow_stopped = s->flow_stopped;
  out->plugged = s->plugged;
}

size_t plotter_hp7470a_segments(const Device* dev, const PlotSeg** out) {
  const plotter_state* s = static_cast<const plotter_state*>(dev->self);
  *out = s->segs;
  return s->seg_count;
}

void plotter_hp7470a_clear_page(const Device* dev) {
  plotter_state* s = static_cast<plotter_state*>(dev->self);
  s->seg_count = 0;
  s->page_overflow = 0;
  s->page_rev++;
}

void plotter_hp7470a_set_plugged(const Device* dev, int on) {
  static_cast<plotter_state*>(dev->self)->plugged = on ? 1 : 0;
}

void plotter_hp7470a_set_baud_divisor(const Device* dev, uint16_t divisor) {
  static_cast<plotter_state*>(dev->self)->divisor = divisor;
}

}  // extern "C"
