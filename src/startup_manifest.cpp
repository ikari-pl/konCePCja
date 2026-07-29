#include "startup_manifest.h"

#include <chrono>
#include <cstdio>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <process.h>  // _getpid
#else
#include <unistd.h>  // getpid
#endif

namespace {

// YAML single-quoted scalar: the only escape inside '' is '' for a literal
// quote. Used for every string so paths with spaces, ':' or '#' stay valid.
std::string quoted(const std::string& s) {
  std::string out = "'";
  for (const char c : s) {
    if (c == '\'') out += '\'';  // '' escapes a quote inside '...'
    out += c;
  }
  out += '\'';
  return out;
}

// Ports are emitted as null rather than 0 when the server is not listening, so
// a consumer cannot mistake "absent" for "port zero".
std::string port_or_null(int port) {
  if (port <= 0) return "null";
  return std::to_string(port);
}

}  // namespace

std::string startup_manifest_yaml(const StartupManifest& m) {
  std::ostringstream o;
  o << "--- # koncepcja\n";
  o << "manifest_version: 1\n";
  o << "version: " << quoted(m.version) << '\n';
  o << "build: " << quoted(m.build) << '\n';
  o << "pid: " << m.pid << '\n';
  o << "mode: " << (m.headless ? "headless" : "gui") << '\n';
  o << "ports:\n";
  o << "  ipc: " << port_or_null(m.ipc_port) << '\n';
  o << "  telnet: " << port_or_null(m.telnet_port) << '\n';
  o << "  m4_http: " << port_or_null(m.m4_http_port) << '\n';
  o << "  m4_bind_ip: " << quoted(m.m4_bind_ip) << '\n';
  o << "machine:\n";
  o << "  model: " << m.model << '\n';
  o << "  ram_size_kb: " << m.ram_size_kb << '\n';
  o << "config_file: " << quoted(m.config_file) << '\n';
  o << "...\n";
  return o.str();
}

int startup_manifest_await_port(const std::function<int()>& get_port,
                                int timeout_ms) {
  constexpr int kStepMs = 5;
  for (int waited = 0;; waited += kStepMs) {
    int const port = get_port();
    if (port > 0) return port;
    if (waited >= timeout_ms) return 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(kStepMs));
  }
}

long startup_manifest_self_pid() {
#ifdef _WIN32
  return static_cast<long>(_getpid());
#else
  return static_cast<long>(getpid());
#endif
}

void startup_manifest_emit(const StartupManifest& m) {
  std::string const yaml = startup_manifest_yaml(m);
  // stdio rather than std::cout: the rest of startup logging is a mix of both,
  // and fwrite+fflush guarantees this lands whole and immediately.
  if (fwrite(yaml.data(), 1, yaml.size(), stdout) != yaml.size()) {
    // Nothing useful to do if stdout is broken; a harness will notice the
    // missing manifest. Explicitly ignored rather than silently unchecked.
    return;
  }
  fflush(stdout);
}
