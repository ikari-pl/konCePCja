#!/usr/bin/env python3
"""
dsk_add_file.py — Add a file to a CPC Extended DSK image (CP/M Data format)

Usage:
    python3 dsk_add_file.py <disk.dsk> <host_file> <CPM_NAME.EXT>

Supports standard CPC Data format ($C1-$C9 sectors, 1K blocks, 0 reserved tracks).
"""

import sys
import struct
import math

SECTOR_SIZE = 512
TRACK_HEADER_SIZE = 256
SECTORS_PER_TRACK = 9
BLOCK_SIZE = 1024  # 1K blocks
SECTORS_PER_BLOCK = BLOCK_SIZE // SECTOR_SIZE  # 2
DIR_BLOCKS = 2     # blocks 0-1 reserved for directory
DIR_ENTRIES = 64   # 64 × 32 bytes = 2K
DIR_ENTRY_SIZE = 32
RECORD_SIZE = 128

def track_offset(disk_data, track_idx):
    """Byte offset of track_idx in the DSK image (extended format)."""
    # Disk header is 256 bytes; track size table at offset 0x34
    off = 0x100
    for i in range(track_idx):
        ts = disk_data[0x34 + i] * 256
        if ts == 0:
            break
        off += ts
    return off

def sector_data_offset(disk_data, track_idx, sector_idx):
    """Byte offset of sector_idx (0-based) within track_idx."""
    t = track_offset(disk_data, track_idx)
    return t + TRACK_HEADER_SIZE + sector_idx * SECTOR_SIZE

def logical_sector_offset(disk_data, lsn):
    """Byte offset of logical sector number lsn (0-based across whole disk)."""
    track_idx = lsn // SECTORS_PER_TRACK
    sector_idx = lsn % SECTORS_PER_TRACK
    return sector_data_offset(disk_data, track_idx, sector_idx)

def block_offsets(disk_data, block_num):
    """Return byte offsets of the two 512-byte sectors in block_num."""
    lsn_base = block_num * SECTORS_PER_BLOCK
    return [logical_sector_offset(disk_data, lsn_base + i) for i in range(SECTORS_PER_BLOCK)]

def read_directory(disk_data):
    """Read all 64 directory entries from blocks 0-1."""
    entries = []
    for i in range(DIR_ENTRIES):
        # Block 0 covers entries 0-15, block 1 entries 16-31... no:
        # 2K directory = 4 sectors, 2 sectors per block, 64 entries × 32 bytes
        lsn = i * DIR_ENTRY_SIZE // SECTOR_SIZE
        off_in_sector = (i * DIR_ENTRY_SIZE) % SECTOR_SIZE
        off = logical_sector_offset(disk_data, lsn) + off_in_sector
        entries.append(bytearray(disk_data[off:off + DIR_ENTRY_SIZE]))
    return entries

def write_directory_entry(disk_data, entry_idx, entry_bytes):
    """Write a single 32-byte directory entry back to the disk image."""
    lsn = entry_idx * DIR_ENTRY_SIZE // SECTOR_SIZE
    off_in_sector = (entry_idx * DIR_ENTRY_SIZE) % SECTOR_SIZE
    off = logical_sector_offset(disk_data, lsn) + off_in_sector
    disk_data[off:off + DIR_ENTRY_SIZE] = entry_bytes

def used_blocks(entries):
    """Collect all block numbers referenced in directory entries."""
    used = set()
    # Blocks 0-1 are always used (directory)
    used.add(0)
    used.add(1)
    for e in entries:
        if e[0] == 0xE5:
            continue  # deleted / free
        for j in range(16, 32):
            b = e[j]
            if b != 0:
                used.add(b)
    return used

def count_tracks(disk_data):
    """Count tracks from extended format size table."""
    n = 0
    for i in range(204):
        if disk_data[0x34 + i] == 0:
            break
        n += 1
    return n

def max_blocks(disk_data):
    tracks = count_tracks(disk_data)
    total_sectors = tracks * SECTORS_PER_TRACK
    return total_sectors // SECTORS_PER_BLOCK

