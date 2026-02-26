#include "symbiface.h"
#include "log.h"
#include <cstring>
#include <ctime>

Symbiface g_symbiface;

// ── Status register bits ────────────────────────
[[maybe_unused]] static constexpr uint8_t ATA_SR_BSY  = 0x80;
static constexpr uint8_t ATA_SR_DRDY = 0x40;
static constexpr uint8_t ATA_SR_DRQ  = 0x08;
static constexpr uint8_t ATA_SR_ERR  = 0x01;

// ── ATA commands ────────────────────────────────
static constexpr uint8_t ATA_CMD_READ_SECTORS  = 0x20;
static constexpr uint8_t ATA_CMD_WRITE_SECTORS = 0x30;
static constexpr uint8_t ATA_CMD_IDENTIFY       = 0xEC;
static constexpr uint8_t ATA_CMD_IDLE_IMMEDIATE = 0xE1;
static constexpr uint8_t ATA_CMD_INIT_PARAMS    = 0x91;

// ── Helpers ─────────────────────────────────────

static IDE_Device& active_ide()
{
   return g_symbiface.active_drive ? g_symbiface.ide_slave : g_symbiface.ide_master;
}

static uint32_t ide_lba(const IDE_Device& dev)
{
   return (static_cast<uint32_t>(dev.drive_head & 0x0F) << 24) |
          (static_cast<uint32_t>(dev.lba_high) << 16) |
          (static_cast<uint32_t>(dev.lba_mid) << 8) |
          dev.lba_low;
}

static void ide_set_string(uint16_t* buf, int word_start, int word_count, const char* str)
{
   // ATA strings are byte-swapped within each word.
   // Track string length to avoid reading past the null terminator.
   int slen = str ? static_cast<int>(strlen(str)) : 0;

   for (int w = 0; w < word_count; w++) {
      int si = w * 2;
      uint8_t c0 = (si < slen) ? static_cast<uint8_t>(str[si]) : 0x20;
      uint8_t c1 = (si + 1 < slen) ? static_cast<uint8_t>(str[si + 1]) : 0x20;
      buf[word_start + w] = (static_cast<uint16_t>(c0) << 8) | c1;
      if (si + 1 >= slen) {
         // Pad rest with spaces
         for (int w2 = w + 1; w2 < word_count; w2++) {
            buf[word_start + w2] = 0x2020;
         }
         break;
      }
   }
}

static void ide_do_identify(IDE_Device& dev)
{
   memset(dev.sector_buf, 0, 512);
   auto* id = reinterpret_cast<uint16_t*>(dev.sector_buf);

   id[0] = 0x0040;  // non-removable, non-magnetic, hard-sectored
   // CHS geometry (approximate)
   uint32_t cyls = dev.total_sectors / (16 * 63);
   if (cyls > 16383) cyls = 16383;
   id[1] = static_cast<uint16_t>(cyls);     // cylinders
   id[3] = 16;                               // heads
   id[6] = 63;                               // sectors per track
   ide_set_string(id, 10, 10, "KONCEPCJA001");  // serial
   ide_set_string(id, 23, 4, "1.00");            // firmware rev
   ide_set_string(id, 27, 20, "konCePCja Virtual CF");  // model
   id[47] = 1;       // max sectors per interrupt (PIO)
   id[49] = 0x0200;  // LBA supported
   id[53] = 0x0001;  // words 54-58 are valid
   id[54] = static_cast<uint16_t>(cyls);
   id[55] = 16;
   id[56] = 63;
   uint32_t chs_secs = cyls * 16 * 63;
   id[57] = static_cast<uint16_t>(chs_secs & 0xFFFF);
   id[58] = static_cast<uint16_t>(chs_secs >> 16);
   id[60] = static_cast<uint16_t>(dev.total_sectors & 0xFFFF);
   id[61] = static_cast<uint16_t>(dev.total_sectors >> 16);

   dev.buf_pos = 0;
   dev.data_ready = true;
   dev.status = ATA_SR_DRDY | ATA_SR_DRQ;
}

static void ide_do_read(IDE_Device& dev)
{
   if (!dev.image) {
      dev.status = ATA_SR_DRDY | ATA_SR_ERR;
      dev.error = 0x04; // abort
      return;
   }
   uint32_t lba = ide_lba(dev);
   if (lba >= dev.total_sectors) {
      dev.status = ATA_SR_DRDY | ATA_SR_ERR;
      dev.error = 0x10; // IDNF
      return;
   }
   fseek(dev.image, lba * 512L, SEEK_SET);
   size_t read = fread(dev.sector_buf, 1, 512, dev.image);
   if (read < 512) memset(dev.sector_buf + read, 0, 512 - read);

   dev.buf_pos = 0;
   dev.data_ready = true;
   dev.status = ATA_SR_DRDY | ATA_SR_DRQ;
}

