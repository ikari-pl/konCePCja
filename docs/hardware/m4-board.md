# M4 Board

Hardware reference for the M4 Board expansion card by Duke (spinpoint.org).
This describes the real hardware protocol, not the emulator's implementation.

The M4 Board is an expansion card that provides WiFi, a ROM board, a
real-time clock, and an SD card slot for mass storage. It connects via
the CPC's expansion bus.

## Architecture

```
                    ┌─────────────────────────┐
                    │      M4 Board           │
  CPC expansion ───┤                         │
  bus              │  STM32 Cortex-M MCU     ├─── microSD card (FAT32)
                    │  ┌──────────────────┐   │
  &FE00 (data) ────→│  │ Command buffer   │   │
  &FC00 (execute) ──→│  │ (accumulates     │   ├─── ESP8266 WiFi module
                    │  │  OUT bytes)      │   │
  ROM overlay ──────┤  │                  │   ├─── Flash (ROM storage)
  &C000-&FFFF      │  │ Response buffer  │   │
  (upper ROM)       │  │ (at &E800 in     │   ├─── RTC
                    │  │  ROM address     │   │
                    │  │  space)          │   │
                    │  └──────────────────┘   │
                    └─────────────────────────┘
```

## I/O Port Protocol

The M4 uses two I/O ports for command/response communication:

| Port  | Direction | Purpose                                |
|-------|-----------|----------------------------------------|
| &FE00 | OUT       | Data port — accumulates command bytes  |
| &FC00 | OUT       | Execute port — triggers command         |

### Command flow

1. Z80 writes command bytes one at a time to &FE00 (OUT &FE00, byte)
2. Z80 writes to &FC00 to trigger execution (OUT &FC00, 0)
3. M4 processes the command and writes response into ROM overlay
4. Z80 reads response from the ROM address space (&E800+)

### Command buffer format

```
Offset 0:  Size prefix (payload length, not including self)
Offset 1:  Command code low byte
Offset 2:  Command code high byte
Offset 3+: Command-specific data
```

Commands are 16-bit values in the &43xx range (high byte always &43).

### Response buffer format

