#import "../template.typ": *
#set-chapter("Disks, Tapes & Snapshots")

= Disks, Tapes, and Snapshots

#intro[
  Chapter 1 showed the quickest way to load a single file. This chapter covers
  the media system in more depth: the two disc drives, the tape deck, and the
  snapshot system for saving and resuming machine state.
]

== Disc drives

#idx("disc drive")The CPC has two 3-inch disc drives, A and B. Insert a `.dsk`
image into a drive by dragging it onto the window, choosing
#menu-path("File", "Load"), or naming it on the command line. With a disc in
drive A, run a program from BASIC:

```
run"game
```

konCePCja reads and writes both standard and extended `.dsk` images. Write
protection is honoured, so software that checks for it behaves correctly.

== Tape

#idx("tape")On a 464 (or any machine with the tape firmware), select the tape,
ask BASIC to load, and press #fkey[F4] to start playback:

```
|TAPE
LOAD""
```

#fkey[F4] toggles tape play and pause at any time. Both `.cdt` and `.voc` tape
images are supported.

== Cartridges

#idx("cartridge")Cartridge (`.cpr`) images are a Plus-range feature. On a 6128+
(#cfg-key[system.model] `= 3`), loading a cartridge boots the machine directly
from it.

== Snapshots

#idx("snapshot")A snapshot freezes the entire machine --- memory, registers, and
hardware state --- into a `.sna` file that resumes instantly. Save the current
state with #fkey[Shift+F3]; load one through #menu-path("File", "Load") or on the
command line. Snapshots are ideal for resuming a game exactly where you left it,
or for sharing a precise machine state.

== Archives and protected formats

// [U7-STUB] ZIP archive loading — document from src/zip.cpp, src/slotshandler.cpp
// [U7-STUB] IPF copy-protected disc format — document from src/ipf.h
