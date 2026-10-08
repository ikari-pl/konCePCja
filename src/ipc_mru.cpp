#include "ipc_mru.h"

#include <filesystem>
#include <mutex>
#include <system_error>
#include <utility>

#include "imgui_state.h"
#include "imgui_ui_testable.h"

extern t_CPC CPC;

namespace {
std::mutex g_mru_mutex;
std::vector<std::pair<CpcMruList, std::string>> g_mru_entries;
}  // namespace

void koncpc_mru_push(std::vector<std::string>& list, const std::string& path) {
  mru_list_push(list, path, t_CPC::MRU_MAX);
  // Opening a file is a rare, explicit user action, so a full config
  // write-back here is cheap. Use the intent-preserving path so a failed
  // printer_start() or live fullscreen toggle cannot poison the file
  // (Options▸Save is the deliberate path that captures new intent).
  koncpc_save_configuration_preserving_intent();
}

std::string ipc_mru_canonical_path(const std::string& path) {
  std::error_code ec;
  auto const abs = std::filesystem::absolute(std::filesystem::path(path), ec);
  if (ec) return path;
  return abs.lexically_normal().string();
}

void ipc_mru_stage(CpcMruList list, const std::string& path) {
  // Resolved here, not at the call sites: an IPC `load game.dsk` is relative
  // to the server's cwd, and every other producer of these lists (the file
  // dialog, drag-drop) supplies an absolute path, so the menu's consumers
  // assume one. A relative entry saved into the config opens nothing the next
  // time the emulator starts somewhere else.
  std::string const resolved = ipc_mru_canonical_path(path);
  std::scoped_lock const lock(g_mru_mutex);
  g_mru_entries.emplace_back(list, resolved);
}

void ipc_mru_apply_staged(bool save_config) {
  if (imgui_state.show_options) return;
  std::vector<std::pair<CpcMruList, std::string>> entries;
  {
    std::scoped_lock const lock(g_mru_mutex);
    entries.swap(g_mru_entries);
  }
  for (const auto& [list, path] : entries) {
    if (save_config) {
      koncpc_mru_push(CPC.*list, path);
    } else {
      mru_list_push(CPC.*list, path, t_CPC::MRU_MAX);
    }
  }
}
