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

Anything you type is fed to the CPC as keyboard input. Printable characters pass
straight through, and the common terminal control sequences are translated to
their CPC equivalents:

#table(
  columns: (auto, auto, 1fr), stroke: 0.4pt + rule-grey, inset: 5pt,
  [*You type*], [*CPC key*], [*Notes*],
  [Arrow keys (`ESC [ A`…`D`)], [Cursor #fkey[Up]/#fkey[Down]/#fkey[Left]/#fkey[Right]], [Standard ANSI cursor sequences],
  [`ESC` (alone)], [#fkey[ESC]], [Bare escape],
  [`Backspace` / `DEL`], [#fkey[DEL]], [`0x7F` or `0x08`],
  [`Tab`], [#fkey[TAB]], [],
  [`Ctrl+C`], [#fkey[ESC]], [Common telnet interrupt],
  [`Return` / `Enter`], [#fkey[RETURN]], [CR and LF both map to RETURN],
  [Printable ASCII], [the matching key], [Direct pass-through],
)

== Example: a remote BASIC session

Connect, and you are talking to the CPC's BASIC prompt directly:

```
$ nc localhost 6544
10 PRINT "HELLO FROM TELNET"
20 GOTO 10
RUN
HELLO FROM TELNET
HELLO FROM TELNET
...
```

Press #fkey[Ctrl+C] (sent as #fkey[ESC]) to break the running program.

== Uses

The console is ideal for driving a CPC BASIC or CP/M session from a script or an
SSH session, copying text in and out of the emulated machine, or watching its
output without the graphical window. It pairs naturally with the IPC interface
(Chapter 9): use IPC to set up the machine deterministically, then the telnet
console to interact with it as text. Note that the port probes forward (up to
+10) if 6544 is already taken, so the actual port is reported in the log on
startup.
