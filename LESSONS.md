# Lessons from Building an Amstrad CPC Emulator

*Notes from the konCePCja project — what we learned about the CPC, emulation,
and software engineering along the way.*

---

## The CPC Hardware: Deceptively Simple, Ruthlessly Precise

### The Z80 Doesn't Care About Your Abstractions

The Z80 CPU is a simple 8-bit processor, but emulating it faithfully means
respecting every timing quirk. The CPC runs at 4 MHz, and every instruction
takes a precise number of T-states. The Gate Array steals cycles from the CPU
during memory access, and CRTC interrupts fire at exact scanline positions.
Get any of these wrong, and demos that push the hardware break immediately.

The biggest lesson: **an emulator is not a simulator**. You're not modeling the
*idea* of a Z80 — you're reproducing its exact behaviour, cycle by cycle. When
we implemented breakpoints and single-stepping, we had to be careful that
debug features don't disturb the timing model. A breakpoint that fires between
a `CALL` and its return address push would corrupt the stack.

### The Keyboard Matrix Is Active-Low

The CPC keyboard is a 10x8 matrix scanned by the PSG (AY-3-8912). Bits are
*cleared* when a key is pressed, *set* when released. This tripped us up more
than once — `applyKeypress()` silently refuses to work when `CPC.paused==true`,
because the matrix scan is part of the main emulation loop. The fix was to
bypass the scan and write directly to `keyboard_matrix[]` for IPC-driven input.

This is a recurring pattern in hardware emulation: **what seems like a software
interface is really a hardware timing dependency**. The keyboard isn't "pressed"
in a function-call sense — it's held down for some number of frames while the
PSG scans the matrix at its own pace.

### Memory Banking: The CPC's Secret Weapon (and Headache)

