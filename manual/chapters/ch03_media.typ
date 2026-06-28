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

== ZIP archives

#idx("ZIP archive")Much CPC software is distributed as `.zip` archives.
konCePCja can open a ZIP directly: it reads the archive's directory, finds a file
with a recognised CPC extension (`.dsk`, `.cdt`, or `.sna`) inside, and loads it
--- so you do not have to unpack downloads by hand before running them.

== IPF discs

#idx("IPF")Some heavily copy-protected discs are preserved in the `.ipf`
(Interchangeable Preservation Format) rather than `.dsk`. konCePCja loads `.ipf`
images into a drive just like ordinary disc images, reproducing the original
protection so the software runs as it did from the real disc.
