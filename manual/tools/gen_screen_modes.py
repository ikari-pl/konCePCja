#!/usr/bin/env python3
"""Generate the CPC screen-modes comparison diagram (images/screen-modes.svg).

Shows the three modes side by side: the same strip width divided into pixels of
each mode's width (mode 0 chunky, mode 2 fine), coloured to convey the palette
size (16 / 4 / 2 inks). Pure-Python SVG, no dependencies."""

import os

RED = "#C41230"
GREY = "#4A4A4A"
RULE = "#999999"

# A representative CPC firmware palette (subset of the 27 hardware colours).
PALETTE = [
    "#000080", "#FFFF00", "#00FFFF", "#FF0000", "#FFFFFF", "#000000",
    "#0000FF", "#FF00FF", "#008080", "#808000", "#80FF80", "#FF8080",
    "#8080FF", "#FFFF80", "#00FF00", "#800080",
]

MODES = [
    (0, "160 × 200", 16, 8),    # (mode, resolution, inks, pixels across the strip)
    (1, "320 × 200", 4, 16),
    (2, "640 × 200", 2, 32),
]

LABEL_X = 20
STRIP_X = 240
STRIP_W = 320
ROW_H = 90
STRIP_H = 56
TOP = 56
W = STRIP_X + STRIP_W + 30
H = TOP + len(MODES) * ROW_H + 20


def main():
    p = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
         f'viewBox="0 0 {W} {H}" font-family="Helvetica, Arial, sans-serif">']
    p.append(f'<rect width="{W}" height="{H}" fill="white"/>')
    p.append(f'<text x="{W/2}" y="30" text-anchor="middle" font-size="14" '
             f'font-weight="bold" fill="{RED}">The three screen modes</text>')

    for i, (mode, res, inks, npix) in enumerate(MODES):
        y = TOP + i * ROW_H
        # label block
        p.append(f'<text x="{LABEL_X}" y="{y + 24}" font-size="17" '
                 f'font-weight="bold" fill="{GREY}">Mode {mode}</text>')
        p.append(f'<text x="{LABEL_X}" y="{y + 44}" font-size="13" '
                 f'fill="{GREY}" font-family="monospace">{res}</text>')
        p.append(f'<text x="{LABEL_X}" y="{y + 62}" font-size="12.5" '
                 f'fill="{RED}">{inks} ink{"s" if inks != 1 else ""}</text>')
        # pixel strip
        pw = STRIP_W / npix
        for k in range(npix):
            col = PALETTE[k % inks]
            p.append(f'<rect x="{STRIP_X + k*pw:.2f}" y="{y}" width="{pw:.2f}" '
                     f'height="{STRIP_H}" fill="{col}" stroke="#ffffff" stroke-width="0.4"/>')
        p.append(f'<rect x="{STRIP_X}" y="{y}" width="{STRIP_W}" height="{STRIP_H}" '
                 f'fill="none" stroke="{GREY}" stroke-width="1.2"/>')

    p.append(f'<text x="{W/2}" y="{H - 6}" text-anchor="middle" font-size="11" '
             f'fill="{GREY}">All three modes use 80 bytes per scan line — colour is traded for resolution.</text>')
    p.append('</svg>')
    out = os.path.join(os.path.dirname(__file__), '..', 'images', 'screen-modes.svg')
    with open(out, 'w', encoding='utf-8') as f:
        f.write('\n'.join(p))
    print(f"Written to {out}")


if __name__ == '__main__':
    main()
