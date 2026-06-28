# konCePCja Manual — Typography Specification

This document records the findings from analysing the sibling CPC6128 manual at
`/Users/ikari/src/cpc/manual/`. All values were extracted from `template.typ` and
`STYLE_GUIDE.md`. Use these as the authoritative source when building the
konCePCja manual template.

---

## 1. Typesetting System

**Typst** — not LaTeX, not Markdown.

Source: `template.typ` (38 KB), `manual_build.typ`, `Makefile`

Build command (English edition):
```
typst compile --font-path <path-to-fonts-dir> manual_build.typ output.pdf
```

Watch/live-preview:
```
typst watch --font-path <path-to-fonts-dir> manual_build.typ output.pdf
```

The font path must be absolute or relative to the Makefile, not the shell CWD.
The sibling manual's `Makefile` resolves fonts via:
```makefile
MKFILE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
FONTS = $(MKFILE_DIR)fonts
```

---

## 2. Page Setup

| Property | Value |
|----------|-------|
| Page size | 176 mm × 250 mm |
| Top margin | 38 mm |
| Bottom margin | 20 mm |
| Inside margin | 70 pt |
| Outside margin | 40 pt |
| Binding (inside) | offset for two-sided printing |

Typst code:
```typst
#set page(
  width: 176mm,
  height: 250mm,
  margin: (top: 38mm, bottom: 20mm, inside: 70pt, outside: 40pt),
)
```

---

## 3. Fonts

### Primary fonts used in the sibling manual

| Role | Font name (as registered in Typst) | Files in fonts/ |
|------|-----------------------------------|-|
| Heading / display | `"Rockwell N90"` | Variant of AnkaCoder-C87 N90 series |
| Body serif | `"CentSchbook BT"` | `CenturySchoolbookBT-Roman.ttf`, `-Bold.ttf`, `-Italic.ttf`, `-BoldItalic.ttf` |
| Monospace / code | `"OCR B"` | `OCRB.otf`, `OCRBPro.TTF`, `OCR-B_Sharp.otf`, etc. |
| Condensed display | `"Madison Antiqua Pro"` | `MadisonAntiquaPro-Regular_N90.otf`, condensed variants |

### Font directory structure (sibling manual)

```
fonts/
├── CenturySchoolbookBT-Roman.ttf
├── CenturySchoolbookBT-Bold.ttf
├── CenturySchoolbookBT-BoldItalic.ttf
├── CenturySchoolbookBT-Italic.ttf
├── CenturySchoolbookBT-Monospace.ttf
├── CenturySchoolbookBT-BoldCond.ttf
├── OCRB.otf
├── OCRBE.otf / OCRBL.otf / OCRBS.otf / OCRBF.otf
├── OCR-B_Sharp.otf / OCR-B_Inverted.otf / OCR-B_Slanted.otf
├── OCRBPro.TTF
├── MadisonAntiquaPro-Regular_N90.otf
├── MadisonAntiquaPro-Bold_N90.otf
├── MadisonAntiquaPro-Italic_N90.otf
├── MadisonAntiquaPro-Condensed_N90.otf
├── MadisonAntiquaPro-CondensedBold_N90.otf
├── AnkaCoder-C87-r-N90.ttf   (and -b, -i, -bi, N80, plain variants)
├── Amstrad_CPC_correct_Regular.ttf   (CPC screen font for on-screen simulations)
├── Amstrad_CPC_Extended_Tweak_Regular.ttf
├── AmstradCPCintlmo.ttf
└── (various CGA/Tandy/DejaVu/Cascadia fonts as fallbacks)
```

### Font decisions for konCePCja manual

| Role | Recommendation |
|------|---------------|
| Body | Copy `CenturySchoolbookBT-*.ttf` from sibling `fonts/` — identical role, no changes needed |
| Headings | Copy AnkaCoder N90 or Rockwell substitute from sibling `fonts/` |
| Code | Copy `OCRB.otf` / `OCRBPro.TTF` from sibling `fonts/` — same CPC era feel |
| Condensed | Copy `MadisonAntiquaPro-*_N90.otf` — used for footer chapter titles |
| CPC screen | Copy `Amstrad_CPC_correct_Regular.ttf` — for simulated CPC screen captures |

**All fonts already exist at `/Users/ikari/src/cpc/manual/fonts/`.** The konCePCja
manual can symlink or copy that directory rather than acquiring fonts separately.

---

## 4. Colour Palette

```typst
let amstrad-red  = rgb("#C41230")   // headers, rules, accents
let amstrad-blue = rgb("#1B3A5C")   // secondary accent
let amstrad-grey = rgb("#4A4A4A")   // body text
let rule-grey    = rgb("#999999")   // table rules, horizontal dividers
let code-bg      = rgb("#F5F3EE")   // code block background (off-white/cream)
```

**Note from STYLE_GUIDE.md:** There is a pending change to remove the code block
background tint (`code-bg`) in favour of a plain white page. The konCePCja manual
should default to white code blocks from the start — simpler and more faithful to
the "monospace on white" convention.

