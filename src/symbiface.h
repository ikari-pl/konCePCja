/* konCePCja - Amstrad CPC Emulator
   Symbiface II - IDE + RTC + PS/2 Mouse expansion board

   Port map (from CPCWiki SYMBiFACE_II:I/O_Map_Summary):
   All ports at &FDxx (port.b.h == 0xFD), full 16-bit address decoding.

     &FD06     IDE Alternate Status (read) / Digital Output (write)
     &FD07     IDE Drive Address (read)
     &FD08     IDE Data Register (read/write)
     &FD09     IDE Error (read) / Features (write)
     &FD0A     IDE Sector Count
     &FD0B     IDE Sector Number / LBA Low
     &FD0C     IDE Cylinder Low / LBA Mid
     &FD0D     IDE Cylinder High / LBA High
     &FD0E     IDE Device/Head
     &FD0F     IDE Status (read) / Command (write)
     &FD10     PS/2 Mouse Status (read) — multiplexed FIFO
     &FD14     RTC Data (read/write)
     &FD15     RTC Index (write)
     &FD18     PS/2 Mouse Status (read) — alias of &FD10

   PS/2 Mouse status byte format (from CPCWiki SYMBiFACE_II:PS/2_mouse):
     Bits 7-6 (mm): 00=no data, 01=X offset, 10=Y offset, 11=buttons/scroll
     Bits 5-0 (D):  signed 6-bit offset (modes 01/10), or button/scroll data (mode 11)
     Mode 11, D[5]=0: D[0]=left, D[1]=right, D[2]=middle, D[3]=fwd, D[4]=back
     Mode 11, D[5]=1: D[0-4]=scroll wheel offset (signed)
     Read repeatedly until mm=00 (no more data).
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

// ── PS/2 Mouse (multiplexed FIFO protocol) ───
struct SF2_Mouse {
   static constexpr int FIFO_SIZE = 64;
   uint8_t fifo[FIFO_SIZE] = {};
   int head = 0;             // write position (main thread)
   int tail = 0;             // read position (Z80 I/O read)
   uint8_t last_buttons = 0; // previous button state (for change detection)
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
