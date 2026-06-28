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

// HTML build only: inline the web theme + a linked table of contents. The
// heading ids (sec-N) are assigned in document order by the broad heading rule
// in template.typ, so enumerate()+1 matches each heading's anchor. (PDF build
// ignores this whole branch.)
#context if target() == "html" {
  html.elem("style", read("web/style.css"))
  html.elem("h1", "konCePCja User Manual")
  let heads = query(heading)
  html.elem("nav", attrs: (class: "toc"),
    html.elem("h2", "Contents") +
    heads.enumerate()
      .filter(((i, h)) => h.level <= 2)
      .map(((i, h)) => html.elem("div", attrs: (class: "toc-l" + str(h.level)),
        html.elem("a", attrs: (href: "#sec-" + str(i + 1)), h.body)))
      .join())
}

// HTML build only: auto-link every "Chapter N" / "Appendix X" reference in the
// prose to that chapter/appendix heading anchor, with computer precision. The
// Nth level-1 heading is Chapter N; appendices A–G follow the 12 chapters. The
// PDF build leaves the text untouched.
#let _num-chapters = 12
#let _xref-href(txt) = {
  let l1 = query(heading).enumerate().filter(p => p.at(1).level == 1).map(p => p.at(0))
  let parts = txt.split(" ")
  let j = if parts.at(0) == "Chapter" {
    int(parts.at(1)) - 1
  } else {
    _num-chapters + (str.to-unicode(parts.at(1)) - str.to-unicode("A"))
  }
  if j >= 0 and j < l1.len() { "#sec-" + str(l1.at(j) + 1) } else { none }
}
#show regex("Chapter \d+|Appendix [A-G]\b"): it => context {
  if target() != "html" {
    it
  } else {
    let href = _xref-href(it.text)
    if href == none { it } else { html.elem("a", attrs: (href: href), it.text) }
  }
}

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

// Appendices
#include "chapters/appendix_a_keys.typ"
#include "chapters/appendix_b_ipc.typ"
#include "chapters/appendix_c_config.typ"
#include "chapters/appendix_d_formats.typ"
#include "chapters/appendix_e_hardware.typ"
#include "chapters/appendix_f_glossary.typ"
#include "chapters/appendix_g_index.typ"