---

## 5. Type Scale

### Body text

| Property | Value |
|----------|-------|
| Font | CentSchbook BT |
| Size | 10.25 pt |
| Leading (extra) | 4 pt |
| Weight | regular |

### Headings

| Level | Size | Weight | Notes |
|-------|------|--------|-------|
| H1 (chapter) | 23 pt | bold | amstrad-red or amstrad-blue accent |
| H2 | 21.5 pt | bold | |
| H3 | 16.5 pt | bold | |
| H4 | 14 pt | bold | |
| H5 | 12 pt | bold | |

### Monospace / code

| Property | Value |
|----------|-------|
| Font | OCR B |
| Size | 10 pt |
| Tracking | 0.10 pt |
| Leading (extra) | 5.65 pt |
| Left indent | 3 em |
| Background | white (no tint) |

### Intro paragraph (chapter opening)

| Property | Value |
|----------|-------|
| Size | 13.3 pt |
| Tracking | -0.2 pt |
| Weight | regular |

---

## 6. Running Headers and Footers

### Running header (top of each page)

```
[page number]          [chapter title or empty]
────────────────────── (1.2 pt red rule, amstrad-red)
```

- Font: Rockwell N90 (or AnkaCoder N90), 8 pt
- Rule: 1.2 pt, amstrad-red, full text width

### Running footer (bottom of each page)

```
════════════════════ (0.4 pt black rule)
[chapter title]          Chapter N — Page M
```

- Font: Madison Antiqua Pro (condensed), 10 pt
- Rule: 0.4 pt, black, full text width

---

## 7. Tables

```typst
table(
  stroke: 0.5pt + rule-grey,
  inset: 6pt,
  fill: none,
  ...
)
```

- Header row: bold
- No background fill on any row
- Hex values: monospace; decimal values: plain text
- Description columns: `1fr` width

---

## 8. Lists

| Property | Value |
|----------|-------|
| Indent | 24 pt |
| Body indent | 6 pt |
| Bullet marker | `•` (U+2022) |

---

## 9. Custom Functions (from template.typ)

The sibling template exports these functions. The konCePCja template should clone
and adapt them — many can be carried over unchanged.

| Function | Purpose | Carry over? |
|----------|---------|------------|
| `#intro[...]` | Large-size introductory paragraph at chapter open | Yes |
| `#keyword(name)[...]` | BASIC keyword reference entry layout | No — repurpose as `#cmd()` for IPC/config |
| `#kw[NAME]` | Cross-reference to a keyword in body font (no monospace) | Adapt for config key xrefs |
| `#cap[...]` | All-caps body-font text | Yes |
| `#key[NAME]` | Physical key label: `[RETURN]`, `[ESC]`, etc. | Yes |
| `#cc[...]` | Inline code (monospace, 10pt) | Yes |
| `#cmd[...]` | Command / RSX / IPC command (monospace with `>` or `|` prefix) | Yes |
| `#arg[...]` | Argument placeholder: `<address>` in monospace | Yes |
| `#opt[...]` | Optional part of syntax: `[,<n>]` in monospace | Yes |
| `#part[title]` | Major section divider (unnumbered) | Yes |
| `#set-chapter(n, title)` | Sets current chapter number and title for headers/footers | Yes |
| `#set-appendix(letter, title)` | Sets appendix letter and title | Yes |
| `#hw[LABEL]` | Hardware/silk-screen label: bold Helvetica | Yes — for key labels, port names |
| `#anchor(name)` | Named anchor for cross-references | Yes |
| `#pref(chap, section)` | Prefix-style cross-reference (Chapter N, Section M) | Yes |
| `#cref(target)` | Chapter cross-reference | Yes |
| `#idx[term]` | Index entry (invisible in text, appears in index) | Yes |
| `#make-index()` | Renders the sorted index | Yes |
| `#xref(target)[text]` | Hyperlink cross-reference with anchor | Yes |
| `#c[...]` | Inline code alias | Yes |
| `#cpc-screen[...]` | Simulated CPC screen output block (CPC font, green on black) | Yes — valuable for showing CPC BASIC output |
| `#note[...]` | Note box: "NOTE ---" prefix in bold, indented | Yes |

### Additional functions to create for konCePCja manual

| Function | Purpose |
|----------|---------|
| `#ipc-cmd[cmd]` | IPC command highlight: monospace, TCP context |
| `#cfg-key[section.key]` | Config file key reference |
| `#port[hex]` | I/O port address in amstrad-red monospace |
| `#reg[name]` | Z80 register name (e.g. `AF`, `HL`) in monospace |
| `#fkey[Fn]` | Function key reference: `[F5]`, `[Shift+F3]` |
| `#menu-path[File > Load]` | GUI menu navigation path |

---

## 10. BASIC Keyword Cross-Linking (sibling manual only)

