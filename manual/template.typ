// ═══════════════════════════════════════════════════════════════════════
// CPC 6128 User Manual — Typst Template
// Recreating the 1985 Amstrad manual typography and layout
// ═══════════════════════════════════════════════════════════════════════
//
// Fonts:
//   Headers:  Rockwell (system) — standing in for Geometric Slabserif 712
//   Body:     Palatino N90 (standing in for Madison Antiqua)
//   Code:     OCR B (public domain / OFL)
//
// Page: 6.5 × 9.19 inches (468 × 661.4 pt) — matched from the original scan

// ─── Font registration ───────────────────────────────────────────────

// When Madison Antiqua is available, drop the TTF/OTF into fonts/ and
// change the body-font constant below.

#let heading-font  = "Rokkitt"
#let body-font     = "TeX Gyre Schola"
#let code-font     = "OCR B"
// Narrower body face for dense blocks (e.g. colour table, side-by-side
// program listings). Manually opt in via `#text(font: font-narrower)[…]`
// or a local `set text(font: font-narrower)` inside a block. Keeping
// it as a shared constant so both editions can reach for it.
#let font-narrower = "TeX Gyre Schola"

// ─── Live-tunable constants ─────────────────────────────────────────
// Override any value via: typst compile --input key=value
// The overlay tool uses this for interactive tuning.

#let _pt(k, d) = if k in sys.inputs { float(sys.inputs.at(k)) * 1pt } else { d }
#let _em(k, d) = if k in sys.inputs { float(sys.inputs.at(k)) * 1em } else { d }
#let _mm(k, d) = if k in sys.inputs { float(sys.inputs.at(k)) * 1mm } else { d }

// Body text
#let body-size     = _pt("fs", 10.25pt)
#let body-leading  = _pt("ld", 4pt)
#let body-spacing  = _em("ps", 1.0em)
#let body-tracking = _pt("ft", 0pt)

// Code
#let code-size     = _pt("cs", 10pt)
#let code-tracking = _pt("ct", 0.10pt)
#let code-leading  = _pt("cld", 5.65pt)

// Page
#let outside-margin = _pt("om", 40pt)

// H1 (Chapter title)
#let h1-size        = _pt("h1", 23pt)
#let h1-leading     = _em("h1ld", 0.35em)
#let h1-tracking    = _pt("h1t", -0.2pt)
#let h1-offset      = _mm("h1off", -13mm)
#let h1-rules-gap   = _pt("h1r", 5.5pt)   // gap from text descenders to thick rule
#let inter-rule-gap = _pt("rr", 1.75pt)
#let rules-body-gap = _pt("r2b", 31pt)

// H2 (Part size)
#let part-size   = _pt("pts", 22.5pt)

// H2 (Section heading)
#let h2-size     = _pt("h2", 21.5pt)
#let h2-tracking = _pt("h2t", -0.1pt)
#let h2-leading  = _pt("h2ld", 9pt)
#let h2-above    = _pt("bh2", 23.5pt)
#let h2-below    = _pt("h2b", 23pt)

// H3 (Subsection heading)
#let h3-size     = _pt("h3", 16.5pt)
#let h3-tracking = _pt("h3t", -0.1pt)
#let h3-above    = _pt("bh3", 24pt)
#let h3-below    = _pt("h3b", 12pt)

// H4 (Sub-subsection)
#let h4-size = _pt("h4", 14pt)
#let h4-above    = _pt("bh4", 14pt)
#let h4-below    = _pt("h4b", 8pt)

// H5
#let h5-size = _pt("h5", 12pt)

// Intro paragraph
#let intro-size     = _pt("ins", 13.3pt)
#let intro-tracking = _pt("int", -0.2pt)
#let intro-leading  = _em("inld", 0.37em)
#let intro-spacing  = _em("inps", 1.60em)

// Code blocks
#let code-above = _pt("ca", 8pt)
#let code-below = _pt("cb", 8pt)
#let code-above-override = state("code-above-override", none)
#let code-below-override = state("code-below-override", none)
#let code-size-override = state("code-size-override", none)
#let code-no-fr = state("code-no-fr", false)

// Inline backticked BASIC keywords are auto-linked to their reference page
// in Chapter 3. Disable this when the target chapter hasn't been
// translated/included yet (e.g. Polish edition while ch3 is a stub) — the
// keywords then render as plain inline code.
// konCePCja: BASIC-keyword cross-linking is off by default — this manual has no
// BASIC keyword reference chapter to link to (the <kw-NAME> targets don't exist).
#let kw-linking = state("kw-linking", false)
// When true, wrapped lines in raw blocks get a per-line hanging indent equal
// to the width of the leading line-number prefix (digits + space). Each line
// becomes its own paragraph so each gets its own measured indent.
#let code-hang = state("code-hang", false)

// Lists
#let list-indent      = _pt("li", 24pt)
#let list-body-indent = _pt("lib", 6pt)

// ─── Colour palette (from the original manual's cover/accents) ───────

#let amstrad-red    = rgb("#C41230")
#let amstrad-blue   = rgb("#1B3A5C")
#let amstrad-grey   = rgb("#4A4A4A")
#let rule-grey      = rgb("#999999")
#let code-bg        = rgb("#F5F3EE")

