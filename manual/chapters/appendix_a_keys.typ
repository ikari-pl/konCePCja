#import "../template.typ": *
#set-appendix(1, "Key Reference")

= Key Reference

The CPC keyboard differs from a modern PC keyboard in a few places. Most keys map
directly; the table below lists the ones worth knowing. When a key has no obvious
host equivalent, use the virtual keyboard (#fkey[Shift+F1]).

#figure(image("../images/keyboard-labels.svg", width: 100%), caption: [The Amstrad CPC keyboard, with the joystick directions on the keypad])

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

== Firmware key numbers

#idx("key numbers")Internally the CPC identifies each key by a number (0--79),
not by its character. These are the numbers the firmware's `KM` routines use, and
the same numbers the keyboard matrix is wired in. They matter when you drive the
machine programmatically --- injecting key presses over the IPC interface
(Chapter 9), or reading the matrix in your own code --- because there the key is
addressed by number, independent of which character it would type.

#figure(image("../images/keyboard-keynums.svg", width: 100%), caption: [The CPC firmware key numbers, including the two joystick ports (joystick 0 shares its numbers with the cursor keys and the keypad)])
