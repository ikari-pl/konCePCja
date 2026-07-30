// konCePCja — tests for the machine-consumable startup manifest.
//
// The manifest exists so harnesses stop assuming port 6543. That only holds if
// the document is well-formed and never reports a port the process is not
// actually listening on, so both properties are pinned here.

#include "startup_manifest.h"

#include <gtest/gtest.h>

#include <string>

namespace {

StartupManifest sample() {
  StartupManifest m;
  m.version = "v6.1.0";
  m.build = "15b10db6";
  m.pid = 4242;
  m.headless = false;
  m.ipc_port = 6545;
  m.telnet_port = 6546;
  m.m4_http_port = 8080;
  m.m4_bind_ip = "127.0.0.1";
  m.model = 2;
  m.ram_size_kb = 128;
  m.config_file = "/home/u/koncepcja.cfg";
  return m;
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST(StartupManifest, IsFramedAsOneYamlDocument) {
  std::string const y = startup_manifest_yaml(sample());
  // A consumer slices the manifest out of interleaved log output using these.
  EXPECT_EQ(y.rfind("--- # koncepcja", 0), 0u) << "must start with the marker";
  EXPECT_TRUE(contains(y, "\n...\n")) << "must close the YAML document";
  EXPECT_TRUE(contains(y, "manifest_version: 1"));
}

TEST(StartupManifest, ReportsActualPorts) {
  std::string const y = startup_manifest_yaml(sample());
  EXPECT_TRUE(contains(y, "ipc: 6545"));
  EXPECT_TRUE(contains(y, "telnet: 6546"));
  EXPECT_TRUE(contains(y, "m4_http: 8080"));
}

// The whole point: a server that is not listening must not look like port 0, or
// a harness could try to connect to it.
TEST(StartupManifest, AbsentServersAreNullNotZero) {
  StartupManifest m = sample();
  m.ipc_port = 0;
  m.telnet_port = -1;
  m.m4_http_port = 0;

  std::string const y = startup_manifest_yaml(m);
  EXPECT_TRUE(contains(y, "ipc: null"));
  EXPECT_TRUE(contains(y, "telnet: null"));
  EXPECT_TRUE(contains(y, "m4_http: null"));
  EXPECT_FALSE(contains(y, ": 0\n")) << "no port should be emitted as 0";
}

TEST(StartupManifest, ModeReflectsHeadless) {
  StartupManifest m = sample();
  m.headless = false;
  EXPECT_TRUE(contains(startup_manifest_yaml(m), "mode: gui"));
  m.headless = true;
  EXPECT_TRUE(contains(startup_manifest_yaml(m), "mode: headless"));
}

// Paths are user data: spaces, colons and '#' must not break the document, and
// an apostrophe must not terminate the quoted scalar early.
TEST(StartupManifest, QuotesAwkwardPaths) {
  StartupManifest m = sample();
  m.config_file = "/Users/o'brien/my games: #1/koncepcja.cfg";

  std::string const y = startup_manifest_yaml(m);
  EXPECT_TRUE(contains(y, "'/Users/o''brien/my games: #1/koncepcja.cfg'"))
      << "single quote must be doubled per YAML single-quoted style";
}

TEST(StartupManifest, EmptyStringsStayValidScalars) {
  StartupManifest m;  // all defaults: empty strings, zero ports
  std::string const y = startup_manifest_yaml(m);
  EXPECT_TRUE(contains(y, "version: ''"));
  EXPECT_TRUE(contains(y, "build: ''"));
  EXPECT_TRUE(contains(y, "config_file: ''"));
}

TEST(StartupManifest, MachineFactsAreReported) {
  std::string const y = startup_manifest_yaml(sample());
  EXPECT_TRUE(contains(y, "model: 2"));
  EXPECT_TRUE(contains(y, "ram_size_kb: 128"));
}

TEST(StartupManifest, SelfPidIsPlausible) {
  EXPECT_GT(startup_manifest_self_pid(), 0);
}

// A port that is already bound returns immediately.
TEST(StartupManifestAwaitPort, ReturnsImmediatelyWhenAlreadyBound) {
  int const port = startup_manifest_await_port([] { return 6543; }, 1000);
  EXPECT_EQ(port, 6543);
}

// Servers bind on their own thread; the manifest must wait rather than report
// 0.
TEST(StartupManifestAwaitPort, WaitsForALateBind) {
  int calls = 0;
  int const port = startup_manifest_await_port(
      [&calls] { return (++calls >= 3) ? 6545 : 0; }, 1000);
  EXPECT_EQ(port, 6545);
  EXPECT_GE(calls, 3);
}

// If it never binds we must give up and say null, not hang startup forever.
TEST(StartupManifestAwaitPort, GivesUpAtTheDeadline) {
  int const port = startup_manifest_await_port([] { return 0; }, 20);
  EXPECT_EQ(port, 0);
}

// An absent bind address must be null, not '' — a consumer should not have to
// distinguish "M4 is not running" from "the empty string".
TEST(StartupManifest, AbsentBindAddressIsNullNotEmptyString) {
  StartupManifest m = sample();
  m.m4_http_port = 0;
  m.m4_bind_ip.clear();

  std::string const y = startup_manifest_yaml(m);
  EXPECT_TRUE(contains(y, "m4_bind_ip: null"));
  EXPECT_FALSE(contains(y, "m4_bind_ip: ''"));
  // A bind address is not a port, so it must not sit inside the ports mapping.
  EXPECT_TRUE(contains(y, "\nm4_bind_ip:"));
}

// The run tier changes debugger semantics (per-cycle observability), so a
// harness needs it alongside the ports.
TEST(StartupManifest, ReportsRunTier) {
  StartupManifest m = sample();
  m.run_tier = "fast";
  EXPECT_TRUE(contains(startup_manifest_yaml(m), "run_tier: 'fast'"));
  m.run_tier.clear();
  EXPECT_TRUE(contains(startup_manifest_yaml(m), "run_tier: null"));
}