The response is written into the upper ROM overlay memory at offset
&2800 within the ROM slot (maps to &E800 in CPC address space when the
M4's ROM is paged in).

```
Offset 0:  Status (0x00 = OK, 0xFF = error)
Offset 1:  (varies by command)
Offset 2:  (varies by command)
Offset 3+: Command-specific response data
```

The ROM source references `rom_response` as the base of this buffer.
The link table at &FF02 in the ROM points to &E800.

### Two command send paths

The M4 ROM uses two different methods to send commands:

**`send_command_iy`** — Standard path. Builds the command in a workspace
buffer pointed to by IY, then sends bytes via OUTs to &FE00 followed
by a trigger OUT to &FC00. Used by most commands.

**`send_command2`** — Write path. Used by `fwrite` and commands that send
large data payloads. Sends command bytes directly via OUTs without
using the workspace buffer. The CPC's output data goes directly into
the OUT stream.

## Command Reference

All command codes have the high byte &43.

### File Operations

| Code   | Name           | Data                                     | Response                              |
|--------|----------------|------------------------------------------|---------------------------------------|
| 0x4301 | C_OPEN         | [mode] [filename\0]                      | [fd] [fatfs_error]                    |
| 0x4302 | C_READ         | [fd] [count_lo] [count_hi]               | [status] [data...]                    |
| 0x4303 | C_WRITE        | [fd] [data...]                           | [status]                              |
| 0x4304 | C_CLOSE        | [fd]                                     | [status]                              |
| 0x4305 | C_SEEK         | [fd] [off0] [off1] [off2] [off3]         | [status]                              |
| 0x4307 | C_EOF          | [fd]                                     | [0=not eof, else eof]                 |
| 0x430A | C_FTELL        | [fd]                                     | [pos0] [pos1] [pos2] [pos3]          |
| 0x4311 | C_FSIZE        | [fd]                                     | [size0] [size1] [size2] [size3]       |
| 0x4312 | C_READ2        | [fd] [count_lo] [count_hi]               | [status] [len_lo] [len_hi] [..] [data...] |

### Directory Operations

| Code   | Name           | Data                                     | Response                              |
|--------|----------------|------------------------------------------|---------------------------------------|
| 0x4306 | C_READDIR      | (none, or [max_name_len])                | [dir_entry] (loop until size==2)      |
| 0x4308 | C_CD           | [dirname\0]                              | [status]                              |
| 0x4309 | C_FREE         | (none)                                   | ["\r\n%iK free\r\n\r\n"]             |
| 0x430E | C_ERASEFILE    | [filename\0]                             | [status]                              |
| 0x430F | C_RENAME       | [newname\0oldname\0]                     | [status] [ascii_error]                |
| 0x4310 | C_MAKEDIR      | [dirname\0]                              | [status]                              |
| 0x4313 | C_GETPATH      | (none)                                   | [ascii_path]                          |
| 0x4316 | C_FSTAT        | [path\0]                                 | [err] [size4] [date2] [time2] [attr] [name] |
| 0x4325 | C_DIRSETARGS   | [pattern\0]                              | (none, sets filter for C_READDIR)     |

### Disc Image Operations

| Code   | Name            | Data                                    | Response                              |
|--------|-----------------|-----------------------------------------|---------------------------------------|
| 0x430B | C_READSECTOR    | [track] [sector] [drive]                | [status] [512 bytes]                  |
| 0x430C | C_WRITESECTOR   | [track] [sector] [drive] [512 bytes]    | [status]                              |
| 0x430D | C_FORMATTRACK   | (not implemented)                       | —                                     |

### Network Operations

| Code   | Name           | Data                                     | Response                              |
|--------|----------------|------------------------------------------|---------------------------------------|
| 0x4320 | C_HTTPGET      | [url:port/file\0]                        | ["Downloaded..."]                     |
| 0x4321 | C_SETNETWORK   | [setup_string\0]                         | (none)                                |
| 0x4323 | C_NETSTAT      | (none)                                   | [status_string] [status_byte]         |
| 0x4324 | C_TIME         | (none)                                   | ["hh:mm:ss yyyy-mm-dd"]              |
| 0x4328 | C_HTTPGETMEM   | [size_hi] [size_lo] [url\0]              | [dl_size_hi] [dl_size_lo]             |

### Socket Operations (v1.0.9+)

| Code   | Name           | Data                                     | Response                              |
|--------|----------------|------------------------------------------|---------------------------------------|
| 0x4331 | C_NETSOCKET    | [domain] [type] [protocol]               | [socket_num or 0xFF]                  |
| 0x4332 | C_NETCONNECT   | [socket] [ip0-3] [port_hi] [port_lo]    | [0=OK, 0xFF=error]                   |
| 0x4333 | C_NETCLOSE     | [socket]                                 | (none)                                |
| 0x4334 | C_NETSEND      | [socket] [size_lo] [size_hi] [data...]   | [0=OK, 0xFF=error]                   |
| 0x4335 | C_NETRECV      | [socket] [size_lo] [size_hi]             | [0] [actual_lo] [actual_hi] [data...] |
| 0x4336 | C_NETHOSTIP    | [hostname\0]                             | [1=lookup in progress]                |

### Raw SD Card Access (v1.0.9+)

| Code   | Name           | Data                                     | Response                              |
|--------|----------------|------------------------------------------|---------------------------------------|
| 0x4314 | C_SDREAD       | [lba0-3] [num_sectors]                   | [error] [sector_data...]              |
| 0x4315 | C_SDWRITE      | [lba0-3] [num_sectors] [data...]         | [error]                               |

### ROM Management

| Code   | Name           | Data                                     | Response                              |
|--------|----------------|------------------------------------------|---------------------------------------|
| 0x43FD | C_ROMWRITE     | [slot] [data...]                         | (none)                                |
| 0x432B | C_ROMSUPDATE   | (none)                                   | (none — applies romconfig changes)    |

### System

| Code   | Name           | Data                                     | Response                              |
|--------|----------------|------------------------------------------|---------------------------------------|
| 0x4326 | C_VERSION      | (none)                                   | [M4 version string] [ESP version]     |
| 0x4322 | C_M4OFF        | (none)                                   | (disables M4 ROM and resets)          |
| 0x43FE | C_CONFIG       | [offset] [config_data...]                | [status]                              |

## C_OPEN: File access modes

The mode byte in C_OPEN uses **FatFs access flags**, not simple read/write
values:

| Flag              | Value | Meaning                              |
|-------------------|-------|--------------------------------------|
| FA_READ           | 0x01  | Open for reading                     |
| FA_WRITE          | 0x02  | Open for writing                     |
| FA_CREATE_NEW     | 0x04  | Create new file (fail if exists)     |
| FA_CREATE_ALWAYS  | 0x08  | Create new file (truncate if exists) |
| FA_OPEN_ALWAYS    | 0x10  | Open existing or create new          |
| FA_OPEN_APPEND    | 0x30  | Open for append                      |

Common combinations:
- **LOAD/RUN**: mode = 0x01 (FA_READ)
- **SAVE**: mode = 0x0A (FA_WRITE | FA_CREATE_ALWAYS)

### Error response format

The error byte in C_OPEN's response (offset 4) is a **FatFs return code
index**, not a CPC error code. The M4 ROM translates it via a lookup table
(`ff_error_map`):

| FatFs code | Name              | CPC error (after lookup) |
|------------|-------------------|--------------------------|
| 0          | FR_OK             | 0x00 (success)           |
| 1          | FR_DISK_ERR       | 0xFF                     |
| 2          | FR_INT_ERR        | 0xFF                     |
| 3          | FR_NOT_READY      | 0xFF                     |
| 4          | FR_NO_FILE        | 0x92 (file not found)    |
| 5          | FR_NO_PATH        | 0xFF                     |
| 6          | FR_INVALID_NAME   | 0xFF                     |
| 7          | FR_DENIED         | 0x92                     |
| 8          | FR_EXIST          | 0x91 (file exists)       |
| 9          | FR_INVALID_OBJECT | 0xFF                     |
| 10         | FR_WRITE_PROT     | 0x92                     |
| 11-19      | (other FatFs)     | 0xFF                     |

**Bug trap**: Putting a CPC error code (like 0x92 = 146) directly into
response[4] causes the ROM to index past the 20-entry table, reading
garbage memory. Always use FatFs codes 0-19.

## C_READ vs. C_READ2

These two commands both read file data, but have different response
formats and purposes:

### C_READ (0x4302) — Used by `fread()` in the ROM

Called by `_cas_in_direct` (LOAD/RUN for binary files) and other bulk
read operations. The ROM's `fread` function reads in chunks of up to
0x800 (2048) bytes.

```
Command: [fd] [count_lo] [count_hi]
Response:
  Offset 3: Status (0 = OK, non-zero = error)
  Offset 4+: Raw data bytes (count bytes)

ROM reads: LD HL, #rom_response+3 → check status → INC HL → LDIR
```

The ROM's `fread` wrapper also checks for and skips the 128-byte AMSDOS
header on the first read of a file.

### C_READ2 (0x4312) — Used by `_cas_in_char()`

Added in firmware v1.0.7. Used for byte-by-byte BASIC file reading
(the CAS IN CHAR vector at &BC80). Fills a buffer that `_cas_in_char`
reads from one byte at a time.

```
Command: [fd=1] [0x00] [0x08] (always requests 2048 bytes)
Response:
  Offset 3: Status (0x00 = OK, 0x14 = EOF)
  Offset 4-5: Bytes actually read (16-bit LE)
  Offset 6-7: (unused)
  Offset 8+: Data bytes

ROM reads: response[4-5] = size, copies from rom_response+8
```

Key difference: **C_READ2 does NOT check for or skip the AMSDOS header.**
It returns raw file data. This is necessary because by the time
`_cas_in_char` is called, the header has already been read and consumed
by `_cas_in_open`.

## Hardcoded file descriptors

The M4 ROM **ignores** the fd returned from C_OPEN for CAS (cassette/file
system) operations. Instead it hardcodes:

| Operation        | File descriptor | ROM code                          |
|------------------|----------------|-----------------------------------|
| All CAS input    | fd = 1          | `ld a, #1` in _cas_in_*          |
| All CAS output   | fd = 2          | `ld a, #2` in _cas_out_*         |

This means:
- The fd in C_OPEN response[3] is stored but never used for CAS ops
- If you open for reading, the file must be at handle 1
- If you open for writing, the file must be at handle 2
- General file access (non-CAS) may use any handle 0-3

**Bug trap**: Returning fd=0 from C_OPEN for a read operation means
subsequent C_READ commands arrive with fd=1 but the file is at slot 0.

## CAS jumpblock patching

When `|SD` is typed, the M4 ROM's `set_SDdrive` function patches all 13
CAS (cassette/file system) firmware vectors:

| Vector | Address | Name              | Patched to              |
|--------|---------|-------------------|-------------------------|
| 0      | &BC77   | CAS_INITIALISE    | RST &18 (far-call M4)   |
| 1      | &BC7A   | CAS_SET_SPEED     | RST &18                 |
| 2      | &BC7D   | CAS_NOISY         | RST &18                 |
| 3      | &BC80   | CAS_START_MOTOR   | RST &18                 |
| 4      | &BC83   | CAS_STOP_MOTOR    | RST &18                 |
| 5      | &BC86   | CAS_RESTORE_MOTOR | RST &18                 |
| 6      | &BC89   | CAS_IN_OPEN       | RST &18                 |
| 7      | &BC8C   | CAS_IN_CLOSE      | RST &18                 |
| 8      | &BC8F   | CAS_IN_ABANDON    | RST &18                 |
| 9      | &BC92   | CAS_IN_CHAR       | RST &18                 |
| 10     | &BC95   | CAS_IN_DIRECT     | RST &18                 |
| 11     | &BC98   | CAS_RETURN        | RST &18                 |
| 12     | &BC9B   | CAS_TEST_EOF      | RST &18                 |

Each patched entry becomes: `DF xx xx` (RST &18, followed by a 2-byte
far-call address into the M4 ROM).

### ROM initialisation order problem

On CPC 6128, upper ROMs initialise from **lowest slot to highest**
(slot 0 → slot 31). If the M4 ROM is in slot 6 and AMSDOS is in slot 7:

1. M4 ROM inits (slot 6): patches CAS vectors with `set_SDdrive`
2. AMSDOS ROM inits (slot 7): **overwrites** M4's patches with its own

Result: After boot, CAS vectors point to AMSDOS, not M4. The user must
type `|SD` to re-patch them.

If the M4 ROM is in slot 7 (replacing AMSDOS), it inits last and its
patches stick. But then there's no floppy disc access.

**Slot 6 is the recommended default** for CPC 6128 — it allows both M4
and AMSDOS to coexist, with `|SD` and `|DISC` to switch between them.

## RSX Commands

The M4 ROM registers these RSX (Resident System Extension) commands:

| Command     | Purpose                                        |
|-------------|------------------------------------------------|
| \|SD        | Redirect CAS to M4 SD card                    |
| \|DISC      | Redirect CAS to AMSDOS floppy drive            |
| \|TAPE      | Redirect CAS to tape                           |
| \|CD        | Change directory on SD card                    |
| \|DIR       | List directory with wildcards                  |
| \|LS        | List directory with long filenames             |
| \|ERA       | Erase file or folder                           |
| \|REN       | Rename file/folder or move between directories |
| \|MKDIR     | Create directory                               |
| \|COPYF     | Copy file (SD card only)                       |
| \|FCP       | Copy between SD card and floppy drive          |
| \|DSKX      | Extract files from DSK image                   |
| \|SNA       | Start a snapshot image                         |
| \|CTR       | Execute cartridge (CPC Plus only)              |
| \|CTRUP     | Upload cartridge to M4 flash                   |
| \|HTTPGET   | Download file from internet                    |
| \|HTTPMEM   | Download to memory                             |
| \|ROMUP     | Upload ROM to slot                             |
| \|ROMSET    | Enable/disable ROM slot                        |
| \|ROMUPD    | Apply ROM changes without reboot               |
| \|ROMSOFF   | Disable ROMs (except one, optional)            |
| \|M4ROMOFF  | Disable M4 ROM until next power cycle          |
| \|NETSET    | Configure WiFi settings                        |
| \|NETSTAT   | Show network status                            |
| \|VERSION   | Show firmware version                          |
| \|UPGRADE   | Download and apply firmware update              |
| \|TIME      | Show RTC time                                  |
| \|GETPATH   | Show current SD card path                      |
| \|LONGNAME  | Show full LFN for 8.3 filename                 |
| \|UDIR      | Internal use                                   |
| \|M4HELP    | Show ROM commands                              |
| \|WIFI      | Enable/disable WiFi (0=off, 1=on)              |

## File loading sequence: SAVE

```
1. BASIC calls CAS_OUT_OPEN (&BC8C)
   → M4: C_OPEN, mode=0x0A (FA_WRITE|FA_CREATE_ALWAYS), filename
   ← Response: fd=2, error=0 (FR_OK)

2. M4 ROM writes 128-byte AMSDOS header
   → M4: C_WRITE, fd=2, [128 bytes of header]
   ← Response: OK

3. BASIC calls CAS_OUT_DIRECT (&BC98) or repeated CAS_OUT_CHAR
   → M4: C_WRITE, fd=2, [data chunks, max 0xFC bytes each]
   ← Response: OK (repeated)

4. BASIC calls CAS_OUT_CLOSE (&BC8F)
   → M4: C_CLOSE, fd=2
   ← Response: OK
```

## File loading sequence: RUN (tokenized BASIC)

```
1. BASIC calls CAS_IN_OPEN (&BC89)
   → M4: C_OPEN, mode=0x01 (FA_READ), filename
   ← Response: fd=1, error=0

2. M4 ROM reads 128-byte AMSDOS header via fread/C_READ
   → M4: C_READ, fd=1, count=128
   ← Response: status=0, [128 bytes header data]

   ROM checks AMSDOS checksum, extracts file type/length/load address

3. If file type = BASIC (tokenized):
   BASIC calls CAS_IN_CHAR (&BC92) repeatedly (byte by byte)

   CAS_IN_CHAR reads from a 2KB buffer. When empty:
   → M4: C_READ2, fd=1, count=2048
   ← Response: status=0 (or 0x14=EOF), bytes_read, [data at offset 8+]

   Buffer refill repeats until EOF.

4. BASIC calls CAS_IN_CLOSE (&BC8C)
   → M4: C_CLOSE, fd=1
   ← Response: OK
```

## File loading sequence: RUN (binary .BIN)

```
1-2. Same as tokenized BASIC (CAS_IN_OPEN, header read)

3. Header says file type = binary. ROM extracts load address and length.
   BASIC calls CAS_IN_DIRECT (&BC95)

   M4 ROM's _cas_in_direct calls fread in chunks of 0x800 (2048) bytes:
   → M4: C_READ, fd=1, count=2048 (or remaining)
   ← Response: status=0, [data at offset 4+]
   (Repeated in a loop until all bytes loaded)

4. CAS_IN_CLOSE, then execution jumps to the binary's entry address
```

## Hardware versions

| Version | MCU         | Key features                            |
|---------|-------------|------------------------------------------|
| v1.x    | STM32       | SD card, WiFi, ROM board, RTC           |
| v2.x    | STM32       | Same features, revised PCB (rev 2.5C)   |
| v2.5C   | STM32       | Current production (2019+)              |

Firmware versions (selected milestones):
- **v1.0.5**: C_FTELL, C_FSIZE added
- **v1.0.7**: C_READ2 added (fixes `_cas_in_char` buffering)
- **v1.0.8**: C_GETPATH added
- **v1.0.9**: Raw SD read/write, TCP sockets
- **v1.1.0**: C_FSTAT (file attributes, dates)
- **v2.0.5**: C_NMI (trigger hack menu)
- **v2.0.8**: Current firmware release (Oct 2024)

## References

- **M4 Board wiki**: https://www.cpcwiki.eu/index.php/M4_Board
- **M4 firmware source**: M4ROM.s by Duke (spinpoint.org)
- **M4 command reference**: m4info.txt, m4cmds.i
- **Extended User Manual**: Amstrad CPC M4 Board Extended User Manual
  (PDF by Csaba Toth, 31-10-2024)
- **FatFs library**: http://elm-chan.org/fsw/ff/ (embedded FAT filesystem
  used by M4 firmware)
