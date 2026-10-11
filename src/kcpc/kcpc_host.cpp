#include "kcpc/kcpc_host.h"

#include <cstdint>

#include "kcpc/kcpc_server.h"
#include "koncepcja.h"
#include "startup_manifest.h"
#include "subcycle_bridge.h"

extern t_CPC CPC;
extern t_CRTC CRTC;
extern byte* memmap_ROM[256];
extern dword dwFrameCountOverall;

namespace {
kcpc::Server g_kcpc_server;
}  // namespace

bool kcpc_host_machine_info(kcpc::MachineInfo& info) {
  // The values below are host configuration the main thread rewrites when it
  // rebuilds the machine, and a frame counter the runner increments. The
  // lease is what makes reading them from a socket thread safe.
  CpcPauseLease lease;
  if (!lease.idle()) {
    lease.restore_run_state();
    return false;
  }
  constexpr unsigned kModel6128Plus = 3;
  info.model = static_cast<std::uint8_t>(CPC.model);
  info.crtc_type = CRTC.crtc_type;
  info.effective_tier =
      static_cast<std::uint8_t>(subcycle_bridge_effective_tier_index());
  info.flags = 0;
  if (g_headless) info.flags |= kcpc::kMachineFlagHeadless;
  if (CPC.model >= kModel6128Plus) info.flags |= kcpc::kMachineFlagAsic;
  info.ram_bytes = static_cast<std::uint32_t>(CPC.ram_size) * 1024U;
  info.rom_slots = 0;
  for (int slot = 0; slot < 32; ++slot) {
    if (memmap_ROM[slot] != nullptr) info.rom_slots |= 1U << slot;
  }
  info.stamp.frame = dwFrameCountOverall;
  info.stamp.paused = lease.was_paused();
  lease.restore_run_state();
  return true;
}

void kcpc_host_start(bool enabled) {
  if (!enabled) return;
  kcpc::ServerConfig config;
#ifdef KONCPC_VERSION_STRING
  config.server_version = KONCPC_VERSION_STRING;
#endif
  config.pid = static_cast<std::uint32_t>(startup_manifest_self_pid());
  config.machine_info = &kcpc_host_machine_info;
  g_kcpc_server.start(config);
}

void kcpc_host_stop() { g_kcpc_server.stop(); }

int kcpc_host_port() { return g_kcpc_server.port(); }
