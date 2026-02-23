/* konCePCja - Amstrad CPC Emulator
   Symbiface II - IDE + RTC + PS/2 Mouse expansion board

   Port map:
   - IDE + RTC: &FD00-&FD3F (port.b.h == 0xFD, port.b.l & 0xC0 == 0x00)
     - IDE regs: port.b.l & 0x38 == 0x08, offset = port.b.l & 0x07
     - IDE alt:  port.b.l & 0x38 == 0x18
     - RTC:      port.b.l & 0x38 == 0x00, addr/data via port.b.l & 0x01
   - PS/2 Mouse: &FBEE (X), &FBEF (Y)
*/

#ifndef SYMBIFACE_H
#define SYMBIFACE_H

#include "types.h"
#include <string>
#include <cstdio>
#include <cstdint>

// ── IDE (ATA PIO) ──────────────────────────────
struct IDE_Device {
   FILE* image = nullptr;
   bool present = false;
   std::string image_path;

   // ATA register file
   uint8_t error = 0;        // read: error, write: features
   uint8_t features = 0;
   uint8_t sector_count = 0;
   uint8_t lba_low = 0;      // sector number
   uint8_t lba_mid = 0;      // cylinder low
   uint8_t lba_high = 0;     // cylinder high
   uint8_t drive_head = 0;   // bit 4: drive select, bits 3-0: head
   uint8_t status = 0;       // bit 7: BSY, bit 6: DRDY, bit 3: DRQ, bit 0: ERR
   uint8_t command = 0;

   // Data transfer state
   uint8_t sector_buf[512] = {};
   int buf_pos = 0;          // current byte position in sector_buf
   bool data_ready = false;  // sector_buf has data for reading
   bool write_pending = false; // sector_buf waiting to be written

   uint32_t total_sectors = 0;  // size in 512-byte sectors
};

// ── RTC (DS12887) ──────────────────────────────
struct SF2_RTC {
   uint8_t address_reg = 0;       // selected register
   uint8_t cmos_ram[50] = {};     // 50 bytes of CMOS NVRAM
};

// ── PS/2 Mouse (Kempston) ──────────────────────
struct SF2_Mouse {
   uint8_t x_counter = 0;    // wrapping 8-bit X position
   uint8_t y_counter = 0;    // wrapping 8-bit Y position
   uint8_t buttons = 0xFF;   // active-high: bit 0=left, bit 1=right, bit 2=middle
};

// ── Master struct ──────────────────────────────
struct Symbiface {
   bool enabled = false;

   IDE_Device ide_master;
   IDE_Device ide_slave;
   int active_drive = 0;  // 0=master, 1=slave

   SF2_RTC rtc;
   SF2_Mouse mouse;
};

extern Symbiface g_symbiface;

void symbiface_reset();
void symbiface_cleanup();

// IDE
byte symbiface_ide_read(byte reg_offset);
void symbiface_ide_write(byte reg_offset, byte val);
bool symbiface_ide_attach(int drive, const std::string& path);
void symbiface_ide_detach(int drive);

// RTC
byte symbiface_rtc_read();
void symbiface_rtc_write_addr(byte val);
void symbiface_rtc_write_data(byte val);

// Mouse
void symbiface_mouse_update(float dx, float dy, uint32_t sdl_buttons);

// I/O dispatch registration
void symbiface_register_io();

#endif
