#import "../template.typ": *
#set-chapter("Peripheral Expansions")

= Peripheral Expansions

#intro[
  Beyond the core machine, konCePCja emulates a range of expansion hardware that
  was sold for the CPC over the years --- sound add-ons, real-time clocks, mice,
  a multi-function interface board, and a light gun. Each is enabled through
  configuration (Chapter 4) or the menu, and is listed here with the I/O it uses.
]

#figure(image("../images/back-connectors.svg", width: 100%), caption: [The sockets on the back of a real CPC. The peripherals in this chapter attach here: sound DACs and the SmartWatch through the expansion socket, the AMX mouse on the joystick socket, and the Symbiface II and M4 boards on the expansion socket])

== Enabling a peripheral

#idx("peripherals, enabling")Peripherals are off by default. Most are switched on
with a configuration key (Chapter 4): for a single session use the #cmd[-O]
override, or set it in #cfg-key[koncepcja.cfg] to make it permanent. For example,
to start with both the AMX mouse and the Symbiface II board active:

```
./koncepcja -O input.amx_mouse=1 -O peripheral.symbiface=1 game.dsk
```

The light gun (phaser) is toggled at runtime with its function key instead. Each
section below names the I/O ports its device uses --- which the developer tools
(Chapter 7) can watch with an I/O breakpoint when you are debugging peripheral
software.

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
