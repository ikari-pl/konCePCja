// Localisation strings for the CPC6128 manual.
//
// Consumed by template.typ. To produce a Polish edition, call
// `manual-lang.update("pl")` in the build entry point before the first
// chapter — see manual_build_pl.typ.
//
// Each string set is a dictionary with the same keys; add a new language by
// adding a new entry to `strings` keyed by ISO 639-1 code.

// Active manual language ("en", "pl", ...). Defined here so this module
// is the single source of truth for the language switch.
#let manual-lang = state("manual-lang", "en")

#let strings = (
  "en": (
    chapter:            "Chapter",
    appendix:           "Appendix",
    section:            "Section",
    part:               "Part",
    page-cap:           "Page",         // "Chapter 5  Page 12" — running header
    page-low:           "page",         // "see Chapter 5, page 12" — inline
    note-label:         "NOTE",
    associated-kws:     "Associated keywords",
    // Smart-quote pair (single, double). English: 'curly' "double".
    quotes-single:      "‘’",
    quotes-double:      "“”",
  ),
  "pl": (
    chapter:            "Rozdział",
    appendix:           "Dodatek",
    section:            "Część",
    part:               "Część",        // PL uses "Część" for both English Part and Section
    page-cap:           "Strona",
    page-low:           "str.",         // Polish convention: "patrz Rozdział 5, str. 12"
    note-label:         "UWAGA",
    associated-kws:     "Powiązane słowa kluczowe",
    // Polish typography: low-9 opening, regular-9 closing for both single
    // and double. „double‟ ‚single‛.
    quotes-single:      "‚‛",
    quotes-double:      "„”",
  ),
)

// Look up a string key for the active language. Falls back to English on
// missing entries so a half-localised dict still produces a valid build.
#let i18n(key) = context {
  let lang = manual-lang.get()
  let table = strings.at(lang, default: strings.en)
  table.at(key, default: strings.en.at(key))
}

// Convenience helpers for the most common labels — return content directly.
#let lbl-chapter()        = i18n("chapter")
#let lbl-appendix()       = i18n("appendix")
#let lbl-section()        = i18n("section")
#let lbl-part()           = i18n("part")
#let lbl-page-cap()       = i18n("page-cap")
#let lbl-page-low()       = i18n("page-low")
#let lbl-note()           = i18n("note-label")
#let lbl-associated-kws() = i18n("associated-kws")
