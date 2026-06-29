#!/usr/bin/env python3
"""Generate the CPC 64K memory-map diagram (images/memory-map.svg).

Pure-Python SVG emitter, no dependencies — same pattern as gen_keyboard.py.
Shows the four 16 KB banks of the Z80 address space with the Lower/Upper ROM
overlays. Palette matches the manual template (Amstrad red/blue/greys)."""

import os

RED = "#C41230"
BLUE = "#1B3A5C"
GREY = "#4A4A4A"
RULE = "#999999"
CREAM = "#F5F3EE"
ROMBG = "#DCE6F0"  # light blue tint for ROM-bearing banks

# Geometry
COL_X, COL_W = 170, 240          # the memory column
TOP_Y, BANK_H = 50, 78           # first bank top, bank height
W, H = 620, TOP_Y + 4 * BANK_H + 50

# Banks, top (high address) to bottom (low address)
BANKS = [
    ("&C000", "&FFFF", "RAM", "Upper ROM", "AMSDOS / expansion ROM"),
    ("&8000", "&BFFF", "RAM", None, None),
    ("&4000", "&7FFF", "RAM", None, None),
    ("&0000", "&3FFF", "RAM", "Lower ROM", "BASIC (cartridge on Plus)"),
]


def esc(s):
    return s.replace("&", "&amp;")


def main():
    p = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
         f'viewBox="0 0 {W} {H}" font-family="Helvetica, Arial, sans-serif">']
    p.append(f'<rect width="{W}" height="{H}" fill="white"/>')

    for i, (lo, hi, ram, rom, romdesc) in enumerate(BANKS):
        y = TOP_Y + i * BANK_H
        fill = ROMBG if rom else CREAM
        p.append(f'<rect x="{COL_X}" y="{y}" width="{COL_W}" height="{BANK_H}" '
                 f'fill="{fill}" stroke="{GREY}" stroke-width="1.5"/>')
        # bank content label (centred)
        cy = y + BANK_H / 2
        p.append(f'<text x="{COL_X + COL_W/2}" y="{cy - 4}" text-anchor="middle" '
                 f'font-size="16" font-weight="bold" fill="{GREY}">{ram}</text>')
        if rom:
            p.append(f'<text x="{COL_X + COL_W/2}" y="{cy + 16}" text-anchor="middle" '
                     f'font-size="12" fill="{BLUE}">or {esc(rom)}</text>')
        # address labels on the left edge (high addr at the bank's top line)
        p.append(f'<text x="{COL_X - 12}" y="{y + 5}" text-anchor="end" '
                 f'font-size="12" fill="{GREY}" font-family="monospace">{esc(hi)}</text>')
    # bottom address (lowest)
    yb = TOP_Y + 4 * BANK_H
    p.append(f'<text x="{COL_X - 12}" y="{yb + 5}" text-anchor="end" '
             f'font-size="12" fill="{GREY}" font-family="monospace">&amp;0000</text>')

    # ROM overlay brackets on the right
    rx = COL_X + COL_W + 14
    for i, (lo, hi, ram, rom, romdesc) in enumerate(BANKS):
        if not rom:
            continue
        y = TOP_Y + i * BANK_H
        p.append(f'<path d="M{rx} {y+6} h10 v{BANK_H-12} h-10" fill="none" '
                 f'stroke="{RED}" stroke-width="2"/>')
        p.append(f'<text x="{rx + 16}" y="{y + BANK_H/2 - 2}" font-size="12" '
                 f'font-weight="bold" fill="{RED}">{esc(rom)}</text>')
        p.append(f'<text x="{rx + 16}" y="{y + BANK_H/2 + 14}" font-size="10.5" '
                 f'fill="{GREY}">{esc(romdesc)}</text>')

    # title
    p.append(f'<text x="{COL_X + COL_W/2}" y="28" text-anchor="middle" '
             f'font-size="14" font-weight="bold" fill="{RED}">Z80 address space (64 KB)</text>')
    # footnote
    p.append(f'<text x="{W/2}" y="{H - 18}" text-anchor="middle" font-size="11" '
             f'fill="{GREY}">ROM reads overlay RAM; writes always reach the RAM underneath.</text>')

    p.append('</svg>')
    out = os.path.join(os.path.dirname(__file__), '..', 'images', 'memory-map.svg')
    with open(out, 'w', encoding='utf-8') as f:
        f.write('\n'.join(p))
    print(f"Written to {out}")


if __name__ == '__main__':
    main()
