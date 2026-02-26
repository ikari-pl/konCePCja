# Peripheral Device Correctness Audit

Systematic review of every emulated peripheral against primary sources (datasheets, official I/O maps, reference implementations).

**Methodology**: Read all peripheral source code, cross-referenced against:
- CPCWiki SYMBiFACE II I/O Map Summary (via Wayback Machine)
- Dallas DS1216 register format (verified against MAME's `timekpr.cpp` constants)
- ATA/ATAPI-1 register file specification
- DS12887 register map and control register encoding
- Cyboard schematic documentation (Symbiface II clone, full 16-bit address decoding)

---

## CRITICAL BUGS

### 1. Symbiface II: PS/2 Mouse at wrong I/O ports

**Files**: `src/symbiface.h:9`, `src/symbiface.cpp:406-411`, `src/symbiface.cpp:437`

**Official I/O Map** (CPCWiki SYMBiFACE_II:I/O_Map_Summary):
```
#FD10 — PS/2 Mouse Status (read)
#FD18 — PS/2 Mouse Status (read)
```

**Our implementation**:
```
#FBEE — Mouse X counter (read)  ← WRONG: not a Symbiface II port
#FBEF — Mouse Y counter (read)  ← WRONG: not a Symbiface II port
```

The `&FBEE`/`&FBEF` addresses are Kempston mouse ports from the ZX Spectrum ecosystem, NOT the Symbiface II. The real Symbiface II presents mouse data at `&FD10` and `&FD18` within its own I/O address space.

**Impact**: CPC software using the Symbiface II PS/2 mouse (e.g., SymbOS mouse driver) reads `&FD10`/`&FD18` — our code doesn't handle those addresses at all, so the mouse appears dead. Our code responds at `&FBEE`/`&FBEF` which no Symbiface II software reads.

**Source**: CPCWiki SYMBiFACE II I/O Map Summary (archived 2020-10-25), Cyboard README (confirms "fully compatible with the original Symbiface II" using "same I/O port addresses")

---

### 2. Symbiface II: RTC at wrong I/O ports

**Files**: `src/symbiface.cpp:399-402`, `src/symbiface.cpp:423-429`

**Official I/O Map**:
```
#FD14 — RTC Data (read/write)
#FD15 — RTC Index (write only)
```

**Our implementation**:
```
#FD00 — RTC address register (write, via sub=0x00, even port)
#FD01 — RTC data register (read/write, via sub=0x00, odd port)
```

Port `&FD14` decodes as `port.b.l & 0x38 = 0x10`, which our handler doesn't match (it only handles `sub == 0x00`, `0x08`, `0x18`). Similarly `&FD15` has `sub = 0x10`.

**Impact**: CPC software accessing the RTC at `&FD14`/`&FD15` gets no response. Our RTC handler sits at `&FD00`/`&FD01` where no Symbiface II software expects it.

---

### 3. Symbiface II: IDE alternate status at wrong port

**Files**: `src/symbiface.cpp:396-398` (IN), `src/symbiface.cpp:420-422` (OUT)

**Official I/O Map**:
```
#FD06 — IDE Alternate Status (read) / IDE Digital Output (write)
#FD07 — IDE Drive Address (read)
```

**Our implementation**: Handles IDE alternate at `sub == 0x18` → ports `&FD18-&FD1F`.

Port `&FD06` decodes as `port.b.l & 0x38 = 0x00`, which in our code goes to the RTC handler, not the IDE alt handler.

**Impact**: Software reading IDE alternate status or writing device control (SRST) at `&FD06` reaches our RTC handler instead. Our IDE alt handler at `&FD18` matches the address where the official mouse status lives.

---

### 4. Symbiface II: Mouse button state never readable

**Files**: `src/symbiface.cpp:373-383`, `src/symbiface.cpp:406-411`

The `symbiface_mouse_update()` function stores button state in `g_symbiface.mouse.buttons`, applying active-low encoding. However, no I/O handler returns this value — only `x_counter` and `y_counter` are exposed. Even at the correct ports, buttons would not be readable.

**Note**: Header comment says "active-high" (`symbiface.h:56`) but code behavior is active-low (0xFF default, bits cleared when pressed). The comment is also wrong.

---

## MODERATE ISSUES

### 5. Multiface II: ROM/RAM mapping is incorrect (known)

**File**: `src/kon_cpc_ja.cpp:395-401`

The TODO comment in the code acknowledges this:
```cpp
// TODO: I think this is why the MF2 doesn't work properly:
// ROM should be loaded R/O at 0x0000-0x1FFF
// Writes should probably be disabled in membank_write
// MF2 also has a RAM (8kB) that should be loaded as R/W at 0x2000-0x3FFF
```

On real hardware:
- MF2 ROM (8K) maps to `&0000-&1FFF` (**read-only**)
- MF2 RAM (8K) maps to `&2000-&3FFF` (**read-write**)

Our code maps both through a single 16K buffer with read AND write access, making the ROM area writable. This is an inherited Caprice32 limitation.

---

### 6. Symbiface II: fwrite return value unchecked

**File**: `src/symbiface.cpp:125`

```cpp
fwrite(dev.sector_buf, 1, 512, dev.image);
```

Per project code conventions (CLAUDE.md): "Check `fwrite`/`fclose`/`fflush` return values — Disk-full or I/O errors are silent if unchecked."

---

## MINOR ISSUES

### 7. AMX Mouse: Sub-pixel motion lost

**File**: `src/amx_mouse.cpp:17-18`

```cpp
g_amx_mouse.accum_x += static_cast<int>(dx);
g_amx_mouse.accum_y += static_cast<int>(dy);
```

SDL3 provides sub-pixel `float` deltas. The `static_cast<int>()` truncates toward zero, losing all motion below 1.0 pixel. For an 8-bit mouse that measures in "mickeys", this is arguably correct behavior (the real AMX Mouse didn't have sub-pixel resolution), but it means very slow mouse movements are completely lost rather than accumulating.

### 8. Magnum Phaser: Very rough approximation

**File**: `src/phazer.cpp:43`

The phaser implementation increments CRTC R17 on every OUT to `&FBFE` when the phaser is not pressed. This is a simplified model inherited from Caprice32 — the real hardware latches the CRTC's beam position when the photosensor detects light. Works for simple phaser games but is not cycle-accurate.

---

## VERIFIED CORRECT

### AmDrum (Cheetah)
- **Port mapping**: `&FFxx` (port.b.h == 0xFF), write-only ✓
- **DAC midpoint**: 128 (unsigned 8-bit) ✓
- **No read handler**: Correct — AmDrum is write-only ✓
- **Mixing**: Via `Level_AmDrum[]` lookup table, same curve as Digiblaster ✓

### SmartWatch (Dobbertin DS1216)
- **Recognition pattern**: `0x5CA33AC55CA33AC5` — matches DS1216 datasheet (C5 3A A3 5C repeated, LSB first) ✓
- **Address line protocol**: A0=data bit, A2=mode (0=pattern write, 1=data read) ✓
- **State machine**: IDLE→MATCHING→READING transitions correct ✓
- **Hours register**: `0x80 | to_bcd(hour)` — bit 7 = 24h mode ✓
- **Day-of-week register**: Bits 4-5 = 0 — OSC=0 means oscillator running (confirmed via MAME `SECONDS_ST` convention: 1=stop, 0=run) ✓
- **Day mapping**: `tm_wday 0(Sun)→7` — DS1216 uses 1-7, Sunday=7 matches Dobbertin driver convention ✓
- **D0 output**: `(rom_byte & 0xFE) | rtc_bit` — replaces only data bit 0 ✓

### AMX Mouse
- **Row 9 bit mapping**: Up=0, Down=1, Left=2, Right=3, Fire2=4, Fire1=5, Fire3=6 ✓
- **Button mapping**: SDL left→Fire2(bit 4), SDL right→Fire1(bit 5) — matches AMX Mouse wiring (left button = fire 2) ✓
- **Mickey protocol**: One mickey consumed per deselect/reselect cycle of row 9 ✓
- **Active-low encoding**: 0xFF = nothing pressed, bits cleared when active ✓

### Symbiface II IDE (primary registers only)
- **Port mapping**: `&FD08-&FD0F` (offset 0-7 via `port.b.l & 0x07` when sub=0x08) ✓
- **Register file**: Data(0), Error/Features(1), SectorCount(2), LBA_Low(3), LBA_Mid(4), LBA_High(5), DriveHead(6), Status/Command(7) ✓
- **LBA construction**: `(drive_head & 0x0F) << 24 | lba_high << 16 | lba_mid << 8 | lba_low` — ATA 28-bit LBA ✓
- **IDENTIFY DEVICE** (0xEC): Word 0=0x0040 (non-removable), word 49=0x0200 (LBA), words 60-61=total sectors, string byte-swapping ✓
- **READ/WRITE SECTORS**: Multi-sector advance, DRQ/BSY protocol ✓
- **Drive selection**: Bit 4 of drive/head register selects master/slave ✓

### DS12887 RTC (register values correct, port address wrong — see bug #2)
- **Time registers**: BCD encoding for seconds(0), minutes(2), hours(4), dow(6), dom(7), month(8), year(9) ✓
- **Register A** (0x26): UIP=0, DV=010 (32.768 KHz), RS=0110 ✓
- **Register B** (0x02): 24h mode, BCD format ✓
- **Register D** (0x80): VRT=1 (valid RAM and time) ✓
- **Address masking**: `& 0x3F` = 6-bit (64 registers) ✓
- **CMOS NVRAM**: Registers 14-63 (50 bytes) read-write ✓

### Digiblaster
- **Mixing**: Via `Level_PP[CPC.printer_port]` lookup table in PSG output ✓
- **DAC curve**: Inverted scale (index 0=loudest, 255=silent) — matches Caprice32 behavior, verified working with Digiblaster software ✓

### Drive/Tape Sounds
- **Procedural generation**: No hardware reference needed — cosmetic audio effects ✓
- **Resampling**: 44100 Hz base rate with ratio-based resampling to target rate ✓
- **Motor/seek/tape**: Independent enable flags with hook-based activation ✓

### I/O Dispatch Framework
- **Architecture**: 256-entry port table indexed by `port.b.h`, max 4 handlers per slot ✓
- **Hook system**: Keyboard read, keyboard line, tape motor, FDC motor hooks ✓
- **Enable flags**: Per-handler enable pointer checked at dispatch time ✓
- **Fast path**: Zero-count check before iterating handlers ✓

### M4 Board
- **Port mapping**: `&FE00` (data accumulate), `&FC00` (execute) ✓
- **Path traversal protection**: `weakly_canonical()` + prefix check against SD root ✓
- **Command protocol**: Matches real M4 firmware command codes (C_OPEN, C_READ, etc.) ✓
- **Response format**: `[status, len_lo, len_hi, data...]` at ROM offset 0x2800 ✓
- **Note**: M4 Board is a modern device with no hardware datasheet — protocol correctness verified against the M4 firmware behavior

---

## Summary

| Peripheral | Status | Issues |
|-----------|--------|--------|
| AmDrum | ✅ Correct | None |
| SmartWatch DS1216 | ✅ Correct | None |
| AMX Mouse | ✅ Correct | Minor: sub-pixel loss |
| Symbiface II IDE | ⚠️ Partial | Alt status at wrong port |
| Symbiface II RTC | ❌ Wrong ports | FD00/FD01 instead of FD14/FD15 |
| Symbiface II Mouse | ❌ Wrong ports | FBEE/FBEF instead of FD10/FD18; buttons unreadable |
| Multiface II | ⚠️ Known issue | ROM/RAM mapping incorrect (TODO in code) |
| Magnum Phaser | ⚠️ Approximation | Inherited from Caprice32 |
| Digiblaster | ✅ Correct | None |
| M4 Board | ✅ Correct | None |
| Drive/Tape Sounds | ✅ Correct | Cosmetic only, no spec |

**Critical findings**: 3 out of 4 Symbiface II sub-devices have incorrect I/O port addresses. The IDE primary register file is the only correct mapping. The PS/2 Mouse, RTC, and IDE alternate status all need to be remapped to match the official Symbiface II I/O Map.