// ─── BASIC keyword cross-linking ─────────────────────────────────────
// Set of canonical BASIC keyword names with entries in Chapter 3.
// Used by the inline-code show rule to turn `PRINT`, `MID$`, etc. into
// clickable PDF links pointing at their definition.
// Generated from chapters/ch3_basic_keywords.typ (#keyword calls).
// Meta-entries like "MID$ (command form)" and "ON \<expression\> GOSUB"
// are intentionally excluded — users type the bare form.

#let basic-keywords = (
  "ABS", "AFTER", "AND", "ASC", "ATN", "AUTO",
  "BIN$", "BORDER", "BREAK", "CALL", "CAT", "CHAIN",
  "CHAIN MERGE", "CHR$", "CINT", "CLEAR", "CLEAR INPUT", "CLG",
  "CLOSEIN", "CLOSEOUT", "CLS", "CONT", "COPYCHR$", "COS",
  "CREAL", "CURSOR", "DATA", "DEC$", "DEF FN", "DEFINT",
  "DEFREAL", "DEFSTR", "DEG", "DELETE", "DERR", "DI",
  "DIM", "DRAW", "DRAWR", "EDIT", "EI", "ELSE",
  "END", "ENT", "ENV", "EOF", "ERASE", "ERL",
  "ERR", "ERROR", "EVERY", "EXP", "FILL", "FIX",
  "FN", "FOR", "FRAME", "FRE", "GOSUB", "GOTO",
  "GRAPHICS PAPER", "GRAPHICS PEN", "HEX$", "HIMEM", "IF", "INK",
  "INKEY", "INKEY$", "INP", "INPUT", "INSTR", "INT",
  "KEY", "KEY DEF", "LEFT$", "LEN", "LET", "LINE INPUT",
  "LIST", "LOAD", "LOCATE", "LOG", "LOG10", "LOWER$",
  "MASK", "MAX", "MEMORY", "MERGE", "MID$", "MIN",
  "MOD", "MODE", "MOVE", "MOVER", "NEW", "NEXT",
  "NOT", "ON BREAK CONT", "ON BREAK GOSUB", "ON BREAK STOP", "ON ERROR GOTO", "ON SQ GOSUB",
  "OPENIN", "OPENOUT", "OR", "ORIGIN", "OUT", "PAPER",
  "PEEK", "PEN", "PI", "PLOT", "PLOTR", "POKE",
  "POS", "PRINT", "PRINT USING", "RAD", "RANDOMIZE", "READ",
  "RELEASE", "REM", "REMAIN", "RENUM", "RESTORE", "RESUME",
  "RESUME NEXT", "RETURN", "RIGHT$", "RND", "ROUND", "RUN",
  "SAVE", "SGN", "SIN", "SOUND", "SPACE$", "SPC",
  "SPEED INK", "SPEED KEY", "SPEED WRITE", "SQ", "SQR", "STEP",
  "STOP", "STR$", "STRING$", "SWAP", "SYMBOL", "SYMBOL AFTER",
  "TAB", "TAG", "TAGOFF", "TAN", "TEST", "TESTR",
  "THEN", "TIME", "TO", "TROFF", "TRON", "UNT",
  "UPPER$", "USING", "VAL", "VPOS", "WAIT", "WEND",
  "WHILE", "WIDTH", "WINDOW", "WINDOW SWAP", "WRITE", "XOR",
  "XPOS", "YPOS", "ZONE",
)

// Convert a BASIC keyword name into a safe Typst label string.
// Spaces become "-"; "$" becomes "-dollar" (Typst labels don't accept "$").
#let _kw-label-name(name) = {
  "kw-" + name.replace(" ", "-").replace("$", "-dollar")
}

// ─── Page setup ──────────────────────────────────────────────────────

#let manual-page = (
  width: 176mm,
  height: 250mm,
  margin: (
    top: 38mm,
    bottom: 20mm,
    inside: 70pt,
    outside: outside-margin,
  ),
)

// ─── Chapter tracking ────────────────────────────────────────────────

#let chapter-title = state("chapter-title", none)
#let chapter-number = state("chapter-number", 0)
// Localisation. Re-exported so `#import "../template.typ": *` in chapter
// files picks up i18n() and the manual-lang state.
#import "i18n.typ": *

// chapter-label stores the i18n KEY ("chapter" / "appendix"), not the
// localised string. Display uses i18n(); comparisons use the key
// directly so logic is language-stable.
#let chapter-label = state("chapter-label", "chapter")
#let chapter-start-page = state("chapter-start-page", 1)
#let is-chapter-page = state("is-chapter-page", false)
#let chapter-h1-sep = state("chapter-h1-sep", [ \ ])

// Tracks which BASIC keyword labels have already been emitted, so that
// duplicate #keyword() entries (e.g. RUN has two forms) only anchor the
// cross-link label on the first occurrence.
#let _kw-seen = state("kw-seen", ())

// ─── Running header/footer ───────────────────────────────────────────
// Header: thick rule at top of every page (except chapter-opening pages
// which have the rule below the title instead), plus absolute page number

