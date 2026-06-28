#import "../template.typ": *
#set-chapter("The Interface")

= The konCePCja Interface

#intro[
  konCePCja wraps the emulated machine in a thin modern interface: a menu bar, a
  set of function-key shortcuts, an on-screen display, and a virtual keyboard.
  This chapter is your map to the controls.
]

== The main window

#idx("main window")The window shows the live CPC display. The emulated machine
runs continuously; you interact with it by typing (the host keyboard maps to the
CPC keyboard) and through the menu and function keys described below.

== The menu bar

Press #fkey[F1] to open the in-emulator menu, which pauses emulation while it is
open. From here you can load media, save and load snapshots, reach the settings
dialog, and open the developer tools.

== Function keys

#idx("function keys")The function keys are the quickest way to drive the
emulator:

#table(
  columns: (auto, 1fr),
  stroke: 0.4pt + rule-grey,
  inset: 5pt,
  [*Key*], [*Action*],
  [#fkey[F1]], [Open the menu (pauses emulation)],
  [#fkey[F2]], [Toggle fullscreen],
  [#fkey[F3]], [Take a screenshot],
  [#fkey[F4]], [Tape play / pause],
  [#fkey[F5]], [Reset the CPC],
  [#fkey[F6]], [Multiface II stop button],
  [#fkey[F7]], [Toggle joystick emulation],
  [#fkey[F8]], [Toggle the FPS display],
  [#fkey[F9]], [Toggle the speed limiter],
  [#fkey[F10]], [Exit konCePCja],
  [#fkey[F12]], [Toggle the developer tools],
  [#fkey[Shift+F1]], [Virtual keyboard],
  [#fkey[Shift+F3]], [Save a snapshot],
)

== Virtual keyboard

#idx("virtual keyboard")Some CPC keys have no obvious host equivalent. Press
#fkey[Shift+F1] for an on-screen CPC keyboard you can click, showing the true
CPC layout including the numeric keypad and the #cmd[COPY] key.

== On-screen display

#idx("OSD")Brief status messages --- a snapshot saved, the speed limiter toggled
--- appear as a transient on-screen display overlaid on the CPC picture.

== Screenshots

Press #fkey[F3] to save a PNG screenshot of the current CPC display.

== Command palette

// [U7-STUB] document from src/command_palette.{cpp,h}

== Configuration profiles

// [U7-STUB] document from src/config_profile.{cpp,h}