def parse_cpm_name(name_ext):
    """Parse 'FILENAME.EXT' into (8-byte name, 3-byte ext) bytearrays."""
    parts = name_ext.upper().split('.')
    name = parts[0].ljust(8)[:8]
    ext  = (parts[1] if len(parts) > 1 else '').ljust(3)[:3]
    return bytearray(name.encode('ascii')), bytearray(ext.encode('ascii'))

def make_dir_entry(name_bytes, ext_bytes, block_num, data_len):
    """Build a 32-byte CP/M directory entry."""
    records = math.ceil(data_len / RECORD_SIZE)
    entry = bytearray(DIR_ENTRY_SIZE)
    entry[0] = 0x00            # user 0
    entry[1:9]  = name_bytes   # filename
    entry[9:12] = ext_bytes    # extension
    entry[12] = 0              # extent lo
    entry[13] = 0
    entry[14] = 0              # extent hi
    entry[15] = records & 0x7F # record count (0-127)
    entry[16] = block_num      # first (only) block
    # rest are 0 (no more blocks)
    return entry

def add_file(dsk_path, src_path, cpm_name):
    with open(dsk_path, 'rb') as f:
        disk_data = bytearray(f.read())

    sig = disk_data[:34]
    if b'EXTENDED CPC DSK' not in sig:
        sys.exit(f"Not an Extended CPC DSK: {dsk_path}")

    file_data = open(src_path, 'rb').read()
    if len(file_data) > BLOCK_SIZE:
        sys.exit(f"File too large for single-block allocation ({len(file_data)} bytes, max {BLOCK_SIZE})")

    name_bytes, ext_bytes = parse_cpm_name(cpm_name)
    entries = read_directory(disk_data)

    # Check for existing file with same name
    for i, e in enumerate(entries):
        if e[0] == 0xE5:
            continue
        en = bytes(e[1:9]).decode('ascii', errors='replace').rstrip()
        ex = bytes(e[9:12]).decode('ascii', errors='replace').rstrip()
        if en == name_bytes.decode().rstrip() and ex == ext_bytes.decode().rstrip():
            print(f"File {cpm_name} already exists in directory (entry {i}). Replacing.")
            # Free its blocks
            for j in range(16, 32):
                disk_data_block = e[j]
                if disk_data_block:
                    # Zero out the block data
                    for off in block_offsets(disk_data, disk_data_block):
                        disk_data[off:off+SECTOR_SIZE] = bytes([0xE5] * SECTOR_SIZE)
            e[0] = 0xE5  # mark as deleted
            write_directory_entry(disk_data, i, e)
            entries[i] = e

    # Find free directory entry
    free_entry_idx = None
    for i, e in enumerate(entries):
        if e[0] == 0xE5:
            free_entry_idx = i
            break
    if free_entry_idx is None:
        sys.exit("Directory is full")

    # Find free block
    used = used_blocks(entries)
    maxb = max_blocks(disk_data)
    free_block = None
    for b in range(DIR_BLOCKS, maxb):
        if b not in used:
            free_block = b
            break
    if free_block is None:
        sys.exit("Disk is full (no free blocks)")

    # Write file data to block (pad with 0x1A = CP/M EOF)
    block_bytes = bytearray([0x1A] * BLOCK_SIZE)
    block_bytes[:len(file_data)] = file_data

    offsets = block_offsets(disk_data, free_block)
    disk_data[offsets[0]:offsets[0]+SECTOR_SIZE] = block_bytes[:SECTOR_SIZE]
    disk_data[offsets[1]:offsets[1]+SECTOR_SIZE] = block_bytes[SECTOR_SIZE:]

    # Create and write directory entry
    entry = make_dir_entry(name_bytes, ext_bytes, free_block, len(file_data))
    write_directory_entry(disk_data, free_entry_idx, entry)

    with open(dsk_path, 'wb') as f:
        f.write(disk_data)

    print(f"Added {cpm_name} ({len(file_data)} bytes) → block {free_block}, dir entry {free_entry_idx}")

if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit(f"Usage: {sys.argv[0]} disk.dsk host_file CPM_NAME.EXT")
    add_file(sys.argv[1], sys.argv[2], sys.argv[3])