static void ide_do_write_commit(IDE_Device& dev)
{
   if (!dev.image) {
      dev.status = ATA_SR_DRDY | ATA_SR_ERR;
      dev.error = 0x04;
      return;
   }
   uint32_t lba = ide_lba(dev);
   if (lba >= dev.total_sectors) {
      dev.status = ATA_SR_DRDY | ATA_SR_ERR;
      dev.error = 0x10;
      return;
   }
   fseek(dev.image, lba * 512L, SEEK_SET);
   if (fwrite(dev.sector_buf, 1, 512, dev.image) != 512) {
      LOG_ERROR("Symbiface IDE: write failed at LBA " << lba);
   }
   fflush(dev.image);

   dev.write_pending = false;
   // Advance LBA for multi-sector writes
   dev.sector_count--;
   if (dev.sector_count > 0) {
      uint32_t next = lba + 1;
      dev.lba_low = next & 0xFF;
      dev.lba_mid = (next >> 8) & 0xFF;
      dev.lba_high = (next >> 16) & 0xFF;
      dev.drive_head = (dev.drive_head & 0xF0) | ((next >> 24) & 0x0F);
      dev.buf_pos = 0;
      dev.write_pending = true;
      dev.status = ATA_SR_DRDY | ATA_SR_DRQ;
   } else {
      dev.status = ATA_SR_DRDY;
   }
}

static void ide_execute_command(IDE_Device& dev)
{
   switch (dev.command) {
      case ATA_CMD_IDENTIFY:
         ide_do_identify(dev);
         break;
      case ATA_CMD_READ_SECTORS:
         ide_do_read(dev);
         break;
      case ATA_CMD_WRITE_SECTORS:
         // Prepare to receive data
         dev.buf_pos = 0;
         dev.write_pending = true;
         dev.data_ready = false;
         memset(dev.sector_buf, 0, 512);
         dev.status = ATA_SR_DRDY | ATA_SR_DRQ;
         break;
      case ATA_CMD_IDLE_IMMEDIATE:
         dev.status = ATA_SR_DRDY;
         break;
      case ATA_CMD_INIT_PARAMS:
         dev.status = ATA_SR_DRDY;
         break;
      default:
         LOG_DEBUG("Symbiface IDE: unknown command 0x" << std::hex << static_cast<int>(dev.command));
         dev.status = ATA_SR_DRDY | ATA_SR_ERR;
         dev.error = 0x04; // abort
         break;
   }
}

// ── Public API ──────────────────────────────────

void symbiface_reset()
{
   g_symbiface.active_drive = 0;
   g_symbiface.ide_master.status = g_symbiface.ide_master.present ? ATA_SR_DRDY : 0;
   g_symbiface.ide_master.error = 0;
   g_symbiface.ide_master.buf_pos = 0;
   g_symbiface.ide_master.data_ready = false;
   g_symbiface.ide_master.write_pending = false;
   g_symbiface.ide_slave.status = g_symbiface.ide_slave.present ? ATA_SR_DRDY : 0;
   g_symbiface.ide_slave.error = 0;
   g_symbiface.ide_slave.buf_pos = 0;
   g_symbiface.ide_slave.data_ready = false;
   g_symbiface.ide_slave.write_pending = false;

   g_symbiface.rtc.address_reg = 0;
   g_symbiface.mouse.head = 0;
   g_symbiface.mouse.tail = 0;
   g_symbiface.mouse.last_buttons = 0;
   g_symbiface.mouse.accum_x = 0.0f;
   g_symbiface.mouse.accum_y = 0.0f;
}

void symbiface_cleanup()
{
   symbiface_ide_detach(0);
   symbiface_ide_detach(1);
}

bool symbiface_ide_attach(int drive, const std::string& path)
{
   IDE_Device& dev = drive ? g_symbiface.ide_slave : g_symbiface.ide_master;
   symbiface_ide_detach(drive);

   dev.image = fopen(path.c_str(), "r+b");
   if (!dev.image) {
      // Try creating the file
      dev.image = fopen(path.c_str(), "w+b");
   }
   if (!dev.image) {
      LOG_ERROR("Symbiface IDE: cannot open " << path);
      return false;
   }
   dev.image_path = path;
   dev.present = true;

   // Determine size in sectors
   fseek(dev.image, 0, SEEK_END);
   long size = ftell(dev.image);
   dev.total_sectors = static_cast<uint32_t>(size / 512);
   dev.status = ATA_SR_DRDY;

   LOG_INFO("Symbiface IDE " << drive << ": attached " << path << " (" << dev.total_sectors << " sectors)");
   return true;
}

