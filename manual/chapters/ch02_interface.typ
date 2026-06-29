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

#figure(image("../images/main-display.png", width: 90%), caption: [The main window: the toolbar across the top --- the menu button, the drive and tape status, transport controls, and the speed indicator --- above the live CPC display (here running a short BASIC colour demo)])

Along the top of the window is a toolbar: the #menu-path("Menu (F1)") button,
the status of each disc drive and the tape, transport controls for tape playback,
and a speed indicator showing the current frame rate and emulation speed. It stays
visible while the machine runs, so the drive and tape state are always in view.

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

#idx("command palette")For quick access to any action without hunting through
menus, press #fkey[Ctrl+K] (or #fkey[Cmd+K] on macOS) to open the command
palette. Start typing and it filters a searchable list of every command --- each
shown with its description and keyboard shortcut --- and runs the one you choose.
It is the fastest route to a feature whose menu location you have forgotten.

== Configuration profiles

#idx("configuration profiles")A configuration profile is a named bundle of
machine settings --- model, RAM size, clock speed, frame-skip, screen scale and
scanlines, the sound options, and the joystick and keyboard modes. Save the
current settings as a named profile and switch between profiles in one step,
rather than changing options one at a time. This is convenient for keeping, say,
a "464 with tape" setup and a "6128+ with sound" setup side by side.
