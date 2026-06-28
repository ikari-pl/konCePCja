#import "../template.typ": *
#set-appendix(5, "Hardware Specifications")

= Hardware Specifications (Emulated)

A summary of the emulated machine. See Chapter 5 for what each part does.

#table(
  columns: (auto, 1fr),
  stroke: 0.4pt + rule-grey, inset: 5pt,
  [*Component*], [*Specification*],
  [CPU], [Zilog Z80A at 4 MHz],
  [RAM], [64 KB (464/664), 128 KB (6128/6128+), expandable to 4 MB],
  [Video controller], [CRTC types 0--3, plus ASIC on the 6128+],
  [Screen modes], [Mode 0 (160×200, 16 colours), Mode 1 (320×200, 4 colours), Mode 2 (640×200, 2 colours)],
  [Palette], [27 colours (4096 on the Plus)],
  [Sound], [AY-3-8912 PSG --- 3 tone channels, 1 noise, hardware envelopes],
  [Disc], [uPD765A FDC, 3-inch discs, standard and extended formats],
  [Refresh], [50 Hz],
)
