# Multiface II — Device spec (DESIGN, implementation pending)

The freeze-and-poke cartridge on the expansion port: a STOP button that
NMIs the CPU, 8K of ROM and 8K of RAM that page over the bottom 16K, and —
the part the pin-level board makes beautiful — a **bus snooper** that
shadows every hardware write so the freeze menu can read and restore the
machine's state. The golden master scatters that shadow across
`kon_cpc_ja.cpp` (`pbMF2ROM + 0x3fcf` and friends); here the Device simply
watches the committed bus like the real silicon watches the edge connector.

## 1. New expansion-bus lines: /ROMDIS and /RAMDIS

`CpuBus` gains `bool romdis` / `bool ramdis` — wired-OR lines any expansion
may assert (like `irq`). The memory Device YIELDS its data drive for reads
hit by them: `romdis` silences the internal ROM decode, `ramdis` the RAM.
Two-phase settling is safe for the same reason WAIT-stretched accesses are:
a Z80 memory read holds its strobes across many master cycles and samples
data at the END of the cycle, so an expansion asserting the line one tick
after seeing the strobes still wins the access. (This pair also unblocks
future ROM-board peripherals; the Symbiface and friends will reuse it.)

## 2. Paging and decode

- OUT `&FEE8` (high byte `0xFE`, low `0xE8`) → **page in** (ACTIVE), unless
  INVISIBLE (§4). OUT `&FEEA` → **page out**. Edge semantics per access.
- While ACTIVE, for CPU reads `addr < 0x2000`: assert `romdis`, drive the
  MF2 ROM byte. For reads `0x2000..0x3FFF`: assert `romdis` (the GA's lower
  ROM would decode there) and `ramdis`, drive the MF2 RAM byte.
- Writes `0x2000..0x3FFF` while ACTIVE land in the MF2 RAM. The golden
  master routes them EXCLUSIVELY there (internal RAM untouched — observable
  after page-out); the real gate array's write path is not disabled by
  ROMDIS, so real hardware may write both. **We follow the golden master**
  and note the divergence here; revisit against real-hardware evidence.

## 3. The STOP button and the freeze

- STOP: assert `nmi` on the bus for one accepted NMI AND set ACTIVE (the
  hardware pages itself in so the NMI vector at &0066 fetches MF2 ROM).
  Host API: `mf2_stop(dev)`.
- The legacy F6 flow (`z80.cpp`): NMI accept sets ACTIVE|RUNNING; the menu's
  exit path clears RUNNING and sets INVISIBLE. In the Device the RUNNING
  bookkeeping is unnecessary — ACTIVE is the paging truth; INVISIBLE (§4)
  latches when the frozen session returns (OUT &FEEA while the NMI return
  address is on the stack is not observable at pin level, so we latch
  INVISIBLE on the page-out that follows a STOP, per the golden master's
  observable behaviour).

## 4. INVISIBLE

After a freeze session ends the real MF2 hides: `&FEE8` stops decoding
until the next machine RESET (the reset line on the bus clears INVISIBLE;
the STOP button always works). This is anti-detection hardware — games
probing `&FEE8` must not see the ROM appear.

## 5. The hardware shadow (bus snooping)

While the machine runs (ACTIVE or not), the Device watches committed I/O
writes and mirrors them into fixed MF2-RAM cells — the layout the MF2's own
ROM expects (offsets are within the 8K RAM, i.e. `pbMF2ROM + 0x2000..`
in the golden master maps to RAM offset `addr - 0x2000`):

| I/O write | RAM cell (golden master offset) |
|---|---|
| GA pen select | `0x3FCF` |
| GA ink for the current pen | `0x3FEF` |
| GA mode/ROM config | `0x3FFF` |
| GA RAM config | `0x37FF` |
| CRTC register select | `0x3CFF` |
| CRTC register R | `0x3DB0 \| (R & 0x0F)` |
| Upper ROM select | `0x3AAC` |

No host hooks, no scattered pokes: the Device decodes the same partial
addresses the GA/CRTC/ROM-select decode and shadows the data bytes.

## 6. Media, state, host API

The 8K ROM is caller-owned live wiring (`mf2_attach_rom`), like every other
ROM. The 8K RAM is Device state and SERIALIZES (it is the freeze state).
`mf2_set_plugged` gates everything (an unplugged cartridge decodes
nothing); plugged state persists across reset, INVISIBLE does not.
`Mf2Regs { plugged, active, invisible }` via `mf2_peek`.