#let header-content() = context {
  let page-num = counter(page).get().first()
  if page-num > 2 and not is-chapter-page.get() {
    set text(font: heading-font, size: 8pt, fill: amstrad-grey)
    if calc.odd(page-num) {
      [#h(1fr) #page-num]
    } else {
      [#page-num #h(1fr)]
    }
    v(2pt)
    line(length: 100%, stroke: 1.2pt + amstrad-red)
  }
}

#let footer-content() = context {
  let page-num = counter(page).get().first()
  if page-num > 2 {
    let ch-title = chapter-title.get()
    let ch-num = chapter-number.get()
    let ch-start = chapter-start-page.get()
    if ch-title != none {
      let ch-page = page-num - ch-start + 1
      line(length: 100%, stroke: 0.4pt + black)
      v(1pt)
      set text(font: heading-font, size: 10pt, weight: "regular")
      let ch-label = i18n(chapter-label.get())
      let pg-label = i18n("page-cap")
      if calc.odd(page-num) {
        [#ch-title #h(1fr) #ch-label #ch-num #h(6pt) #pg-label #ch-page]
      } else {
        [#ch-label #ch-num #h(6pt) #pg-label #ch-page #h(1fr) #ch-title]
      }
    }
  }
}

// ─── Document template ───────────────────────────────────────────────

#let cpc-manual(
  title: "konCePCja User Manual",
  edition: "First Edition, 2026",
  body,
) = {
  // Document metadata
  set document(title: title, author: "konCePCja project")

  // Page geometry
  set page(
    width: manual-page.width,
    height: manual-page.height,
    margin: manual-page.margin,
    header: header-content(),
    header-ascent: 18pt,
    footer: footer-content(),
  )

  // Body text defaults
  set text(
    font: body-font,
    size: body-size,
    fill: black,
    lang: "en",
    hyphenate: false,
    tracking: body-tracking,
  )

  // Prevent "CP/M" from breaking across lines
  show "CP/M": box[CP/M]

  set par(
    justify: true,
    leading: body-leading,
    first-line-indent: 0em,
    spacing: body-spacing,
  )

  // ── Inline-raw renderer (used by body show rule + heading/footnote overrides) ──
  // Renders `code` inline at a caller-specified size, preserving
  // BASIC-keyword cross-linking. We render `it.text` (the string)
  // rather than `it` (the raw element) so the global raw show rule
  // can't re-fire on our output and override the size we just set.
  let _inline-raw-render(it, size) = {
    let styled = text(font: code-font, weight: "regular", size: size, tracking: code-tracking, it.text)
    let padded = box(inset: (right: 1pt), styled)
    if it.text in basic-keywords and kw-linking.get() {
      link(label(_kw-label-name(it.text)), padded)
    } else {
      padded
    }
  }

  // ── Heading styles ──────────────────────────────────────────────

  // Chapter title — two-line format matching original:
  //   Chapter N
  //   Short Title
  //   ══════════ (red double rule)
  // Header suppression via is-chapter-page state (set by set-chapter/set-appendix)
  show heading.where(level: 1): it => {
    // v(1fr) + pagebreak are emitted by set-chapter/set-appendix *before* this heading,
    // so the heading element's location is already on the new page.
    v(h1-offset)  // reclaim empty header space (25mm margin → effective 12mm)
    chapter-number.update(n => n + 1)
    context chapter-start-page.update(counter(page).get().first())
    // Title + rules in one block: par spacing zeroed so h1-rules-gap is the
    // only gap between text descenders and the thick rule — stable across all chapters.
    block(above: 0pt, below: 0pt)[
      #set par(spacing: 0pt)
      #context {
        set par(leading: h1-leading)
        let ch-num = chapter-number.get()
        let ch-title = chapter-title.get()
        let ch-label = i18n(chapter-label.get())
        let sep = chapter-h1-sep.get()
        text(font: heading-font, size: h1-size, weight: "bold", tracking: h1-tracking)[#ch-label #ch-num#sep#ch-title]
      }
      #v(h1-rules-gap)
      #place(line(length: 100%, stroke: 1.2pt + amstrad-red))
      #v(1.2pt)
      #v(inter-rule-gap)
      #place(line(length: 100%, stroke: 0.4pt + amstrad-red))
      #v(0.4pt)
    ]
    v(rules-body-gap)
    is-chapter-page.update(false)
  }

  // Section heading (e.g. "Fitting a Mains Plug", "Part 1: Setting Up ....")
  // Fixed spacing; add v(1fr) manually before headings where pages need breathing room
  show heading.where(level: 2): it => {
    set text(font: heading-font, size: h2-size, weight: "bold", hyphenate: false, tracking: h2-tracking)
    set par(justify: false, leading: h2-leading)
    show raw.where(block: false): r => _inline-raw-render(r, h2-size)
    block(above: h2-above, below: h2-below)[#it.body]
  }

  // Subsection heading
  show heading.where(level: 3): it => {
    set text(font: heading-font, size: h3-size, weight: "bold", hyphenate: false, tracking: h3-tracking)
    set par(justify: false)
    show raw.where(block: false): r => _inline-raw-render(r, h3-size)
    block(above: h3-above, below: h3-below)[#it.body]
  }


  // Sub-subsection heading (tutorial keyword headings like CLS, PRINT)
  show heading.where(level: 4): it => {
    set text(font: heading-font, size: h4-size, weight: "bold", hyphenate: false)
    set par(justify: false)
    show raw.where(block: false): r => _inline-raw-render(r, h4-size)
    block(above: h4-above, below: h4-below)[#it.body]
  }

  show heading.where(level: 5): it => {
    set text(font: heading-font, size: h5-size, weight: "bold", hyphenate: false)
    set par(justify: false)
    show raw.where(block: false): r => _inline-raw-render(r, h5-size)
    block(above: 12pt, below: 6pt)[#it.body]
  }

  // Footnote: inline raw should track the (smaller) footnote text size
  // rather than getting pinned to body code-size by the global rule below.
  // Using 1em scales with whatever set text(...) the footnote.entry inherits.
  show footnote.entry: it => {
    show raw.where(block: false): r => _inline-raw-render(r, 1.1em)
    it
  }

  // ── Code blocks ─────────────────────────────────────────────────

  show raw.where(block: true): it => context {
    let ca = code-above-override.get()
    let cb = code-below-override.get()
    let cs = code-size-override.get()
    let ca = if ca != none { ca } else { code-above }
    let cb = if cb != none { cb } else { code-below }
    let cs = if cs != none { cs } else { code-size }
    v(ca)
    if not code-no-fr.get() { v(0.2fr) }
    set par(leading: code-leading)
    let hang = code-hang.get()
    pad(left: 3em, layout(size => {
      set text(font: code-font, weight:"regular", size: cs, tracking: code-tracking)
      let avail-w = size.width
      // Process each line: insert zero-width spaces after BASIC operators
      // so Typst can wrap long lines without inserting visible spaces.
      let zwsp = "\u{200B}"
      let lines = it.text.split("\n")
      // In hang mode we pre-compute wrap positions so each soft wrap
      // becomes a hard linebreak with a visible ↵ marker. This requires
      // measuring cumulative segment widths against the available width,
      // accounting for the hanging-indent on continuation lines.
      // Marker width: leading 0.3em gap + ↵ glyph at 0.85em. Reserved at
      // the end of every wrapped chunk so the trailing marker can never
      // overflow onto the next line — without this slack a chunk that
      // fits avail-w exactly pushes the marker into the continuation.
      let marker-w = 0.3em.to-absolute() + measure(text(
        font: code-font, size: 0.85 * cs, tracking: code-tracking,
      )[↵]).width
      let hard-wrap = (raw-t, hang-w-local) => {
        let segments = raw-t.split(zwsp)
        let chunks = ()
        let current = ""
        let is-first = true
        for seg in segments {
          if current == "" {
            current = seg
          } else {
            let candidate = current + seg
            let cur-avail = if is-first {
              avail-w - marker-w
            } else {
              avail-w - hang-w-local - marker-w
            }
            let w = measure(text(
              font: code-font, size: cs, tracking: code-tracking,
            )[#candidate]).width
            if w > cur-avail {
              chunks.push(current)
              current = seg
              is-first = false
            } else {
              current = candidate
            }
          }
        }
        chunks.push(current)
        chunks
      }
      for (i, line) in lines.enumerate() {
        // Split off ↵ (RETURN marker) and ⊳ (body-font comment)
        let has-return = line.contains(" ↵")
        let comment = none
        let t = line
        if has-return {
          let parts = t.split(" ↵")
          t = parts.first()
          let rest = parts.slice(1).join(" ↵")
          if rest.starts-with(" ⊳ ") {
            comment = rest.slice(" ⊳ ".len())
          }
        } else if t.contains(" ⊳ ") {
          let parts = t.split(" ⊳ ")
          t = parts.first()
          comment = parts.slice(1).join(" ⊳ ")
        }
        // Detect line-number prefix BEFORE inserting zwsp/nbsp, then measure
        // its width in the actual code font so the hanging indent lines up
        // with the first character after the line number.
        let hang-w = 0pt
        if hang {
          let m = t.match(regex("^([0-9]+ )"))
          if m != none {
            hang-w = measure(text(
              font: code-font, size: cs, tracking: code-tracking,
            )[#m.captures.first().replace(" ", "\u{00A0}")]).width
          }
        }
        for bc in ("+", ":", ",", ";", ")", " ") {
          t = t.replace(bc, bc + zwsp)
        }
        // Non-breaking spaces preserve indentation outside raw context
        t = t.replace(" ", "\u{00A0}")
        let line-body = {
          if hang {
            // Pre-compute wrap chunks so each soft wrap becomes a hard
            // linebreak with a small ↵ marker at the break point.
            let chunks = hard-wrap(t, hang-w)
            for (j, chunk) in chunks.enumerate() {
              [#chunk]
              if j < chunks.len() - 1 {
                h(0.3em)
                // Amstrad-red so it's clearly typesetter-induced, not a
                // real RETURN key the reader should press.
                text(size: 0.85em, fill: amstrad-red)[↵]
                linebreak()
              }
            }
          } else {
            [#t]
          }
          if has-return { [#h(6pt)#box(baseline: -0.5pt)[#text(font: "Helvetica", size: 10pt, weight: "bold", tracking: 0pt)[\[RETURN\]]]] }
          if comment != none {
            // Render comment with backtick sections in code font and ↵ as [RETURN]
            let ret-tag = box(baseline: -0.5pt)[#text(font: "Helvetica", size: 10pt, weight: "bold", tracking: 0pt)[\[RETURN\]]]
            h(1fr)
            text(font: body-font, size: body-size, tracking: 0pt, {
              for segment in comment.split("`") .enumerate() {
                let (idx, s) = segment
                if calc.odd(idx) {
                  // Inside backticks → code font
                  let cs = s.replace("↵", "")
                  text(font: code-font, size: code-size, tracking: code-tracking)[#cs]
                  if s.contains("↵") { ret-tag }
                } else {
                  // Body text — also handle bare ↵
                  let parts = s.split("↵")
                  for (j, p) in parts.enumerate() {
                    [#p]
                    if j < parts.len() - 1 { ret-tag }
                  }
                }
              }
            })
            h(3fr)
          }
        }
        if hang {
          // Each line is its own paragraph so its hanging-indent applies
          // independently. par() applies the indent reliably (block + set
          // par(hanging-indent) does not propagate the rule to the body).
          // Spacing is set explicitly per-call: code-leading − cs cancels
          // each paragraph's natural line-height contribution so adjacent
          // par()s flow with the same baseline distance as wrapped lines
          // within a par.
          // Leading scales by the same point delta as code-size:
          //   leading = code-leading − (default code-size − cs)
          // so a 1.5pt smaller font compresses the leading by 1.5pt too.
          // Spacing is set to the same value so adjacent par()s flow
          // with the same baseline distance as wrapped lines within a par.
          let scaled-leading = code-leading - (code-size - cs)
          par(
            hanging-indent: hang-w,
            leading: scaled-leading,
            spacing: scaled-leading,
            line-body,
          )
        } else {
          line-body
          if i < lines.len() - 1 { linebreak() }
        }
      }
    }))
    v(cb)
    if not code-no-fr.get() { v(0.2fr) }
  }

  // Inline code — body default uses the body-tuned `code-size` (slightly
  // tighter than body text). Heading-level rules above install their own
  // size-aware override so backticked code inside a heading inherits the
  // heading size instead of being pinned to body code-size. Keyword
  // cross-linking to Chapter 3 is preserved by `_inline-raw-render`.
  show raw.where(block: false): it => {
    let cs-override = code-size-override.get()
    let target = if cs-override != none { cs-override } else { code-size }
    _inline-raw-render(it, target)
  }

  // ── Lists ───────────────────────────────────────────────────────

  set enum(indent: list-indent, body-indent: list-body-indent)
  set list(indent: list-indent, body-indent: list-body-indent, marker: [•])

  // ── Tables ──────────────────────────────────────────────────────

  set table(
    stroke: 0.5pt + rule-grey,
    inset: 6pt,
  )

  // ── Body ────────────────────────────────────────────────────────

  body
}

// ─── Helper functions ────────────────────────────────────────────────

// BASIC keyword entry (for Chapter 3 reference section)
#let keyword(name, syntax: none, description, example: none, example-size: none, example-leading: none, associated: none, idx-as: none, no-idx: false) = {
  v(1.5em)

  // Anchor label so inline-code references (e.g. `PRINT`) can link here.
  // Only emit the label on the first occurrence of a given name — some
  // keywords (e.g. RUN) have two #keyword() entries for different forms,
  // and Typst labels must be unique.
  context {
    let seen = _kw-seen.get()
    if name not in seen {
      _kw-seen.update(s => s + (name,))
      [#metadata(name) #label(_kw-label-name(name))]
    }
  }
  // Index marker. Default term is `name`; pass `idx-as: "FOO (LOGO)"`
  // to override (e.g. uppercase LOGO procedures, "(LOGO)" disambiguators
  // for names colliding with BASIC). Pass `no-idx: true` to suppress
  // (e.g. LOGO operator overloads "+", "-" that aren't index-worthy).
  if not no-idx {
    let term = if idx-as != none { idx-as } else { name }
    [#metadata(term)<idx>]
  }

  // Name + syntax + example: unbreakable unit
  block(breakable: false, below: 0pt)[
    #text(font: heading-font, size: 14pt, weight: "bold")[#name]
    #v(2pt)

    #if syntax != none {
      text(size: 11.5pt)[#syntax]
      v(4pt)
    }

    #if example != none {
      {
        let es = if example-size != none { example-size } else { code-size }
        let el = if example-leading != none { example-leading } else { code-leading - (code-size - es) }
        show raw.where(block: true): it => {
          set par(leading: el)
          pad(left: 1.5em, text(font: code-font, weight: "medium", size: es, tracking: code-tracking, {
            let lines = it.text.split("\n")
            for (i, line) in lines.enumerate() {
              [#line]
              if i < lines.len() - 1 { linebreak() }
            }
          }))
        }
        example
      }
      v(1em)
    }
  ]

  // Description — can break between paragraphs for long entries
  text(font: body-font, size: 10pt)[#description]

  // Associated keywords — sticks to description above
  if associated != none {
    block(breakable: false)[
      #v(2pt)
      #text(size: 10pt)[
        #i18n("associated-kws"): #associated
      ]
    ]
  }
  v(0.1fr)  
}

// Intro paragraph — larger font on chapter opening pages
#let intro(body) = {
  v(1fr)
  set text(size: intro-size, tracking: intro-tracking)
  set par(leading: intro-leading, spacing: intro-spacing)
  show raw.where(block: false): it => box(inset: (x: 1pt), text(font: code-font, size: 1.05em, tracking: code-tracking)[#it])
  body
  v(1fr)
}

// Keyword cross-reference in body text — body font, uppercase, monospace
#let kw(name) = context {
  let styled = text(font: code-font, size: code-size, tracking: code-tracking, weight: "regular")[#h(1pt)#upper(name)#h(1pt)]
  let uname = upper(name)
  if uname in basic-keywords and kw-linking.get() {
    link(label(_kw-label-name(uname)), styled)
  } else {
    styled
  }
}

// Table/figure caption — Helvetica regular
#let cap(body) = move(dy:-6pt)[#text(font: "Helvetica", size: body-size)[#body]]

// Keycap glyph wrapper — renders Unicode keyboard arrows ⇦⇧⇨⇩ (and ↵, ⏎)
// in JuliaMono, whose thin-geometric drawing matches the schoolbook-keycap
// look of the original 1985 manual better than the body-font fallback.
#let key(body) = text(font: "JuliaMono")[#body]

// Control-code character display — Cascadia Code with ss20 stylistic set
// (ss20 remaps the Control-Pictures block U+2400..U+2424 to decorative
// pictogram glyphs — bell, return symbol, dial faces, etc.) Pass the
// Control-Picture character directly, e.g. #cc[␇] for BEL.
#let cc(body) = text(
  font: "Cascadia Code",
  features: ("ss20",),
)[#body]

// Syntax line helpers — for mixed-font command syntax
// #cmd[] — literal text the user types (monospace code font)
// #arg[] — placeholder the user substitutes (italic body font, with angle brackets)
// #opt[] — optional grouping brackets (body font)
#let cmd(body) = box(inset: (x: 1pt), text(font: code-font, weight:"semibold", tracking: code-tracking)[#body])
#let arg(name) = text(font: body-font, size: 10.5pt)[‹#name›]
#let opt(body) = text(font: body-font)[\[#body\]]

// konCePCja-specific inline element macros (emulator manual). Modelled on the
// cmd/arg/kw family above; code-font variants for the recurring technical
// element types the outline needs, plus a keycap and a menu-path renderer.
// #ipc-cmd[]  — IPC protocol command literal, e.g. #ipc-cmd[mem read 0x4000 16]
// #cfg-key[]  — config option key, e.g. #cfg-key[system.model]
// #port[]     — I/O port address, e.g. #port[&FD06]
// #reg[]      — Z80 register, e.g. #reg[HL]
// #fkey[]     — physical function-key cap, e.g. #fkey[F12]
// #menu-path[]— menu navigation path, e.g. #menu-path("Settings", "Video")
// Each macro is target-aware: in the PDF (paged) build it renders with the
// styled-text/box version; in the HTML build it emits a classed semantic element
// (web/style.css styles the classes). See KTD: one source, two presentations.
#let ipc-cmd(body) = context {
  if target() == "html" { html.elem("code", attrs: (class: "ipc-cmd"), body) }
  else { box(inset: (x: 1pt), text(font: code-font, weight: "semibold", tracking: code-tracking)[#body]) }
}
#let cfg-key(body) = context {
  if target() == "html" { html.elem("code", attrs: (class: "cfg"), body) }
  else { text(font: code-font, tracking: code-tracking)[#body] }
}
#let port(body) = context {
  if target() == "html" { html.elem("code", attrs: (class: "port"), body) }
  else { text(font: code-font, fill: amstrad-blue, tracking: code-tracking)[#body] }
}
#let reg(body) = context {
  if target() == "html" { html.elem("code", attrs: (class: "reg"), body) }
  else { text(font: code-font, weight: "semibold", tracking: code-tracking)[#body] }
}
#let fkey(body) = context {
  if target() == "html" { html.elem("kbd", attrs: (class: "fkey"), body) }
  else {
    box(inset: (x: 4pt, y: 0.5pt), outset: (y: 2pt), radius: 2pt,
        stroke: 0.5pt + rule-grey, fill: code-bg, text(font: code-font, size: 0.85em)[#body])
  }
}
#let menu-path(..items) = context {
  if target() == "html" {
    html.elem("span", attrs: (class: "menu-path"),
      items.pos().map(part => html.elem("span", part))
        .join(html.elem("span", attrs: (class: "sep"), [ › ])))
  } else {
    items.pos().map(part => text(font: body-font, style: "italic")[#part]).join(text(fill: rule-grey)[ › ])
  }
}

// Chapter part heading ("Part 1: Setting Up ....")
// pagebreak unless it's the first part right after a chapter heading
// Emits a level-2 heading (outlined: false) so it's queryable for TOC page numbers
#let part(number, title) = {
  if number > 1 {
    pagebreak(weak: true)
    v(0.3fr)
  }
  // v(h2-above)
  heading(level: 2, outlined: false, bookmarked: false)[#text(size: part-size)[#i18n("part") #number: #title]]
  v(8pt)
  v(0.1666fr)
}

// Set chapter footer title separately from the heading text
// Use before the = heading: #set-chapter("Foundation Course")
#let set-chapter(short-title) = {
  chapter-title.update(short-title)
  is-chapter-page.update(true)
  chapter-h1-sep.update([ \ ])
  // Page break here (before the heading element) so the heading's location
  // is on the new page — fixes PDF outline bookmark destinations.
  v(1fr)
  pagebreak(weak: true)
}

// Set appendix mode — resets numbering and uses "Appendix" label in footer/heading
// Use before the = heading: #set-appendix(5, "Revisions and Updates")
#let set-appendix(num, short-title) = {
  chapter-title.update(short-title)
  chapter-number.update(num - 1)  // will be incremented by the heading show rule
  chapter-label.update("appendix")
  is-chapter-page.update(true)
  v(1fr)
  pagebreak(weak: true)
}

// Hardware label on the Amstrad case/monitor (bold Helvetica, like the physical labels)
// Use for: TAPE, PRINTER, STEREO, JOYSTICK, VOLUME, FLOPPY DISC, etc. on the CPC or its monitor
// Do NOT use for labels on external devices
// box() prevents the label from breaking across lines; hyphenate:false
// stops the dictionary from chopping multi-syllable labels (e.g.
// BRIGHTNESS, EXPANSION) — both important for Polish hyphenation.
#let hw(label) = box(text(font: "Helvetica", weight: "bold", hyphenate: false)[#label])

// ─── Page-number cross-references ─────────────────────────────
// `#anchor(<label>)` marks an in-content target with a label
// that can be resolved to its in-chapter page number later.
// `#pref(<lbl>)` prints the in-chapter page number (1-based per
// chapter, matching our running-footer convention).
// `#cref(<lbl>)` prints "Chapter N page P" or "Appendix N page P"
// depending on which chapter contains the anchor.
//
// Both resolve dynamically via `query()` at layout time, so they
// auto-update when content moves between pages.
#let anchor(lbl) = [#metadata("anchor") #lbl]

#let _resolve-page(lbl) = {
  let target-loc = query(lbl).first().location()
  let abs = target-loc.page()
  // Find which chapter the target lives in by scanning level-1
  // headings — whichever one starts ≤ target page is the owner.
  let chapters = query(heading.where(level: 1))
  let owner = none
  for h in chapters {
    if h.location().page() <= abs { owner = h } else { break }
  }
  if owner == none {
    return (abs: abs, ch-page: abs, ch-num: 0, is-app: false, ch-heading: none)
  }
  let ch-start = owner.location().page()
  let ch-page = abs - ch-start + 1
  // Read template states AT the target anchor's location — by the
  // time we hit the anchor, the chapter's show rule has already
  // updated `chapter-number` and `chapter-label`.
  let ch-num = chapter-number.at(target-loc)
  let is-app = chapter-label.at(target-loc) == "appendix"
  (abs: abs, ch-page: ch-page, ch-num: ch-num, is-app: is-app, ch-heading: owner)
}

#let pref(lbl) = context [#_resolve-page(lbl).ch-page]

#let cref(lbl) = context {
  let r = _resolve-page(lbl)
  if r.ch-heading == none {
    [#i18n("page-low") #r.abs]
  } else {
    [#i18n(if r.is-app { "appendix" } else { "chapter" }) #r.ch-num, #i18n("page-low") #r.ch-page]
  }
}

// ─── Dynamic index ────────────────────────────────────────────
// `#idx("MODE 1")` — place in body where the term is introduced /
// discussed. Zero-width metadata marker; never produces visible
// output. Multiple labels with the same term across the body all
// resolve to a chapter.page reference list in the index.
//
// `#make-index()` — placed in App 4. Queries every <idx> marker,
// groups by term, sorts (locale-aware via `sort-key`), emits
// term + dot-leader + page list, with letter dividers when the
// first character of the term changes.
#let idx(term) = [#metadata(term)<idx>]

// Resolve an idx-marker location to a "chapter.page" string in
// the same convention as the hand-numbered index (1.44, 5.7, etc.)
// — appendices use the appendix number (e.g. App 4 → "A4.3" would
// be silly; we use the chapter-number register, which for
// appendices is the appendix number, so App 4 page 3 → "4.3"
// is ambiguous with Ch4 page 3. Index entries for appendices are
// rare in the original; for now emit "A{n}.{p}" for appendix-owned
// references and "{n}.{p}" for chapter-owned references.
#let _idx-resolve(loc) = {
  let abs = loc.page()
  let chapters = query(heading.where(level: 1))
  let owner = none
  for h in chapters {
    if h.location().page() <= abs { owner = h } else { break }
  }
  if owner == none {
    return (sort: "", display: str(abs))
  }
  let ch-start = owner.location().page()
  let ch-page = abs - ch-start + 1
  let ch-num = chapter-number.at(loc)
  let is-app = chapter-label.at(loc) == "appendix"
  let prefix = if is-app { "A" + str(ch-num) } else { str(ch-num) }
  let display = prefix + "." + str(ch-page)
  // Sort key: pad chapter and page so "1.5" sorts before "1.50"
  // and chapters sort numerically.
  let sort-num = if is-app { 100 + ch-num } else { ch-num }
  let sort = str(sort-num) + "." + ("0000" + str(ch-page)).slice(-4)
  (sort: sort, display: display)
}

// Letter divider used by make-index. Visible bold letter,
// extra space above. Override locally if a different style is
// wanted in App 4 directly.
#let _idx-letter(c) = {
  v(1.2em)
  text(font: heading-font, size: 14pt, weight: "bold")[#c]
  linebreak()
}

// Entry row used by make-index. Term left, dot leader, page refs right.
#let _idx-row(term, refs) = {
  box(width: 1fr)[
    #set text(size: 9pt)
    #term #box(width: 1fr)[#h(2pt)#repeat[.]#h(2pt)] #refs
  ]
  linebreak()
}

// Sort key for terms — strips leading non-letter characters
// (so ".APV" sorts under A, "|BANK READ|" under B) and uppercases.
#let _idx-letter-set = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyzĄąĆćĘęŁłŃńÓóŚśŹźŻż"
#let _idx-sort-key(term) = {
  let s = term
  // Drop leading punctuation/symbols.
  while s.len() > 0 and not _idx-letter-set.contains(s.first()) {
    s = s.slice(1)
  }
  upper(s)
}

// Build the dynamic index. Place once in App 4 inside `#columns(2)[ … ]`.
// Optional `letter` argument lets a translation override the divider style.
#let make-index(letter: _idx-letter, row: _idx-row) = context {
  let markers = query(<idx>)
  // Group by term value (the metadata payload).
  let groups = (:)
  for m in markers {
    let term = m.value
    let resolved = _idx-resolve(m.location())
    if term in groups {
      groups.at(term).push(resolved)
    } else {
      groups.insert(term, (resolved,))
    }
  }
  // Sorted list of (term, refs[]) pairs.
  let entries = groups.pairs()
    .map(((t, r)) => (t, r.dedup(key: x => x.display)))
    .sorted(key: ((t, r)) => _idx-sort-key(t))
  // Walk entries, emitting letter dividers on first-char change.
  let prev-letter = ""
  for (term, refs) in entries {
    let key = _idx-sort-key(term)
    let cur-letter = if key.len() > 0 { upper(key.first()) } else { "" }
    if cur-letter != prev-letter {
      letter(cur-letter)
      prev-letter = cur-letter
    }
    let pages = refs.sorted(key: r => r.sort).map(r => r.display).join(" ")
    row(term, pages)
  }
}

// Cross-reference helper for chapters, appendices, and parts.
// Usage:
//   #xref(ch: "ch7")                → "Chapter 7" (linked)
//   #xref(ch: "ch7", part: 9)       → "Chapter 7, Part 9" (linked)
//   #xref(ch: "app5")               → "Appendix 5" (linked)
//   #xref(ch: "ch7", part: 9, short: true) → "Part 9" only (linked, omits chapter word)
//
// `ch` is the label anchor string ("ch1".."ch9", "app1".."app6").
// `part` is the integer Part number; the resulting link targets
// "{ch}-p{part}", which must exist as a heading label.
#let xref(ch: none, part: none, short: false) = {
  let segs = ()
  if ch != none and not short {
    let n = ch.trim(regex("[^0-9]"))
    let word = i18n(if ch.starts-with("app") { "appendix" } else { "chapter" })
    segs.push(link(label(ch))[#word #n])
  }
  if part != none {
    let target = if ch != none { ch + "-p" + str(part) } else { "p" + str(part) }
    segs.push(link(label(target))[#i18n("part") #str(part)])
  }
  if segs.len() == 0 {
    return [] // nothing to render
  }
  segs.join(", ")
}

// Inline code font for use in tables (bypasses template raw override)
#let c(body, size: 7.5pt) = text(font: code-font, size: size, body)

// CPC screen display — 40×25 character grid with accurate colours
// Yellow text (#FFFF00) on blue background (#000080), period-accurate CGA font
#let cpc-font = "Amstrad CPC correct"
#let cpc-screen(body) = {
  v(6pt)
  align(center,
    box(
      fill: rgb("#000080"),
      inset: 3em,
      radius: 2pt,
      align(left,
        {
          set text(font: cpc-font, size: 8pt, fill: rgb("#FFFF00"), top-edge: "ascender", bottom-edge: "descender")
          set par(leading: 0em, justify: false)
          body
        }
      )
    )
  )
  v(6pt)
}

// Note/tip box
#let note(body) = {
  v(4pt)
  set text(font: body-font)
  pad(left: 1.5em, right: 1.5em)[*#i18n("note-label")* --- #body]
  v(4pt)
}
