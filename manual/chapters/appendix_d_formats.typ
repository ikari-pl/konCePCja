#import "../template.typ": *
#set-appendix(4, "File Formats")

= Supported File Formats

#table(
  columns: (auto, 1fr),
  stroke: 0.4pt + rule-grey, inset: 5pt,
  [*Extension*], [*Format*],
  [`.dsk`], [Disc image (standard and extended)],
  [`.ipf`], [Copy-protected disc preservation format],
  [`.cdt`], [Tape image (CPC tape data)],
  [`.voc`], [Tape image (alternative)],
  [`.cpr`], [Cartridge image (Plus range only)],
  [`.sna`], [Snapshot --- full machine state],
  [`.zip`], [Archive containing any of the above],
  [`.pok`], [Pokes / cheat file],
  [`.bin`], [Raw binary, injected into memory],
  [`.ym`], [PSG chiptune recording (output)],
  [`.avi`], [Video recording (output)],
  [`.gif` / `.png`], [Animated or still screen capture (output)],
)
