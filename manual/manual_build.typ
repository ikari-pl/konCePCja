// konCePCja User Manual — English edition build entry point.
//
// U1 stub: proves the forked toolchain compiles and the free font substitutes
// resolve (heading = Rokkitt, body = TeX Gyre Schola, code = Skala OCR B).
// Chapters are wired in here as they are authored (U4 onward).

#import "template.typ": *

#show: cpc-manual.with(
  title: "konCePCja User Manual",
  edition: "First Edition, 2026",
)

#include "chapters/front_matter.typ"

= konCePCja User Manual

#intro[
  This is a build-system smoke test for the konCePCja manual. The body text is
  set in TeX Gyre Schola, headings in Rokkitt, and inline code such as
  `mem read 0x4000 16` in Skala OCR B. If this page renders with those three
  faces and no fallback warnings, the toolchain fork (U1) is complete.
]

== A second-level heading

Ordinary paragraph text to exercise the body face and justification. The CPC
is an 8-bit machine built around a Z80A CPU.

```
10 PRINT "HELLO, CPC"
20 GOTO 10
```
