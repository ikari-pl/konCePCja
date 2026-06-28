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
#include "chapters/ch01_getting_started.typ"
