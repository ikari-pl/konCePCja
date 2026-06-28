# konCePCja Manual — Subproject

This directory contains the source for the **konCePCja User Manual**, a typeset PDF
document for end users of the konCePCja Amstrad CPC emulator.

The manual covers everything from first-launch setup to advanced scripting with the
IPC debugging interface, telnet console, and integrated Z80 assembler.

---

## Toolchain

The manual is typeset with **[Typst](https://typst.app/)** — a modern, Rust-based
typesetting system that compiles `.typ` source files to PDF in seconds.

Install Typst:
```
# macOS
brew install typst

# or download binary from https://github.com/typst/typst/releases
```

Typst version: 0.12+ (tested against the version used in the sibling CPC6128 manual).

---

## Directory Layout

```
manual/
├── README.md              — this file
├── TYPOGRAPHY.md          — typesetting specification (fonts, colours, macros)
├── OUTLINE.md             — full content outline / table of contents plan
├── Makefile               — build targets (to be created)
├── template.typ           — Typst document template (to be created, forked from CPC manual)
├── manual_build.typ       — top-level build entry point (to be created)
├── chapters/              — one .typ file per chapter (to be created)
│   ├── front_matter.typ
│   ├── ch01_getting_started.typ
│   ├── ch02_interface.typ
│   ├── ch03_disk_tape_snapshot.typ
│   ├── ch04_configuration.typ
│   ├── ch05_hardware_emulation.typ
│   ├── ch06_peripherals.typ
│   ├── ch07_devtools.typ
│   ├── ch08_assembler.typ
│   ├── ch09_ipc_interface.typ
│   ├── ch10_telnet_console.typ
│   ├── ch11_m4_board.typ
│   ├── ch12_recording.typ
│   ├── appendix_a_key_reference.typ
│   ├── appendix_b_ipc_commands.typ
│   ├── appendix_c_config_reference.typ
│   ├── appendix_d_file_formats.typ
│   └── appendix_e_glossary.typ
├── images/                — SVG/PNG figures (generated and hand-crafted)
│   ├── keyboard-layout.svg        — CPC keyboard diagram
│   ├── memory-map.svg             — Z80 memory map
│   ├── port-map.svg               — I/O port map
│   └── ...
├── tools/                 — Python scripts for generating SVG figures
│   ├── gen_keyboard.py
│   ├── gen_memory_map.py
│   └── gen_port_map.py
└── fonts/                 — local font copies (see TYPOGRAPHY.md)
    ├── (font files — see TYPOGRAPHY.md for the list)
    └── ...
```

---

## Building

Once the source files exist, build with:

```bash
# Build PDF
make

# Watch mode — rebuilds on every file save
make watch

# Clean
make clean
```

The Makefile will call:
```bash
typst compile --font-path fonts manual_build.typ koncepcja_manual.pdf
```

Font paths are always resolved relative to the Makefile, not the shell CWD — this
avoids the silent font-fallback problem that produces wildly different page counts.

---

## Relationship to the CPC6128 Manual

The sibling project at `../manual/` (i.e. `/Users/ikari/src/cpc/manual/`) is a full
typeset reproduction of the original 1985 Amstrad CPC6128 user manual. It uses an
identical toolchain and the same template architecture.

The konCePCja manual **borrows the template structure and style palette** from that
project:
- Fork `template.typ` and adjust the title/palette constants
- Reuse the font files from `../manual/fonts/` (copy or symlink; see TYPOGRAPHY.md)
- Reuse Python SVG generator pattern from `../manual/tools/`

Do **not** include the original CPC6128 manual's copyrighted content here.

---

## Audience

The manual is written for **end users**, not AI agents. It assumes the reader:
- Knows what an Amstrad CPC is (or is curious about retrocomputing)
- Has successfully installed konCePCja
- Does not have C++ or emulator internals knowledge

Power-user chapters (IPC, telnet console, M4 HTTP server, integrated assembler) are
included because these are legitimate features available to technically inclined users.

---

## Content Scope

See `OUTLINE.md` for the full table of contents. High-level chapters:

1. Getting Started
2. The konCePCja Interface
3. Disks, Tapes, and Snapshots
4. Configuration
5. Emulated Hardware
6. Peripheral Expansions
7. Developer Tools (DevTools, F12)
8. Z80 Assembler
9. IPC Debugging Interface
10. Telnet Console
11. M4 Board and Virtual Filesystem
12. Recording (AVI, GIF, YM, Session)
+ Appendices

---

## Building

```bash
make            # builds koncepcja_manual.pdf
make watch      # rebuild on every save
make clean
```

The toolchain and all fonts are self-contained under `fonts/` (free fonts only;
see `fonts/LICENSES.md`), so the build is reproducible with no external font
setup. Requires [Typst](https://typst.app/) 0.12+.

## Status

**Drafted (first edition).** All 12 chapters and 6 appendices plus a dynamic
index are written and build to a ~39-page PDF. Content is harvested from the
project docs and source; figures use the ported CPC keyboard generator and
existing screenshots. Next steps: deeper figures, a proofreading pass, and the
Polish edition (deferred). See `OUTLINE.md` for the structure and
`TYPOGRAPHY.md` for the typesetting specification.
