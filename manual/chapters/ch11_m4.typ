#import "../template.typ": *
#set-chapter("M4 Board")

= M4 Board and Virtual Filesystem

#intro[
  The M4 Board is a popular real-world expansion that gives the CPC a network
  connection and SD-card storage. konCePCja emulates it, backing the virtual SD
  card with a directory on your host and serving the same web file manager the
  real board provides.
]

== Overview

#idx("M4 Board")The emulated M4 Board exposes a host directory to the CPC as a
storage device through a command/response protocol. A path-traversal guard keeps
CPC software confined to that directory. The board is compatible with the
`cpcxfer` tool and the M4 Board Android app.

== The web file manager

#idx("M4 HTTP")An embedded HTTP server provides a browser-based file manager for
the virtual SD card. By default it listens on #port[port 8080]:

```
http://localhost:8080/          # web file browser
http://localhost:8080/status    # JSON status
```

From the browser you can list directories, upload and delete files, create
folders, and remotely run a file on the CPC. The endpoints mirror the real M4
Board's `config.cgi` interface, so existing M4 tooling works unchanged.

== Controlling it over IPC

#idx("m4 http")The HTTP server and its port mappings are managed through IPC
(Chapter 9):

```
m4 http status        # is the server running?
m4 http start / stop  # start or stop it
m4 ports              # list CPC-to-host port mappings
m4 port set 80 8080   # map CPC port 80 to host port 8080
```

Operations that change the CPC's state (such as a reset triggered from the web
interface) are deferred to the emulator's main thread, so the board is safe to
drive from a browser while emulation runs.

== Configuration

#idx("m4 config")The board is configured in the #cfg-key[\[peripheral\]] section:

```ini
[peripheral]
m4_http_port=8080          ; HTTP server port
m4_bind_ip=127.0.0.1       ; bind address (127.0.0.2 works without root on macOS)
m4_port_map_0=80:8080:1    ; cpc_port:host_port:user_override
```
