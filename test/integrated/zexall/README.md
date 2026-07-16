# ZEXALL - Z80 Instruction Exerciser

## About
ZEXALL tests all Z80 instructions including undocumented flags by:
1. Cycling through machine states
2. Executing each instruction
3. Computing CRC over resulting states
4. Comparing against known-good CRCs from real Z80

## Source
- https://github.com/agn453/ZEXALL
- Original by Frank Cringle, CP/M port by J.G. Harston

## Running on CPC
ZEXALL is a CP/M program. To run on CPC:
1. Use AMSDOS with CP/M 2.2 or 3.0
2. Or adapt for direct execution (no BDOS calls)

## Expected Results
All tests should show "OK" with matching CRCs.
Full run takes several hours on real hardware.

## IPC-based Testing
```bash
# Start emulator with CP/M disk
./koncepcja cpm22.dsk &

# Wait for boot, then run ZEXALL via IPC
echo "pause" | nc localhost 6543
echo "load zexall.com" | nc localhost 6543
echo "run" | nc localhost 6543
echo "wait vbl 50000 300000" | nc localhost 6543  # Wait up to 5 min
echo "screenshot zexall_result.bmp" | nc localhost 6543
```
