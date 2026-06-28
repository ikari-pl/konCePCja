#import "../template.typ": *

// ═══════════════════════════════════════════════════════════════════════
// Front Matter — pages before Chapter 1.
// konCePCja's own content (no Amstrad-copyrighted material).
// ═══════════════════════════════════════════════════════════════════════

// ─── Title page ─────────────────────────────────────────────────────
#page(header: none, footer: none, margin: (top: 30mm, bottom: 20mm, inside: 25mm, outside: outside-margin))[
  #v(1.6in)
  #align(center)[
    #text(font: heading-font, size: 40pt, weight: "bold", fill: amstrad-red)[konCePCja]
    #v(10pt)
    #line(length: 60%, stroke: 1.5pt + amstrad-red)
    #v(12pt)
    #text(font: heading-font, size: 16pt, fill: amstrad-blue)[User Manual]
    #v(4pt)
    #text(size: 10pt, fill: amstrad-grey)[An Amstrad CPC emulator]
    #v(2in)
    #text(size: 9pt, fill: amstrad-grey)[First Edition --- 2026]
  ]
]

// ─── Copyright and licence ──────────────────────────────────────────
#page(header: none)[
  #v(1fr)
  #text(size: 9pt, fill: amstrad-grey)[
    konCePCja is an Amstrad CPC emulator based on Caprice32.

    #v(6pt)
    © 1997–2015 Ulrich Doewich · © 2016–2025 Colin Pitrat (Caprice32) \
    © 2024–2026 the konCePCja contributors

    #v(6pt)
    konCePCja is free software, distributed under the terms of the GNU General
    Public License, version 2 (GPLv2), included with the source as `COPYING.txt`.
    This manual is distributed with konCePCja under the same terms.

    #v(6pt)
    Amstrad, CPC, CPC464, CPC664, CPC6128, and CPC6128+ are trademarks of their
    respective owners. This is an independent project and is not affiliated with,
    nor endorsed by, the trademark holders.

    #v(6pt)
    Font credits: Rokkitt (OFL), TeX Gyre Schola (GUST), OCR-B by Matthew Skala,
    JuliaMono (OFL), Cascadia Code (OFL), and "Amstrad CPC correct" by Damien
    Guard (CC-BY-SA 3.0). See `manual/fonts/LICENSES.md`.
  ]
]

// ─── About this manual ──────────────────────────────────────────────
#page[
  #text(font: heading-font, size: 18pt, weight: "bold")[About This Manual]
  #v(8pt)

  This manual is a guide to using konCePCja, from first launch through to its
  power-user features. It assumes you know roughly what an Amstrad CPC is, but
  not how the emulator's internals work.

  Early chapters cover installation, loading software, the interface, and
  configuration. Later chapters document the emulated hardware and peripherals,
  the developer tools, and the scripting interfaces — the IPC debugging protocol
  (e.g. #ipc-cmd[mem read 0x4000 16]), the telnet console, and the M4 Board's
  web interface — for technically inclined readers.

  #v(6pt)
  Conventions used throughout: function keys appear as #fkey[F12]; menu paths as
  #menu-path("Settings", "Video"); configuration keys as #cfg-key[system.model];
  I/O ports as #port[\&FD06]; and Z80 registers as #reg[HL].
]
