# Dobbertin SmartWatch — Device spec

A Dallas **DS1216** SmartWatch: a phantom RTC in the upper ROM socket. It
has no ports and no address of its own — it sits BETWEEN the socket and
the ROM chip, watching the chip-enable and the low address lines, and
answers by stealing **data bit D0** on reads. Software talks to it through
a serial bit-banging protocol encoded in ADDRESSES: A0 carries the data
bit, A2 selects write-pattern (0) vs read-time (1).

## 1. Where it listens

The socket's /CE: CPU memory reads at `0xC000-0xFFFF` while the upper ROM
is paged in. The Device reconstructs the enable the same way the MF2
snoops: it watches GA config writes on the committed bus
(`(addr & 0xC000) == 0x4000`, function 2) and tracks bit 3 (upper-ROM
disable). Unplugged, nothing decodes.

## 2. The DS1216 protocol (the golden master's FSM, verbatim)

- **IDLE**: an A2=0 read starts pattern matching (the A0 bit is the first
  pattern bit); A2=1 reads pass through untouched.
- **MATCHING**: each A2=0 read shifts A0 into a 64-bit register (LSB
  first). After 64 bits: match against `C5 3A A3 5C C5 3A A3 5C` →
  snapshot the time (8 BCD bytes: hundredths, sec, min, hour|0x80 for 24h,
  day-of-week, day, month, year) and enter READING; mismatch → IDLE.
  An A2=1 read during matching resets to IDLE.
- **READING**: each A2=1 read returns the next time bit (LSB first across
  the 8 bytes) on **D0**, bits 7..1 straight from the ROM chip. After 64
  bits → IDLE. An A2=0 read aborts and starts a new match with that bit.

One FSM step per ACCESS (edge semantics), not per master cycle.

## 3. Driving D0 on a two-phase bus

The phantom must pass the ROM's bits 7..1 through while replacing D0 — but
it cannot read the memory Device's output in the same tick. It does what
the silicon does: let the chip answer first, then override.

- Access tick N: the Device sees the strobes, decides the FSM step, and
  asserts **`romdis`** (the §1 enable held for the rest of the access).
- Tick N+1: the memory Device yields under `romdis`; the committed bus
  still carries its byte from tick N — the Device latches it and drives
  `(byte & 0xFE) | rtc_bit` from here on.
- The CPU samples at T3, many master cycles later: the settle is invisible
  (same argument as the Multiface overlay, memory-device.md §4b).

The override only happens in READING with A2=1; in every other state the
Device drives nothing and the ROM answers normally.

## 4. Time source, state, host API

The time snapshot is taken ONCE per successful pattern match, from the
host clock (the DS1216 keeps ticking on its internal battery — the
snapshot-at-match models the read burst's atomicity; the golden master
does the same). The Machine passes a host callback... no: keeping the
Device heap- and callback-free, the host feeds wall-clock time via
`smartwatch_set_time(dev, bcd[8])` whenever it likes (the bridge refreshes
it once per frame); the FSM latches the CURRENT value on match.
Serialized: FSM state, shift register, bit index, snapshot, plugged.
`SmartwatchRegs { plugged, state, bit_index }` via peek. Reset clears the
FSM (the golden master's smartwatch_reset); plugged persists.
