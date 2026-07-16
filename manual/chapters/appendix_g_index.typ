#import "../template.typ": *
#set-appendix(7, "Index")

= Index

#context if target() == "html" {
  // HTML: link each term to the section heading it first appears in. The value
  // of counter("html-head") at a marker's location is the sec-N of the enclosing
  // section (the broad heading rule steps it once per heading, in order).
  let markers = query(<idx>)
  let seen = (:)
  for m in markers {
    let t = m.value
    if t not in seen { seen.insert(t, counter("html-head").at(m.location()).first()) }
  }
  let terms = seen.keys().sorted(key: t => lower(t))
  html.elem("ul", attrs: (class: "manual-index"),
    terms.map(t => html.elem("li",
      html.elem("a", attrs: (href: "#sec-" + str(seen.at(t))), t))).join())
} else {
  columns(2)[ #make-index() ]
}
