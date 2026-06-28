#import "../template.typ": *
#set-chapter("IPC Debugging Interface")

= IPC Debugging Interface

#intro[
  konCePCja runs a TCP server that lets external scripts and tools control and
  inspect the emulated machine: pause and resume it, read and write memory and
  registers, set breakpoints, step, inject input, and capture screenshots. This
  is the foundation for automated testing and remote debugging.
]

== Connecting

#idx("IPC")The server listens on #port[port 6543] (localhost only). The protocol
is line-based plain text --- one command per line, one response per line --- so
any tool that speaks TCP works:

```
nc localhost 6543
socat - TCP:localhost:6543
```

Send #ipc-cmd[help] to list the available commands, or #ipc-cmd[ping] to check
the connection (it replies #cmd[OK pong]).

== Command groups

The commands fall into a few groups. A complete quick reference is in
Appendix B; the most useful are:

#table(
  columns: (auto, 1fr),
  stroke: 0.4pt + rule-grey,
  inset: 5pt,
  [*Command*], [*Purpose*],
  [#ipc-cmd[pause] / #ipc-cmd[run]], [Pause or resume emulation],
  [#ipc-cmd[reset]], [Reset the CPC],
  [#ipc-cmd[regs] / #ipc-cmd[reg get/set]], [Read or write Z80 registers],
  [#ipc-cmd[mem read/write]], [Read or write memory],
  [#ipc-cmd[bp add/del/list]], [Manage execution breakpoints],
  [#ipc-cmd[step \[n\]]], [Step N instructions],
  [#ipc-cmd[wait pc/mem/bp/vbl]], [Block until a condition is met],
  [#ipc-cmd[disasm]], [Disassemble memory],
  [#ipc-cmd[screenshot] / #ipc-cmd[snapshot]], [Capture image or machine state],
  [#ipc-cmd[load]], [Load a `.dsk` / `.sna` / `.cpr` / `.bin`],
)

== Injecting input

#idx("input injection")Keyboard, joystick, and mouse input can be driven over
IPC. Keys take a friendly name or a single character:

```
input keydown SHIFT      # press and hold
input key RETURN         # tap (press, hold, release)
input type "mode 1"      # type literal text
input joy 0 F1           # joystick 0 fire 1
input mouse move 10 -4   # relative mouse motion (needs a mouse device)
```

#note[
  #ipc-cmd[input type] emits only mapped characters --- it does not interpret
  the WinAPE `~KEY~` syntax. For special keys, key holds, or multi-line entry,
  use the #ipc-cmd[autotype] command, which supports tokens such as `~RETURN~`.
]

== A scripting example

Type and run a one-line BASIC program, then capture the result:

```
echo 'input type "10 PRINT CHR\$(42)"' | nc -w 5 localhost 6543
echo 'input key RETURN'                | nc -w 2 localhost 6543
echo 'autotype "run~RETURN~"'          | nc -w 5 localhost 6543
echo 'step frame 20'                   | nc -w 5 localhost 6543
echo 'screenshot /tmp/result.png'      | nc -w 1 localhost 6543
```

Run the emulator headless (no window) for continuous integration with the
#cmd[--headless] flag; the IPC protocol is identical in headless and windowed
modes.
