#import "../template.typ": *
#set-appendix(2, "IPC Command Reference")

= IPC Command Reference

A quick reference to the IPC commands (Chapter 9). Connect to #port[port 6543]
and send one command per line.

#table(
  columns: (auto, 1fr),
  stroke: 0.4pt + rule-grey,
  inset: 4.5pt,
  [*Command*], [*Description*],
  [#ipc-cmd[ping]], [Test the connection (replies #cmd[OK pong])],
  [#ipc-cmd[version]], [Report version and port],
  [#ipc-cmd[help]], [List all commands],
  [#ipc-cmd[pause] / #ipc-cmd[run]], [Pause or resume emulation],
  [#ipc-cmd[reset]], [Reset the CPC],
  [#ipc-cmd[regs]], [Dump all Z80 registers],
  [#ipc-cmd[reg get \<R\>] / #ipc-cmd[reg set \<R\> \<V\>]], [Read or write one register],
  [#ipc-cmd[mem read \<addr\> \<len\>]], [Read memory (add #cmd[ascii] for text)],
  [#ipc-cmd[mem write \<addr\> \<hex\>]], [Write bytes to memory],
  [#ipc-cmd[bp add/del/list/clear]], [Manage execution breakpoints],
  [#ipc-cmd[step \[n\]]], [Step N instructions],
  [#ipc-cmd[wait pc \<addr\>]], [Block until PC reaches an address],
  [#ipc-cmd[wait mem \<addr\> \<val\>]], [Block until a memory value],
  [#ipc-cmd[wait bp \[timeout\]]], [Block until a breakpoint fires],
  [#ipc-cmd[wait vbl \<n\>]], [Wait N vertical blanks],
  [#ipc-cmd[disasm \<addr\> \<count\>]], [Disassemble],
  [#ipc-cmd[input keydown/keyup/key]], [Keyboard input],
  [#ipc-cmd[input type \<text\>]], [Type literal text],
  [#ipc-cmd[input joy \<n\> \<dir\>]], [Joystick input],
  [#ipc-cmd[input mouse move/button/buttons]], [Mouse input (needs a mouse device)],
  [#ipc-cmd[autotype \<text\>]], [Queue text with WinAPE `~KEY~` syntax],
  [#ipc-cmd[screenshot \[path\]]], [Save a screenshot],
  [#ipc-cmd[snapshot save/load \<path\>]], [Save or load machine state],
  [#ipc-cmd[load \<path\>]], [Load a `.dsk` / `.sna` / `.cpr` / `.bin`],
  [#ipc-cmd[asm text/assemble/errors/symbols]], [Drive the assembler],
  [#ipc-cmd[m4 http \<status\|start\|stop\>]], [Control the M4 web server],
  [#ipc-cmd[devtools]], [Open the developer tools],
)