void symbiface_ide_detach(int drive)
{
   IDE_Device& dev = drive ? g_symbiface.ide_slave : g_symbiface.ide_master;
   if (dev.image) {
      fclose(dev.image);
      dev.image = nullptr;
   }
   dev.present = false;
   dev.image_path.clear();
   dev.status = 0;
   dev.total_sectors = 0;
}

// ── IDE I/O ─────────────────────────────────────

byte symbiface_ide_read(byte reg_offset)
{
   IDE_Device& dev = active_ide();
   if (!dev.present) return 0xFF;

   switch (reg_offset & 0x07) {
      case 0: // Data register
         if (dev.data_ready && dev.buf_pos < 512) {
            byte val = dev.sector_buf[dev.buf_pos++];
            if (dev.buf_pos >= 512) {
               dev.data_ready = false;
               // Multi-sector read: advance and read next
               if (dev.command == ATA_CMD_READ_SECTORS) {
                  dev.sector_count--;
                  if (dev.sector_count > 0) {
                     uint32_t next = ide_lba(dev) + 1;
                     dev.lba_low = next & 0xFF;
                     dev.lba_mid = (next >> 8) & 0xFF;
                     dev.lba_high = (next >> 16) & 0xFF;
                     dev.drive_head = (dev.drive_head & 0xF0) | ((next >> 24) & 0x0F);
                     ide_do_read(dev);
                  } else {
                     dev.status = ATA_SR_DRDY;
                  }
               } else {
                  dev.status = ATA_SR_DRDY;
               }
            }
            return val;
         }
         return 0xFF;
      case 1: return dev.error;
      case 2: return dev.sector_count;
      case 3: return dev.lba_low;
      case 4: return dev.lba_mid;
      case 5: return dev.lba_high;
      case 6: return dev.drive_head;
      case 7: return dev.status;
   }
   return 0xFF;
}

void symbiface_ide_write(byte reg_offset, byte val)
{
   // Drive/head register selects active drive
   if ((reg_offset & 0x07) == 6) {
      g_symbiface.active_drive = (val >> 4) & 1;
   }

   IDE_Device& dev = active_ide();
   if (!dev.present && (reg_offset & 0x07) != 6) return;

   switch (reg_offset & 0x07) {
      case 0: // Data register
         if (dev.write_pending && dev.buf_pos < 512) {
            dev.sector_buf[dev.buf_pos++] = val;
            if (dev.buf_pos >= 512) {
               ide_do_write_commit(dev);
            }
         }
         break;
      case 1: dev.features = val; break;
      case 2: dev.sector_count = val; break;
      case 3: dev.lba_low = val; break;
      case 4: dev.lba_mid = val; break;
      case 5: dev.lba_high = val; break;
      case 6: dev.drive_head = val; break;
      case 7: // Command register
         dev.command = val;
         dev.error = 0;
         ide_execute_command(dev);
         break;
   }
}

// ── RTC ─────────────────────────────────────────

static uint8_t to_bcd(int val) {
   return static_cast<uint8_t>(((val / 10) << 4) | (val % 10));
}

void symbiface_rtc_write_addr(byte val)
{
   g_symbiface.rtc.address_reg = val & 0x3F; // 6-bit address space
}

void symbiface_rtc_write_data(byte val)
{
   uint8_t reg = g_symbiface.rtc.address_reg;
   if (reg >= 14 && reg < 64) {
      // CMOS RAM area (regs 14-63)
      g_symbiface.rtc.cmos_ram[reg - 14] = val;
   }
   // Registers 0-13 are time registers (read-only from host clock perspective)
}

byte symbiface_rtc_read()
{
   uint8_t reg = g_symbiface.rtc.address_reg;

   if (reg < 14) {
      // Time registers — read from host clock
      time_t now = time(nullptr);
      struct tm* t = localtime(&now);

      switch (reg) {
         case 0: return to_bcd(t->tm_sec);       // seconds
         case 2: return to_bcd(t->tm_min);       // minutes
         case 4: return to_bcd(t->tm_hour);      // hours (24h)
         case 6: return static_cast<byte>(t->tm_wday == 0 ? 7 : t->tm_wday); // day of week
         case 7: return to_bcd(t->tm_mday);      // day of month
         case 8: return to_bcd(t->tm_mon + 1);   // month
         case 9: return to_bcd(t->tm_year % 100); // year
         case 10: return 0x26; // Register A: UIP=0, DV=010, RS=0110
         case 11: return 0x02; // Register B: 24h mode, BCD
         case 12: return 0x00; // Register C: no interrupts
         case 13: return 0x80; // Register D: VRT=1 (valid RAM)
         default: return 0;
      }
   } else if (reg < 64) {
      return g_symbiface.rtc.cmos_ram[reg - 14];
   }
   return 0xFF;
}

