#import "../template.typ": *
#set-chapter("Configuration")

= Configuration

#intro[
  konCePCja reads its settings from a plain-text configuration file and from
  command-line arguments. This chapter covers where that file lives, the options
  you are most likely to change, and how to override any setting without editing
  the file.
]

== The configuration file

#idx("configuration file")konCePCja looks for a file named #cfg-key[koncepcja.cfg]
in the following locations, in order of precedence (the first one found wins):

+ the path given with #cmd[--cfg_file] (`-c`) on the command line;
+ #cfg-key[\$XDG_CONFIG_HOME/koncepcja/koncepcja.cfg] (or
  `~/.config/koncepcja/koncepcja.cfg`) — your profile configuration;
+ the older flat paths #cfg-key[\$XDG_CONFIG_HOME/koncepcja.cfg]
  (or `~/.config/koncepcja.cfg`) and #cfg-key[~/.koncepcja.cfg];
+ #cfg-key[koncepcja.cfg] in the current working directory, then next to the
  binary;
+ #cfg-key[/etc/koncepcja.cfg];
+ on macOS, #cfg-key[koncepcja.cfg] in the app bundle's `Resources/` folder.

Your profile configuration therefore wins over a #cfg-key[koncepcja.cfg]
that happens to sit in the directory you launch from. The Settings ▸ System
tab shows which file is in use; over IPC, `config get file` reports it. The
window-layout file (`imgui.ini`) and the DevTools `layouts/` folder are kept
next to whichever configuration file is in use.

The file is divided into `[section]` headings, each containing
#cfg-key[key=value] lines.

== Key options

=== System

```ini
[system]
model=2           ; 0=464, 1=664, 2=6128, 3=6128+
ram_size=128      ; RAM in KB
speed=4           ; clock speed in MHz
```

#idx("CPC model")The #cfg-key[system.model] option selects which machine to
emulate. The default, `2`, is the CPC 6128; `3` is the 6128+ (needed for
cartridges and ASIC features).

=== Video

```ini
[video]
scr_scale=2       ; window scale factor
scr_style=1       ; rendering style (0-11)
scr_window=1      ; 1 = start windowed, 0 = start fullscreen
vsync=1           ; 1=VSYNC on (default)
```

#note[
  Keep `vsync=1` on a variable-refresh-rate (VRR) display. VRR locks the
  panel's refresh to the cadence at which frames are presented; with `vsync=0`
  the main window may present in IMMEDIATE mode, which has no cadence for the
  panel to follow, and the picture visibly snaps back to earlier frames. With
  VSYNC a VRR display settles onto the CPC's own 50 Hz. `vsync=0` exists only
  to work around the present stall some remote-desktop setups cause; emulation
  speed is the same either way.
]

=== Sound

```ini
[sound]
enabled=1
playback_rate=2   ; 0=11025, 1=22050, 2=44100, 3=48000, 4=96000 Hz
```

== Overriding settings on the command line

#idx("override")Any configuration option can be overridden for a single run with
#cmd[--override] (`-O`), without touching the file. The argument is
#cfg-key[section.key=value], and the flag may be repeated:

```
./koncepcja -O system.model=3 -O sound.snd_enabled=0 game.dsk
```

This is the quickest way to try a different machine or disable sound for one
session. A full reference of every configuration option is given in Appendix C.
