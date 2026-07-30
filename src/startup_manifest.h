#pragma once

// konCePCja — machine-consumable startup manifest.
//
// Everything an external harness needs to talk to this process, emitted once at
// startup as a single YAML document on stdout.
//
// WHY: every server probes forward when its preferred port is taken (IPC 6543 →
// 6545, telnet 6544 → 6546, …), so a stale instance silently steals the default
// and the next run lands somewhere else. A harness that assumes 6543 then
// drives the *zombie* and reports a false result. The ports must always be read
// from the process output — this manifest makes that a parse instead of a grep.

#include <functional>
#include <string>

// A port value of <= 0 means "that server is not running", and is emitted as
// YAML null.
struct StartupManifest {
  std::string version;  // KONCPC_VERSION_STRING
  std::string build;    // short git hash, may be empty
  long pid = 0;         //
  bool headless = false;
  int ipc_port = 0;
  int telnet_port = 0;
  int m4_http_port = 0;
  std::string m4_bind_ip;
  unsigned int model = 0;        // 0=464, 1=664, 2=6128, 3=6128+
  unsigned int ram_size_kb = 0;  //
  std::string config_file;       // may be empty if no config was found
};

// Renders the manifest as one YAML document, framed by a `--- # koncepcja` line
// and a closing `...`, so a consumer can slice it out of interleaved log
// output. Pure: no globals, no I/O — this is the part worth unit testing.
std::string startup_manifest_yaml(const StartupManifest& m);

// Servers bind on their own threads, so a port read too early reads 0 — which
// would make this manifest actively misleading. Polls get_port() until it
// returns a positive value or the deadline passes; returns 0 on timeout.
int startup_manifest_await_port(const std::function<int()>& get_port,
                                int timeout_ms);

// Writes startup_manifest_yaml(m) to stdout and flushes, so a harness reading
// the pipe sees it immediately rather than when the buffer happens to drain.
void startup_manifest_emit(const StartupManifest& m);

// This process's pid. Wraps the getpid/_getpid split so the MINGW guard lives
// in exactly one place.
long startup_manifest_self_pid();