// ── PS/2 Mouse (multiplexed FIFO protocol) ──────
// CPCWiki SYMBiFACE_II:PS/2_mouse — status byte format:
//   Bits 7-6 (mm): 00=no data, 01=X offset, 10=Y offset, 11=buttons/scroll
//   Bits 5-0 (D):  6-bit payload
// Hardware only sends data that has changed.

static void mouse_fifo_push(SF2_Mouse& m, uint8_t val)
{
   int next = (m.head + 1) % SF2_Mouse::FIFO_SIZE;
   if (next == m.tail) return; // full — drop oldest would complicate, just drop new
   m.fifo[m.head] = val;
   m.head = next;
}

static uint8_t mouse_fifo_pop(SF2_Mouse& m)
{
   if (m.head == m.tail) return 0x00; // empty → mode 00 = no data
   uint8_t val = m.fifo[m.tail];
   m.tail = (m.tail + 1) % SF2_Mouse::FIFO_SIZE;
   return val;
}

void symbiface_mouse_update(float dx, float dy, uint32_t sdl_buttons)
{
   SF2_Mouse& m = g_symbiface.mouse;

   // Accumulate sub-pixel motion from SDL3 float deltas
   m.accum_x += dx;
   m.accum_y += dy;

   // X movement: mode 01, signed 6-bit (-32..+31)
   // SDL: positive xrel = rightward (same as SF2 convention)
   int ix = static_cast<int>(m.accum_x);
   if (ix != 0) {
      m.accum_x -= ix;
      if (ix > 31) ix = 31;
      if (ix < -32) ix = -32;
      mouse_fifo_push(m, 0x40 | (ix & 0x3F));
   }

   // Y movement: mode 10, signed 6-bit (-32..+31)
   // SDL: positive yrel = downward; SF2: positive = upward → negate
   int whole_y = static_cast<int>(m.accum_y);
   if (whole_y != 0) {
      m.accum_y -= whole_y;
      int iy = -whole_y;
      if (iy > 31) iy = 31;
      if (iy < -32) iy = -32;
      mouse_fifo_push(m, 0x80 | (iy & 0x3F));
   }

   // Buttons: mode 11, D[5]=0, D[0-4] = active-high button bits
   // SDL: bit 0=left(1), bit 2=middle(2), bit 1=right(4)
   uint8_t btn = 0;
   if (sdl_buttons & 1) btn |= 0x01; // left
   if (sdl_buttons & 4) btn |= 0x02; // right
   if (sdl_buttons & 2) btn |= 0x04; // middle
   if (btn != m.last_buttons) {
      mouse_fifo_push(m, 0xC0 | (btn & 0x1F));
      m.last_buttons = btn;
   }
}

// ── I/O dispatch registration ──────────────────

#include "io_dispatch.h"

// Port decode: full 16-bit addressing within &FD00-&FD3F.
// CPCWiki SYMBiFACE_II:I/O_Map_Summary (verified against Cyboard clone docs).

static bool symbiface_in_handler_fd(reg_pair port, byte& ret_val)
{
   byte lo = port.b.l;

   // IDE primary registers: &FD08-&FD0F
   if (lo >= 0x08 && lo <= 0x0F) {
      ret_val = symbiface_ide_read(lo - 0x08);
      return true;
   }
   // IDE alternate status: &FD06
   if (lo == 0x06) {
      ret_val = symbiface_ide_read(7); // alt status = same as status
      return true;
   }
   // PS/2 Mouse Status: &FD10 and &FD18 (multiplexed FIFO read)
   if (lo == 0x10 || lo == 0x18) {
      ret_val = mouse_fifo_pop(g_symbiface.mouse);
      return true;
   }
   // RTC data register: &FD14
   if (lo == 0x14) {
      ret_val = symbiface_rtc_read();
      return true;
   }
   return false;
}

static bool symbiface_out_handler_fd(reg_pair port, byte val)
{
   byte lo = port.b.l;

   // IDE primary registers: &FD08-&FD0F
   if (lo >= 0x08 && lo <= 0x0F) {
      symbiface_ide_write(lo - 0x08, val);
      return true;
   }
   // IDE device control (SRST): &FD06
   if (lo == 0x06) {
      if (val & 0x04) symbiface_reset();
      return true;
   }
   // RTC data register: &FD14
   if (lo == 0x14) {
      symbiface_rtc_write_data(val);
      return true;
   }
   // RTC index register: &FD15
   if (lo == 0x15) {
      symbiface_rtc_write_addr(val);
      return true;
   }
   return false;
}

void symbiface_register_io()
{
   io_register_in(0xFD, symbiface_in_handler_fd, &g_symbiface.enabled, "Symbiface II");
   io_register_out(0xFD, symbiface_out_handler_fd, &g_symbiface.enabled, "Symbiface II");
}
