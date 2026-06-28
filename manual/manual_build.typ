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
// Note: chapter numbering is sequential by include order; gaps (ch02/03/07/08/12)
// are filled in later units, and U9 sets the final order.
#include "chapters/ch04_configuration.typ"
#include "chapters/ch05_hardware.typ"
#include "chapters/ch06_peripherals.typ"
#include "chapters/ch09_ipc.typ"
#include "chapters/ch10_telnet.typ"
#include "chapters/ch11_m4.typ"