The CPC 6128 has 128K of RAM in a 64K address space. The Gate Array's memory
management unit (MMU) maps 16K banks into four slots. Extended RAM expansions
(like Yarek's 4MB) add more banking configurations than the original designers
ever imagined.

We learned that **banking is not just about read/write pointers** — the CPC
has separate read and write mappings. The firmware ROM can be mapped into the
read path at 0x0000-0x3FFF while writes go to the underlying RAM. The Silicon
Disc (a battery-backed 256K RAM expansion) occupies banks 4-7, separate from
the main memory. Getting the `ga_memory_manager()` function right was crucial —
one wrong pointer and the CPC boots to a black screen.

### The CRTC: Four Chips, Four Personalities

The CPC used different CRTC (Cathode Ray Tube Controller) chips across its
production run — types 0 through 3, each with subtly different behaviour.
Type 0 (Hitachi HD6845S) handles register writes differently from Type 1
(UM6845R), which again differs from Types 2 and 3. Demo coders exploited
these differences, and some demos specifically require one CRTC type.

Implementing CRTC type selection taught us that **emulating a "family" of
machines isn't emulating one machine with parameters** — it's emulating
several related-but-different machines under one roof. Each type needs its
own handling for overscan, HSYNC/VSYNC timing, and register mirroring.

### Tape Loading: A State Machine Within a State Machine

CDT/TZX tape files encode analog signals digitally. The tape engine is a state
machine: PILOT tone → SYNC pulses → DATA bits → PAUSE. Each stage has its
own timing, and the `CYCLE_ADJUST()` macro scales Spectrum T-states (3.5 MHz)
to CPC T-states (4 MHz). The CPC reads tape data through the PPI port, one
bit at a time, synchronized to the CPU's interrupt-driven tape routine.

The lesson here is about **format archaeology**. The TZX format was designed
for the ZX Spectrum and adopted by the CPC community as CDT. The 10-byte
header is literally `"ZXTape!\x1A"` followed by a version byte. Understanding
why the format works the way it does required reading Spectrum documentation,
not CPC documentation. Formats outlive their original platforms.

---

## Engineering an Emulator: What Worked

### IPC-First Design

The single best architectural decision was building the emulator as an
**IPC-controllable tool** from the start. Every feature — breakpoints,
memory access, screenshots, disc operations — is accessible via a TCP
text protocol on port 6543.

This means:
- **Testing** doesn't require a GUI — our 600+ unit tests run headless
- **Automation** scripts can drive the emulator like a debugging server
- **AI agents** can connect and debug CPC programs through the same interface
- **GUI is secondary** — the ImGui interface calls the same code paths as IPC

The protocol is simple: send a command, get `OK ...` or `ERR ...` back.
No binary framing, no authentication (localhost only), no session state.
This simplicity turned out to be the right call — it's trivially scriptable
with `echo "cmd" | nc localhost 6543`.

### Extract Pure Functions for Testing

The emulator is full of globals (`CPC`, `z80`, `driveA`, `driveB`). We can't
easily mock them, so we extract **pure functions** into testable headers.
`imgui_ui_testable.h` contains functions like `parse_hex_address()` and
`format_ram_size()` that have no side effects. The expression parser, symbol
table, data areas, and session recorder are all standalone classes that can
be tested without initializing the emulator.

The pattern: **if you can't make the whole system testable, make the
interesting parts testable by extracting them from the global-dependent code**.

### Git Worktrees for Parallel Feature Development

Working on three features simultaneously (disassembly export, Silicon Disc,
session recording) in separate git worktrees turned out to be transformative.
Each worktree has its own build directory, so `make` only rebuilds what changed
in that feature. No stashing, no branch-switching overhead.

The workflow:
```bash
git worktree add ../konCePCja-feature-name feature/feature-name
cd ../konCePCja-feature-name
# work, build, test independently
# merge back to master when ready
```

### The Headless Mode Pattern

Adding `--headless` mode early (Phase 0) paid dividends in every later phase.
The headless mode uses a `video_plugin` vtable that renders to a memory buffer
instead of the screen. This means CI can run the full emulator, execute CPC
code, and verify results via hash comparison — all without X11 or a GPU.

---

## Engineering an Emulator: What Bit Us

### Windows Cross-Compilation Is a Different Universe

Our CI runs MINGW32 and MINGW64 cross-compilation on GitHub Actions. Nearly
every feature needed Windows-specific fixes:

- **No `strncpy`** → use `snprintf` instead
- **No POSIX sockets** → `#ifdef _WIN32` with `<winsock2.h>`
- **No `/tmp`** → use `std::filesystem::temp_directory_path()`
- **File locking** → Windows locks files that are open; `remove_all()` in test
  teardown fails if you haven't `fclose()`d first
- **No libjpeg** → compile-time guards for optional dependencies

The lesson: **"works on my machine" means nothing when CI runs on three
different platforms**. Every new feature needs to be mentally compiled for
MINGW before pushing. The Windows file-locking issue alone cost us several
CI cycles.

### Code Review Comments Are Not Optional

We created 34 pull requests with automated Gemini code review. For the first
~18 PRs, we merged without systematically addressing the review comments.
When we audited the backlog, we found **~50 unaddressed comments** including
9+ high-priority issues (path traversal vulnerabilities, unchecked return
values, memory allocation bugs).

The lesson is blunt: **creating PRs without reading the reviews is worse than
not creating PRs at all** — it gives the false impression that code was
reviewed. Now we always check for comments before merging and reply "Fixed"
with a description of what changed.

### The `feof()` Anti-Pattern

C's `feof()` returns true only *after* a read has already failed. Using
`while (!feof(f))` as a loop condition can process garbage data from the
last iteration. The correct pattern is a read-first loop:

```c
for (;;) {
    if (fread(&byte, 1, 1, f) != 1) break;
    // process byte
}
```

We hit this in the session recording playback code. It happened to work
because the inner `fread` check caught the error first, but the `feof()`
condition was misleading and fragile.

### Global State Makes Testing Hard, But Rewriting Is Harder

The emulator inherits Caprice32's architecture of global structs (`CPC`,
`z80`, `driveA`). Ideally we'd inject these as dependencies, but rewriting
the core to support dependency injection would touch thousands of lines and
break the emulation logic.

The pragmatic approach: **accept the globals, test around them**. Use `extern`
declarations in test files, extract pure logic into testable classes, and test
at the IPC protocol level where the globals are hidden behind a text interface.

---

## The CPC Community: Things We Learned

### AMSDOS Is Cleverer Than It Looks

The CPC's disc operating system stores files with a 128-byte header containing
the load address, execution address, file type, and checksum. When we built
the disc file editor (IPC `disk ls/get/put/rm`), we discovered that AMSDOS
uses a simple but effective checksum: sum all header bytes except the last two,
and store the result as a 16-bit little-endian value. Get it wrong and AMSDOS
silently refuses to load the file.

