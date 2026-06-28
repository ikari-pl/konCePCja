#import "../template.typ": *
#set-chapter("Telnet Console")

= Telnet Console

#intro[
  Alongside the binary IPC protocol, konCePCja offers a human-friendly text
  console: a remote terminal for the emulated CPC that mirrors everything the
  machine prints and accepts your keystrokes in return.
]

== Connecting

#idx("telnet console")The console listens on #port[port 6544] (one higher than
the IPC port). Connect with any terminal client:

```
nc localhost 6544
```

Only one client is served at a time; a new connection replaces the previous one.

== What you see and type

#idx("CP/M")Output works in two ways at once. Under AMSDOS, konCePCja hooks the
firmware text-output routine, so every character the CPC prints to the screen is
echoed to your terminal. Under CP/M it hooks the BDOS console-output call, so
CP/M programs are mirrored too --- no configuration needed.

Anything you type is fed to the CPC as keyboard input. Common terminal control
sequences are translated to CPC keys: the arrow keys, #fkey[ESC], #fkey[TAB],
#fkey[DEL], and #fkey[RETURN] all work as expected, and Ctrl+C is mapped to
#fkey[ESC] for convenience.

== Uses

The console is ideal for driving a CPC BASIC or CP/M session from a script or
an SSH session, copying text in and out of the emulated machine, or watching its
output without the graphical window. It pairs naturally with the IPC interface
(Chapter 9): use IPC to set up the machine, and the telnet console to interact
with it as text.
