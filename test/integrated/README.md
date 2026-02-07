# Integration Test Infrastructure

## Test ROM Sources

### 1. ZEXALL - Z80 Instruction Exerciser
- Tests all Z80 opcodes including undocumented flags
- Download: https://github.com/agn453/ZEXALL
- Run under CP/M or adapt for direct execution
- Expected: All tests pass (CRC match)

### 2. Arnold Acid Tests
- CRTC timing, Gate Array behavior, interrupt generation
- Source: Arnold emulator distribution
- Tests edge cases that break on incorrect implementations

### 3. SHAKER Tests
- Portal: https://shaker.logonsystem.eu
- Compatibility matrix for CRTC types 0-4
- Visual comparison against real hardware captures

### 4. Timing Tests (Custom)
Based on gem-knight/references documentation:

#### Z80 Timing (from z80-um0080.pdf)
```
| Instruction | T-states | CPC cycles (stretched) |
|-------------|----------|------------------------|
| NOP         | 4        | 4 (1µs)                |
| LD r,r'     | 4        | 4 (1µs)                |
| LD r,n      | 7        | 8 (2µs)                |
| LD r,(HL)   | 7        | 8 (2µs)                |
| LDIR        | 21/16    | 24/20 (6/5µs)          |
```

#### CRTC Register Tests (from CRTC.txt)
- R0: Horizontal Total (63 for standard, 64 for overscan)
- R1: Horizontal Displayed (40 standard)
- R2: Horizontal Sync Position (46 standard)
- R3: Sync Widths (0x8E standard)
- R4: Vertical Total (38 standard)
- R6: Vertical Displayed (25 standard)
- R7: Vertical Sync Position (30 standard)

#### Gate Array Tests (from Gate_Array.txt)
- Interrupt counter: increments on HSYNC, fires every 52 lines
- Mode changes: take effect on next HSYNC
- Palette: 27 colors from 3-level RGB

## Running Integration Tests

```bash
# Boot emulator with test ROM
./koncepcja --headless test/integrated/zexall.dsk

# Check results via IPC
echo "wait pc 0x0005 60000" | nc localhost 6543  # Wait for BDOS call
echo "mem read 0x0100 256 ascii" | nc localhost 6543  # Read output
```

## Test Automation Framework

```python
#!/usr/bin/env python3
# test_runner.py - Automated integration test harness

import socket
import subprocess
import time

def run_test_rom(rom_path, timeout=60):
    # Start emulator
    proc = subprocess.Popen(['./koncepcja', '--headless', rom_path])
    time.sleep(2)

    # Connect to IPC
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect(('localhost', 6543))

    # Wait for test completion
    sock.send(b'wait vbl 3000 %d\n' % (timeout * 1000))
    result = sock.recv(4096)

    # Take screenshot for visual comparison
    sock.send(b'screenshot test_result.bmp\n')

    proc.terminate()
    return result
```

## Coverage Goals

| Component | Unit Tests | Integration Tests |
|-----------|------------|-------------------|
| Z80 CPU   | Opcode decode | ZEXALL full suite |
| CRTC      | Register write | Acid tests, SHAKER |
| Gate Array| Mode switch | Palette demos |
| PSG       | Register access | Music playback |
| FDC       | State machine | Disk load/save |
| Tape      | TZX parsing | Game loading |
