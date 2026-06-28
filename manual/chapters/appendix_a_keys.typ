#import "../template.typ": *
#set-appendix(1, "Key Reference")

= Key Reference

The CPC keyboard differs from a modern PC keyboard in a few places. Most keys map
directly; the table below lists the ones worth knowing. When a key has no obvious
host equivalent, use the virtual keyboard (#fkey[Shift+F1]).

#table(
  columns: (auto, 1fr),
  stroke: 0.4pt + rule-grey,
  inset: 5pt,
  [*CPC key*], [*Notes*],
  [#cmd[RETURN] / #cmd[ENTER]], [The large RETURN key, and the small ENTER on the numeric keypad --- distinct keys on the CPC],
  [#cmd[DEL]], [Delete (backspace)],
  [#cmd[CLR]], [Clear --- forward delete],
  [#cmd[COPY]], [Copy-cursor key, used with the arrow keys to copy screen text],
  [#cmd[ESC]], [Escape; also breaks a running program with #cmd[CTRL]],
  [#cmd[CTRL]], [Control],
  [#cmd[SHIFT]], [Shift],
  [#cmd[CAPS LOCK]], [Caps lock],
  [Arrow keys], [Cursor movement],
  [Numeric keypad], [The CPC has a dedicated keypad (#cmd[f0]--#cmd[f9] and #cmd[.])],
)
