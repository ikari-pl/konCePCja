#!/usr/bin/env python3
"""Generate the host<->emulator connectivity diagram (images/topology.svg).

Shows the three TCP services konCePCja exposes (IPC, telnet console, M4 HTTP)
and the kind of client that talks to each. Pure-Python SVG, no dependencies."""

import os

RED = "#C41230"
BLUE = "#1B3A5C"
GREY = "#4A4A4A"
RULE = "#999999"
CREAM = "#F5F3EE"

W, H = 620, 320
BOX_W, BOX_H = 180, 220
LX, RX = 30, W - 30 - BOX_W      # left/right box x
BY = 50                          # boxes top

LINKS = [
    ("IPC", "6543", "scripts, debuggers", 0),
    ("Telnet console", "6544", "any terminal", 1),
    ("M4 HTTP", "8080", "web browser", 2),
]


def box(x, title, sub):
    return (f'<rect x="{x}" y="{BY}" width="{BOX_W}" height="{BOX_H}" rx="8" '
            f'fill="{CREAM}" stroke="{GREY}" stroke-width="1.5"/>'
            f'<text x="{x + BOX_W/2}" y="{BY + 28}" text-anchor="middle" '
            f'font-size="16" font-weight="bold" fill="{BLUE}">{title}</text>'
            f'<text x="{x + BOX_W/2}" y="{BY + 48}" text-anchor="middle" '
            f'font-size="11.5" fill="{GREY}">{sub}</text>')


def main():
    p = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
         f'viewBox="0 0 {W} {H}" font-family="Helvetica, Arial, sans-serif">']
    p.append(f'<rect width="{W}" height="{H}" fill="white"/>')
    p.append(f'<text x="{W/2}" y="28" text-anchor="middle" font-size="14" '
             f'font-weight="bold" fill="{RED}">Talking to the emulator over TCP</text>')
    p.append(box(LX, "Your computer", "host"))
    p.append(box(RX, "konCePCja", "the emulator"))

    x1 = LX + BOX_W
    x2 = RX
    for label, port, client, i in LINKS:
        y = BY + 80 + i * 50
        p.append(f'<line x1="{x1}" y1="{y}" x2="{x2}" y2="{y}" stroke="{GREY}" '
                 f'stroke-width="1.5" marker-end="url(#arrow)" marker-start="url(#arrowback)"/>')
        midx = (x1 + x2) / 2
        p.append(f'<text x="{midx}" y="{y - 6}" text-anchor="middle" font-size="12.5" '
                 f'font-weight="bold" fill="{GREY}">{label}</text>')
        p.append(f'<text x="{midx}" y="{y + 14}" text-anchor="middle" font-size="11.5" '
                 f'fill="{RED}" font-family="monospace">port {port}</text>')
        # client hint on the left side
        p.append(f'<text x="{LX + BOX_W/2}" y="{y + 4}" text-anchor="middle" '
                 f'font-size="10.5" fill="{GREY}" font-style="italic">{client}</text>')

    # arrow markers
    p.insert(1, f'<defs>'
             f'<marker id="arrow" markerWidth="9" markerHeight="9" refX="7" refY="3" '
             f'orient="auto"><path d="M0,0 L7,3 L0,6 Z" fill="{GREY}"/></marker>'
             f'<marker id="arrowback" markerWidth="9" markerHeight="9" refX="2" refY="3" '
             f'orient="auto"><path d="M7,0 L0,3 L7,6 Z" fill="{GREY}"/></marker></defs>')

    p.append('</svg>')
    out = os.path.join(os.path.dirname(__file__), '..', 'images', 'topology.svg')
    with open(out, 'w') as f:
        f.write('\n'.join(p))
    print(f"Written to {out}")


if __name__ == '__main__':
    main()
