#include "asic_debug.h"
#include "asic.h"
#include "koncepcja.h"

#include <cstdio>
#include <sstream>

extern t_CRTC CRTC;
extern byte *pbRegisterPage;

std::string asic_dump_dma() {
  std::ostringstream out;
  for (int c = 0; c < NB_DMA_CHANNELS; c++) {
    const dma_channel &ch = asic.dma.ch[c];
    char buf[128];
    snprintf(buf, sizeof(buf),
             "ch%d: addr=%04X prescaler=%02X enabled=%d pause=%d loop_count=%d",
             c, ch.source_address, ch.prescaler,
             ch.enabled ? 1 : 0,
             (ch.pause_ticks > 0) ? 1 : 0,
             ch.loops);
    if (c > 0) out << "\n";
    out << buf;
  }
  return out.str();
}

std::string asic_dump_sprites() {
  std::ostringstream out;
  for (int i = 0; i < 16; i++) {
    char buf[64];
    snprintf(buf, sizeof(buf), "spr%d: x=%d y=%d mag_x=%d mag_y=%d",
             i,
             static_cast<int>(asic.sprites_x[i]),
             static_cast<int>(asic.sprites_y[i]),
             static_cast<int>(asic.sprites_mag_x[i]),
             static_cast<int>(asic.sprites_mag_y[i]));
    if (i > 0) out << "\n";
    out << buf;
  }
  return out.str();
}

std::string asic_dump_interrupts() {
  std::ostringstream out;
  char buf[128];

  // Raster interrupt: the PRI scan line is stored in CRTC.interrupt_sl
  // Line 0 means use normal GA interrupts (not enabled)
  snprintf(buf, sizeof(buf), "raster_interrupt: line=%d enabled=%d",
           static_cast<int>(CRTC.interrupt_sl),
           (CRTC.interrupt_sl != 0) ? 1 : 0);
  out << buf;

  // DMA interrupts: per-channel interrupt flags
  snprintf(buf, sizeof(buf), "\ndma_interrupt: ch0=%d ch1=%d ch2=%d",
           asic.dma.ch[0].interrupt ? 1 : 0,
           asic.dma.ch[1].interrupt ? 1 : 0,
           asic.dma.ch[2].interrupt ? 1 : 0);
  out << buf;

  // Interrupt vector
  snprintf(buf, sizeof(buf), "\ninterrupt_vector: %02X",
           asic.interrupt_vector);
  out << buf;

  // DCSR: reconstruct from channel state (enabled bits 0-2, interrupt bits 6-4)
  byte dcsr = 0;
  for (int c = 0; c < NB_DMA_CHANNELS; c++) {
    if (asic.dma.ch[c].enabled) dcsr |= (0x1 << c);
    if (asic.dma.ch[c].interrupt) dcsr |= (0x40 >> c);
  }
  snprintf(buf, sizeof(buf), "\ndcsr: %02X", dcsr);
  out << buf;

  return out.str();
}

std::string asic_dump_palette() {
  constexpr int kPaletteRegisterOffset = 0x2400;
  std::ostringstream out;
  // Palette at register page offset 0x2400 (addr 0x6400 - 0x4000)
  // 32 entries, 2 bytes each: even byte = RB, odd byte = 0G
  // Even byte: high nibble = R, low nibble = B
  // Odd byte: low nibble = G
  // Output as 0GRB (4 hex digits)
  for (int i = 0; i < 32; i++) {
    int offset = kPaletteRegisterOffset + (i * 2);
    byte rb = pbRegisterPage ? pbRegisterPage[offset] : 0;
    byte g  = pbRegisterPage ? pbRegisterPage[offset + 1] : 0;
    int r_val = (rb >> 4) & 0x0F;
    int g_val = g & 0x0F;
    int b_val = rb & 0x0F;
    char buf[16];
    snprintf(buf, sizeof(buf), "%01X%01X%01X%01X", 0, g_val, r_val, b_val);
    if (i > 0) out << " ";
    if (i < 16) {
      out << "pen" << i << "=" << buf;
    } else {
      out << "ink" << (i - 16) << "=" << buf;
    }
  }
  return out.str();
}

std::string asic_dump_all() {
  std::ostringstream out;
  out << "locked=" << (asic.locked ? 1 : 0) << "\n";
  out << "hscroll=" << asic.hscroll << " vscroll=" << asic.vscroll
      << " extend_border=" << (asic.extend_border ? 1 : 0) << "\n";
  out << "[sprites]\n" << asic_dump_sprites() << "\n";
  out << "[dma]\n" << asic_dump_dma() << "\n";
  out << "[interrupts]\n" << asic_dump_interrupts() << "\n";
  out << "[palette]\n" << asic_dump_palette();
  return out.str();
}