The sibling template has an elaborate auto-link system that converts inline code
matching any CPC BASIC keyword to a hyperlink pointing at its Chapter 3 definition.
This is CPC-BASIC-specific and should **not** be carried over to the konCePCja
manual. Remove the `code-no-fr` state variable and the entire `show raw.where(...)
{ ... }` block that drives it.

---

## 11. Figures and Images

The sibling manual generates all technical diagrams as SVGs using Python scripts
(`tools/gen_keyboard.py`, `tools/gen_bank_diagrams.py`, `tools/gen_port_svgs.py`,
etc.). The SVGs are included in Typst via `#image("images/diagram.svg")`.

**For konCePCja, the same pattern applies.** Figures to generate:
- Keyboard layout SVG (adapt `gen_keyboard.py` for UK/CPC layout annotation)
- Memory map SVG (Z80 address space with ROM/RAM regions)
- I/O port map SVG (gate array, PPI, FDC, ASIC, peripherals)
- CPC hardware block diagram
- IPC connection diagram

The sibling manual's Python tools are in `/Users/ikari/src/cpc/manual/tools/` and
can be adapted or referenced.

---

## 12. Code Block Style

Based on STYLE_GUIDE.md findings:

- No background tint (white page)
- No language tag / syntax highlighting
- BASIC keywords in UPPERCASE, variables in lowercase
- User input and screen output indistinguishable in the same block
- IPC session examples: show prompt `> ` for sent commands, response on next line

Example convention for IPC sessions:
```
> ping
OK pong
> reg get PC
OK 4000
```

Example convention for CPC BASIC:
```
10 PRINT "Hello from konCePCja"
20 GOTO 10
run
Hello from konCePCja
Hello from konCePCja
Break in 20
```

---

## 13. Note and Warning Boxes

Carry over from sibling template:

- `#note[text]` → `NOTE ---` bold prefix, indented, body text follows
- `IMPORTANT` → standalone bold heading paragraph
- `BEWARE` → standalone bold heading, warning text
- `DON'T FORGET` → bold inline label with em-dash

---

## 14. Index

Use `#idx[term]` inline in chapter text. The sibling template's `#make-index()`
collects all entries alphabetically and renders a two-column index. Carry over
unchanged.

---

## 15. Build Script (Makefile skeleton)

```makefile
.PHONY: all watch clean

MKFILE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
PDF        = koncepcja_manual.pdf
SRC        = manual_build.typ
FONTS      = $(MKFILE_DIR)fonts

all: $(PDF)

$(PDF): $(SRC) template.typ $(wildcard chapters/*.typ) $(wildcard images/*)
	typst compile --font-path $(FONTS) $(SRC) $(PDF)

watch:
	typst watch --font-path $(FONTS) $(SRC) $(PDF)

clean:
	rm -f $(PDF)
```

---

## 16. Assets to Copy vs. Recreate

### Copy directly from `/Users/ikari/src/cpc/manual/`

| Asset | Source path | Notes |
|-------|------------|-------|
| All font files | `fonts/*.ttf`, `fonts/*.otf` | Symlink or copy entire `fonts/` directory |
| CPC keyboard SVG | `images/keyboard-layout.svg` | Reusable as-is for CPC key reference |
| Keyboard codes SVG | `images/keyboard-codes.svg` | Useful for key number reference appendix |
| Python SVG tools pattern | `tools/gen_keyboard.py` etc. | Adapt, not copy |

### Recreate for konCePCja

| Asset | Notes |
|-------|-------|
| `template.typ` | Fork sibling template; remove BASIC cross-link system; add konCePCja functions |
| `manual_build.typ` | New top-level entry point with konCePCja title |
| `images/memory-map.svg` | Z80 memory map specific to konCePCja's ROM/RAM layout |
| `images/port-map.svg` | Full I/O port map including ASIC, Symbiface II, M4 ports |
| `images/ipc-diagram.svg` | TCP connection diagram for IPC/telnet/M4 HTTP |
| `images/hardware-block.svg` | CPC hardware block diagram |

---

## 17. Miscellaneous Conventions (from STYLE_GUIDE.md)

- Four dots `....` for continuation/ellipsis (not three)
- Hex values: always monospace with `&` prefix (CPC convention): `&FC00`, `&BE80`
- Key names in square brackets: `[RETURN]`, `[ESC]`, `[SHIFT]`, `[F5]`
- Multi-key combos: `[SHIFT]+[F3]` or `[Shift+F3]`
- Port names, socket labels (hardware silk-screen): `#hw[MONITOR]`, `#hw[VOLUME]`
- File names: monospace UPPERCASE: `` `GAME.DSK` ``, `` `SNAPSHOT.SNA` ``
- Config keys: monospace: `` `system.model` ``, `` `video.scr_scale` ``
- IPC commands: monospace: `` `ping` ``, `` `mem read 0x4000 16` ``
- Hex addresses (Z80): `0x4000` in code contexts, `&4000` in CPC BASIC contexts
