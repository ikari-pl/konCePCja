#import "../template.typ": *
#set-chapter("Peripheral Expansions")

= Peripheral Expansions

#intro[
  Beyond the core machine, konCePCja emulates a range of expansion hardware that
  was sold for the CPC over the years --- sound add-ons, real-time clocks, mice,
  a multi-function interface board, and a light gun. Each is enabled through
  configuration (Chapter 4) or the menu, and is listed here with the I/O it uses.
]

== Digiblaster

#idx("Digiblaster")A simple 8-bit digital-to-analogue converter on the printer
port. konCePCja feeds the printer-port value through a level table and mixes the
result into the sound output, reproducing sampled-audio playback software.

== AmDrum (Cheetah)

#idx("AmDrum")An 8-bit DAC drum machine on port #port[\&FFxx] (the uncontested
I/O space where all upper address bits are high). It mixes into the audio output
the same way as the Digiblaster.

== Dobbertin SmartWatch

#idx("SmartWatch")A Dallas DS1216 phantom real-time clock that sits in the upper
ROM socket. It intercepts ROM reads using a recognition pattern and returns the
host system time in BCD form, so CPC software that expects the SmartWatch reads
the correct date and time.

== AMX Mouse

#idx("AMX Mouse")A mouse that connects to the joystick port and appears on
keyboard-matrix row 9. Movement is reported as direction pulses ("mickeys") and
the buttons map onto the joystick fire lines. Enable it with
#cfg-key[input.amx_mouse] `= 1`.

== Symbiface II

#idx("Symbiface II")A multi-function expansion board providing three devices:

- *IDE hard disc* (ATA PIO) at #port[\&FD06]--#port[\&FD0F], backed by a raw
  `.img` file --- supports reading, writing, and `IDENTIFY DEVICE`.
- *Real-time clock* (DS12887) at #port[\&FD14] / #port[\&FD15], with 14 time
  registers from the host clock plus 50 bytes of CMOS NVRAM.
- *PS/2 mouse* at #port[\&FD10] / #port[\&FD18], using a multiplexed FIFO
  protocol.

Enable it with #cfg-key[peripheral.symbiface] `= 1`.

== M4 Board

#idx("M4 Board")A virtual-filesystem expansion that exposes a host directory to
the CPC as a storage device, with an embedded web file manager. It is covered in
full in Chapter 11.

== Multiface II

#idx("Multiface II")A ROM-based debugging and snapshot interface. Press #fkey[F6]
to trigger its stop button and enter its menu, as on real hardware.

== Amstrad Magnum Phaser

#idx("Magnum Phaser")A light gun emulated by intercepting the CRTC registers.
Aim with the mouse pointer over the display; the trigger maps to a fire button.

== Drive and tape sounds

#idx("drive sounds")For atmosphere, konCePCja can mix in procedurally generated
floppy-drive motor hum and head-seek clicks, and tape loading hiss, alongside the
emulated audio.
