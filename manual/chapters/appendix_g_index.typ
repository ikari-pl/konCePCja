#import "../template.typ": *
#set-appendix(7, "Index")

= Index

#context if target() == "html" {
  // HTML has no page numbers; emit a sorted, de-duplicated term list.
  let terms = query(<idx>).map(m => m.value)
  let uniq = ()
  for t in terms { if t not in uniq { uniq.push(t) } }
  uniq = uniq.sorted(key: t => lower(t))
  html.elem("ul", attrs: (class: "manual-index"),
    uniq.map(t => html.elem("li", t)).join())
} else {
  columns(2)[ #make-index() ]
}
