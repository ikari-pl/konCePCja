#!/usr/bin/env python3
"""Generate the CPC 27-colour hardware palette swatch (images/palette.svg).

Each channel is one of three levels (0 / 0x7F / 0xFF); the 27 distinct
combinations are the CPC's hardware colours. Pure-Python SVG, no dependencies."""

import os

GREY = "#4A4A4A"
RED = "#C41230"

# (firmware ink number, name, hex) — the 27 CPC hardware colours.
COLOURS = [
    (0, "Black", "#000000"), (1, "Blue", "#00007F"), (2, "Bright Blue", "#0000FF"),
    (3, "Red", "#7F0000"), (4, "Magenta", "#7F007F"), (5, "Mauve", "#7F00FF"),
    (6, "Bright Red", "#FF0000"), (7, "Purple", "#FF007F"), (8, "Bright Magenta", "#FF00FF"),
    (9, "Green", "#007F00"), (10, "Cyan", "#007F7F"), (11, "Sky Blue", "#007FFF"),
    (12, "Yellow", "#7F7F00"), (13, "White", "#7F7F7F"), (14, "Pastel Blue", "#7F7FFF"),
    (15, "Orange", "#FF7F00"), (16, "Pink", "#FF7F7F"), (17, "Pastel Magenta", "#FF7FFF"),
    (18, "Bright Green", "#00FF00"), (19, "Sea Green", "#00FF7F"), (20, "Bright Cyan", "#00FFFF"),
    (21, "Lime", "#7FFF00"), (22, "Pastel Green", "#7FFF7F"), (23, "Pastel Cyan", "#7FFFFF"),
    (24, "Bright Yellow", "#FFFF00"), (25, "Pastel Yellow", "#FFFF7F"), (26, "Bright White", "#FFFFFF"),
]

COLS = 3
SW = 28          # swatch size
ROW_H = 30
CELL_W = 200
PAD_X, TOP = 16, 46
ROWS = (len(COLOURS) + COLS - 1) // COLS
W = PAD_X * 2 + COLS * CELL_W
H = TOP + ROWS * ROW_H + 16


def luminance(hexc):
    r, g, b = (int(hexc[i:i+2], 16) for i in (1, 3, 5))
    return 0.299*r + 0.587*g + 0.114*b


def main():
    p = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
         f'viewBox="0 0 {W} {H}" font-family="Helvetica, Arial, sans-serif">']
    p.append(f'<rect width="{W}" height="{H}" fill="white"/>')
    p.append(f'<text x="{W/2}" y="28" text-anchor="middle" font-size="14" '
             f'font-weight="bold" fill="{RED}">The 27 CPC hardware colours</text>')

    for idx, (ink, name, hexc) in enumerate(COLOURS):
        col = idx % COLS
        row = idx // COLS
        x = PAD_X + col * CELL_W
        y = TOP + row * ROW_H
        border = "#cccccc" if luminance(hexc) > 230 else hexc
        p.append(f'<rect x="{x}" y="{y}" width="{SW}" height="{SW}" rx="3" '
                 f'fill="{hexc}" stroke="{border}" stroke-width="1"/>')
        p.append(f'<text x="{x + SW + 8}" y="{y + 12}" font-size="11" '
                 f'fill="{GREY}" font-family="monospace">{ink}</text>')
        p.append(f'<text x="{x + SW + 8}" y="{y + 24}" font-size="12" fill="{GREY}">{name}</text>')

    p.append('</svg>')
    out = os.path.join(os.path.dirname(__file__), '..', 'images', 'palette.svg')
    with open(out, 'w') as f:
        f.write('\n'.join(p))
    print(f"Written to {out}")


if __name__ == '__main__':
    main()