### The .DSK Format Is Honest

Unlike some retro disc formats, the .DSK format stores raw sector data with
explicit track and sector headers. There's no compression, no tricks — what
you see is what the FDC reads. This made the sector editor straightforward:
seek to the track, read the sector info block, read the data. The format's
simplicity is its strength.

### Mode 0 Pixels Are Not What You Think

CPC Mode 0 (160x200, 16 colours) packs two pixels per byte, but the bit
layout is interleaved — pixel 0 uses bits 7,5,3,1 and pixel 1 uses bits
6,4,2,0. Building the graphics finder required implementing `decode_mode0_pixel()`
which extracts and reassembles these scattered bits. Mode 1 (320x200, 4 colours)
and Mode 2 (640x200, 2 colours) have their own bit arrangements. The hardware
does this because the Gate Array shifts bits out to the display in a specific
order — it's not arbitrary, it's a direct consequence of the silicon layout.

---

## Meta-Lessons: Building Software With AI

### The IPC-First Pattern Works For AI Collaboration Too

Having every feature accessible via a text protocol meant an AI agent could
drive the emulator the same way a human debugger would. When we needed to
test tape loading, we could script it: load a CDT, wait for the PC to reach
a specific address, dump memory, compare hashes. This is the **agent-native**
pattern — if an agent can't do it over IPC, it's not done.

### Small PRs With Reviews Beat Large PRs Without

Our most productive workflow was: implement one feature per PR, get automated
review, fix the comments, then merge. When we batched multiple features or
skipped reviews, quality suffered and we accumulated technical debt that took
a whole session to audit and triage.

### Tests Are Documentation

Our 600+ tests serve as the executable specification of CPC hardware behaviour.
When we weren't sure how the Z80's `RST` instruction should interact with
`step_out`, the test told us: `RST` pushes a return address like `CALL`, so
`step_out` should set a breakpoint at `SP+2`. The test caught a real bug in
the implementation.

### Immediate-Mode UI and the Frame Budget

ImGui operates in "immediate mode" — every widget is redrawn from scratch each
frame (~60fps). This means anything in a `render_*()` function runs 60 times
per second. Scanning 256K of Silicon Disc memory to compute bank usage
percentages, or rebuilding a format name string from a `std::vector`, adds up
fast.

The pattern is **cache computed values, invalidate on demand**. Add a `bool
dirty_` flag, recompute only when it's set, and provide a "Refresh" button or
invalidate on known mutation events (load, clear, format). This is the UI
equivalent of a database index — trade a bit of staleness for a massive
reduction in per-frame work.

### Relative vs Absolute Addressing in Buffer APIs

When an API computes `addr = base_address + offset` and uses that to index
into a memory buffer, you need to know whether the buffer represents the
full address space (64K for the CPC) or just a slice starting at `base_address`.
If you pass a small relative buffer but leave `base_address = 0xC000`, the
computed index overflows the buffer and reads all zeros.

We hit this in the graphics finder: `gfx_decode()` uses `params.address`
to compute the read offset, but the UI passed a small buffer already starting
from `params.address`. Fix: set `params.address = 0` for relative buffers.
This is a general lesson about API contracts — **document whether your buffer
parameter is absolute or relative**, and verify the assumption at call sites.

---

## Growing the Codebase: From 34 to 41 PRs

### Batching PRs by Theme Works

Grouping related changes into themed PRs (debugger UI enhancements, hardware
panels, session/graphics) kept reviews focused and merges clean. Each PR
modified the same core files (`devtools_ui.h/cpp`, `imgui_ui.cpp`) but touched
different sections. The risk of merge conflicts is real — we branched each PR
from master independently rather than stacking branches.

### Path Traversal Is Always a Risk

Any UI feature that takes a file path from user input needs path traversal
protection. Even in a desktop emulator. We added `has_path_traversal()` using
`std::filesystem::path` to check for `..` components before allowing symbol
file load/save and disassembly export. The IPC server already had this check —
the lesson is that **UI code paths need the same security as network-facing
code** because they accept the same kind of input.

---

*konCePCja: 41 PRs, 620+ tests, 6 phases of development, and still learning.*
