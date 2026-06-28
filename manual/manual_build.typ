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
// Chapters in order; numbering follows include order. Ch12 (recording) is added
// in U7, and U9 confirms the final ordering.
#include "chapters/ch01_getting_started.typ"
#include "chapters/ch02_interface.typ"
#include "chapters/ch03_media.typ"
#include "chapters/ch04_configuration.typ"
#include "chapters/ch05_hardware.typ"
#include "chapters/ch06_peripherals.typ"
#include "chapters/ch07_devtools.typ"
#include "chapters/ch08_assembler.typ"
#include "chapters/ch09_ipc.typ"
#include "chapters/ch10_telnet.typ"
#include "chapters/ch11_m4.typ"
#include "chapters/ch12_recording.typ"
